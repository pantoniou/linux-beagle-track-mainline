/*
 * Functions for working with device tree overlays
 *
 * Copyright (C) 2012 Pantelis Antoniou <panto@antoniou-consulting.com>
 * Copyright (C) 2012 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */
#define DEBUG
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/err.h>

/* forward decl. */
static int of_overlay_tree_change(struct of_overlay_info *ovinfo,
		unsigned long action, struct device_node *node,
		struct property *prop);
/*
 * Apply a single overlay node recursively.
 *
 * Property or node names that start with '-' signal that
 * the property/node is to be removed.
 *
 * Note that the in case of an error the target node is left
 * in a inconsistent state. Error recovery should be performed
 * by using the tree changes list.
 */
static int of_overlay_apply_one(struct of_overlay_info *ovinfo,
		struct device_node *target, const struct device_node *overlay)
{
	const char *pname, *cname;
	struct device_node *child, *tchild;
	struct property *prop, *propn, *tprop;
	int remove;
	char *full_name;
	const char *suffix;
	int ret;

	/* sanity checks */
	if (target == NULL || overlay == NULL)
		return -EINVAL;

	for_each_property_of_node(overlay, prop) {

		/* don't touch, 'name' */
		if (of_prop_cmp(prop->name, "name") == 0)
			continue;

		/* default is add */
		remove = 0;
		pname = prop->name;
		if (*pname == '-') {	/* skip, - notes removal */
			pname++;
			remove = 1;
			propn = NULL;
		} else {
			propn = __of_copy_property(prop, GFP_KERNEL,
					OF_PROP_ALLOCALL);
			if (propn == NULL)
				return -ENOMEM;
		}

		tprop = of_find_property(target, pname, NULL);

		/* found? */
		if (tprop != NULL) {
			if (propn != NULL) {
				ret = of_overlay_tree_change(ovinfo,
					OF_RECONFIG_UPDATE_PROPERTY,
					target, propn);
				if (ret == 0)
					ret = of_update_property(target, propn);
			} else {
				ret = of_overlay_tree_change(ovinfo,
					OF_RECONFIG_REMOVE_PROPERTY,
					target, tprop);
				if (ret == 0)
					ret = of_remove_property(target, tprop);
			}
		} else {
			if (propn != NULL) {
				ret = of_overlay_tree_change(ovinfo,
					OF_RECONFIG_ADD_PROPERTY,
					target, propn);
				if (ret == 0)
					ret = of_add_property(target, propn);
			} else
				ret = 0;
		}
		if (ret != 0)
			return ret;
	}

	__for_each_child_of_node(overlay, child) {

		/* default is add */
		remove = 0;
		cname = child->name;
		if (*cname == '-') {	/* skip, - notes removal */
			cname++;
			remove = 1;
		}

		/* special case for nodes with a suffix */
		suffix = strrchr(child->full_name, '@');
		if (suffix != NULL) {
			cname = kbasename(child->full_name);
			WARN_ON(cname == NULL);	/* sanity check */
			if (cname == NULL)
				continue;
			if (*cname == '-')
				cname++;
		}

		tchild = of_get_child_by_name(target, cname);
		if (tchild != NULL) {

			if (!remove) {

				/* apply overlay recursively */
				ret = of_overlay_apply_one(ovinfo, tchild,
						child);
				of_node_put(tchild);

				if (ret != 0)
					return ret;

			} else {

				ret = of_overlay_tree_change(ovinfo,
					OF_RECONFIG_DETACH_NODE,
					tchild, NULL);
				if (ret == 0)
					ret = of_detach_node(tchild);

				of_node_put(tchild);
				if (ret != 0)
					return ret;
			}

		} else {

			if (!remove) {
				full_name = kasprintf(GFP_KERNEL, "%s/%s",
						target->full_name, cname);
				if (full_name == NULL)
					return -ENOMEM;

				/* create empty tree as a target */
				tchild = __of_create_empty_node(cname,
						child->type, full_name,
						child->phandle, GFP_KERNEL,
						OF_NODE_ALLOCALL);

				/* free either way */
				kfree(full_name);

				if (tchild == NULL)
					return -ENOMEM;

				/* point to parent */
				tchild->parent = target;

				ret = of_overlay_tree_change(ovinfo,
					OF_RECONFIG_ATTACH_NODE,
					tchild, NULL);
				if (ret == 0)
					ret = of_attach_node(tchild);

				if (ret != 0)
					return ret;

				/* apply the overlay */
				ret = of_overlay_apply_one(ovinfo, tchild,
						child);
				if (ret != 0) {
					__of_free_tree(tchild);
					return ret;
				}
			}
		}
	}

	return 0;
}

/*
 * Lookup an overlay device entry
 */
struct of_overlay_device_entry *of_overlay_device_entry_lookup(
		struct of_overlay_info *ovinfo, struct device_node *node)
{
	struct of_overlay_device_entry *de;

	list_for_each_entry(de, &ovinfo->de_list, node) {
		if (de->np == node)
			return de;
	}
	return NULL;
}

/*
 * Add an overlay log entry
 */
static int of_overlay_log_entry_entry_add(struct of_overlay_info *ovinfo,
		unsigned long action, struct device_node *dn,
		struct property *prop)
{
	struct of_overlay_log_entry *le;

	/* check */
	if (ovinfo == NULL || dn == NULL)
		return -EINVAL;

	le = kzalloc(sizeof(*le), GFP_KERNEL);
	if (le == NULL) {
		pr_err("%s: Failed to allocate\n", __func__);
		return -ENOMEM;
	}

	/* get a reference to the node */
	le->action = action;
	le->np = of_node_get(dn);
	le->prop = prop;

	if (action == OF_RECONFIG_UPDATE_PROPERTY && prop)
		le->old_prop = of_find_property(dn, prop->name, NULL);

	list_add_tail(&le->node, &ovinfo->le_list);

	return 0;
}

/*
 * Add an overlay device entry
 */
static void of_overlay_device_entry_entry_add(struct of_overlay_info *ovinfo,
		struct device_node *node,
		int prevstate, int state)
{
	struct of_overlay_device_entry *de;
	int fresh;

	/* check */
	if (ovinfo == NULL)
		return;

	fresh = 0;
	de = of_overlay_device_entry_lookup(ovinfo, node);
	if (de == NULL) {
		de = kzalloc(sizeof(*de), GFP_KERNEL);
		if (de == NULL) {
			pr_err("%s: Failed to allocate\n", __func__);
			return;
		}
		fresh = 1;
		de->prevstate = -1;
	}

	if (de->np == NULL)
		de->np = of_node_get(node);
	if (fresh)
		de->prevstate = prevstate;
	de->state = state;

	if (fresh)
		list_add_tail(&de->node, &ovinfo->de_list);
}

/*
 * Overlay OF change handler
 *
 * Called every time there's a property/node modification
 * Every modification causes a log entry addition, while
 * any modification that causes a node's state to change
 * from/to disabled to/from enabled causes a device entry
 * addition.
 *
 * We piggy back on the already defined OF_RECONFIG_* action
 * defines, since they suffice.
 */
static int of_overlay_tree_change(struct of_overlay_info *ovinfo,
		unsigned long action, struct device_node *node,
		struct property *prop)
{
	struct property *sprop, *cprop;
	int prevstate, state;
	int err = 0;

	/* add to the log */
	err = of_overlay_log_entry_entry_add(ovinfo, action, node, prop);
	if (err != 0)
		return err;

	/* come up with the device entry (if any) */
	state = 0;
	prevstate = 0;

	/* determine the state the node will end up */
	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		/* we demand that a compatible node is present */
		state = of_find_property(node, "compatible", NULL) &&
			of_device_is_available(node);
		break;
	case OF_RECONFIG_DETACH_NODE:
		prevstate = of_find_property(node, "compatible", NULL) &&
			of_device_is_available(node);
		state = 0;
		break;
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		/* either one cause a change in state */
		if (strcmp(prop->name, "status") != 0 &&
				strcmp(prop->name, "compatible") != 0)
			return 0;

		if (strcmp(prop->name, "status") == 0) {
			/* status */
			cprop = of_find_property(node, "compatible", NULL);
			sprop = action != OF_RECONFIG_REMOVE_PROPERTY ?
				prop : NULL;
		} else {
			/* compatible */
			sprop = of_find_property(node, "status", NULL);
			cprop = action != OF_RECONFIG_REMOVE_PROPERTY ?
				prop : NULL;
		}

		prevstate = of_find_property(node, "compatible", NULL) &&
			of_device_is_available(node);
		state = cprop && cprop->length > 0 &&
			    (!sprop || (sprop->length > 0 &&
				(strcmp(sprop->value, "okay") == 0 ||
				 strcmp(sprop->value, "ok") == 0)));
		break;

	default:
		return 0;
	}

	of_overlay_device_entry_entry_add(ovinfo, node, prevstate, state);

	return err;
}

static int of_overlay_device_entry_change(struct of_overlay_info *ovinfo,
		struct of_overlay_device_entry *de, int revert)
{
	int state;
	int ret;

	/* this funky construct is calculating a target state
	 * taking into account the state and revert inputs
	 * and making sure they are 0/1 (!! does the trick).
	 */
	state = !!de->state ^ !!revert;

	ret = of_reconfig_notify(state ?
			OF_RECONFIG_DYNAMIC_CREATE_DEV :
			OF_RECONFIG_DYNAMIC_DESTROY_DEV,
			de->np);

	if (ret != 0)
		pr_warn("%s: Failed to %s device for node '%s'\n",
				__func__, state ? "create" : "remove",
				de->np->full_name);
	return 0;
}

/*
 * Revert one overlay
 * Either due to an error, or due to normal overlay removal.
 * Using the log entries, we revert any change to the live tree.
 * In the same manner, using the device entries we enable/disable
 * the devices appropriately.
 */
static void of_overlay_revert_one(struct of_overlay_info *ovinfo)
{
	struct of_overlay_device_entry *de, *den;
	struct of_overlay_log_entry *le, *len;
	struct property *prop, **propp;
	struct device_node *np;
	int ret;
	unsigned long flags;

	if (!ovinfo || !ovinfo->target || !ovinfo->overlay)
		return;

	pr_debug("%s: Reverting overlay on '%s'\n", __func__,
			ovinfo->target->full_name);

	/* overlay applied correctly, now create/destroy pdevs */
	list_for_each_entry_safe_reverse(de, den, &ovinfo->de_list, node) {
		of_overlay_device_entry_change(ovinfo, de, 1);
		of_node_put(de->np);
		list_del(&de->node);
		kfree(de);
	}

	list_for_each_entry_safe_reverse(le, len, &ovinfo->le_list, node) {

		/* get node and immediately put */
		np = le->np;
		of_node_put(le->np);
		le->np = NULL;

		ret = 0;
		switch (le->action) {
		case OF_RECONFIG_ATTACH_NODE:
			pr_debug("Reverting ATTACH_NODE %s\n",
					np->full_name);
			ret = of_detach_node(np);
			break;

		case OF_RECONFIG_DETACH_NODE:
			pr_debug("Reverting DETACH_NODE %s\n",
					np->full_name);
			ret = of_attach_node(np);
			break;

		case OF_RECONFIG_ADD_PROPERTY:
			pr_debug("Reverting ADD_PROPERTY %s %s\n",
					np->full_name, le->prop->name);
			ret = of_remove_property(np, le->prop);
			break;

		case OF_RECONFIG_REMOVE_PROPERTY:
		case OF_RECONFIG_UPDATE_PROPERTY:

			pr_debug("Reverting %s_PROPERTY %s %s\n",
				le->action == OF_RECONFIG_REMOVE_PROPERTY ?
					"REMOVE" : "UPDATE",
					np->full_name, le->prop->name);

			/* property is *always* on deadprops */
			raw_spin_lock_irqsave(&devtree_lock, flags);
			prop = le->action == OF_RECONFIG_REMOVE_PROPERTY ?
				le->prop : le->old_prop;
			propp = &np->deadprops;
			while (*propp != NULL) {
				if (*propp == prop)
					break;
				propp = &(*propp)->next;
			}

			/* we should find it in deadprops */
			WARN_ON(*propp == NULL);

			/* remove it from deadprops */
			if (*propp != NULL)
				*propp = prop->next;

			raw_spin_unlock_irqrestore(&devtree_lock, flags);

			if (le->action == OF_RECONFIG_REMOVE_PROPERTY)
				ret = of_add_property(np, prop);
			else
				ret = of_update_property(np, prop);
			break;

		default:
			/* nothing */
			break;
		}

		if (ret != 0)
			pr_err("%s: revert on node %s Failed!\n",
					__func__, np->full_name);

		list_del(&le->node);

		kfree(le);
	}
}

/*
 * Perform the post overlay work.
 *
 * In the case on an error we revert the overlay.
 * If the overlay applied correctly, we iterate over the device entries
 * and create/destroy the devices appropriately.
 */
static int of_overlay_post_one(struct of_overlay_info *ovinfo, int err)
{
	struct of_overlay_device_entry *de, *den;

	if (err != 0) {
		/* revert this (possible partially applied) overlay */
		of_overlay_revert_one(ovinfo);
		return 0;
	}

	/* overlay applied correctly, now create/destroy pdevs */
	list_for_each_entry_safe(de, den, &ovinfo->de_list, node) {

		/* no state change? just remove this entry */
		if (de->prevstate == de->state) {
			of_node_put(de->np);
			list_del(&de->node);
			kfree(de);
			continue;
		}

		of_overlay_device_entry_change(ovinfo, de, 0);
	}

	return 0;
}

/**
 * of_overlay_apply - Apply @count overlays pointed at by @ovinfo_tab
 * @count:	Number of of_overlay_info's
 * @ovinfo_tab:	Array of overlay_info's to apply
 *
 * Applies the overlays given, while handling all error conditions
 * appropriately. Either the operation succeeds, or if it fails the
 * live tree is reverted to the state before the attempt.
 * Returns 0, or an error if the overlay attempt failed.
 */
int of_overlay_apply(int count, struct of_overlay_info *ovinfo_tab)
{
	struct of_overlay_info *ovinfo;
	int i, err;

	if (!ovinfo_tab)
		return -EINVAL;

	/* first we apply the overlays atomically */
	for (i = 0; i < count; i++) {

		ovinfo = &ovinfo_tab[i];

		err = of_overlay_apply_one(ovinfo, ovinfo->target,
				ovinfo->overlay);
		of_overlay_post_one(ovinfo, err);

		if (err != 0) {
			pr_err("%s: overlay failed '%s'\n",
				__func__, ovinfo->target->full_name);
			goto err_fail;
		}
	}

	return 0;

err_fail:
	while (--i >= 0) {
		ovinfo = &ovinfo_tab[i];

		of_overlay_revert_one(ovinfo);
	}

	return err;
}
EXPORT_SYMBOL_GPL(of_overlay_apply);

/**
 * of_overlay_revert	- Revert a previously applied overlay
 * @count:	Number of of_overlay_info's
 * @ovinfo_tab:	Array of overlay_info's to apply
 *
 * Revert a previous overlay. The state of the live tree
 * is reverted to the one before the overlay.
 * Returns 0, or an error if the overlay table is not given.
 */
int of_overlay_revert(int count, struct of_overlay_info *ovinfo_tab)
{
	struct of_overlay_info *ovinfo;
	int i;

	if (!ovinfo_tab)
		return -EINVAL;

	/* revert the overlays in reverse */
	for (i = count - 1; i >= 0; i--) {

		ovinfo = &ovinfo_tab[i];

		of_overlay_revert_one(ovinfo);

	}

	return 0;
}
EXPORT_SYMBOL_GPL(of_overlay_revert);

/**
 * of_init_overlay_info	- Initialize a single of_overlay_info structure
 * @ovinfo:	Pointer to the overlay info structure to initialize
 *
 * Initialize a single overlay info structure.
 */
void of_init_overlay_info(struct of_overlay_info *ovinfo)
{
	memset(ovinfo, 0, sizeof(*ovinfo));
	INIT_LIST_HEAD(&ovinfo->de_list);
	INIT_LIST_HEAD(&ovinfo->le_list);
}

/*
 * Find the target node using a number of different strategies
 * in order of preference
 *
 * "target" property containing the phandle of the target
 * "target-path" property containing the path of the target
 *
 */
struct device_node *find_target_node(struct device_node *info_node)
{
	const char *path;
	u32 val;
	int ret;

	/* first try to go by using the target as a phandle */
	ret = of_property_read_u32(info_node, "target", &val);
	if (ret == 0)
		return of_find_node_by_phandle(val);

	/* now try to locate by path */
	ret = of_property_read_string(info_node, "target-path", &path);
	if (ret == 0)
		return of_find_node_by_path(path);

	pr_err("%s: Failed to find target for node %p (%s)\n", __func__,
			info_node, info_node->name);

	return NULL;
}

/**
 * of_fill_overlay_info	- Fill an overlay info structure
 * @info_node:	Device node containing the overlay
 * @ovinfo:	Pointer to the overlay info structure to fill
 *
 * Fills an overlay info structure with the overlay information
 * from a device node. This device node must have a target property
 * which contains a phandle of the overlay target node, and an
 * __overlay__ child node which has the overlay contents.
 * Both ovinfo->target & ovinfo->overlay have their references taken.
 *
 * Returns 0 on success, or a negative error value.
 */
int of_fill_overlay_info(struct device_node *info_node,
		struct of_overlay_info *ovinfo)
{
	if (!info_node || !ovinfo)
		return -EINVAL;

	ovinfo->overlay = of_get_child_by_name(info_node, "__overlay__");
	if (ovinfo->overlay == NULL)
		goto err_fail;

	ovinfo->target = find_target_node(info_node);
	if (ovinfo->target == NULL)
		goto err_fail;

	return 0;

err_fail:
	of_node_put(ovinfo->target);
	of_node_put(ovinfo->overlay);

	memset(ovinfo, 0, sizeof(*ovinfo));
	return -EINVAL;
}

/**
 * of_build_overlay_info	- Build an overlay info array
 * @tree:	Device node containing all the overlays
 * @cntp:	Pointer to where the overlay info count will be help
 * @ovinfop:	Pointer to the pointer of an overlay info structure.
 *
 * Helper function that given a tree containing overlay information,
 * allocates and builds an overlay info array containing it, ready
 * for use using of_overlay_apply.
 *
 * Returns 0 on success with the @cntp @ovinfop pointers valid,
 * while on error a negative error value is returned.
 */
int of_build_overlay_info(struct device_node *tree,
		int *cntp, struct of_overlay_info **ovinfop)
{
	struct device_node *node;
	struct of_overlay_info *ovinfo;
	int cnt, err;

	if (tree == NULL || cntp == NULL || ovinfop == NULL)
		return -EINVAL;

	/* worst case; every child is a node */
	cnt = 0;
	for_each_child_of_node(tree, node)
		cnt++;

	ovinfo = kcalloc(cnt, sizeof(*ovinfo), GFP_KERNEL);
	if (ovinfo == NULL)
		return -ENOMEM;

	cnt = 0;
	for_each_child_of_node(tree, node) {

		of_init_overlay_info(&ovinfo[cnt]);
		err = of_fill_overlay_info(node, &ovinfo[cnt]);
		if (err == 0)
			cnt++;
	}

	/* if nothing filled, return error */
	if (cnt == 0) {
		kfree(ovinfo);
		return -ENODEV;
	}

	*cntp = cnt;
	*ovinfop = ovinfo;

	return 0;
}
EXPORT_SYMBOL_GPL(of_build_overlay_info);

/**
 * of_free_overlay_info	- Free an overlay info array
 * @count:	Number of of_overlay_info's
 * @ovinfo_tab:	Array of overlay_info's to free
 *
 * Releases the memory of a previously allocate ovinfo array
 * by of_build_overlay_info.
 * Returns 0, or an error if the arguments are bogus.
 */
int of_free_overlay_info(int count, struct of_overlay_info *ovinfo_tab)
{
	struct of_overlay_info *ovinfo;
	int i;

	if (!ovinfo_tab || count < 0)
		return -EINVAL;

	/* do it in reverse */
	for (i = count - 1; i >= 0; i--) {
		ovinfo = &ovinfo_tab[i];

		of_node_put(ovinfo->target);
		of_node_put(ovinfo->overlay);
	}
	kfree(ovinfo_tab);

	return 0;
}
EXPORT_SYMBOL_GPL(of_free_overlay_info);

static LIST_HEAD(ov_list);
static DEFINE_MUTEX(ov_lock);
static DEFINE_IDR(ov_idr);

/**
 * of_overlay_create	- Create and apply an overlay
 * @tree:	Device node containing all the overlays
 *
 * Creates and applies an overlay while also keeping track
 * of the overlay in a list. This list can be use to prevent
 * illegal overlay removals.
 *
 * Returns the id of the created overlay, or an negative error number
 */
int of_overlay_create(struct device_node *tree)
{
	struct of_overlay *ov;
	int err, id;

	/* allocate the overlay structure */
	ov = kzalloc(sizeof(*ov), GFP_KERNEL);
	if (ov == NULL)
		return -ENOMEM;
	ov->id = -1;

	INIT_LIST_HEAD(&ov->node);

	mutex_lock(&ov_lock);

	id = idr_alloc(&ov_idr, ov, 0, 0, GFP_KERNEL);
	if (id < 0) {
		pr_err("%s: idr_alloc() failed for tree@%s\n",
				__func__, tree->full_name);
		err = id;
		goto err_free_ov;
	}
	ov->id = id;

	/* build the overlay info structures */
	err = of_build_overlay_info(tree, &ov->count, &ov->ovinfo_tab);
	if (err) {
		pr_err("%s: of_build_overlay_info() failed for tree@%s\n",
				__func__, tree->full_name);
		goto err_free_idr;
	}

	/* apply the overlay */
	err = of_overlay_apply(ov->count, ov->ovinfo_tab);
	if (err) {
		pr_err("%s: of_overlay_apply() failed for tree@%s\n",
				__func__, tree->full_name);
		goto err_free_ovinfo;
	}

	/* add to the tail of the overlay list */
	list_add_tail(&ov->node, &ov_list);

	mutex_unlock(&ov_lock);

	return id;

err_free_ovinfo:
	of_free_overlay_info(ov->count, ov->ovinfo_tab);
err_free_idr:
	idr_remove(&ov_idr, ov->id);
	mutex_unlock(&ov_lock);
err_free_ov:
	kfree(ov);

	return err;
}
EXPORT_SYMBOL_GPL(of_overlay_create);

/* check whether the given node, lies under the given tree */
static int overlay_subtree_check(struct device_node *tree,
		struct device_node *dn)
{
	struct device_node *child;

	/* match? */
	if (tree == dn)
		return 1;

	__for_each_child_of_node(tree, child) {
		if (overlay_subtree_check(child, dn))
			return 1;
	}

	return 0;
}

/* check whether this overlay is the topmost */
static int overlay_is_topmost(struct of_overlay *ov, struct device_node *dn)
{
	struct of_overlay *ovt;
	struct of_overlay_info *ovinfo;
	struct of_overlay_log_entry *le;
	int i;

	list_for_each_entry_reverse(ovt, &ov_list, node) {

		/* if we hit ourselves, we're done */
		if (ovt == ov)
			break;

		/* check against each subtree affected by this overlay */
		for (i = 0; i < ovt->count; i++) {
			ovinfo = &ovt->ovinfo_tab[i];
			list_for_each_entry(le, &ovinfo->le_list, node) {
				if (overlay_subtree_check(le->np, dn)) {
					pr_err("%s: #%d clashes #%d @%s\n",
						__func__, ov->id, ovt->id,
						dn->full_name);
					return 0;
				}
			}
		}
	}

	/* overlay is topmost */
	return 1;
}

/*
 * We can safely remove the overlay only if it's the top-most one.
 * Newly applied overlays are inserted at the tail of the overlay list,
 * so a top most overlay is the one that is closest to the tail.
 *
 * The topmost check is done by exploiting this property. For each
 * affected device node in the log list we check if this overlay is
 * the one closest to the tail. If another overlay has affected this
 * device node and is closest to the tail, then removal is not permited.
 */
static int overlay_removal_is_ok(struct of_overlay *ov)
{
	struct of_overlay_info *ovinfo;
	struct of_overlay_log_entry *le;
	int i;

	for (i = 0; i < ov->count; i++) {
		ovinfo = &ov->ovinfo_tab[i];
		list_for_each_entry(le, &ovinfo->le_list, node) {
			if (!overlay_is_topmost(ov, le->np)) {
				pr_err("%s: overlay #%d is not topmost\n",
						__func__, ov->id);
				return 0;
			}
		}
	}

	return 1;
}

/**
 * of_overlay_destroy	- Removes an overlay
 * @id:	Overlay id number returned by a previous call to of_overlay_create
 *
 * Removes an overlay if it is permissible.
 *
 * Returns 0 on success, or an negative error number
 */
int of_overlay_destroy(int id)
{
	struct of_overlay *ov;
	int err;

	mutex_lock(&ov_lock);
	ov = idr_find(&ov_idr, id);
	if (ov == NULL) {
		err = -ENODEV;
		pr_err("%s: Could not find overlay #%d\n",
				__func__, id);
		goto out;
	}

	/* check whether the overlay is safe to remove */
	if (!overlay_removal_is_ok(ov)) {
		err = -EBUSY;
		pr_err("%s: removal check failed for overlay #%d\n",
				__func__, id);
		goto out;
	}

	list_del(&ov->node);

	of_overlay_revert(ov->count, ov->ovinfo_tab);
	of_free_overlay_info(ov->count, ov->ovinfo_tab);

	idr_remove(&ov_idr, id);

	kfree(ov);

	err = 0;

out:
	mutex_unlock(&ov_lock);

	return err;
}
EXPORT_SYMBOL_GPL(of_overlay_destroy);

/**
 * of_overlay_destroy_all	- Removes all overlays from the system
 *
 * Removes all overlays from the system in the correct order.
 *
 * Returns 0 on success, or an negative error number
 */
int of_overlay_destroy_all(void)
{
	struct of_overlay *ov, *ovn;

	mutex_lock(&ov_lock);

	/* the tail of list is guaranteed to be safe to remove */
	list_for_each_entry_safe_reverse(ov, ovn, &ov_list, node) {

		list_del(&ov->node);

		of_overlay_revert(ov->count, ov->ovinfo_tab);
		of_free_overlay_info(ov->count, ov->ovinfo_tab);

		idr_remove(&ov_idr, ov->id);

		kfree(ov);
	}

	mutex_unlock(&ov_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(of_overlay_destroy_all);
