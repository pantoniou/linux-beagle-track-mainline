/*
 * proc_devtree.c - handles /proc/device-tree
 *
 * Copyright 1997 Paul Mackerras
 */
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/printk.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <asm/prom.h>
#include <asm/uaccess.h>
#include <linux/of_fdt.h>
#include <linux/idr.h>

#include "internal.h"

static inline void set_node_proc_entry(struct device_node *np,
				       struct proc_dir_entry *de)
{
#ifdef HAVE_ARCH_DEVTREE_FIXUPS
	np->pde = de;
#endif
}

static struct proc_dir_entry *proc_device_tree;

#if defined(CONFIG_OF_OVERLAY)
static struct proc_dir_entry *proc_device_tree_overlay;
static struct proc_dir_entry *proc_device_tree_overlay_status;
static DEFINE_MUTEX(overlay_lock);
static DEFINE_IDR(overlay_idr);

struct proc_overlay_data {
	void			*buf;
	size_t			alloc;
	size_t			size;

	int 			id;
	struct device_node	*overlay;
	int			ovinfo_cnt;
	struct of_overlay_info	*ovinfo;
	unsigned int		failed : 1;
	unsigned int		applied : 1;
	unsigned int		removing : 1;
};

static int overlay_proc_open(struct inode *inode, struct file *file)
{
	struct proc_overlay_data *od;

	od = kzalloc(sizeof(*od), GFP_KERNEL);
	if (od == NULL)
		return -ENOMEM;

	od->id = -1;

	/* save it */
	file->private_data = od;

	return 0;
}

static ssize_t overlay_proc_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos)
{
	struct proc_overlay_data *od = file->private_data;
	void *new_buf;

	/* need to alloc? */
	if (od->size + size > od->alloc) {

		/* start at 256K at first */
		if (od->alloc == 0)
			od->alloc = SZ_256K / 2;

		/* double buffer */
		od->alloc <<= 1;
		new_buf = kzalloc(od->alloc, GFP_KERNEL);
		if (new_buf == NULL) {
			pr_err("%s: failed to grow buffer\n", __func__);
			od->failed = 1;
			return -ENOMEM;
		}

		/* copy all we had previously */
		memcpy(new_buf, od->buf, od->size);

		/* free old buffer and assign new */
		kfree(od->buf);
		od->buf = new_buf;
	}

	if (unlikely(copy_from_user(od->buf + od->size, buf, size))) {
		pr_err("%s: fault copying from userspace\n", __func__);
		return -EFAULT;
	}

	od->size += size;
	*ppos += size;

	return size;
}

static int overlay_proc_release(struct inode *inode, struct file *file)
{
	struct proc_overlay_data *od = file->private_data;
	int id;
	int err = 0;

	/* perfectly normal when not loading */
	if (od == NULL)
		return 0;

	if (od->failed)
		goto out_free;

	of_fdt_unflatten_tree(od->buf, &od->overlay);
	if (od->overlay == NULL) {
		pr_err("%s: failed to unflatten tree\n", __func__);
		err = -EINVAL;
		goto out_free;
	}
	pr_debug("%s: unflattened OK\n", __func__);

	/* mark it as detached */
	of_node_set_flag(od->overlay, OF_DETACHED);

	/* perform resolution */
	err = of_resolve(od->overlay);
	if (err != 0) {
		pr_err("%s: Failed to resolve tree\n", __func__);
		goto out_free;
	}
	pr_debug("%s: resolved OK\n", __func__);

	/* now build an overlay info array */
	err = of_build_overlay_info(od->overlay,
			&od->ovinfo_cnt, &od->ovinfo);
	if (err != 0) {
		pr_err("%s: Failed to build overlay info\n", __func__);
		goto out_free;
	}

	pr_debug("%s: built %d overlay segments\n", __func__,
			od->ovinfo_cnt);

	err = of_overlay(od->ovinfo_cnt, od->ovinfo);
	if (err != 0) {
		pr_err("%s: Failed to apply overlay\n", __func__);
		goto out_free;
	}

	od->applied = 1;

	mutex_lock(&overlay_lock);
	idr_preload(GFP_KERNEL);
	id = idr_alloc(&overlay_idr, od, 0, -1, GFP_KERNEL);
	idr_preload_end();
	mutex_unlock(&overlay_lock);

	if (id < 0) {
		err = id;
		pr_err("%s: failed to get id for overlay\n", __func__);
		goto out_free;
	}
	od->id = id;

	pr_info("%s: Applied #%d overlay segments @%d\n", __func__,
			od->ovinfo_cnt, od->id);

	return 0;

out_free:
	if (od->id != -1)
		idr_remove(&overlay_idr, od->id);
	if (od->applied)
		of_overlay_revert(od->ovinfo_cnt, od->ovinfo);
	if (od->ovinfo)
		of_free_overlay_info(od->ovinfo_cnt, od->ovinfo);
	/* release memory */
	kfree(od->buf);
	kfree(od);

	return 0;
}

static const struct file_operations overlay_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= overlay_proc_open,
	.write		= overlay_proc_write,
	.release	= overlay_proc_release,
};

/*
 * Supply data on a read from /proc/device-tree-overlay-status
 */
static int overlay_status_proc_show(struct seq_file *m, void *v)
{
	int err, id;
	struct proc_overlay_data *od;
	const char *part_number, *version;

	rcu_read_lock();
	idr_for_each_entry(&overlay_idr, od, id) {
		seq_printf(m, "%d: %d bytes", id, od->size);
		/* TODO Make this standardized? */
		err = of_property_read_string(od->overlay, "part-number",
					&part_number);
		if (err != 0)
			part_number = NULL;
		err = of_property_read_string(od->overlay, "version",
					&version);
		if (err != 0)
			version = NULL;
		if (part_number) {
			seq_printf(m, " %s", part_number);
			if (version)
				seq_printf(m, ":%s", version);
		}
		seq_printf(m, "\n");
	}
	rcu_read_unlock();

	return 0;
}

static int overlay_status_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, overlay_status_proc_show, __PDE_DATA(inode));
}

static ssize_t overlay_status_proc_write(struct file *file, const char __user *buf,
			     size_t size, loff_t *ppos)
{
	struct proc_overlay_data *od;
	char buffer[PROC_NUMBUF + 1];
	char *ptr;
	int id, err, count;

	memset(buffer, 0, sizeof(buffer));
	count = size;
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	ptr = buffer;
	if (*ptr != '-') {	/* only removal supported */
		err = -EINVAL;
		goto out;
	}
	ptr++;
	err = kstrtoint(strstrip(ptr), 0, &id);
	if (err)
		goto out;

	/* find it */
	mutex_lock(&overlay_lock);
	od = idr_find(&overlay_idr, id);
	if (od == NULL) {
		mutex_unlock(&overlay_lock);
		err = -EINVAL;
		goto out;
	}
	/* remove it */
	idr_remove(&overlay_idr, id);
	mutex_unlock(&overlay_lock);

	err = of_overlay_revert(od->ovinfo_cnt, od->ovinfo);
	if (err != 0) {
		pr_err("%s: of_overlay_revert failed\n", __func__);
		goto out;
	}
	/* release memory */
	of_free_overlay_info(od->ovinfo_cnt, od->ovinfo);
	kfree(od->buf);
	kfree(od);

	pr_info("%s: Removed overlay with id %d\n", __func__, id);
out:
	return err < 0 ? err : count;
}

static const struct file_operations overlay_status_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= overlay_status_proc_open,
	.read		= seq_read,
	.write		= overlay_status_proc_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

/*
 * Supply data on a read from /proc/device-tree/node/property.
 */
static int property_proc_show(struct seq_file *m, void *v)
{
	struct property *pp = m->private;

	seq_write(m, pp->value, pp->length);
	return 0;
}

static int property_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, property_proc_show, __PDE_DATA(inode));
}

static const struct file_operations property_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= property_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * For a node with a name like "gc@10", we make symlinks called "gc"
 * and "@10" to it.
 */

/*
 * Add a property to a node
 */
static struct proc_dir_entry *
__proc_device_tree_add_prop(struct proc_dir_entry *de, struct property *pp,
		const char *name)
{
	struct proc_dir_entry *ent;

	/*
	 * Unfortunately proc_register puts each new entry
	 * at the beginning of the list.  So we rearrange them.
	 */
	ent = proc_create_data(name,
			       strncmp(name, "security-", 9) ? S_IRUGO : S_IRUSR,
			       de, &property_proc_fops, pp);
	if (ent == NULL)
		return NULL;

	if (!strncmp(name, "security-", 9))
		ent->size = 0; /* don't leak number of password chars */
	else
		ent->size = pp->length;

	return ent;
}


void proc_device_tree_add_prop(struct proc_dir_entry *pde, struct property *prop)
{
	__proc_device_tree_add_prop(pde, prop, prop->name);
}

void proc_device_tree_remove_prop(struct proc_dir_entry *pde,
				  struct property *prop)
{
	remove_proc_entry(prop->name, pde);
}

void proc_device_tree_update_prop(struct proc_dir_entry *pde,
				  struct property *newprop,
				  struct property *oldprop)
{
	struct proc_dir_entry *ent;

	if (!oldprop) {
		proc_device_tree_add_prop(pde, newprop);
		return;
	}

	for (ent = pde->subdir; ent != NULL; ent = ent->next)
		if (ent->data == oldprop)
			break;
	if (ent == NULL) {
		pr_warn("device-tree: property \"%s\" does not exist\n",
			oldprop->name);
	} else {
		ent->data = newprop;
		ent->size = newprop->length;
	}
}

/*
 * Various dodgy firmware might give us nodes and/or properties with
 * conflicting names. That's generally ok, except for exporting via /proc,
 * so munge names here to ensure they're unique.
 */

static int duplicate_name(struct proc_dir_entry *de, const char *name)
{
	struct proc_dir_entry *ent;
	int found = 0;

	spin_lock(&proc_subdir_lock);

	for (ent = de->subdir; ent != NULL; ent = ent->next) {
		if (strcmp(ent->name, name) == 0) {
			found = 1;
			break;
		}
	}

	spin_unlock(&proc_subdir_lock);

	return found;
}

static const char *fixup_name(struct device_node *np, struct proc_dir_entry *de,
		const char *name)
{
	char *fixed_name;
	int fixup_len = strlen(name) + 2 + 1; /* name + #x + \0 */
	int i = 1, size;

realloc:
	fixed_name = kmalloc(fixup_len, GFP_KERNEL);
	if (fixed_name == NULL) {
		pr_err("device-tree: Out of memory trying to fixup "
		       "name \"%s\"\n", name);
		return name;
	}

retry:
	size = snprintf(fixed_name, fixup_len, "%s#%d", name, i);
	size++; /* account for NULL */

	if (size > fixup_len) {
		/* We ran out of space, free and reallocate. */
		kfree(fixed_name);
		fixup_len = size;
		goto realloc;
	}

	if (duplicate_name(de, fixed_name)) {
		/* Multiple duplicates. Retry with a different offset. */
		i++;
		goto retry;
	}

	pr_warn("device-tree: Duplicate name in %s, renamed to \"%s\"\n",
		np->full_name, fixed_name);

	return fixed_name;
}

/*
 * Process a node, adding entries for its children and its properties.
 */
void proc_device_tree_add_node(struct device_node *np,
			       struct proc_dir_entry *de)
{
	struct property *pp;
	struct proc_dir_entry *ent;
	struct device_node *child;
	const char *p;

	set_node_proc_entry(np, de);
	for (child = NULL; (child = of_get_next_child(np, child));) {
		/* Use everything after the last slash, or the full name */
		p = kbasename(child->full_name);

		if (duplicate_name(de, p))
			p = fixup_name(np, de, p);

		ent = proc_mkdir(p, de);
		if (ent == NULL)
			break;
		proc_device_tree_add_node(child, ent);
	}
	of_node_put(child);

	for (pp = np->properties; pp != NULL; pp = pp->next) {
		p = pp->name;

		if (strchr(p, '/'))
			continue;

		if (duplicate_name(de, p))
			p = fixup_name(np, de, p);

		ent = __proc_device_tree_add_prop(de, pp, p);
		if (ent == NULL)
			break;
	}
}

/*
 * Called on initialization to set up the /proc/device-tree subtree
 */
void __init proc_device_tree_init(void)
{
	struct device_node *root;

	proc_device_tree = proc_mkdir("device-tree", NULL);
	if (proc_device_tree == NULL)
		return;
	root = of_find_node_by_path("/");
	if (root == NULL) {
		pr_debug("/proc/device-tree: can't find root\n");
		return;
	}
	proc_device_tree_add_node(root, proc_device_tree);
#if defined(CONFIG_OF_OVERLAY)
	proc_device_tree_overlay = proc_create_data("device-tree-overlay",
			       S_IWUSR, NULL, &overlay_proc_fops, NULL);
	proc_device_tree_overlay_status = proc_create_data("device-tree-overlay-status",
			       S_IRUSR| S_IWUSR, NULL, &overlay_status_proc_fops, NULL);
#endif
	of_node_put(root);
}
