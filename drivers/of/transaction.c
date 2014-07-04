/*
 * Functions for DT transactions
 *
 * Copyright (C) 2014 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */
#undef DEBUG
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

#include "of_private.h"

static struct of_transaction_entry *__of_transaction_entry_create(
		struct of_transaction *oft, unsigned long action,
		struct device_node *dn, struct property *prop)
{
	struct of_transaction_entry *te;

	te = kzalloc(sizeof(*te), GFP_KERNEL);
	if (te == NULL) {
		pr_err("%s: Failed to allocate\n", __func__);
		return ERR_PTR(-ENOMEM);
	}
	/* get a reference to the node */
	te->action = action;
	te->np = of_node_get(dn);
	te->prop = prop;

	if (action == OF_RECONFIG_UPDATE_PROPERTY && prop)
		te->old_prop = of_transaction_find_property(oft, dn,
				prop->name, NULL);

	return te;
}

static void __of_transaction_entry_destroy(struct of_transaction_entry *te)
{
	of_node_put(te->np);
	list_del(&te->node);
	kfree(te);
}

#ifdef DEBUG
static void __of_transaction_entry_dump(struct of_transaction_entry *te)
{
	switch (te->action) {
	case OF_RECONFIG_ADD_PROPERTY:
		pr_debug("%p: %s %s/%s\n",
			te, "ADD_PROPERTY   ", te->np->full_name,
			te->prop->name);
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		pr_debug("%p: %s %s/%s\n",
			te, "REMOVE_PROPERTY", te->np->full_name,
			te->prop->name);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		pr_debug("%p: %s %s/%s\n",
			te, "UPDATE_PROPERTY", te->np->full_name,
			te->prop->name);
		break;
	case OF_RECONFIG_ATTACH_NODE:
		pr_debug("%p: %s %s\n",
			te, "ATTACH_NODE    ", te->np->full_name);
		break;
	case OF_RECONFIG_DETACH_NODE:
		pr_debug("%p: %s %s\n",
			te, "DETACH_NODE    ", te->np->full_name);
		break;
	}
}
#else
static inline void __of_transaction_entry_dump(struct of_transaction_entry *te)
{
	/* empty */
}
#endif

static int __of_transaction_entry_device_state(struct of_transaction *oft,
		struct of_transaction_entry *te, int revert)
{
	int curr, target;
	unsigned long action;
	struct property *prop;

	/* filter */
	if (te->action != OF_RECONFIG_ADD_PROPERTY &&
	    te->action != OF_RECONFIG_REMOVE_PROPERTY &&
	    te->action != OF_RECONFIG_UPDATE_PROPERTY &&
	    te->action != OF_RECONFIG_ATTACH_NODE &&
	    te->action != OF_RECONFIG_ATTACH_NODE)
		return -1;

	action = te->action;
	prop = te->prop;

	/* on revert convert to the opposite */
	if (revert) {
		if (action == OF_RECONFIG_ADD_PROPERTY)
			action = OF_RECONFIG_REMOVE_PROPERTY;
		else if (action == OF_RECONFIG_REMOVE_PROPERTY)
			action = OF_RECONFIG_ADD_PROPERTY;
		else if (action == OF_RECONFIG_UPDATE_PROPERTY)
			prop = te->old_prop;
		else if (action == OF_RECONFIG_ATTACH_NODE)
			action = OF_RECONFIG_DETACH_NODE;
		else
			action = OF_RECONFIG_ATTACH_NODE;
	}

	/* we only support state changes on "status" property change */
	if ((te->action == OF_RECONFIG_ADD_PROPERTY ||
	    te->action == OF_RECONFIG_REMOVE_PROPERTY ||
	    te->action == OF_RECONFIG_UPDATE_PROPERTY) &&
		of_prop_cmp(prop->name, "status"))
		return -1;

	/* note that we don't use of_transaction_find_property() */

	/* current device state */
	curr = of_device_is_available(te->np) &&
		of_find_property(te->np, "compatible", NULL) &&
		of_find_property(te->np, "status", NULL);

	switch (action) {
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:

		/* target device state */
		if (action == OF_RECONFIG_ADD_PROPERTY ||
		    action == OF_RECONFIG_UPDATE_PROPERTY)
			target = !strcmp(prop->value, "okay") ||
				!strcmp(prop->value, "ok");
		else
			target = 0;	/* NOTE: status removal -> disabled */

		break;
	case OF_RECONFIG_ATTACH_NODE:
		target = curr;
		curr = 0;
		break;
	case OF_RECONFIG_DETACH_NODE:
		target = 0;
		break;
	}

	return curr != target ? target : -1;
}

static int __of_transaction_entry_apply(struct of_transaction *oft,
		struct of_transaction_entry *te)
{
	struct property *old_prop;
	unsigned long flags;
	int ret, state;

	ret = 0;

	state = __of_transaction_entry_device_state(oft, te, 0);

	switch (te->action) {
	case OF_RECONFIG_ATTACH_NODE:
	case OF_RECONFIG_DETACH_NODE:
		ret = of_reconfig_notify(te->action, te->np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		ret = of_property_notify(te->action, te->np, te->prop);
		break;
	}

	if (ret) {
		pr_err("%s: notifier error @%s\n", __func__,
				te->np->full_name);
		return ret;
	}

	mutex_lock(&of_mutex);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	switch (te->action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_attach_node(te->np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_detach_node(te->np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		ret = __of_add_property(te->np, te->prop);
		if (ret) {
			pr_err("%s: add_property failed @%s/%s\n",
				__func__, te->np->full_name,
				te->prop->name);
			break;
		}
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		ret = __of_remove_property(te->np, te->prop);
		if (ret) {
			pr_err("%s: remove_property failed @%s/%s\n",
				__func__, te->np->full_name,
				te->prop->name);
			break;
		}
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		ret = __of_update_property(te->np, te->prop, &old_prop);
		if (ret) {
			pr_err("%s: update_property failed @%s/%s\n",
				__func__, te->np->full_name,
				te->prop->name);
			break;
		}
		break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	mutex_unlock(&of_mutex);

	if (ret) {
		/* we can't rollback the notifier */
		return ret;
	}

	switch (te->action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_attach_node_post(te->np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_detach_node_post(te->np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		/* ignore duplicate names */
		__of_add_property_post(te->np, te->prop, 1);
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		__of_remove_property_post(te->np, te->prop);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		__of_update_property_post(te->np, te->prop, te->old_prop);
		break;
	}

	if (state != -1) {
		pr_debug("of_transaction: %s device for node '%s'\n",
				state ? "create" : "remove",
				te->np->full_name);

		ret = of_reconfig_notify(state ?
				OF_RECONFIG_DYNAMIC_CREATE_DEV :
				OF_RECONFIG_DYNAMIC_DESTROY_DEV,
				te->np);
		if (ret != 0) {
			pr_err("of_transaction: failed %s device @%s (%d)\n",
					state ? "create" : "remove",
					te->np->full_name, ret);
			/* drop the error; devices probe fails; that's OK */
			ret = 0;
		}

	}

	return 0;
}

static int __of_transaction_entry_revert(struct of_transaction *oft,
		struct of_transaction_entry *te)
{
	struct property *prop, *old_prop, **propp;
	unsigned long action, flags;
	struct device_node *np;
	int ret, state;

	state = __of_transaction_entry_device_state(oft, te, 1);

	if (state != -1) {
		pr_debug("of_transaction: %s device for node '%s'\n",
				state  ? "create" : "remove",
				te->np->full_name);

		ret = of_reconfig_notify(state ?
				OF_RECONFIG_DYNAMIC_CREATE_DEV :
				OF_RECONFIG_DYNAMIC_DESTROY_DEV,
				te->np);
		if (ret != 0) {
			pr_err("of_transaction: failed %s device @%s (%d)\n",
					state ? "create" : "remove",
					te->np->full_name, ret);
			/* drop the error; devices probe fails; that's OK */
			ret = 0;
		}
	}

	/* get node and immediately put */
	action = te->action;
	np = te->np;
	prop = te->prop;
	old_prop = te->old_prop;

	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		ret = of_reconfig_notify(OF_RECONFIG_DETACH_NODE, np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		ret = of_reconfig_notify(OF_RECONFIG_ATTACH_NODE, np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		ret = of_property_notify(OF_RECONFIG_REMOVE_PROPERTY,
				np, prop);
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		ret = of_property_notify(OF_RECONFIG_ADD_PROPERTY,
				np, prop);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		ret = of_property_notify(OF_RECONFIG_UPDATE_PROPERTY,
				np, old_prop);
		break;
	}

	if (ret) {
		pr_err("%s: notifier error @%s\n", __func__,
				te->np->full_name);
		goto out_revert;
	}

	mutex_lock(&of_mutex);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_detach_node(np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_attach_node(np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		ret = __of_remove_property(np, prop);
		if (ret) {
			pr_err("%s: remove_property failed @%s/%s\n",
				__func__, np->full_name, prop->name);
			break;
		}
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
	case OF_RECONFIG_UPDATE_PROPERTY:
		/* property is *always* on deadprops */
		if (action == OF_RECONFIG_UPDATE_PROPERTY)
			prop = old_prop;
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

		if (action == OF_RECONFIG_REMOVE_PROPERTY) {
			ret = __of_add_property(np, prop);
			if (ret) {
				pr_err("%s: add_property failed @%s/%s\n",
					__func__, np->full_name,
					prop->name);
				break;
			}
		} else {
			ret = __of_update_property(np, prop, &old_prop);
			if (ret) {
				pr_err("%s: update_property failed @%s/%s\n",
					__func__, np->full_name,
					prop->name);
				break;
			}
		}
		break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (ret)
		goto out_unlock;

	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_detach_node_post(np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_attach_node_post(np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		__of_remove_property_post(np, prop);
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		__of_add_property_post(np, prop, 0);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		__of_update_property_post(np, prop, old_prop);
		break;
	}

out_unlock:
	mutex_unlock(&of_mutex);

out_revert:
	/* revert creation of device */
	if (ret && state != -1)
		of_reconfig_notify(!state ?
			OF_RECONFIG_DYNAMIC_CREATE_DEV :
			OF_RECONFIG_DYNAMIC_DESTROY_DEV,
			te->np);

	return ret;
}

/**
 * of_transaction_init - Initialize a transaction for use
 *
 * @oft:	transaction pointer
 *
 * Initialize a transaction structure
 */
void of_transaction_init(struct of_transaction *oft)
{
	memset(oft, 0, sizeof(*oft));
	INIT_LIST_HEAD(&oft->te_list);
}

/**
 * of_transaction_destroy - Destroy a transaction
 *
 * @oft:	transaction pointer
 *
 * Destroys a transaction. Note that if a transaction is applied,
 * its changes to the tree cannot be reverted.
 */
void of_transaction_destroy(struct of_transaction *oft)
{
	struct of_transaction_entry *te, *ten;

	list_for_each_entry_safe_reverse(te, ten, &oft->te_list, node)
		__of_transaction_entry_destroy(te);
}

/**
 * of_transaction_start - Start a transaction
 *
 * @oft:	transaction pointer
 *
 * Starts a transaction, by simply grabbing hold of the of_mutex.
 * This prevents any modification of the live tree while the
 * transaction is in process.
 */
void of_transaction_start(struct of_transaction *oft)
{
	/* take the global of transaction mutex */
	mutex_lock(&of_mutex);
}

/**
 * of_transaction_abort - Aborts a transaction in progress
 *
 * @oft:	transaction pointer
 *
 * Aborts a transaction, this simply releases the of_mutex
 * and destroys all the pending transaction entries.
 */
void of_transaction_abort(struct of_transaction *oft)
{
	struct of_transaction_entry *te, *ten;

	mutex_unlock(&of_mutex);

	list_for_each_entry_safe_reverse(te, ten, &oft->te_list, node)
		__of_transaction_entry_destroy(te);
}

/**
 * of_transaction_apply - Applies a transaction
 *
 * @oft:	transaction pointer
 * @force:	continue even in case of an error
 *
 * Applies a transaction to the live tree.
 * Any side-effects of live tree state changes are applied here on
 * sucess, like creation/destruction of devices and side-effects
 * like creation of sysfs properties and directories.
 * Returns 0 on success, a negative error value in case of an error.
 * On error the partially applied effects are reverted if the @force
 * parameter is not set.
 */
int of_transaction_apply(struct of_transaction *oft, int force)
{
	struct of_transaction_entry *te;
	int ret;

	/* drop the global lock here (notifiers and devices need it) */
	mutex_unlock(&of_mutex);

	ret = 0;

	/* perform the rest of the work */
	pr_debug("of_transaction: applying...\n");
	list_for_each_entry(te, &oft->te_list, node) {
		__of_transaction_entry_dump(te);
		ret = __of_transaction_entry_apply(oft, te);
		if (ret) {
			pr_err("%s: Error applying transaction (%d)\n",
					__func__, ret);
			if (!force) {
				list_for_each_entry_continue_reverse(te,
						&oft->te_list, node) {
					__of_transaction_entry_dump(te);
					__of_transaction_entry_revert(oft, te);
				}
				return ret;
			}
		}
	}

	pr_debug("of_transaction: applied.\n");

	return 0;
}

/**
 * of_transaction_revert - Reverts an applied transaction
 *
 * @oft:	transaction pointer
 * @force:	continue even in case of an error
 *
 * Reverts a transaction returning the state of the tree to what it
 * was before the application.
 * Any side-effects like creation/destruction of devices and
 * removal of sysfs properties and directories are applied.
 * Returns 0 on success, a negative error value in case of an error.
 */
int of_transaction_revert(struct of_transaction *oft, int force)
{
	struct of_transaction_entry *te, *ten;
	int ret;

	pr_debug("of_transaction: reverting...\n");
	list_for_each_entry_reverse(te, &oft->te_list, node) {
		__of_transaction_entry_dump(te);
		ret = __of_transaction_entry_revert(oft, te);
		if (ret) {
			pr_err("%s: Error reverting transaction (%d)\n",
					__func__, ret);
			if (!force) {
				list_for_each_entry_continue(te,
						&oft->te_list, node) {
					__of_transaction_entry_dump(te);
					__of_transaction_entry_apply(oft, te);
				}
				return ret;
			}
		}
	}

	/* destroy everything */
	list_for_each_entry_safe_reverse(te, ten, &oft->te_list, node)
		__of_transaction_entry_destroy(te);
	pr_debug("of_transaction: reverted.\n");

	return 0;
}

/**
 * of_transaction_action - Perform a transaction action
 *
 * @oft:	transaction pointer
 * @action:	action to perform
 * @np:		Pointer to device node
 * @prop:	Pointer to property
 *
 * On action being one of:
 * + OF_RECONFIG_ATTACH_NODE
 * + OF_RECONFIG_DETACH_NODE,
 * + OF_RECONFIG_ADD_PROPERTY
 * + OF_RECONFIG_REMOVE_PROPERTY,
 * + OF_RECONFIG_UPDATE_PROPERTY
 * Returns 0 on success, a negative error value in case of an error.
 */
int of_transaction_action(struct of_transaction *oft, unsigned long action,
		struct device_node *np, struct property *prop)
{
	struct of_transaction_entry *te;

	/* create the transaction entry */
	te = __of_transaction_entry_create(oft, action, np, prop);
	if (IS_ERR(te)) {
		pr_err("%s: failed to create entry for @%s\n",
				__func__, np->full_name);
		return PTR_ERR(te);
	}

	/* add it to the list */
	list_add_tail(&te->node, &oft->te_list);
	return 0;
}

/* utility functions for advanced transaction users */

/* find a property in the transaction list while not applied */
struct property *of_transaction_find_property(struct of_transaction *oft,
		const struct device_node *np, const char *name, int *lenp)
{
	struct of_transaction_entry *te;

	/* possibly exists in the transaction list as
	 * part of an attachment action
	 */
	list_for_each_entry_reverse(te, &oft->te_list, node) {

		if (te->action != OF_RECONFIG_ADD_PROPERTY &&
		    te->action != OF_RECONFIG_REMOVE_PROPERTY &&
		    te->action != OF_RECONFIG_UPDATE_PROPERTY)
			continue;

		/* match of node and name? */
		if (te->np != np || strcmp(te->prop->name, name))
			continue;

		pr_debug("%s: found property \"%s\" in transaction list @%s\n",
			__func__, name, te->np->full_name);

		if (te->action == OF_RECONFIG_ADD_PROPERTY ||
		    te->action == OF_RECONFIG_UPDATE_PROPERTY)
			return te->prop;
		else
			return NULL;
	}

	/* now fallback to live tree */
	return of_find_property(np, name, lenp);
}

/* special find property method for use by transaction users */
int of_transaction_device_is_available(struct of_transaction *oft,
		const struct device_node *np)
{
	struct property *prop;

	if (!np)
		return 0;

	prop = of_transaction_find_property(oft, np, "status", NULL);
	if (prop == NULL)
		return 1;

	return prop->length > 0 &&
		(!strcmp(prop->value, "okay") || !strcmp(prop->value, "ok"));
}

struct device_node *of_transaction_get_child_by_name(
		struct of_transaction *oft, struct device_node *node,
		const char *name)
{
	struct of_transaction_entry *te;

	/* possibly exists in the transaction list as
	 * part of an attachment action
	 */
	list_for_each_entry_reverse(te, &oft->te_list, node) {

		if (te->action != OF_RECONFIG_ATTACH_NODE &&
		    te->action != OF_RECONFIG_DETACH_NODE)
			continue;

		/* look at the parent and if node matches return */
		if (te->np->parent != node || strcmp(te->np->name, "name"))
			continue;

		pr_debug("%s: found child \"%s\" in transaction list @%s\n",
			__func__, name, te->np->full_name);
		return te->action == OF_RECONFIG_ATTACH_NODE ? te->np : NULL;
	}

	/* not found in the transaction list? try normal */
	return of_get_child_by_name(node, name);
}
