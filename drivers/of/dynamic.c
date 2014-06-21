/*
 * Support for dynamic device trees.
 *
 * On some platforms, the device tree can be manipulated at runtime.
 * The routines in this section support adding, removing and changing
 * device tree nodes.
 */

#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/proc_fs.h>

#include "of_private.h"

/**
 *	of_node_get - Increment refcount of a node
 *	@node:	Node to inc refcount, NULL is supported to
 *		simplify writing of callers
 *
 *	Returns node.
 */
struct device_node *of_node_get(struct device_node *node)
{
	if (node)
		kobject_get(&node->kobj);
	return node;
}
EXPORT_SYMBOL(of_node_get);

/**
 *	of_node_put - Decrement refcount of a node
 *	@node:	Node to dec refcount, NULL is supported to
 *		simplify writing of callers
 *
 */
void of_node_put(struct device_node *node)
{
	if (node)
		kobject_put(&node->kobj);
}
EXPORT_SYMBOL(of_node_put);

static void of_node_remove(struct device_node *np)
{
	struct property *pp;

	BUG_ON(!of_node_is_initialized(np));

	/* only remove properties if on sysfs */
	if (of_node_is_attached(np)) {
		for_each_property_of_node(np, pp)
			sysfs_remove_bin_file(&np->kobj, &pp->attr);
		kobject_del(&np->kobj);
	}

	/* finally remove the kobj_init ref */
	of_node_put(np);
}

static BLOCKING_NOTIFIER_HEAD(of_reconfig_chain);

int of_reconfig_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_register);

int of_reconfig_notifier_unregister(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_unregister);

int of_reconfig_notify(unsigned long action, void *p)
{
	int rc;

	rc = blocking_notifier_call_chain(&of_reconfig_chain, action, p);
	return notifier_to_errno(rc);
}

int of_property_notify(int action, struct device_node *np,
			      struct property *prop)
{
	struct of_prop_reconfig pr;

	/* only call notifiers if the node is attached */
	if (!of_node_is_attached(np))
		return 0;

	pr.dn = np;
	pr.prop = prop;
	return of_reconfig_notify(action, &pr);
}

/**
 * of_attach_node() - Plug a device node into the tree and global list.
 */
int of_attach_node(struct device_node *np)
{
	unsigned long flags;
	int rc;

	rc = of_reconfig_notify(OF_RECONFIG_ATTACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np->sibling = np->parent->child;
	np->allnext = of_allnodes;
	np->parent->child = np;
	of_allnodes = np;
	of_node_clear_flag(np, OF_DETACHED);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_node_add(np);
	return 0;
}

/**
 * of_detach_node() - "Unplug" a node from the device tree.
 *
 * The caller must hold a reference to the node.  The memory associated with
 * the node is not freed until its refcount goes to zero.
 */
int of_detach_node(struct device_node *np)
{
	struct device_node *parent;
	unsigned long flags;
	int rc = 0;

	rc = of_reconfig_notify(OF_RECONFIG_DETACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	if (of_node_check_flag(np, OF_DETACHED)) {
		/* someone already detached it */
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
		return rc;
	}

	parent = np->parent;
	if (!parent) {
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
		return rc;
	}

	if (of_allnodes == np)
		of_allnodes = np->allnext;
	else {
		struct device_node *prev;

		for (prev = of_allnodes;
		     prev->allnext != np;
		     prev = prev->allnext)
			;
		prev->allnext = np->allnext;
	}

	if (parent->child == np)
		parent->child = np->sibling;
	else {
		struct device_node *prevsib;

		for (prevsib = np->parent->child;
		     prevsib->sibling != np;
		     prevsib = prevsib->sibling)
			;
		prevsib->sibling = np->sibling;
	}

	of_node_set_flag(np, OF_DETACHED);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_node_remove(np);
	return rc;
}

static struct device_node *of_alldeadnodes;
static DEFINE_RAW_SPINLOCK(deadtree_lock);

/**
 *	of_node_release - release a dynamically allocated node
 *	@kref:  kref element of the node to be released
 *
 *	In of_node_put() this function is passed to kref_put()
 *	as the destructor.
 *
 *	There is an option to not free the node, since due to the
 *	way that node and property life-cycles are not completely
 *	managed, we can't free the memory of a node at will.
 *	Instead we move the node to the dead nodes list where it will
 *	remain until the life-cycle issues are resolved.
 */
void of_node_release(struct kobject *kobj)
{
	/*  we probe the of-node-keep chosen prop once */
	static int node_keep = -1;
	struct device_node *node = kobj_to_device_node(kobj);
	struct property *prop;
	unsigned long flags;

	/* We should never be releasing nodes that haven't been detached. */
	if (!of_node_check_flag(node, OF_DETACHED)) {
		pr_err("ERROR: Bad of_node_put() on %s\n", node->full_name);
		dump_stack();
		return;
	}

	/* we should not be trying to release the root */
	if (WARN_ON(node == of_allnodes))
		return;

	/* read the chosen boolean property "of-node-keep" once */
	if (node_keep == -1)
		node_keep = of_property_read_bool(of_chosen, "of-node-keep");

	/* default is to free everything */
	if (!node_keep) {

		/* free normal properties */
		while ((prop = node->properties) != NULL) {
			node->properties = prop->next;
			kfree(prop->name);
			kfree(prop->value);
			kfree(prop);
		}

		/* free deap properties */
		while ((prop = node->deadprops) != NULL) {
			node->deadprops = prop->next;
			kfree(prop->name);
			kfree(prop->value);
			kfree(prop);
		}

		/* free the node */
		kfree(node->full_name);
		kfree(node->data);
		kfree(node);
		return;
	}

	pr_info("%s: dead node \"%s\"\n", __func__, node->full_name);

	/* can't use devtree lock; at of_node_put caller might be holding it */
	raw_spin_lock_irqsave(&deadtree_lock, flags);

	/* move all properties to dead properties */
	while ((prop = node->properties) != NULL) {
		node->properties = prop->next;
		prop->next = node->deadprops;
		node->deadprops = prop;
	}

	/* move node to alldeadnodes */
	node->allnext = of_alldeadnodes;
	of_alldeadnodes = node;

	raw_spin_unlock_irqrestore(&deadtree_lock, flags);
}
