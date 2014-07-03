#ifndef _LINUX_OF_PRIVATE_H
#define _LINUX_OF_PRIVATE_H
/*
 * Private symbols used by OF support code
 *
 * Paul Mackerras	August 1996.
 * Copyright (C) 1996-2005 Paul Mackerras.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/**
 * struct alias_prop - Alias property in 'aliases' node
 * @link:	List node to link the structure in aliases_lookup list
 * @alias:	Alias property name
 * @np:		Pointer to device_node that the alias stands for
 * @id:		Index value from end of alias name
 * @stem:	Alias string without the index
 *
 * The structure represents one alias property of 'aliases' node as
 * an entry in aliases_lookup list.
 */
struct alias_prop {
	struct list_head link;
	const char *alias;
	struct device_node *np;
	int id;
	char stem[0];
};

extern struct mutex of_mutex;
extern struct list_head aliases_lookup;

static inline struct device_node *kobj_to_device_node(struct kobject *kobj)
{
	return container_of(kobj, struct device_node, kobj);
}

/* if dynamic is located in dynamic.c, otherwise in base.c */
extern void of_node_release(struct kobject *kobj);

#if defined(CONFIG_OF_DYNAMIC)
extern int of_property_notify(int action, struct device_node *np,
			      struct property *prop);
extern int of_reconfig_notify(unsigned long action, void *p);
#else /* CONFIG_OF_DYNAMIC */
static inline int of_property_notify(int action, struct device_node *np,
				     struct property *prop)
{
	return 0;
}

static inline int of_reconfig_notify(unsigned long action, void *p)
{
	return 0;
}
#endif /* CONFIG_OF_DYNAMIC */

#endif /* _LINUX_OF_PRIVATE_H */
