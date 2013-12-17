/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/mount.h>
#include <linux/mpage.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/statfs.h>
#include <linux/compat.h>
#include <linux/parser.h>
#include <linux/ctype.h>
#include <linux/namei.h>
#include <linux/miscdevice.h>
#include <linux/magic.h>
#include "compat.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "btrfs_inode.h"
#include "ioctl.h"
#include "print-tree.h"
#include "xattr.h"
#include "volumes.h"
#include "version.h"
#include "export.h"
#include "compression.h"

static const struct super_operations btrfs_super_ops;

static void btrfs_put_super(struct super_block *sb)
{
	struct btrfs_root *root = btrfs_sb(sb);
	int ret;

	ret = close_ctree(root);
	sb->s_fs_info = NULL;
}

enum {
	Opt_degraded, Opt_subvol, Opt_device, Opt_nodatasum, Opt_nodatacow,
	Opt_max_extent, Opt_max_inline, Opt_alloc_start, Opt_nobarrier,
	Opt_ssd, Opt_nossd, Opt_ssd_spread, Opt_thread_pool, Opt_noacl,
	Opt_compress, Opt_compress_force, Opt_notreelog, Opt_ratio,
	Opt_flushoncommit,
	Opt_discard, Opt_err,
};

static match_table_t tokens = {
	{Opt_degraded, "degraded"},
	{Opt_subvol, "subvol=%s"},
	{Opt_device, "device=%s"},
	{Opt_nodatasum, "nodatasum"},
	{Opt_nodatacow, "nodatacow"},
	{Opt_nobarrier, "nobarrier"},
	{Opt_max_extent, "max_extent=%s"},
	{Opt_max_inline, "max_inline=%s"},
	{Opt_alloc_start, "alloc_start=%s"},
	{Opt_thread_pool, "thread_pool=%d"},
	{Opt_compress, "compress"},
	{Opt_compress_force, "compress-force"},
	{Opt_ssd, "ssd"},
	{Opt_ssd_spread, "ssd_spread"},
	{Opt_nossd, "nossd"},
	{Opt_noacl, "noacl"},
	{Opt_notreelog, "notreelog"},
	{Opt_flushoncommit, "flushoncommit"},
	{Opt_ratio, "metadata_ratio=%d"},
	{Opt_discard, "discard"},
	{Opt_err, NULL},
};

u64 btrfs_parse_size(char *str)
{
	u64 res;
	int mult = 1;
	char *end;
	char last;

	res = simple_strtoul(str, &end, 10);

	last = end[0];
	if (isalpha(last)) {
		last = tolower(last);
		switch (last) {
		case 'g':
			mult *= 1024;
		case 'm':
			mult *= 1024;
		case 'k':
			mult *= 1024;
		}
		res = res * mult;
	}
	return res;
}

/*
 * Regular mount options parser.  Everything that is needed only when
 * reading in a new superblock is parsed here.
 */
int btrfs_parse_options(struct btrfs_root *root, char *options)
{
	struct btrfs_fs_info *info = root->fs_info;
	substring_t args[MAX_OPT_ARGS];
	char *p, *num, *orig;
	int intarg;
	int ret = 0;

	if (!options)
		return 0;

	/*
	 * strsep changes the string, duplicate it because parse_options
	 * gets called twice
	 */
	options = kstrdup(options, GFP_NOFS);
	if (!options)
		return -ENOMEM;

	orig = options;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_degraded:
			printk(KERN_INFO "btrfs: allowing degraded mounts\n");
			btrfs_set_opt(info->mount_opt, DEGRADED);
			break;
		case Opt_subvol:
		case Opt_device:
			/*
			 * These are parsed by btrfs_parse_early_options
			 * and can be happily ignored here.
			 */
			break;
		case Opt_nodatasum:
			printk(KERN_INFO "btrfs: setting nodatasum\n");
			btrfs_set_opt(info->mount_opt, NODATASUM);
			break;
		case Opt_nodatacow:
			printk(KERN_INFO "btrfs: setting nodatacow\n");
			btrfs_set_opt(info->mount_opt, NODATACOW);
			btrfs_set_opt(info->mount_opt, NODATASUM);
			break;
		case Opt_compress:
			printk(KERN_INFO "btrfs: use compression\n");
			btrfs_set_opt(info->mount_opt, COMPRESS);
			break;
		case Opt_compress_force:
			printk(KERN_INFO "btrfs: forcing compression\n");
			btrfs_set_opt(info->mount_opt, FORCE_COMPRESS);
			btrfs_set_opt(info->mount_opt, COMPRESS);
			break;
		case Opt_ssd:
			printk(KERN_INFO "btrfs: use ssd allocation scheme\n");
			btrfs_set_opt(info->mount_opt, SSD);
			break;
		case Opt_ssd_spread:
			printk(KERN_INFO "btrfs: use spread ssd "
			       "allocation scheme\n");
			btrfs_set_opt(info->mount_opt, SSD);
			btrfs_set_opt(info->mount_opt, SSD_SPREAD);
			break;
		case Opt_nossd:
			printk(KERN_INFO "btrfs: not using ssd allocation "
			       "scheme\n");
			btrfs_set_opt(info->mount_opt, NOSSD);
			btrfs_clear_opt(info->mount_opt, SSD);
			btrfs_clear_opt(info->mount_opt, SSD_SPREAD);
			break;
		case Opt_nobarrier:
			printk(KERN_INFO "btrfs: turning off barriers\n");
			btrfs_set_opt(info->mount_opt, NOBARRIER);
			break;
		case Opt_thread_pool:
			intarg = 0;
			match_int(&args[0], &intarg);
			if (intarg) {
				info->thread_pool_size = intarg;
				printk(KERN_INFO "btrfs: thread pool %d\n",
				       info->thread_pool_size);
			}
			break;
		case Opt_max_extent:
			num = match_strdup(&args[0]);
			if (num) {
				info->max_extent = btrfs_parse_size(num);
				kfree(num);

				info->max_extent = max_t(u64,
					info->max_extent, root->sectorsize);
				printk(KERN_INFO "btrfs: max_extent at %llu\n",
				       (unsigned long long)info->max_extent);
			}
			break;
		case Opt_max_inline:
			num = match_strdup(&args[0]);
			if (num) {
				info->max_inline = btrfs_parse_size(num);
				kfree(num);

				if (info->max_inline) {
					info->max_inline = max_t(u64,
						info->max_inline,
						root->sectorsize);
				}
				printk(KERN_INFO "btrfs: max_inline at %llu\n",
					(unsigned long long)info->max_inline);
			}
			break;
		case Opt_alloc_start:
			num = match_strdup(&args[0]);
			if (num) {
				info->alloc_start = btrfs_parse_size(num);
				kfree(num);
				printk(KERN_INFO
					"btrfs: allocations start at %llu\n",
					(unsigned long long)info->alloc_start);
			}
			break;
		case Opt_noacl:
			root->fs_info->sb->s_flags &= ~MS_POSIXACL;
			break;
		case Opt_notreelog:
			printk(KERN_INFO "btrfs: disabling tree log\n");
			btrfs_set_opt(info->mount_opt, NOTREELOG);
			break;
		case Opt_flushoncommit:
			printk(KERN_INFO "btrfs: turning on flush-on-commit\n");
			btrfs_set_opt(info->mount_opt, FLUSHONCOMMIT);
			break;
		case Opt_ratio:
			intarg = 0;
			match_int(&args[0], &intarg);
			if (intarg) {
				info->metadata_ratio = intarg;
				printk(KERN_INFO "btrfs: metadata ratio %d\n",
				       info->metadata_ratio);
			}
			break;
		case Opt_discard:
			btrfs_set_opt(info->mount_opt, DISCARD);
			break;
		case Opt_err:
			printk(KERN_INFO "btrfs: unrecognized mount option "
			       "'%s'\n", p);
			ret = -EINVAL;
			goto out;
		default:
			break;
		}
	}
out:
	kfree(orig);
	return ret;
}

/*
 * Parse mount options that are required early in the mount process.
 *
 * All other options will be parsed on much later in the mount process and
 * only when we need to allocate a new super block.
 */
static int btrfs_parse_early_options(const char *options, fmode_t flags,
		void *holder, char **subvol_name,
		struct btrfs_fs_devices **fs_devices)
{
	substring_t args[MAX_OPT_ARGS];
	char *opts, *p;
	int error = 0;

	if (!options)
		goto out;

	/*
	 * strsep changes the string, duplicate it because parse_options
	 * gets called twice
	 */
	opts = kstrdup(options, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	while ((p = strsep(&opts, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_subvol:
			*subvol_name = match_strdup(&args[0]);
			break;
		case Opt_device:
			error = btrfs_scan_one_device(match_strdup(&args[0]),
					flags, holder, fs_devices);
			if (error)
				goto out_free_opts;
			break;
		default:
			break;
		}
	}

 out_free_opts:
	kfree(opts);
 out:
	/*
	 * If no subvolume name is specified we use the default one.  Allocate
	 * a copy of the string "." here so that code later in the
	 * mount path doesn't care if it's the default volume or another one.
	 */
	if (!*subvol_name) {
		*subvol_name = kstrdup(".", GFP_KERNEL);
		if (!*subvol_name)
			return -ENOMEM;
	}
	return error;
}

static int btrfs_fill_super(struct super_block *sb,
			    struct btrfs_fs_devices *fs_devices,
			    void *data, int silent)
{
	struct inode *inode;
	struct dentry *root_dentry;
	struct btrfs_super_block *disk_super;
	struct btrfs_root *tree_root;
	struct btrfs_key key;
	int err;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = BTRFS_SUPER_MAGIC;
	sb->s_op = &btrfs_super_ops;
	sb->s_export_op = &btrfs_export_ops;
	sb->s_xattr = btrfs_xattr_handlers;
	sb->s_time_gran = 1;
#ifdef CONFIG_BTRFS_FS_POSIX_ACL
	sb->s_flags |= MS_POSIXACL;
#endif

	tree_root = open_ctree(sb, fs_devices, (char *)data);

	if (IS_ERR(tree_root)) {
		printk("btrfs: open_ctree failed\n");
		return PTR_ERR(tree_root);
	}
	sb->s_fs_info = tree_root;
	disk_super = &tree_root->fs_info->super_copy;

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	inode = btrfs_iget(sb, &key, tree_root->fs_info->fs_root);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto fail_close;
	}

	root_dentry = d_alloc_root(inode);
	if (!root_dentry) {
		iput(inode);
		err = -ENOMEM;
		goto fail_close;
	}
#if 0
	/* this does the super kobj at the same time */
	err = btrfs_sysfs_add_super(tree_root->fs_info);
	if (err)
		goto fail_close;
#endif

	sb->s_root = root_dentry;

	save_mount_options(sb, data);
	return 0;

fail_close:
	close_ctree(tree_root);
	return err;
}

int btrfs_sync_fs(struct super_block *sb, int wait)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = btrfs_sb(sb);
	int ret;

	if (!wait) {
		filemap_flush(root->fs_info->btree_inode->i_mapping);
		return 0;
	}

	btrfs_start_delalloc_inodes(root, 0);
	btrfs_wait_ordered_extents(root, 0, 0);

	trans = btrfs_start_transaction(root, 1);
	ret = btrfs_commit_transaction(trans, root);
	return ret;
}

static int btrfs_show_options(struct seq_file *seq, struct vfsmount *vfs)
{
	struct btrfs_root *root = btrfs_sb(vfs->mnt_sb);
	struct btrfs_fs_info *info = root->fs_info;

	if (btrfs_test_opt(root, DEGRADED))
		seq_puts(seq, ",degraded");
	if (btrfs_test_opt(root, NODATASUM))
		seq_puts(seq, ",nodatasum");
	if (btrfs_test_opt(root, NODATACOW))
		seq_puts(seq, ",nodatacow");
	if (btrfs_test_opt(root, NOBARRIER))
		seq_puts(seq, ",nobarrier");
	if (info->max_extent != (u64)-1)
		seq_printf(seq, ",max_extent=%llu",
			   (unsigned long long)info->max_extent);
	if (info->max_inline != 8192 * 1024)
		seq_printf(seq, ",max_inline=%llu",
			   (unsigned long long)info->max_inline);
	if (info->alloc_start != 0)
		seq_printf(seq, ",alloc_start=%llu",
			   (unsigned long long)info->alloc_start);
	if (info->thread_pool_size !=  min_t(unsigned long,
					     num_online_cpus() + 2, 8))
		seq_printf(seq, ",thread_pool=%d", info->thread_pool_size);
	if (btrfs_test_opt(root, COMPRESS))
		seq_puts(seq, ",compress");
	if (btrfs_test_opt(root, NOSSD))
		seq_puts(seq, ",nossd");
	if (btrfs_test_opt(root, SSD_SPREAD))
		seq_puts(seq, ",ssd_spread");
	else if (btrfs_test_opt(root, SSD))
		seq_puts(seq, ",ssd");
	if (btrfs_test_opt(root, NOTREELOG))
		seq_puts(seq, ",notreelog");
	if (btrfs_test_opt(root, FLUSHONCOMMIT))
		seq_puts(seq, ",flushoncommit");
	if (btrfs_test_opt(root, DISCARD))
		seq_puts(seq, ",discard");
	if (!(root->fs_info->sb->s_flags & MS_POSIXACL))
		seq_puts(seq, ",noacl");
	return 0;
}

static int btrfs_test_super(struct super_block *s, void *data)
{
	struct btrfs_fs_devices *test_fs_devices = data;
	struct btrfs_root *root = btrfs_sb(s);

	return root->fs_info->fs_devices == test_fs_devices;
}

/*
 * Find a superblock for the given device / mount point.
 *
 * Note:  This is based on get_sb_bdev from fs/super.c with a few additions
 *	  for multiple device setup.  Make sure to keep it in sync.
 */
static int btrfs_get_sb(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data, struct vfsmount *mnt)
{
	char *subvol_name = NULL;
	struct block_device *bdev = NULL;
	struct super_block *s;
	struct dentry *root;
	struct btrfs_fs_devices *fs_devices = NULL;
	fmode_t mode = FMODE_READ;
	int error = 0;

	if (!(flags & MS_RDONLY))
		mode |= FMODE_WRITE;

	error = btrfs_parse_early_options(data, mode, fs_type,
					  &subvol_name, &fs_devices);
	if (error)
		return error;

	error = btrfs_scan_one_device(dev_name, mode, fs_type, &fs_devices);
	if (error)
		goto error_free_subvol_name;

	error = btrfs_open_devices(fs_devices, mode, fs_type);
	if (error)
		goto error_free_subvol_name;

	if (!(flags & MS_RDONLY) && fs_devices->rw_devices == 0) {
		error = -EACCES;
		goto error_close_devices;
	}

	bdev = fs_devices->latest_bdev;
	s = sget(fs_type, btrfs_test_super, set_anon_super, fs_devices);
	if (IS_ERR(s))
		goto error_s;

	if (s->s_root) {
		if ((flags ^ s->s_flags) & MS_RDONLY) {
			deactivate_locked_super(s);
			error = -EBUSY;
			goto error_close_devices;
		}

		btrfs_close_devices(fs_devices);
	} else {
		char b[BDEVNAME_SIZE];

		s->s_flags = flags;
		strlcpy(s->s_id, bdevname(bdev, b), sizeof(s->s_id));
		error = btrfs_fill_super(s, fs_devices, data,
					 flags & MS_SILENT ? 1 : 0);
		if (error) {
			deactivate_locked_super(s);
			goto error_free_subvol_name;
		}

		btrfs_sb(s)->fs_info->bdev_holder = fs_type;
		s->s_flags |= MS_ACTIVE;
	}

	if (!strcmp(subvol_name, "."))
		root = dget(s->s_root);
	else {
		mutex_lock(&s->s_root->d_inode->i_mutex);
		root = lookup_one_len(subvol_name, s->s_root,
				      strlen(subvol_name));
		mutex_unlock(&s->s_root->d_inode->i_mutex);

		if (IS_ERR(root)) {
			deactivate_locked_super(s);
			error = PTR_ERR(root);
			goto error_free_subvol_name;
		}
		if (!root->d_inode) {
			dput(root);
			deactivate_locked_super(s);
			error = -ENXIO;
			goto error_free_subvol_name;
		}
	}

	mnt->mnt_sb = s;
	mnt->mnt_root = root;

	kfree(subvol_name);
	return 0;

error_s:
	error = PTR_ERR(s);
error_close_devices:
	btrfs_close_devices(fs_devices);
error_free_subvol_name:
	kfree(subvol_name);
	return error;
}

static int btrfs_remount(struct super_block *sb, int *flags, char *data)
{
	struct btrfs_root *root = btrfs_sb(sb);
	int ret;

	ret = btrfs_parse_options(root, data);
	if (ret)
		return -EINVAL;

	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;

	if (*flags & MS_RDONLY) {
		sb->s_flags |= MS_RDONLY;

		ret =  btrfs_commit_super(root);
		WARN_ON(ret);
	} else {
		if (root->fs_info->fs_devices->rw_devices == 0)
			return -EACCES;

		if (btrfs_super_log_root(&root->fs_info->super_copy) != 0)
			return -EINVAL;

		/* recover relocation */
		ret = btrfs_recover_relocation(root);
		WARN_ON(ret);

		ret = btrfs_cleanup_fs_roots(root->fs_info);
		WARN_ON(ret);

		sb->s_flags &= ~MS_RDONLY;
	}

	return 0;
}

static int btrfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct btrfs_root *root = btrfs_sb(dentry->d_sb);
	struct btrfs_super_block *disk_super = &root->fs_info->super_copy;
	int bits = dentry->d_sb->s_blocksize_bits;
	__be32 *fsid = (__be32 *)root->fs_info->fsid;

	buf->f_namelen = BTRFS_NAME_LEN;
	buf->f_blocks = btrfs_super_total_bytes(disk_super) >> bits;
	buf->f_bfree = buf->f_blocks -
		(btrfs_super_bytes_used(disk_super) >> bits);
	buf->f_bavail = buf->f_bfree;
	buf->f_bsize = dentry->d_sb->s_blocksize;
	buf->f_type = BTRFS_SUPER_MAGIC;

	/* We treat it as constant endianness (it doesn't matter _which_)
	   because we want the fsid to come out the same whether mounted
	   on a big-endian or little-endian host */
	buf->f_fsid.val[0] = be32_to_cpu(fsid[0]) ^ be32_to_cpu(fsid[2]);
	buf->f_fsid.val[1] = be32_to_cpu(fsid[1]) ^ be32_to_cpu(fsid[3]);
	/* Mask in the root object ID too, to disambiguate subvols */
	buf->f_fsid.val[0] ^= BTRFS_I(dentry->d_inode)->root->objectid >> 32;
	buf->f_fsid.val[1] ^= BTRFS_I(dentry->d_inode)->root->objectid;

	return 0;
}

static struct file_system_type btrfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "btrfs",
	.get_sb		= btrfs_get_sb,
	.kill_sb	= kill_anon_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

/*
 * used by btrfsctl to scan devices when no FS is mounted
 */
static long btrfs_control_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct btrfs_ioctl_vol_args *vol;
	struct btrfs_fs_devices *fs_devices;
	int ret = -ENOTTY;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	vol = memdup_user((void __user *)arg, sizeof(*vol));
	if (IS_ERR(vol))
		return PTR_ERR(vol);

	switch (cmd) {
	case BTRFS_IOC_SCAN_DEV:
		ret = btrfs_scan_one_device(vol->name, FMODE_READ,
					    &btrfs_fs_type, &fs_devices);
		break;
	}

	kfree(vol);
	return ret;
}

static int btrfs_freeze(struct super_block *sb)
{
	struct btrfs_root *root = btrfs_sb(sb);
	mutex_lock(&root->fs_info->transaction_kthread_mutex);
	mutex_lock(&root->fs_info->cleaner_mutex);
	return 0;
}

static int btrfs_unfreeze(struct super_block *sb)
{
	struct btrfs_root *root = btrfs_sb(sb);
	mutex_unlock(&root->fs_info->cleaner_mutex);
	mutex_unlock(&root->fs_info->transaction_kthread_mutex);
	return 0;
}

static const struct super_operations btrfs_super_ops = {
	.drop_inode	= btrfs_drop_inode,
	.delete_inode	= btrfs_delete_inode,
	.put_super	= btrfs_put_super,
	.sync_fs	= btrfs_sync_fs,
	.show_options	= btrfs_show_options,
	.write_inode	= btrfs_write_inode,
	.dirty_inode	= btrfs_dirty_inode,
	.alloc_inode	= btrfs_alloc_inode,
	.destroy_inode	= btrfs_destroy_inode,
	.statfs		= btrfs_statfs,
	.remount_fs	= btrfs_remount,
	.freeze_fs	= btrfs_freeze,
	.unfreeze_fs	= btrfs_unfreeze,
};

static const struct file_operations btrfs_ctl_fops = {
	.unlocked_ioctl	 = btrfs_control_ioctl,
	.compat_ioctl = btrfs_control_ioctl,
	.owner	 = THIS_MODULE,
};

static struct miscdevice btrfs_misc = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "btrfs-control",
	.fops		= &btrfs_ctl_fops
};

static int btrfs_interface_init(void)
{
	return misc_register(&btrfs_misc);
}

static void btrfs_interface_exit(void)
{
	if (misc_deregister(&btrfs_misc) < 0)
		printk(KERN_INFO "misc_deregister failed for control device");
}

static int __init init_btrfs_fs(void)
{
	int err;

	err = btrfs_init_sysfs();
	if (err)
		return err;

	err = btrfs_init_cachep();
	if (err)
		goto free_sysfs;

	err = extent_io_init();
	if (err)
		goto free_cachep;

	err = extent_map_init();
	if (err)
		goto free_extent_io;

	err = btrfs_interface_init();
	if (err)
		goto free_extent_map;

	err = register_filesystem(&btrfs_fs_type);
	if (err)
		goto unregister_ioctl;

	printk(KERN_INFO "%s loaded\n", BTRFS_BUILD_VERSION);
	return 0;

unregister_ioctl:
	btrfs_interface_exit();
free_extent_map:
	extent_map_exit();
free_extent_io:
	extent_io_exit();
free_cachep:
	btrfs_destroy_cachep();
free_sysfs:
	btrfs_exit_sysfs();
	return err;
}

static void __exit exit_btrfs_fs(void)
{
	btrfs_destroy_cachep();
	extent_map_exit();
	extent_io_exit();
	btrfs_interface_exit();
	unregister_filesystem(&btrfs_fs_type);
	btrfs_exit_sysfs();
	btrfs_cleanup_fs_uuids();
	btrfs_zlib_exit();
}

module_init(init_btrfs_fs)
module_exit(exit_btrfs_fs)

MODULE_LICENSE("GPL");
