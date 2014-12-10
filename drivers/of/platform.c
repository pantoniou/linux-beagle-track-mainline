/*
 *    Copyright (C) 2006 Benjamin Herrenschmidt, IBM Corp.
 *			 <benh@kernel.crashing.org>
 *    and		 Arnd Bergmann, IBM Corp.
 *    Merged from powerpc/kernel/of_platform.c and
 *    sparc{,64}/kernel/of_device.c by Stephen Rothwell
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/amba/bus.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_iommu.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

const struct of_device_id of_default_bus_match_table[] = {
	{ .compatible = "simple-bus", },
#ifdef CONFIG_ARM_AMBA
	{ .compatible = "arm,amba-bus", },
#endif /* CONFIG_ARM_AMBA */
	{} /* Empty terminated list */
};

static int of_dev_node_match(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/**
 * of_find_device_by_node - Find the platform_device associated with a node
 * @np: Pointer to device tree node
 *
 * Returns platform_device pointer, or NULL if not found
 */
struct platform_device *of_find_device_by_node(struct device_node *np)
{
	struct device *dev;

	dev = bus_find_device(&platform_bus_type, NULL, np, of_dev_node_match);
	return dev ? to_platform_device(dev) : NULL;
}
EXPORT_SYMBOL(of_find_device_by_node);

#ifdef CONFIG_OF_ADDRESS
/*
 * The following routines scan a subtree and registers a device for
 * each applicable node.
 *
 * Note: sparc doesn't use these routines because it has a different
 * mechanism for creating devices from device tree nodes.
 */

/**
 * of_device_make_bus_id - Use the device node data to assign a unique name
 * @dev: pointer to device structure that is linked to a device tree node
 *
 * This routine will first try using the translated bus address to
 * derive a unique name. If it cannot, then it will prepend names from
 * parent nodes until a unique name can be derived.
 */
void of_device_make_bus_id(struct device *dev)
{
	struct device_node *node = dev->of_node;
	const __be32 *reg;
	u64 addr;

	/* Construct the name, using parent nodes if necessary to ensure uniqueness */
	while (node->parent) {
		/*
		 * If the address can be translated, then that is as much
		 * uniqueness as we need. Make it the first component and return
		 */
		reg = of_get_property(node, "reg", NULL);
		if (reg && (addr = of_translate_address(node, reg)) != OF_BAD_ADDR) {
			dev_set_name(dev, dev_name(dev) ? "%llx.%s:%s" : "%llx.%s",
				     (unsigned long long)addr, node->name,
				     dev_name(dev));
			return;
		}

		/* format arguments only used if dev_name() resolves to NULL */
		dev_set_name(dev, dev_name(dev) ? "%s:%s" : "%s",
			     strrchr(node->full_name, '/') + 1, dev_name(dev));
		node = node->parent;
	}
}

/**
 * of_device_alloc - Allocate and initialize an of_device
 * @np: device node to assign to device
 * @bus_id: Name to assign to the device.  May be null to use default name.
 * @parent: Parent device.
 */
struct platform_device *of_device_alloc(struct device_node *np,
				  const char *bus_id,
				  struct device *parent)
{
	struct platform_device *dev;
	int rc, i, num_reg = 0, num_irq;
	struct resource *res, temp_res;

	dev = platform_device_alloc("", -1);
	if (!dev)
		return NULL;

	/* count the io and irq resources */
	while (of_address_to_resource(np, num_reg, &temp_res) == 0)
		num_reg++;
	num_irq = of_irq_count(np);

	/* Populate the resource table */
	if (num_irq || num_reg) {
		res = kzalloc(sizeof(*res) * (num_irq + num_reg), GFP_KERNEL);
		if (!res) {
			platform_device_put(dev);
			return NULL;
		}

		dev->num_resources = num_reg + num_irq;
		dev->resource = res;
		for (i = 0; i < num_reg; i++, res++) {
			rc = of_address_to_resource(np, i, res);
			WARN_ON(rc);
		}
		if (of_irq_to_resource_table(np, res, num_irq) != num_irq)
			pr_debug("not all legacy IRQ resources mapped for %s\n",
				 np->name);
	}

	dev->dev.of_node = of_node_get(np);
	dev->dev.parent = parent ? : &platform_bus;

	if (bus_id)
		dev_set_name(&dev->dev, "%s", bus_id);
	else
		of_device_make_bus_id(&dev->dev);

	return dev;
}
EXPORT_SYMBOL(of_device_alloc);

/**
 * of_dma_configure - Setup DMA configuration
 * @dev:	Device to apply DMA configuration
 *
 * Try to get devices's DMA configuration from DT and update it
 * accordingly.
 *
 * In case if platform code need to use own special DMA configuration,it
 * can use Platform bus notifier and handle BUS_NOTIFY_ADD_DEVICE event
 * to fix up DMA configuration.
 */
static void of_dma_configure(struct device *dev)
{
	u64 dma_addr, paddr, size;
	int ret;
	bool coherent;
	unsigned long offset;
	struct iommu_ops *iommu;

	/*
	 * Set default dma-mask to 32 bit. Drivers are expected to setup
	 * the correct supported dma_mask.
	 */
	dev->coherent_dma_mask = DMA_BIT_MASK(32);

	/*
	 * Set it to coherent_dma_mask by default if the architecture
	 * code has not set it.
	 */
	if (!dev->dma_mask)
		dev->dma_mask = &dev->coherent_dma_mask;

	ret = of_dma_get_range(dev->of_node, &dma_addr, &paddr, &size);
	if (ret < 0) {
		dma_addr = offset = 0;
		size = dev->coherent_dma_mask;
	} else {
		offset = PFN_DOWN(paddr - dma_addr);
		dev_dbg(dev, "dma_pfn_offset(%#08lx)\n", offset);
	}
	dev->dma_pfn_offset = offset;

	coherent = of_dma_is_coherent(dev->of_node);
	dev_dbg(dev, "device is%sdma coherent\n",
		coherent ? " " : " not ");

	iommu = of_iommu_configure(dev);
	dev_dbg(dev, "device is%sbehind an iommu\n",
		iommu ? " " : " not ");

	arch_setup_dma_ops(dev, dma_addr, size, iommu, coherent);
}

static void of_dma_deconfigure(struct device *dev)
{
	arch_teardown_dma_ops(dev);
}

/**
 * of_platform_device_create_pdata - Alloc, initialize and register an of_device
 * @np: pointer to node to create device for
 * @bus_id: name to assign device
 * @platform_data: pointer to populate platform_data pointer with
 * @parent: Linux device model parent device.
 *
 * Returns pointer to created platform device, or NULL if a device was not
 * registered.  Unavailable devices will not get registered.
 */
static struct platform_device *of_platform_device_create_pdata(
					struct device_node *np,
					const char *bus_id,
					void *platform_data,
					struct device *parent)
{
	struct platform_device *dev;

	if (!of_device_is_available(np) ||
	    of_node_test_and_set_flag(np, OF_POPULATED))
		return NULL;

	dev = of_device_alloc(np, bus_id, parent);
	if (!dev)
		goto err_clear_flag;

	dev->dev.bus = &platform_bus_type;
	dev->dev.platform_data = platform_data;
	of_dma_configure(&dev->dev);

	if (of_device_add(dev) != 0) {
		of_dma_deconfigure(&dev->dev);
		platform_device_put(dev);
		goto err_clear_flag;
	}

	return dev;

err_clear_flag:
	of_node_clear_flag(np, OF_POPULATED);
	return NULL;
}

/**
 * of_platform_device_create - Alloc, initialize and register an of_device
 * @np: pointer to node to create device for
 * @bus_id: name to assign device
 * @parent: Linux device model parent device.
 *
 * Returns pointer to created platform device, or NULL if a device was not
 * registered.  Unavailable devices will not get registered.
 */
struct platform_device *of_platform_device_create(struct device_node *np,
					    const char *bus_id,
					    struct device *parent)
{
	return of_platform_device_create_pdata(np, bus_id, NULL, parent);
}
EXPORT_SYMBOL(of_platform_device_create);

#ifdef CONFIG_ARM_AMBA
static struct amba_device *of_amba_device_create(struct device_node *node,
						 const char *bus_id,
						 void *platform_data,
						 struct device *parent)
{
	struct amba_device *dev;
	const void *prop;
	int i, ret;

	pr_debug("Creating amba device %s\n", node->full_name);

	if (!of_device_is_available(node) ||
	    of_node_test_and_set_flag(node, OF_POPULATED))
		return NULL;

	dev = amba_device_alloc(NULL, 0, 0);
	if (!dev) {
		pr_err("%s(): amba_device_alloc() failed for %s\n",
		       __func__, node->full_name);
		goto err_clear_flag;
	}

	/* setup generic device info */
	dev->dev.of_node = of_node_get(node);
	dev->dev.parent = parent ? : &platform_bus;
	dev->dev.platform_data = platform_data;
	if (bus_id)
		dev_set_name(&dev->dev, "%s", bus_id);
	else
		of_device_make_bus_id(&dev->dev);
	of_dma_configure(&dev->dev);

	/* Allow the HW Peripheral ID to be overridden */
	prop = of_get_property(node, "arm,primecell-periphid", NULL);
	if (prop)
		dev->periphid = of_read_ulong(prop, 1);

	/* Decode the IRQs and address ranges */
	for (i = 0; i < AMBA_NR_IRQS; i++)
		dev->irq[i] = irq_of_parse_and_map(node, i);

	ret = of_address_to_resource(node, 0, &dev->res);
	if (ret) {
		pr_err("%s(): of_address_to_resource() failed (%d) for %s\n",
		       __func__, ret, node->full_name);
		goto err_free;
	}

	ret = amba_device_add(dev, &iomem_resource);
	if (ret) {
		pr_err("%s(): amba_device_add() failed (%d) for %s\n",
		       __func__, ret, node->full_name);
		goto err_free;
	}

	return dev;

err_free:
	amba_device_put(dev);
err_clear_flag:
	of_node_clear_flag(node, OF_POPULATED);
	return NULL;
}
#else /* CONFIG_ARM_AMBA */
static struct amba_device *of_amba_device_create(struct device_node *node,
						 const char *bus_id,
						 void *platform_data,
						 struct device *parent)
{
	return NULL;
}
#endif /* CONFIG_ARM_AMBA */

/**
 * of_devname_lookup() - Given a device node, lookup the preferred Linux name
 */
static const struct of_dev_auxdata *of_dev_lookup(const struct of_dev_auxdata *lookup,
				 struct device_node *np)
{
	struct resource res;

	if (!lookup)
		return NULL;

	for(; lookup->compatible != NULL; lookup++) {
		if (!of_device_is_compatible(np, lookup->compatible))
			continue;
		if (!of_address_to_resource(np, 0, &res))
			if (res.start != lookup->phys_addr)
				continue;
		pr_debug("%s: devname=%s\n", np->full_name, lookup->name);
		return lookup;
	}

	return NULL;
}

/**
 * of_platform_bus_create() - Create a device for a node and its children.
 * @bus: device node of the bus to instantiate
 * @matches: match table for bus nodes
 * @lookup: auxdata table for matching id and platform_data with device nodes
 * @parent: parent for new device, or NULL for top level.
 * @strict: require compatible property
 *
 * Creates a platform_device for the provided device_node, and optionally
 * recursively create devices for all the child nodes.
 */
static int of_platform_bus_create(struct device_node *bus,
				  const struct of_device_id *matches,
				  const struct of_dev_auxdata *lookup,
				  struct device *parent, bool strict)
{
	const struct of_dev_auxdata *auxdata;
	struct device_node *child;
	struct platform_device *dev;
	const char *bus_id = NULL;
	void *platform_data = NULL;
	int rc = 0;

	/* Make sure it has a compatible property */
	if (strict && (!of_get_property(bus, "compatible", NULL))) {
		pr_debug("%s() - skipping %s, no compatible prop\n",
			 __func__, bus->full_name);
		return 0;
	}

	auxdata = of_dev_lookup(lookup, bus);
	if (auxdata) {
		bus_id = auxdata->name;
		platform_data = auxdata->platform_data;
	}

	if (of_device_is_compatible(bus, "arm,primecell")) {
		/*
		 * Don't return an error here to keep compatibility with older
		 * device tree files.
		 */
		of_amba_device_create(bus, bus_id, platform_data, parent);
		return 0;
	}

	dev = of_platform_device_create_pdata(bus, bus_id, platform_data, parent);
	if (!dev || !of_match_node(matches, bus))
		return 0;

	for_each_child_of_node(bus, child) {
		pr_debug("   create child: %s\n", child->full_name);
		rc = of_platform_bus_create(child, matches, lookup, &dev->dev, strict);
		if (rc) {
			of_node_put(child);
			break;
		}
	}
	of_node_set_flag(bus, OF_POPULATED_BUS);
	return rc;
}

/**
 * of_platform_bus_probe() - Probe the device-tree for platform buses
 * @root: parent of the first level to probe or NULL for the root of the tree
 * @matches: match table for bus nodes
 * @parent: parent to hook devices from, NULL for toplevel
 *
 * Note that children of the provided root are not instantiated as devices
 * unless the specified root itself matches the bus list and is not NULL.
 */
int of_platform_bus_probe(struct device_node *root,
			  const struct of_device_id *matches,
			  struct device *parent)
{
	struct device_node *child;
	int rc = 0;

	root = root ? of_node_get(root) : of_find_node_by_path("/");
	if (!root)
		return -EINVAL;

	pr_debug("of_platform_bus_probe()\n");
	pr_debug(" starting at: %s\n", root->full_name);

	/* Do a self check of bus type, if there's a match, create children */
	if (of_match_node(matches, root)) {
		rc = of_platform_bus_create(root, matches, NULL, parent, false);
	} else for_each_child_of_node(root, child) {
		if (!of_match_node(matches, child))
			continue;
		rc = of_platform_bus_create(child, matches, NULL, parent, false);
		if (rc)
			break;
	}

	of_node_put(root);
	return rc;
}
EXPORT_SYMBOL(of_platform_bus_probe);

struct of_pop_ref_entry {
	struct list_head node;
	struct device_node *np;
};

struct of_pop_dep_entry {
	struct list_head node;
	struct of_pop_entry *pe;
};

struct of_pop_entry {
	struct of_pop_entry *parent;
	struct list_head children;
	struct list_head node;

	struct device_node *np;
	unsigned int bus : 1;
	unsigned int amba : 1;
	unsigned int children_loop : 1;
	struct list_head refs;
	struct list_head deps;

	unsigned int loop : 1;
	unsigned int temp_mark : 1;
	unsigned int perm_mark : 1;
	struct list_head sort_children;
	struct list_head sort_node;
	int refcnt;

	int id;
};

static phandle __phandle_ref(struct device_node *lfnp, const char *prop,
		uint32_t off)
{
	struct device_node *np;
	const void *value;
	const char *name;
	int len;

	name = of_node_full_name(lfnp);
	name += strlen("/__local_fixups__");
	/* pr_info("%s: %s\n", __func__, name); */
	np = of_find_node_by_path(name);
	if (!np)
		return 0;
	value = of_get_property(np, prop, &len);
	if (off + sizeof(uint32_t) > len)
		return 0;
	return be32_to_cpup(value + off);
}

/* returns true is np_ref is pointing to an external tree */
static bool __external_ref(struct device_node *np, struct device_node *np_ref)
{
	while (np_ref != NULL) {
		if (np_ref == np)
			return false;
		np_ref = np_ref->parent;
	}
	return true;
}

/* returns true is np_ref is pointing to an internal tree */
static bool __internal_ref(struct device_node *np, struct device_node *np_ref)
{
	do {
		if (np_ref == np)
			return true;
		np_ref = np_ref->parent;
	} while (np_ref != NULL);
	return false;
}

static void __local_fixup_ref(struct of_pop_entry *pe, struct device_node *lfnp)
{
	struct device_node *child;
	struct property *prop;
	int i, len;
	uint32_t off;
	phandle ph;
	struct device_node *phnp;
	struct of_pop_ref_entry *re;
	bool found;

	if (!lfnp)
		return;

	for_each_property_of_node(lfnp, prop) {

		/* skip the auto-generated properties */
		if (!of_prop_cmp(prop->name, "name") ||
		    !of_prop_cmp(prop->name, "phandle") ||
		    !of_prop_cmp(prop->name, "linux,phandle") ||
		    !of_prop_cmp(prop->name, "ibm,phandle"))
			continue;

		len = prop->length / 4;

		/* pr_info("%s/%s (%d)\n", of_node_full_name(lfnp),
				prop->name, len); */

		for (i = 0; i < len; i++) {
			off = be32_to_cpup((void *)prop->value + i * 4);
			ph = __phandle_ref(lfnp, prop->name, off);
			if (ph == 0)
				continue;
			phnp = of_find_node_by_phandle(ph);
			if (!phnp)
				continue;
			if (!__external_ref(pe->np, phnp))
				continue;

			/* check whether the ref is already there */
			found = false;
			list_for_each_entry(re, &pe->refs, node) {
				if (re->np == phnp) {
					found = true;
					break;
				}
			}

			if (!found) {
				re = kzalloc(sizeof(*re), GFP_KERNEL);
				BUG_ON(re == NULL);
				re->np = phnp;
				list_add_tail(&re->node, &pe->refs);

			}

			/* pr_info("  %08x - ph %08x - @%s\n", off, ph,
					of_node_full_name(phnp)); */

			of_node_put(phnp);
		}
	}

	for_each_child_of_node(lfnp, child)
		__local_fixup_ref(pe, child);
}

static void __of_platform_populate_get_refs_internal(struct of_pop_entry *pe)
{
	struct device_node *np;
	struct of_pop_ref_entry *re;
	char *base;
	bool found;

	/* first find the local fixups */
	base = kasprintf(GFP_KERNEL, "/__local_fixups__%s",
			of_node_full_name(pe->np));
	if (base) {
		np = of_find_node_by_path(base);
		if (np) {
			__local_fixup_ref(pe, np);
			of_node_put(np);
		}
		kfree(base);
	}

	/* now try the old style interrupt */
	if (of_get_property(pe->np, "interrupts", NULL) &&
			(np = of_irq_find_parent(pe->np)) != NULL) {

		/* check whether the ref is already there */
		found = false;
		list_for_each_entry(re, &pe->refs, node) {
			if (re->np == np) {
				found = true;
				break;
			}
		}

		if (!found) {
			re = kzalloc(sizeof(*re), GFP_KERNEL);
			BUG_ON(re == NULL);
			re->np = np;
			list_add_tail(&re->node, &pe->refs);

		}

		/* pr_info("  %08x - ph %08x - @%s\n", off, ph,
				of_node_full_name(phnp)); */
		of_node_put(np);
	}
}

static void __of_platform_populate_scan_internal(struct device_node *root,
			const struct of_device_id *matches,
			struct of_pop_entry *pep, int level)
{
	struct device_node *child;
	struct of_pop_entry *pe;

	BUG_ON(root == NULL);

	for_each_child_of_node(root, child) {

		/* of_platform_bus_create (strict) */
		if (!of_get_property(child, "compatible", NULL) ||
			!of_device_is_available(child) ||
			of_node_check_flag(child, OF_POPULATED))
			continue;

		pe = kzalloc(sizeof(*pe), GFP_KERNEL);
		BUG_ON(pe == NULL);
		pe->parent = pep;
		INIT_LIST_HEAD(&pe->node);
		INIT_LIST_HEAD(&pe->children);
		INIT_LIST_HEAD(&pe->refs);
		INIT_LIST_HEAD(&pe->deps);
		INIT_LIST_HEAD(&pe->sort_children);
		INIT_LIST_HEAD(&pe->sort_node);
		pe->np = child;

		list_add_tail(&pe->node, &pep->children);

		if (of_device_is_compatible(pe->np, "arm,primecell"))
			pe->amba = 1;
		else if (of_match_node(matches, pe->np)) {
			pe->bus = 1;
			__of_platform_populate_scan_internal(child, matches, pe, level + 1);
		}
	}
}

static void __of_platform_get_refs(struct of_pop_entry *pep, int level)
{
	struct of_pop_entry *pe;

	__of_platform_populate_get_refs_internal(pep);

	list_for_each_entry(pe, &pep->children, node) {
		if (pe->bus)
			__of_platform_get_refs(pe, level + 1);
		__of_platform_populate_get_refs_internal(pe);
	}
}

static void __of_platform_make_deps_internal(struct of_pop_entry *pe)
{
	struct of_pop_ref_entry *re;
	struct of_pop_entry *ppe, *tpe;
	struct of_pop_dep_entry *de;
	bool found;

	/* for each ref, find sibling that contains it */
	list_for_each_entry(re, &pe->refs, node) {
		if (!pe->parent)
			continue;
		ppe = pe->parent;
		list_for_each_entry(tpe, &ppe->children, node) {
			if (tpe == pe)
				continue;
			if (!__internal_ref(tpe->np, re->np))
				continue;

			/* check whether the ref is already there */
			found = false;
			list_for_each_entry(de, &pe->deps, node) {
				if (de->pe == tpe) {
					found = true;
					break;
				}
			}

			if (!found) {
				de = kzalloc(sizeof(*de), GFP_KERNEL);
				BUG_ON(de == NULL);
				de->pe = tpe;
				tpe->refcnt++;
				list_add_tail(&de->node, &pe->deps);

				/* pr_info("* %s depends on %s\n",
						of_node_full_name(pe->np),
						of_node_full_name(tpe->np)); */
			}
		}
	}
}

static void __of_platform_make_deps(struct of_pop_entry *pep, int level)
{
	struct of_pop_entry *pe;

	__of_platform_make_deps_internal(pep);
	list_for_each_entry(pe, &pep->children, node)
		__of_platform_make_deps(pe, level + 1);
}

static int __of_platform_visit(struct of_pop_entry *pe)
{
	struct of_pop_dep_entry *de;
	bool circle;
	int rc;

	/* don't do anything with root or a visited node */
	if (!pe->parent || pe->perm_mark)
		return 0;

	/* cycle */
	circle = false;
	if (pe->temp_mark) {
		pr_info("%s: circle at @%s\n", __func__,
				of_node_full_name(pe->np));
		circle = true;
	} else {
		pe->temp_mark = 1;
		list_for_each_entry(de, &pe->deps, node) {
			rc = __of_platform_visit(de->pe);
			if (rc != 0)
				circle = true;
		}
		pe->temp_mark = 0;
		list_add_tail(&pe->sort_node, &pe->parent->sort_children);
	}

	pe->perm_mark = 1;

	if (circle)
		pe->loop = 1;

	return circle ? -1 : 0;
}

static int __of_platform_reorder_internal(struct of_pop_entry *pep)
{
	struct of_pop_entry *pe;
	int ret;
	bool circle;

	circle = false;
	list_for_each_entry(pe, &pep->children, node) {
		ret = __of_platform_visit(pe);
		if (ret != 0)
			circle = true;
	}
	return circle ? -1 : 0;
}

static void __of_platform_reorder(struct of_pop_entry *pep, int level)
{
	struct of_pop_entry *pe;
	int ret;

	ret = __of_platform_reorder_internal(pep);
	if (ret != 0) {
		pr_info("%s: circle at @%s\n", __func__,
			of_node_full_name(pep->np));
		pep->children_loop = 1;
	}

	list_for_each_entry(pe, &pep->children, node)
		__of_platform_reorder(pe, level + 1);
}

static int __of_platform_assign_order(struct of_pop_entry *pep, int level, int id)
{
	struct of_pop_entry *pe;

	pep->id = id++;
	list_for_each_entry(pe, &pep->sort_children, sort_node)
		id = __of_platform_assign_order(pe, level + 1, id);
	return id;
}

static int __count_children(struct of_pop_entry *pep)
{
	struct of_pop_entry *pe;
	int cnt;

	cnt = 0;
	list_for_each_entry(pe, &pep->children, node)
		cnt++;
	return cnt;
}

static int __count_sort_children(struct of_pop_entry *pep)
{
	struct of_pop_entry *pe;
	int cnt;

	cnt = 0;
	list_for_each_entry(pe, &pep->sort_children, sort_node)
		cnt++;
	return cnt;
}

static void __of_platform_populate_scan_dump(struct of_pop_entry *pep, int level)
{
	struct of_pop_entry *pe;
	struct of_pop_ref_entry *re;
	struct of_pop_dep_entry *de;

	pr_debug("| %s %*s @%s (%d) - count=%d\n",
		pep->bus ? "BUS" : (pep->amba ? "AMB" : "PLT"),
		level * 4, "", of_node_full_name(pep->np), pep->refcnt,
		__count_children(pep));
	list_for_each_entry(re, &pep->refs, node)
		pr_debug("+     %*s @%s\n", level * 4, "", of_node_full_name(re->np));
	list_for_each_entry(de, &pep->deps, node)
		pr_debug(">     %*s @%s\n", level * 4, "", of_node_full_name(de->pe->np));

	list_for_each_entry(pe, &pep->children, node)
		__of_platform_populate_scan_dump(pe, level + 1);
}

static void __of_platform_populate_scan_sort_dump(struct of_pop_entry *pep, int level)
{
	struct of_pop_entry *pe;
	struct of_pop_dep_entry *de;

	pr_debug("* %s %*s @%s (%d) - sort-count=%d - id=%d\n",
		pep->bus ? "BUS" : (pep->amba ? "AMB" : "PLT"),
		level * 4, "", of_node_full_name(pep->np), pep->refcnt,
		__count_sort_children(pep), pep->id);
	list_for_each_entry(de, &pep->deps, node)
		pr_debug("%%     %*s @%s - id=%d\n", level * 4, "",
				of_node_full_name(de->pe->np), de->pe->id);

	list_for_each_entry(pe, &pep->sort_children, sort_node)
		__of_platform_populate_scan_sort_dump(pe, level + 1);
}

static void __of_platform_check_dep_order(struct of_pop_entry *pep, int level)
{
	struct of_pop_entry *pe;
	struct of_pop_dep_entry *de;

	list_for_each_entry(de, &pep->deps, node) {
		if (de->pe->id >= pep->id) {
			pr_info("%s: backwards reference @%s(%d) to @%s(%d)\n", __func__,
					of_node_full_name(pep->np), pep->id,
					of_node_full_name(de->pe->np), de->pe->id);
		}
	}

	list_for_each_entry(pe, &pep->sort_children, sort_node)
		__of_platform_check_dep_order(pe, level + 1);
}


static int __of_platform_populate_probe(struct of_pop_entry *pep,
			const struct of_device_id *matches,
			const struct of_dev_auxdata *lookup,
			struct device *parent,
			int level)
{
	struct of_pop_entry *pe;
	const char *bus_id = NULL;
	void *platform_data = NULL;
	const struct of_dev_auxdata *auxdata;
	struct platform_device *dev;
	int rc = 0;

	if (level > 0) {
		auxdata = of_dev_lookup(lookup, pep->np);
		if (auxdata) {
			bus_id = auxdata->name;
			platform_data = auxdata->platform_data;
		}

		if (pep->amba) {
			/* pr_info("of_amba_device_create(%s, %s, ..)\n",
					of_node_full_name(pep->np), bus_id ? bus_id : "<NULL>"); */
			of_amba_device_create(pep->np, bus_id, platform_data, parent);
			return 0;
		}

		/* pr_info("of_platform_device_create_pdata(%s, %s, ..)\n",
				of_node_full_name(pep->np), bus_id ? bus_id : "<NULL>"); */
		dev = of_platform_device_create_pdata(pep->np, bus_id, platform_data, parent);
		if (!dev || !of_match_node(matches, pep->np))
			return 0;
	}

	list_for_each_entry(pe, &pep->children, node) {
		rc = __of_platform_populate_probe(pe, matches, lookup, parent,
				level + 1);
		if (rc)
			break;
	}

	of_node_set_flag(pep->np, OF_POPULATED_BUS);

	return rc;
}


static void __of_platform_populate_free(struct of_pop_entry *pep)
{
	struct of_pop_entry *pe, *pen;
	struct of_pop_ref_entry *re, *ren;
	struct of_pop_dep_entry *de, *den;

	list_for_each_entry_safe(pe, pen, &pep->children, node) {
		list_del(&pe->node);
		__of_platform_populate_free(pe);
	}

	list_for_each_entry_safe(re, ren, &pep->refs, node) {
		list_del(&re->node);
		kfree(re);
	}
	list_for_each_entry_safe(de, den, &pep->deps, node) {
		list_del(&de->node);
		kfree(de);
	}

	kfree(pep);
}


static struct of_pop_entry *__of_platform_populate_scan(struct device_node *root,
			const struct of_device_id *matches)
{
	struct of_pop_entry *pe;
	struct device_node *np;

	/* if no local fixups are present do not proceed */
	np = of_find_node_by_path("/__local_fixups__");
	if (!np)
		return NULL;

	of_node_put(np);

	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (pe == NULL)
		return NULL;
	pe->parent = NULL;
	INIT_LIST_HEAD(&pe->node);
	INIT_LIST_HEAD(&pe->children);
	INIT_LIST_HEAD(&pe->refs);
	INIT_LIST_HEAD(&pe->deps);
	INIT_LIST_HEAD(&pe->sort_children);
	INIT_LIST_HEAD(&pe->sort_node);
	pe->np = root;

	__of_platform_populate_scan_internal(root, matches, pe, 0);
	return pe;
}


/**
 * of_platform_populate() - Populate platform_devices from device tree data
 * @root: parent of the first level to probe or NULL for the root of the tree
 * @matches: match table, NULL to use the default
 * @lookup: auxdata table for matching id and platform_data with device nodes
 * @parent: parent to hook devices from, NULL for toplevel
 *
 * Similar to of_platform_bus_probe(), this function walks the device tree
 * and creates devices from nodes.  It differs in that it follows the modern
 * convention of requiring all device nodes to have a 'compatible' property,
 * and it is suitable for creating devices which are children of the root
 * node (of_platform_bus_probe will only create children of the root which
 * are selected by the @matches argument).
 *
 * New board support should be using this function instead of
 * of_platform_bus_probe().
 *
 * Returns 0 on success, < 0 on failure.
 */
int of_platform_populate(struct device_node *root,
			const struct of_device_id *matches,
			const struct of_dev_auxdata *lookup,
			struct device *parent)
{
	struct of_pop_entry *pe;
	int rc = 0;

	root = root ? of_node_get(root) : of_find_node_by_path("/");
	if (!root)
		return -EINVAL;

	pe = __of_platform_populate_scan(root, matches);
	if (pe) {
		__of_platform_get_refs(pe, 0);
		__of_platform_make_deps(pe, 0);
		__of_platform_populate_scan_dump(pe, 0);
		__of_platform_reorder(pe, 0);
		__of_platform_assign_order(pe, 0, 0);
		__of_platform_populate_scan_sort_dump(pe, 0);
		__of_platform_check_dep_order(pe, 0);
		rc = __of_platform_populate_probe(pe, matches, lookup, parent, 0);
		__of_platform_populate_free(pe);
	} else {
		struct device_node *child;

		for_each_child_of_node(root, child) {
			rc = of_platform_bus_create(child, matches, lookup, parent, true);
			if (rc)
				break;
		}
	}

	of_node_set_flag(root, OF_POPULATED_BUS);

	of_node_put(root);
	return rc;
}
EXPORT_SYMBOL_GPL(of_platform_populate);

static int of_platform_device_destroy(struct device *dev, void *data)
{
	/* Do not touch devices not populated from the device tree */
	if (!dev->of_node || !of_node_check_flag(dev->of_node, OF_POPULATED))
		return 0;

	/* Recurse for any nodes that were treated as busses */
	if (of_node_check_flag(dev->of_node, OF_POPULATED_BUS))
		device_for_each_child(dev, NULL, of_platform_device_destroy);

	if (dev->bus == &platform_bus_type)
		platform_device_unregister(to_platform_device(dev));
#ifdef CONFIG_ARM_AMBA
	else if (dev->bus == &amba_bustype)
		amba_device_unregister(to_amba_device(dev));
#endif

	of_dma_deconfigure(dev);
	of_node_clear_flag(dev->of_node, OF_POPULATED);
	of_node_clear_flag(dev->of_node, OF_POPULATED_BUS);
	return 0;
}

/**
 * of_platform_depopulate() - Remove devices populated from device tree
 * @parent: device which children will be removed
 *
 * Complementary to of_platform_populate(), this function removes children
 * of the given device (and, recurrently, their children) that have been
 * created from their respective device tree nodes (and only those,
 * leaving others - eg. manually created - unharmed).
 *
 * Returns 0 when all children devices have been removed or
 * -EBUSY when some children remained.
 */
void of_platform_depopulate(struct device *parent)
{
	if (parent->of_node && of_node_check_flag(parent->of_node, OF_POPULATED_BUS)) {
		device_for_each_child(parent, NULL, of_platform_device_destroy);
		of_node_clear_flag(parent->of_node, OF_POPULATED_BUS);
	}
}
EXPORT_SYMBOL_GPL(of_platform_depopulate);

#ifdef CONFIG_OF_DYNAMIC
static int of_platform_notify(struct notifier_block *nb,
				unsigned long action, void *arg)
{
	struct of_reconfig_data *rd = arg;
	struct platform_device *pdev_parent, *pdev;
	bool children_left;

	switch (of_reconfig_get_state_change(action, rd)) {
	case OF_RECONFIG_CHANGE_ADD:
		/* verify that the parent is a bus */
		if (!of_node_check_flag(rd->dn->parent, OF_POPULATED_BUS))
			return NOTIFY_OK;	/* not for us */

		/* already populated? (driver using of_populate manually) */
		if (of_node_check_flag(rd->dn, OF_POPULATED))
			return NOTIFY_OK;

		/* pdev_parent may be NULL when no bus platform device */
		pdev_parent = of_find_device_by_node(rd->dn->parent);
		pdev = of_platform_device_create(rd->dn, NULL,
				pdev_parent ? &pdev_parent->dev : NULL);
		of_dev_put(pdev_parent);

		if (pdev == NULL) {
			pr_err("%s: failed to create for '%s'\n",
					__func__, rd->dn->full_name);
			/* of_platform_device_create tosses the error code */
			return notifier_from_errno(-EINVAL);
		}
		break;

	case OF_RECONFIG_CHANGE_REMOVE:

		/* already depopulated? */
		if (!of_node_check_flag(rd->dn, OF_POPULATED))
			return NOTIFY_OK;

		/* find our device by node */
		pdev = of_find_device_by_node(rd->dn);
		if (pdev == NULL)
			return NOTIFY_OK;	/* no? not meant for us */

		/* unregister takes one ref away */
		of_platform_device_destroy(&pdev->dev, &children_left);

		/* and put the reference of the find */
		of_dev_put(pdev);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block platform_of_notifier = {
	.notifier_call = of_platform_notify,
};

void of_platform_register_reconfig_notifier(void)
{
	WARN_ON(of_reconfig_notifier_register(&platform_of_notifier));
}
#endif /* CONFIG_OF_DYNAMIC */

#endif /* CONFIG_OF_ADDRESS */
