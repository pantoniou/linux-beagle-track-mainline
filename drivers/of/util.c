/*
 * Utility functions for working with device tree(s)
 *
 * Copyright (C) 2012 Pantelis Antoniou <panto@antoniou-consulting.com>
 * Copyright (C) 2012 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

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

/**
 * __of_free_property - release the memory of an allocated property
 * @prop:	Property to release
 *
 * Release the memory of an allocated property only after checking
 * that the property has been marked as OF_DYNAMIC.
 * Only call on known allocated properties.
 */
void __of_free_property(struct property *prop)
{
	if (prop == NULL)
		return;

	if (of_property_check_flag(prop, OF_DYNAMIC)) {
		if (of_property_check_flag(prop, OF_ALLOCVALUE))
			kfree(prop->value);
		if (of_property_check_flag(prop, OF_ALLOCNAME))
			kfree(prop->name);
		kfree(prop);
	}
}

/**
 * __of_free_tree - release the memory of a device tree node and
 *		    of all it's children + properties.
 * @node:	Device Tree node to release
 *
 * Release the memory of a device tree node and of all it's children.
 * Also release the properties and the dead properties.
 * Only call on detached node trees, and you better be sure that
 * no pointer exist for any properties. Only safe to do if you
 * absolutely control the life cycle of the node.
 * Also note that the node is not removed from the all_nodes list,
 * neither from the parent's child list; this should be handled before
 * calling this function.
 */
void __of_free_tree(struct device_node *node)
{
	struct property *prop;
	struct device_node *noden;

	/* sanity check */
	if (!node)
		return;

	/* free recursively any children */
	while ((noden = node->child) != NULL) {
		node->child = noden->sibling;
		__of_free_tree(noden);
	}

	/* free every property already allocated */
	while ((prop = node->properties) != NULL) {
		node->properties = prop->next;
		__of_free_property(prop);
	}

	/* free dead properties already allocated */
	while ((prop = node->deadprops) != NULL) {
		node->deadprops = prop->next;
		__of_free_property(prop);
	}

	if (of_node_check_flag(node, OF_DYNAMIC)) {
		if (of_node_check_flag(node, OF_ALLOCFULL))
			kfree(node->full_name);
		if (of_node_check_flag(node, OF_ALLOCTYPE))
			kfree(node->type);
		if (of_node_check_flag(node, OF_ALLOCNAME))
			kfree(node->name);
		kfree(node);
	}
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

	if (prop->length > 0) {
		if (of_property_check_flag(propn, OF_ALLOCVALUE)) {
			propn->value = kmalloc(prop->length, allocflags);
			if (propn->value == NULL)
				goto err_fail_value;
			memcpy(propn->value, prop->value, prop->length);
		} else
			propn->value = prop->value;

		propn->length = prop->length;
	}

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
			goto err_return;
	} else
		node->name = name;

	if (of_node_check_flag(node, OF_ALLOCTYPE)) {
		node->type = kstrdup(type, allocflags);
		if (node->type == NULL)
			goto err_return;
	} else
		node->type = type;

	if (of_node_check_flag(node, OF_ALLOCFULL)) {
		node->full_name = kstrdup(full_name, allocflags);
		if (node->full_name == NULL)
			goto err_return;
	} else
		node->full_name = full_name;

	node->phandle = phandle;
	of_node_set_flag(node, OF_DYNAMIC);
	of_node_set_flag(node, OF_DETACHED);

	of_node_init(node);

	return node;

err_return:
	__of_free_tree(node);
	return NULL;
}
