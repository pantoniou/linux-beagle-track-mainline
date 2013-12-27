/*
 * pdevtest.c
 *
 * Tester of platform device's operation.
 *
 * Copyright (C) 2013, Pantelis Antoniou <panto@antoniou-consulting.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/sysfs.h>

static ssize_t
action_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct platform_device *target, *parent;
	struct device_node *dn, *dnp;
	unsigned long phandle;
	int ret;

	ret = kstrtoul(buf, 16, &phandle);
	if (ret != 0)
		return ret;

	dn = of_find_node_by_phandle(phandle);
	if (dn == NULL) {
		dev_err(dev, "No node with phandle 0x%lx\n", phandle);
		return -EINVAL;
	}

	dnp = dn->parent;
	if (dnp == NULL) {
		dev_err(dev, "Can't work with root node\n");
		return -EINVAL;
	}

	parent = of_find_device_by_node(dnp);
	if (parent == NULL) {
		dev_err(dev, "No parent device\n");
		return -EINVAL;
	}

	target = of_find_device_by_node(dn);
	if (target == NULL) {
		dev_info(dev, "Creating device for target node %s\n",
				dn->full_name);
		target = of_platform_device_create(dn, NULL, &parent->dev);
		if (target == NULL) {
			dev_err(dev, "Failed to create platform device "
					"for '%s'\n", 
					dn->full_name);
			return -ENODEV;
		}
	} else {
		dev_info(dev, "Destroying device for target node %s\n",
				dn->full_name);

		platform_device_unregister(target);
	}

	return size;
}

DEVICE_ATTR(action, S_IWUSR, NULL, action_store);

static int pdevtest_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	/* OF only */
	if (dev->of_node == NULL) {
		dev_err(dev, "Unsupported platform (not OF)!\n");
		return -ENODEV;
	}

	ret = device_create_file(dev, &dev_attr_action);
	if (ret != 0) {
		dev_err(dev, "Failed to create device attribute file\n");
		return ret;
	}

	return 0;
}

static int pdevtest_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	device_remove_file(dev, &dev_attr_action);
	return 0;
}

static const struct of_device_id pdevtest_of_match[] = {
	{
		.compatible = "pdevtest",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, pdevtest_of_match);

static struct platform_driver pdevtest_driver = {
	.probe		= pdevtest_probe,
	.remove		= pdevtest_remove,
	.driver		= {
		.name	= "pdevtest",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(pdevtest_of_match),
	},
};

module_platform_driver(pdevtest_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pantelis Antoniou <panto@antoniou-consulting.com>");
MODULE_DESCRIPTION("Platform device tester");
