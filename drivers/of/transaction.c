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

static void __of_transaction_entry_invert(struct of_transaction_entry *te,
					  struct of_transaction_entry *rte)
{
	*rte = *te;
	switch (te->action) {
	case OF_RECONFIG_ATTACH_NODE:
		rte->action = OF_RECONFIG_DETACH_NODE;
		break;
	case OF_RECONFIG_DETACH_NODE:
		rte->action = OF_RECONFIG_ATTACH_NODE;
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		rte->action = OF_RECONFIG_REMOVE_PROPERTY;
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		rte->action = OF_RECONFIG_ADD_PROPERTY;
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		rte->old_prop = te->prop;
		rte->prop = te->old_prop;
		break;
	}
}

static int __of_transaction_entry_notify(struct of_transaction_entry *te, bool revert)
{
	struct of_transaction_entry te_inverted;
	int ret = -EINVAL;

	if (revert) {
		__of_transaction_entry_invert(te, &te_inverted);
		te = &te_inverted;
	}

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

	if (ret)
		pr_err("%s: notifier error @%s\n", __func__, te->np->full_name);
	return ret;
}

static int __of_transaction_entry_apply(struct of_transaction_entry *te)
{
	struct property *old_prop;
	unsigned long flags;
	int ret = -EINVAL;

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

	if (ret) {
		/* we can't rollback the notifier */
		return ret;
	}

	switch (te->action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_attach_node_sysfs(te->np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_detach_node_sysfs(te->np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		/* ignore duplicate names */
		__of_add_property_sysfs(te->np, te->prop); /* GCL: remove duplicate names?? */
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		__of_remove_property_sysfs(te->np, te->prop);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		__of_update_property_sysfs(te->np, te->prop, te->old_prop);
		break;
	}

	return 0;
}

static int __of_transaction_entry_revert(struct of_transaction_entry *te)
{
	struct property *prop, *old_prop, **propp;
	unsigned long action, flags;
	struct device_node *np;
	int ret = -EINVAL;

	/* get node and immediately put */
	action = te->action;
	np = te->np;
	prop = te->prop;
	old_prop = te->old_prop;

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
		return ret;

	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		__of_detach_node_sysfs(np);
		break;
	case OF_RECONFIG_DETACH_NODE:
		__of_attach_node_sysfs(np);
		break;
	case OF_RECONFIG_ADD_PROPERTY:
		__of_remove_property_sysfs(np, prop);
		break;
	case OF_RECONFIG_REMOVE_PROPERTY:
		__of_add_property_sysfs(np, prop);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		__of_update_property_sysfs(np, prop, old_prop);
		break;
	}

	return 0;
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
 * of_transaction_apply - Applies a transaction
 *
 * @oft:	transaction pointer
 *
 * Applies a transaction to the live tree.
 * Any side-effects of live tree state changes are applied here on
 * sucess, like creation/destruction of devices and side-effects
 * like creation of sysfs properties and directories.
 * Returns 0 on success, a negative error value in case of an error.
 * On error the partially applied effects are reverted.
 */
int of_transaction_apply(struct of_transaction *oft)
{
	struct of_transaction_entry *te;
	int ret;

	/* drop the global lock while emitting notifiers */
	mutex_unlock(&of_mutex);
	list_for_each_entry(te, &oft->te_list, node) {
		ret = __of_transaction_entry_notify(te, 0);
		if (ret) {
			list_for_each_entry_continue_reverse(te, &oft->te_list, node)
				ret = __of_transaction_entry_notify(te, 1);
			mutex_lock(&of_mutex);
			return ret;
		}
	}
	mutex_lock(&of_mutex);

	/* perform the rest of the work */
	pr_debug("of_transaction: applying...\n");
	list_for_each_entry(te, &oft->te_list, node) {
		__of_transaction_entry_dump(te);
		ret = __of_transaction_entry_apply(te);
		if (ret) {
			pr_err("%s: Error applying transaction (%d)\n", __func__, ret);
			list_for_each_entry_continue_reverse(te, &oft->te_list, node) {
				__of_transaction_entry_dump(te);
				__of_transaction_entry_revert(te);
			}
			return ret;
		}
	}

	pr_debug("of_transaction: applied.\n");

	return 0;
}

/**
 * of_transaction_revert - Reverts an applied transaction
 *
 * @oft:	transaction pointer
 *
 * Reverts a transaction returning the state of the tree to what it
 * was before the application.
 * Any side-effects like creation/destruction of devices and
 * removal of sysfs properties and directories are applied.
 * Returns 0 on success, a negative error value in case of an error.
 */
int of_transaction_revert(struct of_transaction *oft)
{
	struct of_transaction_entry *te;
	int ret;

	/* drop the global lock while emitting notifiers */
	mutex_unlock(&of_mutex);
	list_for_each_entry_reverse(te, &oft->te_list, node) {
		ret = __of_transaction_entry_notify(te, 1);
		if (ret) {
			list_for_each_entry_continue(te, &oft->te_list, node)
				ret = __of_transaction_entry_notify(te, 0);
			mutex_lock(&of_mutex);
			return ret;
		}
	}
	mutex_lock(&of_mutex);

	pr_debug("of_transaction: reverting...\n");
	list_for_each_entry_reverse(te, &oft->te_list, node) {
		__of_transaction_entry_dump(te);
		ret = __of_transaction_entry_revert(te);
		if (ret) {
			pr_err("%s: Error reverting transaction (%d)\n", __func__, ret);
			list_for_each_entry_continue(te, &oft->te_list, node) {
				__of_transaction_entry_dump(te);
				__of_transaction_entry_apply(te);
			}
			return ret;
		}
	}
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

	te = kzalloc(sizeof(*te), GFP_KERNEL);
	if (!te) {
		pr_err("%s: Failed to allocate\n", __func__);
		return -ENOMEM;
	}
	/* get a reference to the node */
	te->action = action;
	te->np = of_node_get(np);
	te->prop = prop;

	if (action == OF_RECONFIG_UPDATE_PROPERTY && prop)
		te->old_prop = of_find_property(np, prop->name, NULL);

	/* add it to the list */
	list_add_tail(&te->node, &oft->te_list);
	return 0;
}
