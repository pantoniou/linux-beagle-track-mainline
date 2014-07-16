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
#include <linux/of_platform.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/device.h>

#include "of_private.h"

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

static void __init of_selftest_find_node_by_name(void)
{
	struct device_node *np;

	np = of_find_node_by_path("/testcase-data");
	selftest(np && !strcmp("/testcase-data", np->full_name),
		"find /testcase-data failed\n");
	of_node_put(np);

	/* Test if trailing '/' works */
	np = of_find_node_by_path("/testcase-data/");
	selftest(!np, "trailing '/' on /testcase-data/ should fail\n");

	np = of_find_node_by_path("/testcase-data/phandle-tests/consumer-a");
	selftest(np && !strcmp("/testcase-data/phandle-tests/consumer-a", np->full_name),
		"find /testcase-data/phandle-tests/consumer-a failed\n");
	of_node_put(np);

	np = of_find_node_by_path("testcase-alias");
	selftest(np && !strcmp("/testcase-data", np->full_name),
		"find testcase-alias failed\n");
	of_node_put(np);

	/* Test if trailing '/' works on aliases */
	np = of_find_node_by_path("testcase-alias/");
	selftest(!np, "trailing '/' on testcase-alias/ should fail\n");

	np = of_find_node_by_path("testcase-alias/phandle-tests/consumer-a");
	selftest(np && !strcmp("/testcase-data/phandle-tests/consumer-a", np->full_name),
		"find testcase-alias/phandle-tests/consumer-a failed\n");
	of_node_put(np);

	np = of_find_node_by_path("/testcase-data/missing-path");
	selftest(!np, "non-existent path returned node %s\n", np->full_name);
	of_node_put(np);

	np = of_find_node_by_path("missing-alias");
	selftest(!np, "non-existent alias returned node %s\n", np->full_name);
	of_node_put(np);

	np = of_find_node_by_path("testcase-alias/missing-path");
	selftest(!np, "non-existent alias with relative path returned node %s\n", np->full_name);
	of_node_put(np);
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

#define propcmp(p1, p2) (((p1)->length == (p2)->length) && \
			(p1)->value && (p2)->value && \
			!memcmp((p1)->value, (p2)->value, (p1)->length) && \
			!strcmp((p1)->name, (p2)->name))
static void __init of_selftest_property_copy(void)
{
#ifdef CONFIG_OF_DYNAMIC
	struct property p1 = { .name = "p1", .length = 0, .value = "" };
	struct property p2 = { .name = "p2", .length = 5, .value = "abcd" };
	struct property *new;

	new = __of_prop_dup(&p1, GFP_KERNEL);
	selftest(new && propcmp(&p1, new), "empty property didn't copy correctly\n");
	kfree(new->value);
	kfree(new->name);
	kfree(new);

	new = __of_prop_dup(&p2, GFP_KERNEL);
	selftest(new && propcmp(&p2, new), "non-empty property didn't copy correctly\n");
	kfree(new->value);
	kfree(new->name);
	kfree(new);
#endif
}

static void __init of_selftest_changeset(void)
{
#ifdef CONFIG_OF_DYNAMIC
	struct property *ppadd, padd = { .name = "prop-add", .length = 0, .value = "" };
	struct property *ppupdate, pupdate = { .name = "prop-update", .length = 5, .value = "abcd" };
	struct property *ppremove;
	struct device_node *n1, *n2, *n21, *nremove, *parent;
	struct of_changeset chgset;

	of_changeset_init(&chgset);
	n1 = __of_node_alloc("n1", "<NULL>", "/testcase-data/changeset/n1", 0x1234, GFP_KERNEL);
	selftest(n1, "testcase setup failure\n");
	n2 = __of_node_alloc("n2", "<NULL>", "/testcase-data/changeset/n2", 0x1235, GFP_KERNEL);
	selftest(n2, "testcase setup failure\n");
	n21 = __of_node_alloc("n21", "<NULL>", "/testcase-data/changeset/n2/n21", 0x1236, GFP_KERNEL);
	selftest(n21, "testcase setup failure %p\n", n21);
	nremove = of_find_node_by_path("/testcase-data/changeset/node-remove");
	selftest(nremove, "testcase setup failure\n");
	ppadd = __of_prop_dup(&padd, GFP_KERNEL);
	selftest(ppadd, "testcase setup failure\n");
	ppupdate = __of_prop_dup(&pupdate, GFP_KERNEL);
	selftest(ppupdate, "testcase setup failure\n");
	parent = nremove->parent;
	n1->parent = parent;
	n2->parent = parent;
	n21->parent = n2;
	n2->child = n21;
	ppremove = of_find_property(parent, "prop-remove", NULL);
	selftest(ppremove, "failed to find removal prop");

	of_changeset_init(&chgset);
	selftest(!of_changeset_attach_node(&chgset, n1), "fail attach n1\n");
	selftest(!of_changeset_attach_node(&chgset, n2), "fail attach n2\n");
	selftest(!of_changeset_detach_node(&chgset, nremove), "fail remove node\n");
	selftest(!of_changeset_attach_node(&chgset, n21), "fail attach n21\n");
	selftest(!of_changeset_add_property(&chgset, parent, ppadd), "fail add prop\n");
	selftest(!of_changeset_update_property(&chgset, parent, ppupdate), "fail update prop\n");
	selftest(!of_changeset_remove_property(&chgset, parent, ppremove), "fail remove prop\n");
	mutex_lock(&of_mutex);
	selftest(!of_changeset_apply(&chgset), "apply failed\n");
	mutex_unlock(&of_mutex);

	mutex_lock(&of_mutex);
	selftest(!of_changeset_revert(&chgset), "revert failed\n");
	mutex_unlock(&of_mutex);
#endif
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

static void __init of_selftest_platform_populate(void)
{
	int irq;
	struct device_node *np, *child;
	struct platform_device *pdev;
	struct of_device_id match[] = {
		{ .compatible = "test-device", },
		{}
	};

	np = of_find_node_by_path("/testcase-data");
	of_platform_populate(np, of_default_bus_match_table, NULL, NULL);

	/* Test that a missing irq domain returns -EPROBE_DEFER */
	np = of_find_node_by_path("/testcase-data/testcase-device1");
	pdev = of_find_device_by_node(np);
	selftest(pdev, "device 1 creation failed\n");

	irq = platform_get_irq(pdev, 0);
	selftest(irq == -EPROBE_DEFER, "device deferred probe failed - %d\n", irq);

	/* Test that a parsing failure does not return -EPROBE_DEFER */
	np = of_find_node_by_path("/testcase-data/testcase-device2");
	pdev = of_find_device_by_node(np);
	selftest(pdev, "device 2 creation failed\n");
	irq = platform_get_irq(pdev, 0);
	selftest(irq < 0 && irq != -EPROBE_DEFER, "device parsing error failed - %d\n", irq);

	np = of_find_node_by_path("/testcase-data/platform-tests");
	if (!np) {
		pr_err("No testcase data in device tree\n");
		return;
	}

	for_each_child_of_node(np, child) {
		struct device_node *grandchild;
		of_platform_populate(child, match, NULL, NULL);
		for_each_child_of_node(child, grandchild)
			selftest(of_find_device_by_node(grandchild),
				 "Could not create device for node '%s'\n",
				 grandchild->name);
	}
}

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
	of_selftest_find_node_by_name();
	of_selftest_dynamic();
	of_selftest_parse_phandle_with_args();
	of_selftest_property_match_string();
	of_selftest_property_copy();
	of_selftest_changeset();
	of_selftest_parse_interrupts();
	of_selftest_parse_interrupts_extended();
	of_selftest_match_node();
	of_selftest_platform_populate();
	pr_info("end of selftest - %i passed, %i failed\n",
		selftest_results.passed, selftest_results.failed);
	return 0;
}
late_initcall(of_selftest);
