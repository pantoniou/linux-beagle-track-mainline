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

#define __OFT_REVERT_PRE	(1 << 0)
#define __OFT_REVERT_POST	(1 << 1)
#define __OFT_APPLY_PRE		(1 << 2)
#define __OFT_APPLY_POST	(1 << 3)
#define __OFT_FREE		(1 << 4)
#define __OFT_DEV_CHANGE	(1 << 5)
#define __OFT_FIRE_NOTIFIERS	(1 << 6)

static struct of_transaction_entry *__of_transaction_entry_create(
		unsigned long action, struct device_node *dn,
		struct property *prop)
{
	struct of_transaction_entry *te;
	int prop_avail, prev_avail;

	te = kzalloc(sizeof(*te), GFP_KERNEL);
	if (te == NULL) {
		pr_err("%s: Failed to allocate\n", __func__);
		return ERR_PTR(-ENOMEM);
	}
	te->device_state_change = -1;

	/* get a reference to the node */
	te->action = action;
	te->np = of_node_get(dn);
	te->prop = prop;

	if (action == OF_RECONFIG_UPDATE_PROPERTY && prop)
		te->old_prop = of_find_property(dn, prop->name, NULL);

	if (action == OF_RECONFIG_ADD_PROPERTY ||
			action == OF_RECONFIG_REMOVE_PROPERTY ||
			action == OF_RECONFIG_UPDATE_PROPERTY) {
		BUG_ON(te->prop == NULL);
		BUG_ON(action == OF_RECONFIG_UPDATE_PROPERTY && te->old_prop == NULL);

		prop_avail = -1;
		if (of_prop_cmp(prop->name, "status") == 0)
			prop_avail = (strcmp(prop->value, "okay") == 0 ||
					strcmp(prop->value, "ok") == 0);
		else if (of_prop_cmp(prop->name, "-status") == 0)
			prop_avail = 1;

		if (prop_avail != -1) {
			/* state, but change? */
			prev_avail = of_device_is_available(te->np) &&
				of_find_property(te->np, "compatible", NULL) &&
				of_find_property(te->np, "status", NULL);

			if (prop_avail != prev_avail)
				te->device_state_change = prop_avail;
		}
	}
	return te;
}

static void __of_transaction_entry_destroy(struct of_transaction_entry *te)
{
	of_node_put(te->np);
	list_del(&te->node);
	kfree(te);
}

static int __of_transaction_entry_apply_op(
		struct of_transaction_entry *te, unsigned int op)
{
	struct property *old_prop;
	unsigned long flags;
	int ret;

	ret = 0;

	if (op & __OFT_FIRE_NOTIFIERS) {
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
	}

	if (ret == 0 && (op & __OFT_APPLY_PRE)) {
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
			if (ret != 0) {
				pr_err("%s: __of_add_property failed @%s/%s\n",
					__func__, te->np->full_name,
					te->prop->name);
				break;
			}
			break;
		case OF_RECONFIG_REMOVE_PROPERTY:
			ret = __of_remove_property(te->np, te->prop);
			if (ret != 0) {
				pr_err("%s: __of_remove_property failed @%s/%s\n",
					__func__, te->np->full_name,
					te->np->name);
				break;
			}
			break;
		case OF_RECONFIG_UPDATE_PROPERTY:
			ret = __of_update_property(te->np, te->prop, &old_prop);
			if (ret != 0) {
				pr_err("%s: __of_update_property failed @%s/%s\n",
					__func__, te->np->full_name,
					te->np->name);
				break;
			}
			break;
		}
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
	}

	if (ret == 0 && (op & __OFT_APPLY_POST)) {
		switch (te->action) {
		case OF_RECONFIG_ATTACH_NODE:
			__of_attach_node_post(te->np);
			break;
		case OF_RECONFIG_DETACH_NODE:
			__of_detach_node(te->np);
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
	}

	if (ret == 0 && (op & __OFT_DEV_CHANGE) &&
			te->device_state_change != -1) {

		pr_debug("%s: %s device for node '%s'\n", __func__,
				te->device_state_change ? "create" : "remove",
				te->np->full_name);

		of_reconfig_notify(te->device_state_change ?
				OF_RECONFIG_DYNAMIC_CREATE_DEV :
				OF_RECONFIG_DYNAMIC_DESTROY_DEV,
				te->np);
	}

	return ret;
}

static int __of_transaction_entry_revert_op(
		struct of_transaction_entry *te, unsigned int op)
{
	struct property *prop, *old_prop, **propp;
	unsigned long action, flags;
	struct device_node *np;
	int ret, device_state_change;

	/* get node and immediately put */
	action = te->action;
	np = te->np;
	prop = te->prop;
	old_prop = te->old_prop;
	device_state_change = te->device_state_change;

	/* if free, do it upfront */
	if (op & __OFT_FREE) {
		__of_transaction_entry_destroy(te);
		te = NULL;
	}

	if (op & __OFT_FIRE_NOTIFIERS) {
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
			ret = of_property_notify(OF_RECONFIG_UPDATE_PROPERTY, np,
					old_prop);
			break;
		}
	}

	if (ret == 0 && (op & __OFT_DEV_CHANGE) && device_state_change != -1) {

		pr_debug("%s: %s device for node '%s'\n", __func__,
				!device_state_change ? "create" : "remove",
				np->full_name);

		of_reconfig_notify(!device_state_change ?
				OF_RECONFIG_DYNAMIC_CREATE_DEV :
				OF_RECONFIG_DYNAMIC_DESTROY_DEV,
				np);
	}

	ret = 0;
	if (op & __OFT_REVERT_PRE) {
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

			if (action == OF_RECONFIG_REMOVE_PROPERTY)
				ret = __of_add_property(np, prop);
			else
				ret = __of_update_property(np, prop, &old_prop);
			break;
		}
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
	}

	if (ret == 0 && (op & __OFT_REVERT_POST)) {
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
	}

	return ret;
}

int of_transaction_init(struct of_transaction *oft)
{
	memset(oft, 0, sizeof(*oft));

	mutex_init(&oft->lock);
	INIT_LIST_HEAD(&oft->te_list);
	oft->state = OFT_READY;

	return 0;
}

int of_transaction_destroy(struct of_transaction *oft)
{
	struct of_transaction_entry *te, *ten;
	int ret;

	ret = 0;
	mutex_lock(&oft->lock);
	if (oft->state != OFT_READY && oft->state != OFT_COMMITTED) {
		ret = -EBUSY;
		pr_err("%s: transaction is busy; can't destroy\n", __func__);
		goto out;
	}

	/* for a commited transaction, just free the memory */
	if (oft->state == OFT_COMMITTED) {
		list_for_each_entry_safe_reverse(te, ten, &oft->te_list, node)
			__of_transaction_entry_revert_op(te, __OFT_FREE);
		oft->state = OFT_READY;
	}
out:
	mutex_unlock(&oft->lock);

	return ret;
}

/* start a transaction */
int of_transaction_start(struct of_transaction *oft)
{
	int ret;

	ret = 0;

	/* take the global of transaction mutex */
	mutex_lock(&of_transaction_mutex);

	mutex_lock(&oft->lock);
	if (oft->state != OFT_READY) {
		ret = -EBUSY;
		pr_err("%s: transaction is busy; can't start\n", __func__);
		goto out;
	}
	oft->state = OFT_IN_PROGRESS;
out:
	mutex_unlock(&oft->lock);

	/* release the global transaction lock in case of an error */
	if (ret != 0)
		mutex_unlock(&of_transaction_mutex);

	return ret;
}

/* abort a transaction in progress */
int of_transaction_abort(struct of_transaction *oft)
{
	int ret, rel_global_lock;
	struct of_transaction_entry *te, *ten;

	ret = 0;
	rel_global_lock = 0;
	mutex_lock(&oft->lock);
	if (oft->state != OFT_IN_PROGRESS) {
		ret = -EINVAL;
		pr_err("%s: transaction invalid state\n", __func__);
		goto out;
	}
	/* release global lock */
	rel_global_lock = 1;

	list_for_each_entry_safe_reverse(te, ten, &oft->te_list, node)
		__of_transaction_entry_revert_op(te, __OFT_FIRE_NOTIFIERS |
				__OFT_REVERT_PRE | __OFT_FREE);

	oft->state = OFT_READY;
out:
	mutex_unlock(&oft->lock);

	if (rel_global_lock)
		mutex_unlock(&of_transaction_mutex);

	return ret;
}

int of_transaction_commit(struct of_transaction *oft)
{
	struct of_transaction_entry *te;
	int ret;

	ret = 0;
	mutex_lock(&oft->lock);
	if (oft->state != OFT_IN_PROGRESS) {
		ret = -EINVAL;
		pr_err("%s: transaction invalid state\n", __func__);
		goto out;
	}
	/* change state to committing */
	oft->state = OFT_COMMITTING;
	mutex_unlock(&oft->lock);

	/* drop the global lock here (notifiers and devices need it) */
	mutex_unlock(&of_transaction_mutex);

	/* grab the lock again */
	mutex_lock(&oft->lock);
	if (oft->state != OFT_COMMITTING) {
		ret = -EINVAL;
		pr_err("%s: transaction not in committing state\n",
				__func__);
		goto out;
	}
	/* perform the rest of the work */
	list_for_each_entry(te, &oft->te_list, node)
		__of_transaction_entry_apply_op(te, __OFT_APPLY_POST |
				__OFT_DEV_CHANGE);
	oft->state = OFT_COMMITTED;
out:
	mutex_unlock(&oft->lock);

	return ret;
}

int of_transaction_revert(struct of_transaction *oft)
{
	int ret, rel_global_lock;
	struct of_transaction_entry *te, *ten;

	ret = 0;
	rel_global_lock = 0;

	mutex_lock(&oft->lock);
	if (oft->state != OFT_COMMITTED) {
		ret = -EBUSY;
		pr_err("%s: transaction is not commited\n", __func__);
		goto out;
	}
	/* change state to reverting */
	oft->state = OFT_REVERTING;

	/* fire the device change notifiers */
	list_for_each_entry_reverse(te, &oft->te_list, node)
		__of_transaction_entry_revert_op(te, __OFT_DEV_CHANGE |
				__OFT_FIRE_NOTIFIERS);

	mutex_unlock(&oft->lock);

	/* take the global of transaction mutex */
	mutex_lock(&of_transaction_mutex);

	mutex_lock(&oft->lock);
	rel_global_lock = 1;

	if (oft->state != OFT_REVERTING) {
		ret = -EBUSY;
		pr_err("%s: transaction is not reverting\n", __func__);
		goto out;
	}

	list_for_each_entry_safe_reverse(te, ten, &oft->te_list, node)
		__of_transaction_entry_revert_op(te, __OFT_REVERT_PRE |
				__OFT_REVERT_POST | __OFT_FREE);
	/* and we're done */
	oft->state = OFT_READY;
out:
	mutex_unlock(&oft->lock);

	if (rel_global_lock)
		mutex_unlock(&of_transaction_mutex);

	return ret;
}

int of_transaction_action(struct of_transaction *oft, unsigned long action,
		struct device_node *np, struct property *prop)
{
	struct of_transaction_entry *te;
	int ret;

	ret = 0;

	mutex_lock(&oft->lock);

	if (oft->state != OFT_IN_PROGRESS) {
		ret = -EINVAL;
		pr_err("%s: transaction invalid state\n", __func__);
		goto out;
	}

	/* create the transaction entry */
	te = __of_transaction_entry_create(action, np, prop);
	if (IS_ERR(te)) {
		pr_err("%s: failed to create entry for @%s\n",
				__func__, np->full_name);
		ret = PTR_ERR(te);
		goto out;
	}

	/* apply it */
	ret = __of_transaction_entry_apply_op(te, __OFT_FIRE_NOTIFIERS |
			__OFT_APPLY_PRE);
	if (ret != 0) {
		pr_err("%s: failed to apply for @%s\n",
				__func__, np->full_name);
		__of_transaction_entry_destroy(te);
		goto out;
	}

	/* add it to the list */
	list_add_tail(&te->node, &oft->te_list);
out:
	mutex_unlock(&oft->lock);

	return ret;
}
