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

/* protect the handlers list */
static DEFINE_MUTEX(of_handler_mutex);
static struct list_head of_handler_list = LIST_HEAD_INIT(of_handler_list);

int of_overlay_handler_register(struct of_overlay_handler *handler)
{
	/* guard against bad data */
	if (!handler || !handler->name || !handler->ops ||
		!handler->ops->create || !handler->ops->remove)
		return -EINVAL;

	mutex_lock(&of_handler_mutex);
	list_add_tail(&handler->list, &of_handler_list);
	mutex_unlock(&of_handler_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(of_overlay_handler_register);

void of_overlay_handler_unregister(struct of_overlay_handler *handler)
{
	struct of_overlay_handler *curr;

	mutex_lock(&of_handler_mutex);
	list_for_each_entry(curr, &of_handler_list, list) {
		if (handler == curr) {
			list_del(&handler->list);
			break;
		}
	}
	mutex_unlock(&of_handler_mutex);
}
EXPORT_SYMBOL_GPL(of_overlay_handler_unregister);

static int handler_create(struct of_overlay_device_entry *entry, int revert)
{
	struct of_overlay_handler *handler;
	int ret;

	mutex_lock(&of_handler_mutex);
	list_for_each_entry(handler, &of_handler_list, list) {
		pr_debug("OF: %s trying '%s'\n", entry->np->full_name, handler->name);
		ret = (*handler->ops->create)(entry, revert);
		/* ENOTSUPP means try next */
		if (ret == -ENOTSUPP)
			continue;
		/* anything else means something happened */
		if (ret == 0)
			pr_info("OF: %s created on '%s'\n", entry->np->full_name, handler->name);
		else
			pr_info("OF: %s failed on '%s'\n", entry->np->full_name, handler->name);
		break;
	}
	mutex_unlock(&of_handler_mutex);

	return ret;
}

static int handler_remove(struct of_overlay_device_entry *entry, int revert)
{
	struct of_overlay_handler *handler;
	int ret;

	mutex_lock(&of_handler_mutex);
	list_for_each_entry(handler, &of_handler_list, list) {
		ret = (*handler->ops->remove)(entry, revert);
		/* ENOTSUPP means try next */
		if (ret == -ENOTSUPP)
			continue;
		/* anything else means something happened */
		break;
	}
	mutex_unlock(&of_handler_mutex);

	return ret;
}

/*
 * Apply a single overlay node recursively.
 *
 * Property or node names that start with '-' signal that
 * the property/node is to be removed.
 *
 * All the property notifiers are appropriately called.
 * Note that the in case of an error the target node is left
 * in a inconsistent state. Error recovery should be performed
 * by recording the modification using the of notifiers.
 */
static int of_overlay_apply_one(struct device_node *target,
		const struct device_node *overlay)
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
			if (propn != NULL)
				ret = of_update_property(target, propn);
			else
				ret = of_remove_property(target, tprop);
		} else {
			if (propn != NULL)
				ret = of_add_property(target, propn);
			else
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
				ret = of_overlay_apply_one(tchild, child);
				of_node_put(tchild);

				if (ret != 0)
					return ret;

			} else {

				ret = of_detach_node(tchild);
				of_node_put(tchild);
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

				ret = of_attach_node(tchild);
				if (ret != 0)
					return ret;

				/* apply the overlay */
				ret = of_overlay_apply_one(tchild, child);
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

	/* no need for locks, we'de under the ovinfo->lock */
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
 * Overlay OF notifier
 *
 * Called every time there's a property/node modification
 * Every modification causes a log entry addition, while
 * any modification that causes a node's state to change
 * from/to disabled to/from enabled causes a device entry
 * addition.
 */
static int of_overlay_notify(struct notifier_block *nb,
				unsigned long action, void *arg)
{
	struct of_overlay_info *ovinfo;
	struct device_node *node;
	struct property *prop, *sprop, *cprop;
	struct of_prop_reconfig *pr;
	struct device_node *tnode;
	int depth;
	int prevstate, state;
	int err = 0;

	ovinfo = container_of(nb, struct of_overlay_info, notifier);

	/* prep vars */
	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
	case OF_RECONFIG_DETACH_NODE:
		node = arg;
		if (node == NULL)
			return notifier_from_errno(-EINVAL);
		prop = NULL;
		break;
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		pr = arg;
		if (pr == NULL)
			return notifier_from_errno(-EINVAL);
		node = pr->dn;
		if (node == NULL)
			return notifier_from_errno(-EINVAL);
		prop = pr->prop;
		if (prop == NULL)
			return notifier_from_errno(-EINVAL);
		break;
	default:
		return notifier_from_errno(0);
	}

	/* add to the log */
	err = of_overlay_log_entry_entry_add(ovinfo, action, node, prop);
	if (err != 0)
		return notifier_from_errno(err);

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
			return notifier_from_errno(0);

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
		return notifier_from_errno(0);
	}

	/* find depth */
	depth = 1;
	tnode = node;
	while (tnode != NULL && tnode != ovinfo->target) {
		tnode = tnode->parent;
		depth++;
	}

	/* respect overlay's maximum depth */
	if (ovinfo->device_depth != 0 && depth > ovinfo->device_depth) {
		pr_debug("OF: skipping device creation for node=%s depth=%d\n",
				node->name, depth);
		goto out;
	}

	of_overlay_device_entry_entry_add(ovinfo, node, prevstate, state);
out:

	return notifier_from_errno(err);
}

/*
 * Prepare for the overlay, for now it just registers the
 * notifier.
 */
static int of_overlay_prep_one(struct of_overlay_info *ovinfo)
{
	int err;

	err = of_reconfig_notifier_register(&ovinfo->notifier);
	if (err != 0) {
		pr_err("%s: failed to register notifier for '%s'\n",
			__func__, ovinfo->target->full_name);
		return err;
	}
	return 0;
}

static int of_overlay_device_entry_change(struct of_overlay_info *ovinfo,
		struct of_overlay_device_entry *de, int revert)
{
	int state;
	int ret;

	state = !!de->state ^ !!revert;

	if (state)
		ret = handler_create(de, revert);
	else
		ret = handler_remove(de, revert);

	if (ret != 0 && ret != -ENOTSUPP)
		pr_warn("%s: Failed to %s device "
				"for node '%s'\n", __func__,
				state ? "create" : "remove",
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

			/* property is possibly on deadprops (avoid alloc) */
			raw_spin_lock_irqsave(&devtree_lock, flags);
			prop = le->action == OF_RECONFIG_REMOVE_PROPERTY ?
				le->prop : le->old_prop;
			propp = &np->deadprops;
			while (*propp != NULL) {
				if (*propp == prop)
					break;
				propp = &(*propp)->next;
			}
			if (*propp != NULL) {
				/* remove it from deadprops */
				(*propp)->next = prop->next;
				raw_spin_unlock_irqrestore(&devtree_lock,
						flags);
			} else {
				raw_spin_unlock_irqrestore(&devtree_lock,
						flags);
				/* not found, just make a copy */
				prop = __of_copy_property(prop, GFP_KERNEL,
						OF_PROP_ALLOCALL);
				if (prop == NULL) {
					pr_err("%s: Failed to copy property\n",
							__func__);
					break;
				}
			}

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
 * We unregister the notifier, and in the case on an error we
 * revert the overlay.
 * If the overlay applied correctly, we iterate over the device entries
 * and create/destroy the devices appropriately.
 */
static int of_overlay_post_one(struct of_overlay_info *ovinfo, int err)
{
	struct of_overlay_device_entry *de, *den;

	of_reconfig_notifier_unregister(&ovinfo->notifier);

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
 * of_overlay	- Apply @count overlays pointed at by @ovinfo_tab
 * @count:	Number of of_overlay_info's
 * @ovinfo_tab:	Array of overlay_info's to apply
 *
 * Applies the overlays given, while handling all error conditions
 * appropriately. Either the operation succeeds, or if it fails the
 * live tree is reverted to the state before the attempt.
 * Returns 0, or an error if the overlay attempt failed.
 */
int of_overlay(int count, struct of_overlay_info *ovinfo_tab)
{
	struct of_overlay_info *ovinfo;
	int i, err;

	if (!ovinfo_tab)
		return -EINVAL;

	/* first we apply the overlays atomically */
	for (i = 0; i < count; i++) {

		ovinfo = &ovinfo_tab[i];

		mutex_lock(&ovinfo->lock);

		err = of_overlay_prep_one(ovinfo);
		if (err == 0)
			err = of_overlay_apply_one(ovinfo->target,
					ovinfo->overlay);
		of_overlay_post_one(ovinfo, err);

		mutex_unlock(&ovinfo->lock);

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

		mutex_lock(&ovinfo->lock);
		of_overlay_revert_one(ovinfo);
		mutex_unlock(&ovinfo->lock);
	}

	return err;
}
EXPORT_SYMBOL_GPL(of_overlay);

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

		mutex_lock(&ovinfo->lock);
		of_overlay_revert_one(ovinfo);
		mutex_unlock(&ovinfo->lock);

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
	mutex_init(&ovinfo->lock);
	INIT_LIST_HEAD(&ovinfo->de_list);
	INIT_LIST_HEAD(&ovinfo->le_list);

	ovinfo->notifier.notifier_call = of_overlay_notify;
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
	u32 val;
	int ret;

	if (!info_node || !ovinfo)
		return -EINVAL;

	ovinfo->overlay = of_get_child_by_name(info_node, "__overlay__");
	if (ovinfo->overlay == NULL)
		goto err_fail;

	ovinfo->target = find_target_node(info_node);
	if (ovinfo->target == NULL)
		goto err_fail;

	ret = of_property_read_u32(info_node, "depth", &val);
	if (ret == 0)
		ovinfo->device_depth = val;
	else
		ovinfo->device_depth = 0;

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
 * for use using of_overlay.
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

	ovinfo = kzalloc(cnt * sizeof(*ovinfo), GFP_KERNEL);
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
