#ifndef _LINUX_OF_H
#define _LINUX_OF_H
/*
 * Definitions for talking to the Open Firmware PROM on
 * Power Macintosh and other computers.
 *
 * Copyright (C) 1996-2005 Paul Mackerras.
 *
 * Updates for PPC64 by Peter Bergner & David Engebretsen, IBM Corp.
 * Updates for SPARC64 by David S. Miller
 * Derived from PowerPC and Sparc prom.h files by Stephen Rothwell, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/mod_devicetable.h>
#include <linux/spinlock.h>
#include <linux/topology.h>
#include <linux/notifier.h>
#include <linux/list.h>

#include <asm/byteorder.h>
#include <asm/errno.h>

typedef u32 phandle;
typedef u32 ihandle;

struct property {
	char	*name;
	int	length;
	void	*value;
	struct property *next;
	unsigned long _flags;
	unsigned int unique_id;
	struct bin_attribute attr;
};

#if defined(CONFIG_SPARC)
struct of_irq_controller;
#endif

struct device_node {
	const char *name;
	const char *type;
	phandle phandle;
	const char *full_name;

	struct	property *properties;
	struct	property *deadprops;	/* removed properties */
	struct	device_node *parent;
	struct	device_node *child;
	struct	device_node *sibling;
	struct	device_node *next;	/* next device of same type */
	struct	device_node *allnext;	/* next in list of all nodes */
	struct	kobject kobj;
	unsigned long _flags;
	void	*data;
#if defined(CONFIG_SPARC)
	const char *path_component_name;
	unsigned int unique_id;
	struct of_irq_controller *irq_trans;
#endif
};

#define MAX_PHANDLE_ARGS 16
struct of_phandle_args {
	struct device_node *np;
	int args_count;
	uint32_t args[MAX_PHANDLE_ARGS];
};

extern int of_node_add(struct device_node *node);

/* initialize a node */
extern struct kobj_type of_node_ktype;
static inline void of_node_init(struct device_node *node)
{
	kobject_init(&node->kobj, &of_node_ktype);
}

/* true when node is initialized */
static inline int of_node_is_initialized(struct device_node *node)
{
	return node && node->kobj.state_initialized;
}

/* true when node is attached (i.e. present on sysfs) */
static inline int of_node_is_attached(struct device_node *node)
{
	return node && node->kobj.state_in_sysfs;
}

#ifdef CONFIG_OF_DYNAMIC
extern struct device_node *of_node_get(struct device_node *node);
extern void of_node_put(struct device_node *node);
extern int of_property_notify(int action, struct device_node *np,
			      struct property *prop);
#else /* CONFIG_OF_DYNAMIC */
/* Dummy ref counting routines - to be implemented later */
static inline struct device_node *of_node_get(struct device_node *node)
{
	return node;
}
static inline void of_node_put(struct device_node *node) { }
static inline int of_property_notify(int action, struct device_node *np,
			      struct property *prop)
{
	return 0;
}
#endif /* !CONFIG_OF_DYNAMIC */

#ifdef CONFIG_OF

/* Pointer for first entry in chain of all nodes. */
extern struct device_node *of_allnodes;
extern struct device_node *of_chosen;
extern struct device_node *of_aliases;
extern raw_spinlock_t devtree_lock;
extern struct mutex of_aliases_mutex;
extern struct mutex of_transaction_mutex;

static inline bool of_have_populated_dt(void)
{
	return of_allnodes != NULL;
}

static inline bool of_node_is_root(const struct device_node *node)
{
	return node && (node->parent == NULL);
}

static inline int of_node_check_flag(struct device_node *n, unsigned long flag)
{
	return test_bit(flag, &n->_flags);
}

static inline int of_node_test_and_set_flag(struct device_node *n,
					    unsigned long flag)
{
	return test_and_set_bit(flag, &n->_flags);
}

static inline void of_node_set_flag(struct device_node *n, unsigned long flag)
{
	set_bit(flag, &n->_flags);
}

static inline void of_node_clear_flag(struct device_node *n, unsigned long flag)
{
	clear_bit(flag, &n->_flags);
}

static inline int of_property_check_flag(struct property *p, unsigned long flag)
{
	return test_bit(flag, &p->_flags);
}

static inline void of_property_set_flag(struct property *p, unsigned long flag)
{
	set_bit(flag, &p->_flags);
}

static inline void of_property_clear_flag(struct property *p, unsigned long flag)
{
	clear_bit(flag, &p->_flags);
}

extern struct device_node *of_find_all_nodes(struct device_node *prev);

/*
 * OF address retrieval & translation
 */

/* Helper to read a big number; size is in cells (not bytes) */
static inline u64 of_read_number(const __be32 *cell, int size)
{
	u64 r = 0;
	while (size--)
		r = (r << 32) | be32_to_cpu(*(cell++));
	return r;
}

/* Like of_read_number, but we want an unsigned long result */
static inline unsigned long of_read_ulong(const __be32 *cell, int size)
{
	/* toss away upper bits if unsigned long is smaller than u64 */
	return of_read_number(cell, size);
}

#if defined(CONFIG_SPARC)
#include <asm/prom.h>
#endif

/* Default #address and #size cells.  Allow arch asm/prom.h to override */
#if !defined(OF_ROOT_NODE_ADDR_CELLS_DEFAULT)
#define OF_ROOT_NODE_ADDR_CELLS_DEFAULT 1
#define OF_ROOT_NODE_SIZE_CELLS_DEFAULT 1
#endif

/* Default string compare functions, Allow arch asm/prom.h to override */
#if !defined(of_compat_cmp)
#define of_compat_cmp(s1, s2, l)	strcasecmp((s1), (s2))
#define of_prop_cmp(s1, s2)		strcmp((s1), (s2))
#define of_node_cmp(s1, s2)		strcasecmp((s1), (s2))
#endif

/* flag descriptions */
#define OF_DYNAMIC	1 /* node and properties were allocated via kmalloc */
#define OF_DETACHED	2 /* node has been detached from the device tree */
#define OF_POPULATED	3 /* device already created for the node */
#define OF_ALLOCNAME	4 /* name was kmalloc-ed */
#define OF_ALLOCTYPE	5 /* type was kmalloc-ed */
#define OF_ALLOCFULL	6 /* full_name was kmalloc-ed */
#define OF_ALLOCVALUE	7 /* value was kmalloc-ed */

#define OF_NODE_ALLOCALL \
	((1 << OF_ALLOCNAME) | (1 << OF_ALLOCTYPE) | (1 << OF_ALLOCFULL))
#define OF_PROP_ALLOCALL \
	((1 << OF_ALLOCNAME) | (1 << OF_ALLOCVALUE))

#define OF_IS_DYNAMIC(x) test_bit(OF_DYNAMIC, &x->_flags)
#define OF_MARK_DYNAMIC(x) set_bit(OF_DYNAMIC, &x->_flags)

#define OF_BAD_ADDR	((u64)-1)

static inline const char *of_node_full_name(const struct device_node *np)
{
	return np ? np->full_name : "<no-node>";
}

#define for_each_of_allnodes(dn) \
	for (dn = of_allnodes; dn; dn = dn->allnext)
extern struct device_node *of_find_node_by_name(struct device_node *from,
	const char *name);
extern struct device_node *of_find_node_by_type(struct device_node *from,
	const char *type);
extern struct device_node *of_find_compatible_node(struct device_node *from,
	const char *type, const char *compat);
extern struct device_node *of_find_matching_node_and_match(
	struct device_node *from,
	const struct of_device_id *matches,
	const struct of_device_id **match);

extern struct device_node *of_find_node_by_path(const char *path);
extern struct device_node *of_find_node_by_phandle(phandle handle);
struct device_node *__of_find_node_by_full_name(struct device_node *node,
						const char *full_name);
struct device_node *of_find_node_by_full_name(struct device_node *node,
						const char *full_name);
extern struct device_node *of_get_parent(const struct device_node *node);
extern struct device_node *of_get_next_parent(struct device_node *node);
extern struct device_node *of_get_next_child(const struct device_node *node,
					     struct device_node *prev);
struct device_node *__of_get_next_child(const struct device_node *node,
						struct device_node *prev);
extern struct device_node *of_get_next_available_child(
	const struct device_node *node, struct device_node *prev);

extern struct device_node *of_get_child_by_name(const struct device_node *node,
					const char *name);

/* cache lookup */
extern struct device_node *of_find_next_cache_node(const struct device_node *);
extern struct device_node *of_find_node_with_property(
	struct device_node *from, const char *prop_name);

extern struct property *of_find_property(const struct device_node *np,
					 const char *name,
					 int *lenp);
extern int of_property_count_elems_of_size(const struct device_node *np,
				const char *propname, int elem_size);
extern int of_property_read_u32_index(const struct device_node *np,
				       const char *propname,
				       u32 index, u32 *out_value);
extern int of_property_read_u8_array(const struct device_node *np,
			const char *propname, u8 *out_values, size_t sz);
extern int of_property_read_u16_array(const struct device_node *np,
			const char *propname, u16 *out_values, size_t sz);
extern int of_property_read_u32_array(const struct device_node *np,
				      const char *propname,
				      u32 *out_values,
				      size_t sz);
extern int of_property_read_u64(const struct device_node *np,
				const char *propname, u64 *out_value);

extern int of_property_read_string(struct device_node *np,
				   const char *propname,
				   const char **out_string);
extern int of_property_read_string_index(struct device_node *np,
					 const char *propname,
					 int index, const char **output);
extern int of_property_match_string(struct device_node *np,
				    const char *propname,
				    const char *string);
extern int of_property_count_strings(struct device_node *np,
				     const char *propname);
int __of_device_is_compatible(const struct device_node *device,
				     const char *compat, const char *type,
				     const char *name);
extern int of_device_is_compatible(const struct device_node *device,
				   const char *);
extern int __of_device_is_available(const struct device_node *device);
extern int of_device_is_available(const struct device_node *device);

extern const void *__of_get_property(const struct device_node *node,
				const char *name,
				int *lenp);
extern const void *of_get_property(const struct device_node *node,
				const char *name,
				int *lenp);
extern struct device_node *of_get_cpu_node(int cpu, unsigned int *thread);
#define for_each_property_of_node(dn, pp) \
	for (pp = dn->properties; pp != NULL; pp = pp->next)

extern int of_n_addr_cells(struct device_node *np);
extern int of_n_size_cells(struct device_node *np);
extern const struct of_device_id *of_match_node(
	const struct of_device_id *matches, const struct device_node *node);
extern int of_modalias_node(struct device_node *node, char *modalias, int len);
extern void of_print_phandle_args(const char *msg, const struct of_phandle_args *args);
extern struct device_node *of_parse_phandle(const struct device_node *np,
					    const char *phandle_name,
					    int index);
extern int of_parse_phandle_with_args(const struct device_node *np,
	const char *list_name, const char *cells_name, int index,
	struct of_phandle_args *out_args);
extern int of_parse_phandle_with_fixed_args(const struct device_node *np,
	const char *list_name, int cells_count, int index,
	struct of_phandle_args *out_args);
extern int of_count_phandle_with_args(const struct device_node *np,
	const char *list_name, const char *cells_name);

extern void of_alias_scan(void * (*dt_alloc)(u64 size, u64 align));
extern int of_alias_get_id(struct device_node *np, const char *stem);

extern int of_machine_is_compatible(const char *compat);

int __of_add_property(struct device_node *np, struct property *prop);
void __of_add_property_post(struct device_node *np, struct property *prop,
		int ignore_dup_name);
int __of_remove_property(struct device_node *np, struct property *prop);
void __of_remove_property_post(struct device_node *np, struct property *prop);
int __of_update_property(struct device_node *np, struct property *newprop,
		struct property **oldprop);
void __of_update_property_post(struct device_node *np, struct property *newprop,
		struct property *oldprop);

extern int of_add_property(struct device_node *np, struct property *prop);
extern int of_remove_property(struct device_node *np, struct property *prop);
extern int of_update_property(struct device_node *np, struct property *newprop);

/* For updating the device tree at runtime */
#define OF_RECONFIG_ATTACH_NODE		0x0001
#define OF_RECONFIG_DETACH_NODE		0x0002
#define OF_RECONFIG_ADD_PROPERTY	0x0003
#define OF_RECONFIG_REMOVE_PROPERTY	0x0004
#define OF_RECONFIG_UPDATE_PROPERTY	0x0005
#define OF_RECONFIG_DYNAMIC_CREATE_DEV	0x0006
#define OF_RECONFIG_DYNAMIC_DESTROY_DEV	0x0007

struct of_prop_reconfig {
	struct device_node	*dn;
	struct property		*prop;
};

extern int of_reconfig_notifier_register(struct notifier_block *);
extern int of_reconfig_notifier_unregister(struct notifier_block *);
extern int of_reconfig_notify(unsigned long, void *);

void __of_attach_node(struct device_node *np);
void __of_attach_node_post(struct device_node *np);
void __of_detach_node(struct device_node *np);
void __of_detach_node_post(struct device_node *np);

extern int of_attach_node(struct device_node *);
extern int of_detach_node(struct device_node *);

#define of_match_ptr(_ptr)	(_ptr)

/*
 * struct property *prop;
 * const __be32 *p;
 * u32 u;
 *
 * of_property_for_each_u32(np, "propname", prop, p, u)
 *         printk("U32 value: %x\n", u);
 */
const __be32 *of_prop_next_u32(struct property *prop, const __be32 *cur,
			       u32 *pu);
/*
 * struct property *prop;
 * const char *s;
 *
 * of_property_for_each_string(np, "propname", prop, s)
 *         printk("String value: %s\n", s);
 */
const char *of_prop_next_string(struct property *prop, const char *cur);

int of_device_is_stdout_path(struct device_node *dn);

#else /* CONFIG_OF */

static inline const char* of_node_full_name(const struct device_node *np)
{
	return "<no-node>";
}

static inline struct device_node *of_find_node_by_name(struct device_node *from,
	const char *name)
{
	return NULL;
}

static inline struct device_node *of_find_node_by_type(struct device_node *from,
	const char *type)
{
	return NULL;
}

static inline struct device_node *of_find_matching_node_and_match(
	struct device_node *from,
	const struct of_device_id *matches,
	const struct of_device_id **match)
{
	return NULL;
}

static inline struct device_node *of_find_node_by_path(const char *path)
{
	return NULL;
}

static inline struct device_node *of_get_parent(const struct device_node *node)
{
	return NULL;
}

static inline struct device_node *of_get_next_child(
	const struct device_node *node, struct device_node *prev)
{
	return NULL;
}

static inline struct device_node *__of_get_next_child(
	const struct device_node *node, struct device_node *prev)
{
	return NULL;
}

static inline struct device_node *of_get_next_available_child(
	const struct device_node *node, struct device_node *prev)
{
	return NULL;
}

static inline struct device_node *of_find_node_with_property(
	struct device_node *from, const char *prop_name)
{
	return NULL;
}

static inline bool of_have_populated_dt(void)
{
	return false;
}

static inline struct device_node *of_get_child_by_name(
					const struct device_node *node,
					const char *name)
{
	return NULL;
}

static inline int of_device_is_compatible(const struct device_node *device,
					  const char *name)
{
	return 0;
}

static inline int of_device_is_available(const struct device_node *device)
{
	return 0;
}

static inline struct property *of_find_property(const struct device_node *np,
						const char *name,
						int *lenp)
{
	return NULL;
}

static inline struct device_node *of_find_compatible_node(
						struct device_node *from,
						const char *type,
						const char *compat)
{
	return NULL;
}

static inline int of_property_count_elems_of_size(const struct device_node *np,
			const char *propname, int elem_size)
{
	return -ENOSYS;
}

static inline int of_property_read_u32_index(const struct device_node *np,
			const char *propname, u32 index, u32 *out_value)
{
	return -ENOSYS;
}

static inline int of_property_read_u8_array(const struct device_node *np,
			const char *propname, u8 *out_values, size_t sz)
{
	return -ENOSYS;
}

static inline int of_property_read_u16_array(const struct device_node *np,
			const char *propname, u16 *out_values, size_t sz)
{
	return -ENOSYS;
}

static inline int of_property_read_u32_array(const struct device_node *np,
					     const char *propname,
					     u32 *out_values, size_t sz)
{
	return -ENOSYS;
}

static inline int of_property_read_string(struct device_node *np,
					  const char *propname,
					  const char **out_string)
{
	return -ENOSYS;
}

static inline int of_property_read_string_index(struct device_node *np,
						const char *propname, int index,
						const char **out_string)
{
	return -ENOSYS;
}

static inline int of_property_count_strings(struct device_node *np,
					    const char *propname)
{
	return -ENOSYS;
}

static inline const void *of_get_property(const struct device_node *node,
				const char *name,
				int *lenp)
{
	return NULL;
}

static inline struct device_node *of_get_cpu_node(int cpu,
					unsigned int *thread)
{
	return NULL;
}

static inline int of_property_read_u64(const struct device_node *np,
				       const char *propname, u64 *out_value)
{
	return -ENOSYS;
}

static inline int of_property_match_string(struct device_node *np,
					   const char *propname,
					   const char *string)
{
	return -ENOSYS;
}

static inline struct device_node *of_parse_phandle(const struct device_node *np,
						   const char *phandle_name,
						   int index)
{
	return NULL;
}

static inline int of_parse_phandle_with_args(struct device_node *np,
					     const char *list_name,
					     const char *cells_name,
					     int index,
					     struct of_phandle_args *out_args)
{
	return -ENOSYS;
}

static inline int of_parse_phandle_with_fixed_args(const struct device_node *np,
	const char *list_name, int cells_count, int index,
	struct of_phandle_args *out_args)
{
	return -ENOSYS;
}

static inline int of_count_phandle_with_args(struct device_node *np,
					     const char *list_name,
					     const char *cells_name)
{
	return -ENOSYS;
}

static inline int of_alias_get_id(struct device_node *np, const char *stem)
{
	return -ENOSYS;
}

static inline int of_machine_is_compatible(const char *compat)
{
	return 0;
}

static inline int of_device_is_stdout_path(struct device_node *dn)
{
	return 0;
}

static inline const __be32 *of_prop_next_u32(struct property *prop,
		const __be32 *cur, u32 *pu)
{
	return NULL;
}

static inline const char *of_prop_next_string(struct property *prop,
		const char *cur)
{
	return NULL;
}

#define of_match_ptr(_ptr)	NULL
#define of_match_node(_matches, _node)	NULL
#endif /* CONFIG_OF */

#if defined(CONFIG_OF) && defined(CONFIG_NUMA)
extern int of_node_to_nid(struct device_node *np);
#else
static inline int of_node_to_nid(struct device_node *device) { return 0; }
#endif

static inline struct device_node *of_find_matching_node(
	struct device_node *from,
	const struct of_device_id *matches)
{
	return of_find_matching_node_and_match(from, matches, NULL);
}

/**
 * of_property_count_u8_elems - Count the number of u8 elements in a property
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 *
 * Search for a property in a device node and count the number of u8 elements
 * in it. Returns number of elements on sucess, -EINVAL if the property does
 * not exist or its length does not match a multiple of u8 and -ENODATA if the
 * property does not have a value.
 */
static inline int of_property_count_u8_elems(const struct device_node *np,
				const char *propname)
{
	return of_property_count_elems_of_size(np, propname, sizeof(u8));
}

/**
 * of_property_count_u16_elems - Count the number of u16 elements in a property
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 *
 * Search for a property in a device node and count the number of u16 elements
 * in it. Returns number of elements on sucess, -EINVAL if the property does
 * not exist or its length does not match a multiple of u16 and -ENODATA if the
 * property does not have a value.
 */
static inline int of_property_count_u16_elems(const struct device_node *np,
				const char *propname)
{
	return of_property_count_elems_of_size(np, propname, sizeof(u16));
}

/**
 * of_property_count_u32_elems - Count the number of u32 elements in a property
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 *
 * Search for a property in a device node and count the number of u32 elements
 * in it. Returns number of elements on sucess, -EINVAL if the property does
 * not exist or its length does not match a multiple of u32 and -ENODATA if the
 * property does not have a value.
 */
static inline int of_property_count_u32_elems(const struct device_node *np,
				const char *propname)
{
	return of_property_count_elems_of_size(np, propname, sizeof(u32));
}

/**
 * of_property_count_u64_elems - Count the number of u64 elements in a property
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 *
 * Search for a property in a device node and count the number of u64 elements
 * in it. Returns number of elements on sucess, -EINVAL if the property does
 * not exist or its length does not match a multiple of u64 and -ENODATA if the
 * property does not have a value.
 */
static inline int of_property_count_u64_elems(const struct device_node *np,
				const char *propname)
{
	return of_property_count_elems_of_size(np, propname, sizeof(u64));
}

/**
 * of_property_read_bool - Findfrom a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 *
 * Search for a property in a device node.
 * Returns true if the property exist false otherwise.
 */
static inline bool of_property_read_bool(const struct device_node *np,
					 const char *propname)
{
	struct property *prop = of_find_property(np, propname, NULL);

	return prop ? true : false;
}

static inline int of_property_read_u8(const struct device_node *np,
				       const char *propname,
				       u8 *out_value)
{
	return of_property_read_u8_array(np, propname, out_value, 1);
}

static inline int of_property_read_u16(const struct device_node *np,
				       const char *propname,
				       u16 *out_value)
{
	return of_property_read_u16_array(np, propname, out_value, 1);
}

static inline int of_property_read_u32(const struct device_node *np,
				       const char *propname,
				       u32 *out_value)
{
	return of_property_read_u32_array(np, propname, out_value, 1);
}

#define of_property_for_each_u32(np, propname, prop, p, u)	\
	for (prop = of_find_property(np, propname, NULL),	\
		p = of_prop_next_u32(prop, NULL, &u);		\
		p;						\
		p = of_prop_next_u32(prop, p, &u))

#define of_property_for_each_string(np, propname, prop, s)	\
	for (prop = of_find_property(np, propname, NULL),	\
		s = of_prop_next_string(prop, NULL);		\
		s;						\
		s = of_prop_next_string(prop, s))

#define for_each_node_by_name(dn, name) \
	for (dn = of_find_node_by_name(NULL, name); dn; \
	     dn = of_find_node_by_name(dn, name))
#define for_each_node_by_type(dn, type) \
	for (dn = of_find_node_by_type(NULL, type); dn; \
	     dn = of_find_node_by_type(dn, type))
#define for_each_compatible_node(dn, type, compatible) \
	for (dn = of_find_compatible_node(NULL, type, compatible); dn; \
	     dn = of_find_compatible_node(dn, type, compatible))
#define for_each_matching_node(dn, matches) \
	for (dn = of_find_matching_node(NULL, matches); dn; \
	     dn = of_find_matching_node(dn, matches))
#define for_each_matching_node_and_match(dn, matches, match) \
	for (dn = of_find_matching_node_and_match(NULL, matches, match); \
	     dn; dn = of_find_matching_node_and_match(dn, matches, match))

#define for_each_child_of_node(parent, child) \
	for (child = of_get_next_child(parent, NULL); child != NULL; \
	     child = of_get_next_child(parent, child))
#define __for_each_child_of_node(parent, child) \
	for (child = __of_get_next_child(parent, NULL); child != NULL; \
	     child = __of_get_next_child(parent, child))
#define for_each_available_child_of_node(parent, child) \
	for (child = of_get_next_available_child(parent, NULL); child != NULL; \
	     child = of_get_next_available_child(parent, child))

#define for_each_node_with_property(dn, prop_name) \
	for (dn = of_find_node_with_property(NULL, prop_name); dn; \
	     dn = of_find_node_with_property(dn, prop_name))

static inline int of_get_child_count(const struct device_node *np)
{
	struct device_node *child;
	int num = 0;

	for_each_child_of_node(np, child)
		num++;

	return num;
}

static inline int of_get_available_child_count(const struct device_node *np)
{
	struct device_node *child;
	int num = 0;

	for_each_available_child_of_node(np, child)
		num++;

	return num;
}

#ifdef CONFIG_OF
#define _OF_DECLARE(table, name, compat, fn, fn_type)			\
	static const struct of_device_id __of_table_##name		\
		__used __section(__##table##_of_table)			\
		 = { .compatible = compat,				\
		     .data = (fn == (fn_type)NULL) ? fn : fn  }
#else
#define _OF_DECLARE(table, name, compat, fn, fn_type)					\
	static const struct of_device_id __of_table_##name		\
		__attribute__((unused))					\
		 = { .compatible = compat,				\
		     .data = (fn == (fn_type)NULL) ? fn : fn }
#endif

typedef int (*of_init_fn_2)(struct device_node *, struct device_node *);
typedef void (*of_init_fn_1)(struct device_node *);

#define OF_DECLARE_1(table, name, compat, fn) \
		_OF_DECLARE(table, name, compat, fn, of_init_fn_1)
#define OF_DECLARE_2(table, name, compat, fn) \
		_OF_DECLARE(table, name, compat, fn, of_init_fn_2)

/**
 * General utilities for working with live trees.
 *
 * All functions with two leading underscores operate
 * without taking node references, so you either have to
 * own the devtree lock or work on detached trees only.
 */

#ifdef CONFIG_OF

void __of_free_property(struct property *prop);
void __of_free_tree(struct device_node *node);
struct property *__of_copy_property(const struct property *prop,
		gfp_t allocflags, unsigned long propflags);
struct device_node *__of_create_empty_node( const char *name,
		const char *type, const char *full_name,
		phandle phandle, gfp_t allocflags, unsigned long nodeflags);

#else /* !CONFIG_OF */

#define __for_each_child_of_node(dn, chld) \
	while (0)

static inline void __of_free_property(struct property *prop) { }

static inline void __of_free_tree(struct device_node *node) { }

static inline struct property *__of_copy_property(const struct property *prop,
		gfp_t allocflags, unsigned long propflags)
{
	return NULL;
}

static inline struct device_node *__of_create_empty_node( const char *name,
		const char *type, const char *full_name,
		phandle phandle, gfp_t allocflags, unsigned long nodeflags)
{
	return NULL;
}

#endif	/* !CONFIG_OF */

/**
 * struct of_transaction_entry	- Holds a DT log entry
 * @node:	list_head for the log list
 * @action:	notifier action
 * @np:		pointer to the device node affected
 * @prop:	pointer to the property affected
 * @old_prop:	hold a pointer to the original property
 *
 * Every modification of the device tree during application of the
 * overlay is held in a list of of_overlay_log_entry structures.
 * That way we can recover from a partial application, or we can
 * revert the overlay properly.
 */
struct of_transaction_entry {
	struct list_head node;
	unsigned long action;
	struct device_node *np;
	struct property *prop;
	struct property *old_prop;

	/* if property change on status
	*/
	int device_state_change;
};

enum of_transaction_state {
	OFT_READY,
	OFT_IN_PROGRESS,
	OFT_COMMITTING,
	OFT_COMMITTED,
	OFT_REVERTING,
};

struct of_transaction {
	struct list_head te_list;
	struct mutex lock;
	enum of_transaction_state state;
};

#define for_each_transaction_entry(_oft, _te) \
	list_for_each_entry(_te, &(_oft)->te_list, node)

#define for_each_transaction_entry_reverse(_oft, _te) \
	list_for_each_entry_reverse(_te, &(_oft)->te_list, node)

#ifdef CONFIG_OF

int of_transaction_init(struct of_transaction *oft);
int of_transaction_destroy(struct of_transaction *oft);
int of_transaction_start(struct of_transaction *oft);
int of_transaction_abort(struct of_transaction *oft);
int of_transaction_commit(struct of_transaction *oft);
int of_transaction_revert(struct of_transaction *oft);
int of_transaction_action(struct of_transaction *oft, unsigned long action,
		struct device_node *np, struct property *prop);

#else

static inline int of_transaction_init(struct of_transaction *oft)
{
	return -ENOTSUPP;
}

static inline int of_transaction_destroy(struct of_transaction *oft)
{
	return -ENOTSUPP;
}

static inline int of_transaction_start(struct of_transaction *oft)
{
	return -ENOTSUPP;
}

static inline int of_transaction_abort(struct of_transaction *oft)
{
	return -ENOTSUPP;
}

static inline int of_transaction_commit(struct of_transaction *oft)
{
	return -ENOTSUPP;
}

static inline int of_transaction_revert(struct of_transaction *oft)
{
	return -ENOTSUPP;
}

static inline int of_transaction_action(struct of_transaction *oft,
		unsigned long action, struct device_node *np,
		struct property *prop)
{
	return -ENOTSUPP;
}

#endif

static inline int of_transaction_attach_node(struct of_transaction *oft,
		struct device_node *np)
{
	return of_transaction_action(oft, OF_RECONFIG_ATTACH_NODE, np, NULL);
}

static inline int of_transaction_detach_node(struct of_transaction *oft,
		struct device_node *np)
{
	return of_transaction_action(oft,
			OF_RECONFIG_DETACH_NODE, np, NULL);
}

static inline int of_transaction_add_property(struct of_transaction *oft,
		struct device_node *np, struct property *prop)
{
	return of_transaction_action(oft,
			OF_RECONFIG_ADD_PROPERTY, np, prop);
}

static inline int of_transaction_remove_property(struct of_transaction *oft,
		struct device_node *np, struct property *prop)
{
	return of_transaction_action(oft,
			OF_RECONFIG_REMOVE_PROPERTY, np, prop);
}

static inline int of_transaction_update_property(struct of_transaction *oft,
		struct device_node *np, struct property *prop)
{
	return of_transaction_action(oft,
			OF_RECONFIG_UPDATE_PROPERTY, np, prop);
}

/* illegal phandle value (set when unresolved) */
#define OF_PHANDLE_ILLEGAL	0xdeadbeef

#ifdef CONFIG_OF_RESOLVE

int of_resolve(struct device_node *resolve);

#else

static inline int of_resolve(struct device_node *resolve)
{
	return -ENOTSUPP;
}

#endif

/**
 * Overlay support
 */

/**
 * struct of_overlay_log_entry	- Holds a DT log entry
 * @node:	list_head for the log list
 * @action:	notifier action
 * @np:		pointer to the device node affected
 * @prop:	pointer to the property affected
 * @old_prop:	hold a pointer to the original property
 *
 * Every modification of the device tree during application of the
 * overlay is held in a list of of_overlay_log_entry structures.
 * That way we can recover from a partial application, or we can
 * revert the overlay properly.
 */
struct of_overlay_log_entry {
	struct list_head node;
	unsigned long action;
	struct device_node *np;
	struct property *prop;
	struct property *old_prop;
};

struct of_overlay_device_entry;

/**
 * struct of_overlay_device_entry	- Holds an overlay device entry
 * @node:	list_head for the device list
 * @np:		device node pointer to the device node affected
 * @state:	new device state
 * @prevstate:	previous device state
 * @priv:	private pointer for use by bus handlers
 *
 * When the overlay results in a device node's state to change this
 * fact is recorded in a list of device entries. After the overlay
 * is applied we can create/destroy the devices according
 * to the new state of the live tree.
 */
struct of_overlay_device_entry {
	struct list_head node;
	struct device_node *np;
	int prevstate;
	int state;
	void *priv;
};

/**
 * struct of_overlay_info	- Holds a single overlay info
 * @target:	target of the overlay operation
 * @overlay:	pointer to the overlay contents node
 * @le_list:	List of the overlay logs
 * @de_list:	List of the overlay records
 *
 * Holds a single overlay state, including all the overlay logs &
 * records.
 */
struct of_overlay_info {
	struct device_node *target;
	struct device_node *overlay;
	struct list_head le_list;
	struct list_head de_list;
};

/**
 * struct of_overlay - Holds a complete overlay transaction
 * @node:	List on which we are located
 * @count:	Count of ovinfo structures
 * @ovinfo:	Overlay info array (count size)
 *
 * Holds a complete overlay transaction
 */
struct of_overlay {
	int id;
	struct list_head node;
	int count;
	struct of_overlay_info *ovinfo_tab;
	/* optional properties will follow eventually */
};

#ifdef CONFIG_OF_OVERLAY

/* the following is the internal API */
int of_overlay_apply(int count, struct of_overlay_info *ovinfo_tab);
int of_overlay_revert(int count, struct of_overlay_info *ovinfo_tab);

int of_fill_overlay_info(struct device_node *info_node,
		struct of_overlay_info *ovinfo);
int of_build_overlay_info(struct device_node *tree,
		int *cntp, struct of_overlay_info **ovinfop);
int of_free_overlay_info(int cnt, struct of_overlay_info *ovinfo);

/* ID based overlays; the API for external users */
int of_overlay_create(struct device_node *tree);
int of_overlay_destroy(int id);
int of_overlay_destroy_all(void);

#else

static inline int of_overlay_apply(int count,
		struct of_overlay_info *ovinfo_tab)
{
	return -ENOTSUPP;
}

static inline int of_overlay_revert(int count,
		struct of_overlay_info *ovinfo_tab)
{
	return -ENOTSUPP;
}

static inline int of_fill_overlay_info(struct device_node *info_node,
		struct of_overlay_info *ovinfo)
{
	return -ENOTSUPP;
}

static inline int of_build_overlay_info(struct device_node *tree,
		int *cntp, struct of_overlay_info **ovinfop)
{
	return -ENOTSUPP;
}

static inline int of_free_overlay_info(int cnt,
		struct of_overlay_info *ovinfo)
{
	return -ENOTSUPP;
}

int of_overlay_create(struct device_node *tree)
{
	return -ENOTSUPP;
}

int of_overlay_destroy(int id)
{
	return -ENOTSUPP;
}

int of_overlay_destroy_all(void)
{
	return -ENOTSUPP;
}

#endif

#endif /* _LINUX_OF_H */
