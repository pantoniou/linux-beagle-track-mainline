/*
 * Self tests for device tree subsystem
 */

#define pr_fmt(fmt) "### dt-test ### " fmt

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>

static struct selftest_results {
	int passed;
	int failed;
} selftest_results;

#define selftest(result, fmt, ...) { \
	if (!(result)) { \
		selftest_results.failed++; \
		pr_err("FAIL %s():%i " fmt, __func__, __LINE__, ##__VA_ARGS__); \
	} else { \
		selftest_results.passed++; \
		pr_debug("pass %s():%i\n", __func__, __LINE__); \
	} \
}

static void __init of_selftest_dynamic(void)
{
	struct device_node *np;
	struct property *prop;

	np = of_find_node_by_path("/testcase-data");
	if (!np) {
		pr_err("missing testcase data\n");
		return;
	}

	/* Array of 4 properties for the purpose of testing */
	prop = kzalloc(sizeof(*prop) * 4, GFP_KERNEL);
	if (!prop) {
		selftest(0, "kzalloc() failed\n");
		return;
	}

	/* Add a new property - should pass*/
	prop->name = "new-property";
	prop->value = "new-property-data";
	prop->length = strlen(prop->value);
	selftest(of_add_property(np, prop) == 0, "Adding a new property failed\n");

	/* Try to add an existing property - should fail */
	prop++;
	prop->name = "new-property";
	prop->value = "new-property-data-should-fail";
	prop->length = strlen(prop->value);
	selftest(of_add_property(np, prop) != 0,
		 "Adding an existing property should have failed\n");

	/* Try to modify an existing property - should pass */
	prop->value = "modify-property-data-should-pass";
	prop->length = strlen(prop->value);
	selftest(of_update_property(np, prop) == 0,
		 "Updating an existing property should have passed\n");

	/* Try to modify non-existent property - should pass*/
	prop++;
	prop->name = "modify-property";
	prop->value = "modify-missing-property-data-should-pass";
	prop->length = strlen(prop->value);
	selftest(of_update_property(np, prop) == 0,
		 "Updating a missing property should have passed\n");

	/* Remove property - should pass */
	selftest(of_remove_property(np, prop) == 0,
		 "Removing a property should have passed\n");

	/* Adding very large property - should pass */
	prop++;
	prop->name = "large-property-PAGE_SIZEx8";
	prop->length = PAGE_SIZE * 8;
	prop->value = kzalloc(prop->length, GFP_KERNEL);
	selftest(prop->value != NULL, "Unable to allocate large buffer\n");
	if (prop->value)
		selftest(of_add_property(np, prop) == 0,
			 "Adding a large property should have passed\n");
}

static void __init of_selftest_parse_phandle_with_args(void)
{
	struct device_node *np;
	struct of_phandle_args args;
	int i, rc;

	np = of_find_node_by_path("/testcase-data/phandle-tests/consumer-a");
	if (!np) {
		pr_err("missing testcase data\n");
		return;
	}

	rc = of_count_phandle_with_args(np, "phandle-list", "#phandle-cells");
	selftest(rc == 7, "of_count_phandle_with_args() returned %i, expected 7\n", rc);

	for (i = 0; i < 8; i++) {
		bool passed = true;
		rc = of_parse_phandle_with_args(np, "phandle-list",
						"#phandle-cells", i, &args);

		/* Test the values from tests-phandle.dtsi */
		switch (i) {
		case 0:
			passed &= !rc;
			passed &= (args.args_count == 1);
			passed &= (args.args[0] == (i + 1));
			break;
		case 1:
			passed &= !rc;
			passed &= (args.args_count == 2);
			passed &= (args.args[0] == (i + 1));
			passed &= (args.args[1] == 0);
			break;
		case 2:
			passed &= (rc == -ENOENT);
			break;
		case 3:
			passed &= !rc;
			passed &= (args.args_count == 3);
			passed &= (args.args[0] == (i + 1));
			passed &= (args.args[1] == 4);
			passed &= (args.args[2] == 3);
			break;
		case 4:
			passed &= !rc;
			passed &= (args.args_count == 2);
			passed &= (args.args[0] == (i + 1));
			passed &= (args.args[1] == 100);
			break;
		case 5:
			passed &= !rc;
			passed &= (args.args_count == 0);
			break;
		case 6:
			passed &= !rc;
			passed &= (args.args_count == 1);
			passed &= (args.args[0] == (i + 1));
			break;
		case 7:
			passed &= (rc == -ENOENT);
			break;
		default:
			passed = false;
		}

		selftest(passed, "index %i - data error on node %s rc=%i\n",
			 i, args.np->full_name, rc);
	}

	/* Check for missing list property */
	rc = of_parse_phandle_with_args(np, "phandle-list-missing",
					"#phandle-cells", 0, &args);
	selftest(rc == -ENOENT, "expected:%i got:%i\n", -ENOENT, rc);
	rc = of_count_phandle_with_args(np, "phandle-list-missing",
					"#phandle-cells");
	selftest(rc == -ENOENT, "expected:%i got:%i\n", -ENOENT, rc);

	/* Check for missing cells property */
	rc = of_parse_phandle_with_args(np, "phandle-list",
					"#phandle-cells-missing", 0, &args);
	selftest(rc == -EINVAL, "expected:%i got:%i\n", -EINVAL, rc);
	rc = of_count_phandle_with_args(np, "phandle-list",
					"#phandle-cells-missing");
	selftest(rc == -EINVAL, "expected:%i got:%i\n", -EINVAL, rc);

	/* Check for bad phandle in list */
	rc = of_parse_phandle_with_args(np, "phandle-list-bad-phandle",
					"#phandle-cells", 0, &args);
	selftest(rc == -EINVAL, "expected:%i got:%i\n", -EINVAL, rc);
	rc = of_count_phandle_with_args(np, "phandle-list-bad-phandle",
					"#phandle-cells");
	selftest(rc == -EINVAL, "expected:%i got:%i\n", -EINVAL, rc);

	/* Check for incorrectly formed argument list */
	rc = of_parse_phandle_with_args(np, "phandle-list-bad-args",
					"#phandle-cells", 1, &args);
	selftest(rc == -EINVAL, "expected:%i got:%i\n", -EINVAL, rc);
	rc = of_count_phandle_with_args(np, "phandle-list-bad-args",
					"#phandle-cells");
	selftest(rc == -EINVAL, "expected:%i got:%i\n", -EINVAL, rc);
}

static void __init of_selftest_property_match_string(void)
{
	struct device_node *np;
	int rc;

	np = of_find_node_by_path("/testcase-data/phandle-tests/consumer-a");
	if (!np) {
		pr_err("No testcase data in device tree\n");
		return;
	}

	rc = of_property_match_string(np, "phandle-list-names", "first");
	selftest(rc == 0, "first expected:0 got:%i\n", rc);
	rc = of_property_match_string(np, "phandle-list-names", "second");
	selftest(rc == 1, "second expected:0 got:%i\n", rc);
	rc = of_property_match_string(np, "phandle-list-names", "third");
	selftest(rc == 2, "third expected:0 got:%i\n", rc);
	rc = of_property_match_string(np, "phandle-list-names", "fourth");
	selftest(rc == -ENODATA, "unmatched string; rc=%i", rc);
	rc = of_property_match_string(np, "missing-property", "blah");
	selftest(rc == -EINVAL, "missing property; rc=%i", rc);
	rc = of_property_match_string(np, "empty-property", "blah");
	selftest(rc == -ENODATA, "empty property; rc=%i", rc);
	rc = of_property_match_string(np, "unterminated-string", "blah");
	selftest(rc == -EILSEQ, "unterminated string; rc=%i", rc);
}

static void __init of_selftest_parse_interrupts(void)
{
	struct device_node *np;
	struct of_phandle_args args;
	int i, rc;

	np = of_find_node_by_path("/testcase-data/interrupts/interrupts0");
	if (!np) {
		pr_err("missing testcase data\n");
		return;
	}

	for (i = 0; i < 4; i++) {
		bool passed = true;
		args.args_count = 0;
		rc = of_irq_parse_one(np, i, &args);

		passed &= !rc;
		passed &= (args.args_count == 1);
		passed &= (args.args[0] == (i + 1));

		selftest(passed, "index %i - data error on node %s rc=%i\n",
			 i, args.np->full_name, rc);
	}
	of_node_put(np);

	np = of_find_node_by_path("/testcase-data/interrupts/interrupts1");
	if (!np) {
		pr_err("missing testcase data\n");
		return;
	}

	for (i = 0; i < 4; i++) {
		bool passed = true;
		args.args_count = 0;
		rc = of_irq_parse_one(np, i, &args);

		/* Test the values from tests-phandle.dtsi */
		switch (i) {
		case 0:
			passed &= !rc;
			passed &= (args.args_count == 1);
			passed &= (args.args[0] == 9);
			break;
		case 1:
			passed &= !rc;
			passed &= (args.args_count == 3);
			passed &= (args.args[0] == 10);
			passed &= (args.args[1] == 11);
			passed &= (args.args[2] == 12);
			break;
		case 2:
			passed &= !rc;
			passed &= (args.args_count == 2);
			passed &= (args.args[0] == 13);
			passed &= (args.args[1] == 14);
			break;
		case 3:
			passed &= !rc;
			passed &= (args.args_count == 2);
			passed &= (args.args[0] == 15);
			passed &= (args.args[1] == 16);
			break;
		default:
			passed = false;
		}
		selftest(passed, "index %i - data error on node %s rc=%i\n",
			 i, args.np->full_name, rc);
	}
	of_node_put(np);
}

static void __init of_selftest_parse_interrupts_extended(void)
{
	struct device_node *np;
	struct of_phandle_args args;
	int i, rc;

	np = of_find_node_by_path("/testcase-data/interrupts/interrupts-extended0");
	if (!np) {
		pr_err("missing testcase data\n");
		return;
	}

	for (i = 0; i < 7; i++) {
		bool passed = true;
		rc = of_irq_parse_one(np, i, &args);

		/* Test the values from tests-phandle.dtsi */
		switch (i) {
		case 0:
			passed &= !rc;
			passed &= (args.args_count == 1);
			passed &= (args.args[0] == 1);
			break;
		case 1:
			passed &= !rc;
			passed &= (args.args_count == 3);
			passed &= (args.args[0] == 2);
			passed &= (args.args[1] == 3);
			passed &= (args.args[2] == 4);
			break;
		case 2:
			passed &= !rc;
			passed &= (args.args_count == 2);
			passed &= (args.args[0] == 5);
			passed &= (args.args[1] == 6);
			break;
		case 3:
			passed &= !rc;
			passed &= (args.args_count == 1);
			passed &= (args.args[0] == 9);
			break;
		case 4:
			passed &= !rc;
			passed &= (args.args_count == 3);
			passed &= (args.args[0] == 10);
			passed &= (args.args[1] == 11);
			passed &= (args.args[2] == 12);
			break;
		case 5:
			passed &= !rc;
			passed &= (args.args_count == 2);
			passed &= (args.args[0] == 13);
			passed &= (args.args[1] == 14);
			break;
		case 6:
			passed &= !rc;
			passed &= (args.args_count == 1);
			passed &= (args.args[0] == 15);
			break;
		default:
			passed = false;
		}

		selftest(passed, "index %i - data error on node %s rc=%i\n",
			 i, args.np->full_name, rc);
	}
	of_node_put(np);
}

static struct of_device_id match_node_table[] = {
	{ .data = "A", .name = "name0", }, /* Name alone is lowest priority */
	{ .data = "B", .type = "type1", }, /* followed by type alone */

	{ .data = "Ca", .name = "name2", .type = "type1", }, /* followed by both together */
	{ .data = "Cb", .name = "name2", }, /* Only match when type doesn't match */
	{ .data = "Cc", .name = "name2", .type = "type2", },

	{ .data = "E", .compatible = "compat3" },
	{ .data = "G", .compatible = "compat2", },
	{ .data = "H", .compatible = "compat2", .name = "name5", },
	{ .data = "I", .compatible = "compat2", .type = "type1", },
	{ .data = "J", .compatible = "compat2", .type = "type1", .name = "name8", },
	{ .data = "K", .compatible = "compat2", .name = "name9", },
	{}
};

static struct {
	const char *path;
	const char *data;
} match_node_tests[] = {
	{ .path = "/testcase-data/match-node/name0", .data = "A", },
	{ .path = "/testcase-data/match-node/name1", .data = "B", },
	{ .path = "/testcase-data/match-node/a/name2", .data = "Ca", },
	{ .path = "/testcase-data/match-node/b/name2", .data = "Cb", },
	{ .path = "/testcase-data/match-node/c/name2", .data = "Cc", },
	{ .path = "/testcase-data/match-node/name3", .data = "E", },
	{ .path = "/testcase-data/match-node/name4", .data = "G", },
	{ .path = "/testcase-data/match-node/name5", .data = "H", },
	{ .path = "/testcase-data/match-node/name6", .data = "G", },
	{ .path = "/testcase-data/match-node/name7", .data = "I", },
	{ .path = "/testcase-data/match-node/name8", .data = "J", },
	{ .path = "/testcase-data/match-node/name9", .data = "K", },
};

static void __init of_selftest_match_node(void)
{
	struct device_node *np;
	const struct of_device_id *match;
	int i;

	for (i = 0; i < ARRAY_SIZE(match_node_tests); i++) {
		np = of_find_node_by_path(match_node_tests[i].path);
		if (!np) {
			selftest(0, "missing testcase node %s\n",
				match_node_tests[i].path);
			continue;
		}

		match = of_match_node(match_node_table, np);
		if (!match) {
			selftest(0, "%s didn't match anything\n",
				match_node_tests[i].path);
			continue;
		}

		if (strcmp(match->data, match_node_tests[i].data) != 0) {
			selftest(0, "%s got wrong match. expected %s, got %s\n",
				match_node_tests[i].path, match_node_tests[i].data,
				(const char *)match->data);
			continue;
		}
		selftest(1, "passed");
	}
}

#ifdef CONFIG_OF_OVERLAY

static int selftest_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	if (np == NULL) {
		dev_err(dev, "No OF data for device\n");
		return -EINVAL;

	}

	dev_dbg(dev, "%s for node @%s\n", __func__, np->full_name);
	return 0;
}

static int selftest_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	dev_dbg(dev, "%s for node @%s\n", __func__, np->full_name);
	return 0;
}

static struct of_device_id selftest_match[] = {
	{ .compatible = "selftest", },
	{},
};
MODULE_DEVICE_TABLE(of, altera_jtaguart_match);

static struct platform_driver selftest_driver = {
	.probe			= selftest_probe,
	.remove			= selftest_remove,
	.driver = {
		.name		= "selftest",
		.owner		= THIS_MODULE,
		.of_match_table	= of_match_ptr(selftest_match),
	},
};

/* get the platform device instantiated at the path */
static struct platform_device *of_path_to_platform_device(const char *path)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_find_node_by_path(path);
	if (np == NULL)
		return NULL;

	pdev = of_find_device_by_node(np);
	of_node_put(np);

	return pdev;
}

/* find out if a platform device exists at that path */
static int of_path_platform_device_exists(const char *path)
{
	struct platform_device *pdev;

	pdev = of_path_to_platform_device(path);
	platform_device_put(pdev);
	return pdev != NULL;
}

static const char *selftest_path(int nr)
{
	static char buf[256];

	snprintf(buf, sizeof(buf) - 1,
		"/testcase-data/overlay-node/test-bus/test-selftest%d", nr);
	buf[sizeof(buf) - 1] = '\0';

	return buf;
}

static const char *overlay_path(int nr)
{
	static char buf[256];

	snprintf(buf, sizeof(buf) - 1,
		"/testcase-data/overlay%d", nr);
	buf[sizeof(buf) - 1] = '\0';

	return buf;
}

static const char *bus_path = "/testcase-data/overlay-node/test-bus";

static int of_selftest_apply_overlay(int selftest_nr, int overlay_nr,
		int *ovcount_arg, struct of_overlay_info **ovinfo_arg)
{
	struct device_node *np = NULL;
	int ret, ovcount_val, *ovcount;
	struct of_overlay_info *ovinfo_val, **ovinfo;

	if (ovcount_arg == NULL || ovinfo_arg == NULL) {
		ovcount = &ovcount_val;
		ovinfo = &ovinfo_val;
	} else {
		ovcount = ovcount_arg;
		ovinfo = ovinfo_arg;
	}

	*ovcount = 0;
	*ovinfo = NULL;

	np = of_find_node_by_path(overlay_path(overlay_nr));
	if (np == NULL) {
		selftest(0, "could not find overlay node @\"%s\"\n",
				overlay_path(overlay_nr));
		ret = -EINVAL;
		goto out;
	}

	ret = of_build_overlay_info(np, ovcount, ovinfo);
	if (ret != 0) {
		selftest(0, "could not build overlay from \"%s\"\n",
				overlay_path(overlay_nr));
		goto out;
	}

	ret = of_overlay(*ovcount, *ovinfo);
	if (ret != 0) {
		selftest(0, "could not apply overlay from \"%s\"\n",
				overlay_path(overlay_nr));
		goto out;
	}

	ret = 0;

out:
	/* free if no argument passed */
	if (ovinfo == &ovinfo_val)
		of_free_overlay_info(*ovcount, *ovinfo);
	of_node_put(np);
	return ret;
}

/* apply an overlay while checking before and after states */
static int of_selftest_apply_overlay_check(int overlay_nr, int selftest_nr,
		int before, int after)
{
	int ret;

	/* selftest device must not be in before state */
	if (of_path_platform_device_exists(selftest_path(selftest_nr))
			!= before) {
		selftest(0, "overlay @\"%s\" with device @\"%s\" %s\n",
				overlay_path(overlay_nr),
				selftest_path(selftest_nr),
				!before ? "enabled" : "disabled");
		return -EINVAL;
	}

	ret = of_selftest_apply_overlay(overlay_nr, selftest_nr, NULL, NULL);
	if (ret != 0) {
		/* of_selftest_apply_overlay already called selftest() */
		return ret;
	}

	/* selftest device must be to set to after state */
	if (of_path_platform_device_exists(selftest_path(selftest_nr))
			!= after) {
		selftest(0, "overlay @\"%s\" failed to create @\"%s\" %s\n",
				overlay_path(overlay_nr),
				selftest_path(selftest_nr),
				!after ? "enabled" : "disabled");
		return -EINVAL;
	}

	return 0;
}

/* apply an overlay and then revert it while checking before, after states */
static int of_selftest_apply_revert_overlay_check(int overlay_nr,
		int selftest_nr, int before, int after)
{
	int ret, ovcount;
	struct of_overlay_info *ovinfo;

	/* selftest device must be in before state */
	if (of_path_platform_device_exists(selftest_path(selftest_nr))
			!= before) {
		selftest(0, "overlay @\"%s\" with device @\"%s\" %s\n",
				overlay_path(overlay_nr),
				selftest_path(selftest_nr),
				!before ? "enabled" : "disabled");
		return -EINVAL;
	}

	/* apply the overlay */
	ret = of_selftest_apply_overlay(overlay_nr, selftest_nr,
			&ovcount, &ovinfo);
	if (ret != 0) {
		/* of_selftest_apply_overlay already called selftest() */
		return ret;
	}

	/* selftest device must be in after state */
	if (of_path_platform_device_exists(selftest_path(selftest_nr))
			!= after) {
		selftest(0, "overlay @\"%s\" failed to create @\"%s\" %s\n",
				overlay_path(overlay_nr),
				selftest_path(selftest_nr),
				!after ? "enabled" : "disabled");
		return -EINVAL;
	}

	ret = of_overlay_revert(ovcount, ovinfo);
	if (ret != 0) {
		selftest(0, "overlay @\"%s\" failed to revert @\"%s\"\n",
				overlay_path(overlay_nr),
				selftest_path(selftest_nr));
		return ret;
	}

	of_free_overlay_info(ovcount, ovinfo);

	/* selftest device must be again in before state */
	if (of_path_platform_device_exists(selftest_path(selftest_nr))
			!= before) {
		selftest(0, "overlay @\"%s\" with device @\"%s\" %s\n",
				overlay_path(overlay_nr),
				selftest_path(selftest_nr),
				!before ? "enabled" : "disabled");
		return -EINVAL;
	}

	return 0;
}

/* test activation of device */
static void of_selftest_overlay_0(void)
{
	int ret;

	/* device should enable */
	ret = of_selftest_apply_overlay_check(0, 0, 0, 1);
	if (ret != 0)
		return;

	selftest(1, "overlay test %d passed\n", 0);
}

/* test deactivation of device */
static void of_selftest_overlay_1(void)
{
	int ret;

	/* device should disable */
	ret = of_selftest_apply_overlay_check(1, 1, 1, 0);
	if (ret != 0)
		return;

	selftest(1, "overlay test %d passed\n", 1);
}

/* test activation of device */
static void of_selftest_overlay_2(void)
{
	int ret;

	/* device should enable */
	ret = of_selftest_apply_overlay_check(2, 2, 0, 1);
	if (ret != 0)
		return;

	selftest(1, "overlay test %d passed\n", 2);
}

/* test deactivation of device */
static void of_selftest_overlay_3(void)
{
	int ret;

	/* device should disable */
	ret = of_selftest_apply_overlay_check(3, 3, 1, 0);
	if (ret != 0)
		return;

	selftest(1, "overlay test %d passed\n", 3);
}

/* test activation of a full device node */
static void of_selftest_overlay_4(void)
{
	int ret;

	/* device should disable */
	ret = of_selftest_apply_overlay_check(4, 4, 0, 1);
	if (ret != 0)
		return;

	selftest(1, "overlay test %d passed\n", 4);
}

/* test overlay apply/revert sequence */
static void of_selftest_overlay_5(void)
{
	int ret;

	/* device should disable */
	ret = of_selftest_apply_revert_overlay_check(5, 5, 0, 1);
	if (ret != 0)
		return;

	selftest(1, "overlay test %d passed\n", 5);
}

static void __init of_selftest_overlay(void)
{
	struct device_node *bus_np = NULL;
	int ret;

	ret = platform_driver_register(&selftest_driver);
	if (ret != 0) {
		selftest(0, "could not register selftest driver\n");
		goto out;
	}

	bus_np = of_find_node_by_path(bus_path);
	if (bus_np == NULL) {
		selftest(0, "could not find bus_path \"%s\"\n", bus_path);
		goto out;
	}

	ret = of_platform_populate(bus_np, of_default_bus_match_table,
			NULL, NULL);
	if (ret != 0) {
		selftest(0, "could not populate bus @ \"%s\"\n", bus_path);
		goto out;
	}

	if (!of_path_platform_device_exists(selftest_path(100))) {
		selftest(0, "could not find selftest0 @ \"%s\"\n", selftest_path(100));
		goto out;
	}

	if (of_path_platform_device_exists(selftest_path(101))) {
		selftest(0, "selftest1 @ \"%s\" should not exist\n", selftest_path(101));
		goto out;
	}

	selftest(1, "basic infrastructure of overlays passed");

	/* tests in sequence */
	of_selftest_overlay_0();
	of_selftest_overlay_1();
	of_selftest_overlay_2();
	of_selftest_overlay_3();
	of_selftest_overlay_4();
	of_selftest_overlay_5();

out:
	of_node_put(bus_np);
}

#else
static inline void __init of_selftest_overlay(void) { }
#endif

static int __init of_selftest(void)
{
	struct device_node *np;

	np = of_find_node_by_path("/testcase-data/phandle-tests/consumer-a");
	if (!np) {
		pr_info("No testcase data in device tree; not running tests\n");
		return 0;
	}
	of_node_put(np);

	pr_info("start of selftest - you will see error messages\n");
	of_selftest_dynamic();
	of_selftest_parse_phandle_with_args();
	of_selftest_property_match_string();
	of_selftest_parse_interrupts();
	of_selftest_parse_interrupts_extended();
	of_selftest_match_node();
	of_selftest_overlay();
	pr_info("end of selftest - %i passed, %i failed\n",
		selftest_results.passed, selftest_results.failed);
	return 0;
}
late_initcall(of_selftest);
