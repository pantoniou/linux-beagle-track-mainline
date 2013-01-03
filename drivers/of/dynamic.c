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

void __of_attach_node(struct device_node *np)
{
	np->sibling = np->parent->child;
	np->allnext = np->parent->allnext;
	np->parent->allnext = np;
	np->parent->child = np;
	of_node_clear_flag(np, OF_DETACHED);
}

/**
 * of_attach_node - Plug a device node into the tree and global list.
 */
int of_attach_node(struct device_node *np)
{
	unsigned long flags;
	int rc;

	rc = of_reconfig_notify(OF_RECONFIG_ATTACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	__of_attach_node(np);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_node_add(np);
	return 0;
}

void __of_detach_node(struct device_node *np)
{
	struct device_node *parent;
	struct device_node *prev;
	struct device_node *prevsib;

	if (WARN_ON(of_node_check_flag(np, OF_DETACHED)))
		return;

	parent = np->parent;
	if (WARN_ON(!parent))
		return;

	if (of_allnodes == np)
		of_allnodes = np->allnext;
	else {
		for (prev = of_allnodes;
		     prev->allnext != np;
		     prev = prev->allnext)
			;
		prev->allnext = np->allnext;
	}

	if (parent->child == np)
		parent->child = np->sibling;
	else {
		for (prevsib = np->parent->child;
		     prevsib->sibling != np;
		     prevsib = prevsib->sibling)
			;
		prevsib->sibling = np->sibling;
	}

	of_node_set_flag(np, OF_DETACHED);
}

/**
 * of_detach_node - "Unplug" a node from the device tree.
 *
 * The caller must hold a reference to the node.  The memory associated with
 * the node is not freed until its refcount goes to zero.
 */
int of_detach_node(struct device_node *np)
{
	unsigned long flags;
	int rc = 0;

	rc = of_reconfig_notify(OF_RECONFIG_DETACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	__of_detach_node(np);
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

/**
 * __of_copy_property - Copy a property dynamically.
 * @prop:	Property to copy
 * @allocflags:	Allocation flags (typically pass GFP_KERNEL)
 * @propflags:	Property flags
 *
 * Copy a property by dynamically allocating the memory of both the
 * property stucture and the property name & contents. The property's
 * flags have the OF_DYNAMIC bit set so that we can differentiate between
 * dynamically allocated properties and not.
 * Returns the newly allocated property or NULL on out of memory error.
 */
struct property *__of_copy_property(const struct property *prop,
		gfp_t allocflags, unsigned long propflags)
{
	struct property *propn;

	propn = kzalloc(sizeof(*prop), allocflags);
	if (propn == NULL)
		return NULL;

	propn->_flags = propflags;

	if (of_property_check_flag(propn, OF_ALLOCNAME)) {
		propn->name = kstrdup(prop->name, allocflags);
		if (propn->name == NULL)
			goto err_fail_name;
	} else
		propn->name = prop->name;

	/*
	 * NOTE: There is no check for zero length value.
	 * In case of a boolean property This will allocate a value
	 * of zero bytes. We do this to work around the use
	 * of of_get_property() calls on boolean values.
	 */
	if (of_property_check_flag(propn, OF_ALLOCVALUE)) {
		propn->value = kmalloc(prop->length, allocflags);
		if (propn->value == NULL)
			goto err_fail_value;
		memcpy(propn->value, prop->value, prop->length);
	} else
		propn->value = prop->value;

	propn->length = prop->length;

	/* mark the property as dynamic */
	of_property_set_flag(propn, OF_DYNAMIC);

	return propn;

err_fail_value:
	if (of_property_check_flag(propn, OF_ALLOCNAME))
		kfree(propn->name);
err_fail_name:
	kfree(propn);
	return NULL;
}

/**
 * __of_create_empty_node - Create an empty device node dynamically.
 * @name:	Name of the new device node
 * @type:	Type of the new device node
 * @full_name:	Full name of the new device node
 * @phandle:	Phandle of the new device node
 * @allocflags:	Allocation flags (typically pass GFP_KERNEL)
 * @nodeflags:	Node flags
 *
 * Create an empty device tree node, suitable for further modification.
 * The node data are dynamically allocated and all the node flags
 * have the OF_DYNAMIC & OF_DETACHED bits set.
 * Returns the newly allocated node or NULL on out of memory error.
 */
struct device_node *__of_create_empty_node(
		const char *name, const char *type, const char *full_name,
		phandle phandle, gfp_t allocflags, unsigned long nodeflags)
{
	struct device_node *node;

	node = kzalloc(sizeof(*node), allocflags);
	if (node == NULL)
		return NULL;

	node->_flags = nodeflags;

	if (of_node_check_flag(node, OF_ALLOCNAME)) {
		node->name = kstrdup(name, allocflags);
		if (node->name == NULL)
			goto err_free_node;
	} else
		node->name = name;

	if (of_node_check_flag(node, OF_ALLOCTYPE)) {
		node->type = kstrdup(type, allocflags);
		if (node->type == NULL)
			goto err_free_name;
	} else
		node->type = type;

	if (of_node_check_flag(node, OF_ALLOCFULL)) {
		node->full_name = kstrdup(full_name, allocflags);
		if (node->full_name == NULL)
			goto err_free_type;
	} else
		node->full_name = full_name;

	node->phandle = phandle;
	of_node_set_flag(node, OF_DYNAMIC);
	of_node_set_flag(node, OF_DETACHED);

	of_node_init(node);

	return node;
err_free_type:
	if (of_node_check_flag(node, OF_ALLOCTYPE))
		kfree(node->type);
err_free_name:
	if (of_node_check_flag(node, OF_ALLOCNAME))
		kfree(node->name);
err_free_node:
	kfree(node);
	return NULL;
}
