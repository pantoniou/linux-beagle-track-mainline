/*
 * PCI <-> OF mapping helpers
 *
 * Copyright 2011 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include "pci.h"

void pci_set_of_node(struct pci_dev *dev)
{
	if (!dev->bus->dev.of_node)
		return;
	dev->dev.of_node = of_pci_find_child_device(dev->bus->dev.of_node,
						    dev->devfn);
}

void pci_release_of_node(struct pci_dev *dev)
{
	of_node_put(dev->dev.of_node);
	dev->dev.of_node = NULL;
}

void pci_set_bus_of_node(struct pci_bus *bus)
{
	if (bus->self == NULL)
		bus->dev.of_node = pcibios_get_phb_of_node(bus);
	else
		bus->dev.of_node = of_node_get(bus->self->dev.of_node);
}

void pci_release_bus_of_node(struct pci_bus *bus)
{
	of_node_put(bus->dev.of_node);
	bus->dev.of_node = NULL;
}

struct device_node * __weak pcibios_get_phb_of_node(struct pci_bus *bus)
{
	pr_info("%s: check bus %s\n", __func__, dev_name(&bus->dev));

	/* This should only be called for PHBs */
	if (WARN_ON(bus->self || bus->parent))
		return NULL;

	/* Look for a node pointer in either the intermediary device we
	 * create above the root bus or it's own parent. Normally only
	 * the later is populated.
	 */
	if (bus->bridge->of_node) {
		pr_info("%s: bus %s bus->bridge->of_node != NULL\n", __func__, dev_name(&bus->dev));
		return of_node_get(bus->bridge->of_node);
	}
	if (bus->bridge->parent && bus->bridge->parent->of_node) {
		pr_info("%s: bus %s bus->bridge->parent && bus->bridge->parent->of_node\n", __func__, dev_name(&bus->dev));
		return of_node_get(bus->bridge->parent->of_node);
	}
	pr_info("%s: bus %s NULL\n", __func__, dev_name(&bus->dev));
	return NULL;
}

#if defined(CONFIG_OF_DYNAMIC)

/* PCI device names are of the form XXXX-XX-XX.X */
#define OF_PCI_DEV_NAME_MAX	13

static char *of_pci_dev_name(struct pci_dev *pdev, char *buf, int bufsz)
{
	if (pdev == NULL || buf == NULL || bufsz < OF_PCI_DEV_NAME_MAX)
		return NULL;

	snprintf(buf, bufsz - 1, "%04x-%02x-%02x.%d",
			pci_domain_nr(pdev->bus), pdev->bus->number,
			PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
	buf[bufsz - 1] = '\0';

	return buf;
}

int of_pci_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	pr_info("%s: %s dev\n", __func__, dev_name(&bridge->dev));
	return 0;
}

void of_pci_add_bus(struct pci_bus *bus)
{
	struct device *dev = &bus->dev;

	pr_info("%s: %s\n", __func__, dev_name(dev));
}

void of_pci_remove_bus(struct pci_bus *bus)
{
	struct device *dev = &bus->dev;

	pr_info("%s: %s\n", __func__, dev_name(dev));
}

void of_pci_add_device(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dn;
#ifdef CONFIG_OF_DYNAMIC
	struct device_node *parent_dn;
	char *full_name;
	char name[OF_PCI_DEV_NAME_MAX + 1];
	int rc;
	char buf[32];	/* enough for pciclass,XXXX */
	struct property propbuf, *prop;
	u32 tmp32;
#endif

	dn = dev->of_node;
	if (dn != NULL) {
		pr_info("%s: %s of_node!=NULL on node '%s'\n", __func__, dev_name(dev), dn->full_name);
		return;
	}
#ifdef CONFIG_OF_DYNAMIC
	if (pdev->bus == NULL) {
		pr_info("%s: %s NULL\n", __func__, dev_name(dev));
		return;
	}
	parent_dn = pdev->bus->dev.of_node;
	/* bus not on of tree; going to add it (if autocomplete is set) */
	if (parent_dn == NULL) {
		pr_info("%s: %s has no parent with of_node != NULL\n",
				__func__, dev_name(dev));
		return;
	}
	pr_info("%s: %s going to create on bus %s '%s'\n", __func__, dev_name(dev),
			dev_name(&pdev->bus->dev), parent_dn->full_name);

	of_pci_dev_name(pdev, name, sizeof(name));

	full_name = kasprintf(GFP_KERNEL, "%s/pci-%s",
			parent_dn->full_name, name);
	if (full_name == NULL) {
		pr_err("%s: %s failed to allocate name\n",
				dev_name(dev), __func__);
		return;
	}
	dn = __of_create_empty_node(name, "pci", full_name, 0, GFP_KERNEL);
	kfree(full_name);
	if (dn == NULL) {
		pr_err("%s: %s failed to create node\n",
				dev_name(dev), __func__);
		return;
	}

	dn->parent = parent_dn;
	rc = of_attach_node(dn);
	if (rc != 0) {
		pr_err("%s: %s failed to attach device node\n",
				dev_name(dev), __func__);
		return;
	}

	snprintf(buf, sizeof(buf) - 1, "pciclass,%04x", (pdev->class >> 8) & 0xffffff);
	buf[sizeof(buf) - 1] = '\0';
	memset(&propbuf, 0, sizeof(propbuf));
	propbuf.name = "compatible";
	propbuf.length = strlen(buf) + 1;
	propbuf.value = buf;
	prop = __of_copy_property(&propbuf, GFP_KERNEL);
	BUG_ON(prop == NULL);
	rc = of_add_property(dn, prop);
	BUG_ON(rc != 0);

	tmp32 = __cpu_to_be32(pdev->vendor);
	memset(&propbuf, 0, sizeof(propbuf));
	propbuf.name = "vendor-id";
	propbuf.length = sizeof(tmp32);
	propbuf.value = &tmp32;
	prop = __of_copy_property(&propbuf, GFP_KERNEL);
	BUG_ON(prop == NULL);
	rc = of_add_property(dn, prop);
	BUG_ON(rc != 0);

	tmp32 = __cpu_to_be32(pdev->device);
	memset(&propbuf, 0, sizeof(propbuf));
	propbuf.name = "device-id";
	propbuf.length = sizeof(tmp32);
	propbuf.value = &tmp32;
	prop = __of_copy_property(&propbuf, GFP_KERNEL);
	BUG_ON(prop == NULL);
	rc = of_add_property(dn, prop);
	BUG_ON(rc != 0);

	memset(&propbuf, 0, sizeof(propbuf));
	propbuf.name = "device_type";
	propbuf.length = strlen("pci") + 1;
	propbuf.value = "pci";
	prop = __of_copy_property(&propbuf, GFP_KERNEL);
	BUG_ON(prop == NULL);
	rc = of_add_property(dn, prop);
	BUG_ON(rc != 0);

	dev->of_node = dn;
#endif
}

void of_pci_release_device(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;

	pr_info("%s: %s\n", __func__, dev_name(dev));
}

#endif
