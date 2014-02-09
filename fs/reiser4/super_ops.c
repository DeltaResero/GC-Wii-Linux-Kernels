/* Copyright 2005 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "inode.h"
#include "page_cache.h"
#include "ktxnmgrd.h"
#include "flush.h"
#include "safe_link.h"

#include <linux/vfs.h>
#include <linux/writeback.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

/* slab cache for inodes */
static struct kmem_cache *inode_cache;

static struct dentry *reiser4_debugfs_root = NULL;

/**
 * init_once - constructor for reiser4 inodes
 * @cache: cache @obj belongs to
 * @obj: inode to be initialized
 *
 * Initialization function to be called when new page is allocated by reiser4
 * inode cache. It is set on inode cache creation.
 */
static void init_once(void *obj)
{
	struct reiser4_inode_object *info;

	info = obj;

	/* initialize vfs inode */
	inode_init_once(&info->vfs_inode);

	/*
	 * initialize reiser4 specific part fo inode.
	 * NOTE-NIKITA add here initializations for locks, list heads,
	 * etc. that will be added to our private inode part.
	 */
	INIT_LIST_HEAD(get_readdir_list(&info->vfs_inode));
	init_rwsem(&info->p.conv_sem);
	/* init semaphore which is used during inode loading */
	loading_init_once(&info->p);
	INIT_RADIX_TREE(jnode_tree_by_reiser4_inode(&info->p),
			GFP_ATOMIC);
#if REISER4_DEBUG
	info->p.nr_jnodes = 0;
#endif
}

/**
 * init_inodes - create znode cache
 *
 * Initializes slab cache of inodes. It is part of reiser4 module initialization
 */
static int init_inodes(void)
{
	inode_cache = kmem_cache_create("reiser4_inode",
					sizeof(struct reiser4_inode_object),
					0,
					SLAB_HWCACHE_ALIGN |
					SLAB_RECLAIM_ACCOUNT, init_once);
	if (inode_cache == NULL)
		return RETERR(-ENOMEM);
	return 0;
}

/**
 * done_inodes - delete inode cache
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
static void done_inodes(void)
{
	destroy_reiser4_cache(&inode_cache);
}

/**
 * reiser4_alloc_inode - alloc_inode of super operations
 * @super: super block new inode is allocated for
 *
 * Allocates new inode, initializes reiser4 specific part of it.
 */
static struct inode *reiser4_alloc_inode(struct super_block *super)
{
	struct reiser4_inode_object *obj;

	assert("nikita-1696", super != NULL);
	obj = kmem_cache_alloc(inode_cache, reiser4_ctx_gfp_mask_get());
	if (obj != NULL) {
		reiser4_inode *info;

		info = &obj->p;

		info->pset = plugin_set_get_empty();
		info->hset = plugin_set_get_empty();
		info->extmask = 0;
		info->locality_id = 0ull;
		info->plugin_mask = 0;
		info->heir_mask = 0;
#if !REISER4_INO_IS_OID
		info->oid_hi = 0;
#endif
		reiser4_seal_init(&info->sd_seal, NULL, NULL);
		coord_init_invalid(&info->sd_coord, NULL);
		info->flags = 0;
		spin_lock_init(&info->guard);
		/* this deals with info's loading semaphore */
		loading_alloc(info);
		info->vroot = UBER_TREE_ADDR;
		return &obj->vfs_inode;
	} else
		return NULL;
}

/**
 * reiser4_destroy_inode - destroy_inode of super operations
 * @inode: inode being destroyed
 *
 * Puts reiser4 specific portion of inode, frees memory occupied by inode.
 */
static void reiser4_destroy_inode(struct inode *inode)
{
	reiser4_inode *info;

	info = reiser4_inode_data(inode);

	assert("vs-1220", inode_has_no_jnodes(info));

	if (!is_bad_inode(inode) && is_inode_loaded(inode)) {
		file_plugin *fplug = inode_file_plugin(inode);
		if (fplug->destroy_inode != NULL)
			fplug->destroy_inode(inode);
	}
	reiser4_dispose_cursors(inode);
	if (info->pset)
		plugin_set_put(info->pset);
	if (info->hset)
		plugin_set_put(info->hset);

	/*
	 * cannot add similar assertion about ->i_list as prune_icache return
	 * inode into slab with dangling ->list.{next,prev}. This is safe,
	 * because they are re-initialized in the new_inode().
	 */
	assert("nikita-2895", list_empty(&inode->i_dentry));
	assert("nikita-2896", hlist_unhashed(&inode->i_hash));
	assert("nikita-2898", list_empty_careful(get_readdir_list(inode)));

	/* this deals with info's loading semaphore */
	loading_destroy(info);

	kmem_cache_free(inode_cache,
			container_of(info, struct reiser4_inode_object, p));
}

/**
 * reiser4_dirty_inode - dirty_inode of super operations
 * @inode: inode being dirtied
 *
 * Updates stat data.
 */
static void reiser4_dirty_inode(struct inode *inode)
{
	int result;

	if (!is_in_reiser4_context())
		return;
	assert("", !IS_RDONLY(inode));
	assert("", (inode_file_plugin(inode)->estimate.update(inode) <=
		    get_current_context()->grabbed_blocks));

	result = reiser4_update_sd(inode);
	if (result)
		warning("", "failed to dirty inode for %llu: %d",
			get_inode_oid(inode), result);
}

/**
 * reiser4_delete_inode - delete_inode of super operations
 * @inode: inode to delete
 *
 * Calls file plugin's delete_object method to delete object items from
 * filesystem tree and calls clear_inode.
 */
static void reiser4_delete_inode(struct inode *inode)
{
	reiser4_context *ctx;
	file_plugin *fplug;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx)) {
		warning("vs-15", "failed to init context");
		return;
	}

	if (is_inode_loaded(inode)) {
		fplug = inode_file_plugin(inode);
		if (fplug != NULL && fplug->delete_object != NULL)
			fplug->delete_object(inode);
	}

	truncate_inode_pages(&inode->i_data, 0);
	inode->i_blocks = 0;
	clear_inode(inode);
	reiser4_exit_context(ctx);
}

/**
 * reiser4_put_super - put_super of super operations
 * @super: super block to free
 *
 * Stops daemons, release resources, umounts in short.
 */
static void reiser4_put_super(struct super_block *super)
{
	reiser4_super_info_data *sbinfo;
	reiser4_context *ctx;

	sbinfo = get_super_private(super);
	assert("vs-1699", sbinfo);

	debugfs_remove(sbinfo->tmgr.debugfs_atom_count);
	debugfs_remove(sbinfo->tmgr.debugfs_id_count);
	debugfs_remove(sbinfo->debugfs_root);

	ctx = reiser4_init_context(super);
	if (IS_ERR(ctx)) {
		warning("vs-17", "failed to init context");
		return;
	}

	/* have disk format plugin to free its resources */
	if (get_super_private(super)->df_plug->release)
		get_super_private(super)->df_plug->release(super);

	reiser4_done_formatted_fake(super);

	/* stop daemons: ktxnmgr and entd */
	reiser4_done_entd(super);
	reiser4_done_ktxnmgrd(super);
	reiser4_done_txnmgr(&sbinfo->tmgr);

	reiser4_done_fs_info(super);
	reiser4_exit_context(ctx);
}

/**
 * reiser4_write_super - write_super of super operations
 * @super: super block to write
 *
 * Captures znode associated with super block, comit all transactions.
 */
static void reiser4_write_super(struct super_block *super)
{
	int ret;
	reiser4_context *ctx;

	assert("vs-1700", !rofs_super(super));

	ctx = reiser4_init_context(super);
	if (IS_ERR(ctx)) {
		warning("vs-16", "failed to init context");
		return;
	}

	ret = reiser4_capture_super_block(super);
	if (ret != 0)
		warning("vs-1701",
			"reiser4_capture_super_block failed in write_super: %d",
			ret);
	ret = txnmgr_force_commit_all(super, 0);
	if (ret != 0)
		warning("jmacd-77113",
			"txn_force failed in write_super: %d", ret);

	super->s_dirt = 0;

	reiser4_exit_context(ctx);
}

/**
 * reiser4_statfs - statfs of super operations
 * @super: super block of file system in queried
 * @stafs: buffer to fill with statistics
 *
 * Returns information about filesystem.
 */
static int reiser4_statfs(struct dentry *dentry, struct kstatfs *statfs)
{
	sector_t total;
	sector_t reserved;
	sector_t free;
	sector_t forroot;
	sector_t deleted;
	reiser4_context *ctx;
	struct super_block *super = dentry->d_sb;

	assert("nikita-408", super != NULL);
	assert("nikita-409", statfs != NULL);

	ctx = reiser4_init_context(super);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	statfs->f_type = reiser4_statfs_type(super);
	statfs->f_bsize = super->s_blocksize;

	/*
	 * 5% of total block space is reserved. This is needed for flush and
	 * for truncates (so that we are able to perform truncate/unlink even
	 * on the otherwise completely full file system). If this reservation
	 * is hidden from statfs(2), users will mistakenly guess that they
	 * have enough free space to complete some operation, which is
	 * frustrating.
	 *
	 * Another possible solution is to subtract ->blocks_reserved from
	 * ->f_bfree, but changing available space seems less intrusive than
	 * letting user to see 5% of disk space to be used directly after
	 * mkfs.
	 */
	total = reiser4_block_count(super);
	reserved = get_super_private(super)->blocks_reserved;
	deleted = txnmgr_count_deleted_blocks();
	free = reiser4_free_blocks(super) + deleted;
	forroot = reiser4_reserved_blocks(super, 0, 0);

	/*
	 * These counters may be in inconsistent state because we take the
	 * values without keeping any global spinlock.  Here we do a sanity
	 * check that free block counter does not exceed the number of all
	 * blocks.
	 */
	if (free > total)
		free = total;
	statfs->f_blocks = total - reserved;
	/* make sure statfs->f_bfree is never larger than statfs->f_blocks */
	if (free > reserved)
		free -= reserved;
	else
		free = 0;
	statfs->f_bfree = free;

	if (free > forroot)
		free -= forroot;
	else
		free = 0;
	statfs->f_bavail = free;

	statfs->f_files = 0;
	statfs->f_ffree = 0;

	/* maximal acceptable name length depends on directory plugin. */
	assert("nikita-3351", super->s_root->d_inode != NULL);
	statfs->f_namelen = reiser4_max_filename_len(super->s_root->d_inode);
	reiser4_exit_context(ctx);
	return 0;
}

/**
 * reiser4_clear_inode - clear_inode of super operation
 * @inode: inode about to destroy
 *
 * Does sanity checks: being destroyed should have all jnodes detached.
 */
static void reiser4_clear_inode(struct inode *inode)
{
#if REISER4_DEBUG
	reiser4_inode *r4_inode;

	r4_inode = reiser4_inode_data(inode);
	if (!inode_has_no_jnodes(r4_inode))
		warning("vs-1732", "reiser4 inode has %ld jnodes\n",
			r4_inode->nr_jnodes);
#endif
}

/**
 * reiser4_writeback_inodes - writeback_inodes of super operations
 * @super:
 * @wb:
 * @wbc:
 *
 * This method is called by background and non-backgound writeback. Reiser4's
 * implementation uses generic_writeback_sb_inodes to call reiser4_writepages
 * for each of dirty inodes. reiser4_writepages handles pages dirtied via shared
 * mapping - dirty pages get into atoms. Writeout is called to flush some atoms.
 */
static int reiser4_writeback_inodes(struct super_block *super,
				    struct bdi_writeback *wb,
				    struct writeback_control *wbc)
{
	int ret;
	long to_write;
	reiser4_context *ctx;

	if (wbc->for_kupdate)
		/* reiser4 has its own means of periodical write-out */
		goto skip;
	assert("vs-49", wbc->older_than_this == NULL);

	spin_unlock(&inode_lock);
	ctx = reiser4_init_context(super);
	if (IS_ERR(ctx)) {
		warning("vs-13", "failed to init context");
		spin_lock(&inode_lock);
		goto skip;
	}
	to_write = wbc->nr_to_write;
	/*
	 * call reiser4_writepages for each of dirty inodes to turn
	 * dirty pages into transactions if they were not yet.
	 */
	spin_lock(&inode_lock);
	ret = generic_writeback_sb_inodes(super, wb, wbc);
	spin_unlock(&inode_lock);

	wbc->nr_to_write = to_write;

	/* flush goes here */
	reiser4_writeout(super, wbc);

	/* avoid recursive calls to ->writeback_inodes */
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	spin_lock(&inode_lock);

	return wbc->nr_to_write <= 0 ? 1 : ret;
 skip:
	writeback_skip_sb_inodes(super, wb);
	return 0;
}

/**
 * reiser4_show_options - show_options of super operations
 * @m: file where to write information
 * @mnt: mount structure
 *
 * Makes reiser4 mount options visible in /proc/mounts.
 */
static int reiser4_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	struct super_block *super;
	reiser4_super_info_data *sbinfo;

	super = mnt->mnt_sb;
	sbinfo = get_super_private(super);

	seq_printf(m, ",atom_max_size=0x%x", sbinfo->tmgr.atom_max_size);
	seq_printf(m, ",atom_max_age=0x%x", sbinfo->tmgr.atom_max_age);
	seq_printf(m, ",atom_min_size=0x%x", sbinfo->tmgr.atom_min_size);
	seq_printf(m, ",atom_max_flushers=0x%x",
		   sbinfo->tmgr.atom_max_flushers);
	seq_printf(m, ",cbk_cache_slots=0x%x",
		   sbinfo->tree.cbk_cache.nr_slots);

	return 0;
}

struct super_operations reiser4_super_operations = {
	.alloc_inode = reiser4_alloc_inode,
	.destroy_inode = reiser4_destroy_inode,
	.dirty_inode = reiser4_dirty_inode,
	.delete_inode = reiser4_delete_inode,
	.put_super = reiser4_put_super,
	.write_super = reiser4_write_super,
	.statfs = reiser4_statfs,
	.clear_inode = reiser4_clear_inode,
	.writeback_inodes = reiser4_writeback_inodes,
	.show_options = reiser4_show_options
};

/**
 * fill_super - initialize super block on mount
 * @super: super block to fill
 * @data: reiser4 specific mount option
 * @silent:
 *
 * This is to be called by reiser4_get_sb. Mounts filesystem.
 */
static int fill_super(struct super_block *super, void *data, int silent)
{
	reiser4_context ctx;
	int result;
	reiser4_super_info_data *sbinfo;

	assert("zam-989", super != NULL);

	super->s_op = NULL;
	init_stack_context(&ctx, super);

	/* allocate reiser4 specific super block */
	if ((result = reiser4_init_fs_info(super)) != 0)
		goto failed_init_sinfo;

	sbinfo = get_super_private(super);
	/* initialize various reiser4 parameters, parse mount options */
	if ((result = reiser4_init_super_data(super, data)) != 0)
		goto failed_init_super_data;

	/* read reiser4 master super block, initialize disk format plugin */
	if ((result = reiser4_init_read_super(super, silent)) != 0)
		goto failed_init_read_super;

	/* initialize transaction manager */
	reiser4_init_txnmgr(&sbinfo->tmgr);

	/* initialize ktxnmgrd context and start kernel thread ktxnmrgd */
	if ((result = reiser4_init_ktxnmgrd(super)) != 0)
		goto failed_init_ktxnmgrd;

	/* initialize entd context and start kernel thread entd */
	if ((result = reiser4_init_entd(super)) != 0)
		goto failed_init_entd;

	/* initialize address spaces for formatted nodes and bitmaps */
	if ((result = reiser4_init_formatted_fake(super)) != 0)
		goto failed_init_formatted_fake;

	/* initialize disk format plugin */
	if ((result = get_super_private(super)->df_plug->init_format(super,
								    data)) != 0)
		goto failed_init_disk_format;

	/*
	 * There are some 'committed' versions of reiser4 super block counters,
	 * which correspond to reiser4 on-disk state. These counters are
	 * initialized here
	 */
	sbinfo->blocks_free_committed = sbinfo->blocks_free;
	sbinfo->nr_files_committed = oids_used(super);

	/* get inode of root directory */
	if ((result = reiser4_init_root_inode(super)) != 0)
		goto failed_init_root_inode;

	if ((result = get_super_private(super)->df_plug->version_update(super)) != 0)
		goto failed_update_format_version;

	process_safelinks(super);
	reiser4_exit_context(&ctx);

	sbinfo->debugfs_root = debugfs_create_dir(super->s_id,
						  reiser4_debugfs_root);
	if (sbinfo->debugfs_root) {
		sbinfo->tmgr.debugfs_atom_count =
			debugfs_create_u32("atom_count", S_IFREG|S_IRUSR,
					   sbinfo->debugfs_root,
					   &sbinfo->tmgr.atom_count);
		sbinfo->tmgr.debugfs_id_count =
			debugfs_create_u32("id_count", S_IFREG|S_IRUSR,
					   sbinfo->debugfs_root,
					   &sbinfo->tmgr.id_count);
	}
	return 0;

 failed_update_format_version:
 failed_init_root_inode:
	if (sbinfo->df_plug->release)
		sbinfo->df_plug->release(super);
 failed_init_disk_format:
	reiser4_done_formatted_fake(super);
 failed_init_formatted_fake:
	reiser4_done_entd(super);
 failed_init_entd:
	reiser4_done_ktxnmgrd(super);
 failed_init_ktxnmgrd:
	reiser4_done_txnmgr(&sbinfo->tmgr);
 failed_init_read_super:
 failed_init_super_data:
	reiser4_done_fs_info(super);
 failed_init_sinfo:
	reiser4_exit_context(&ctx);
	return result;
}

/**
 * reiser4_get_sb - get_sb of file_system_type operations
 * @fs_type:
 * @flags: mount flags MS_RDONLY, MS_VERBOSE, etc
 * @dev_name: block device file name
 * @data: specific mount options
 *
 * Reiser4 mount entry.
 */
static int reiser4_get_sb(struct file_system_type *fs_type, int flags,
			const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, fill_super, mnt);
}

/* structure describing the reiser4 filesystem implementation */
static struct file_system_type reiser4_fs_type = {
	.owner = THIS_MODULE,
	.name = "reiser4",
	.fs_flags = FS_REQUIRES_DEV,
	.get_sb = reiser4_get_sb,
	.kill_sb = kill_block_super,
	.next = NULL
};

void destroy_reiser4_cache(struct kmem_cache **cachep)
{
	BUG_ON(*cachep == NULL);
	kmem_cache_destroy(*cachep);
	*cachep = NULL;
}

/**
 * init_reiser4 - reiser4 initialization entry point
 *
 * Initializes reiser4 slabs, registers reiser4 filesystem type. It is called
 * on kernel initialization or during reiser4 module load.
 */
static int __init init_reiser4(void)
{
	int result;

	printk(KERN_INFO
	       "Loading Reiser4. "
	       "See www.namesys.com for a description of Reiser4.\n");

	/* initialize slab cache of inodes */
	if ((result = init_inodes()) != 0)
		goto failed_inode_cache;

	/* initialize cache of znodes */
	if ((result = init_znodes()) != 0)
		goto failed_init_znodes;

	/* initialize all plugins */
	if ((result = init_plugins()) != 0)
		goto failed_init_plugins;

	/* initialize cache of plugin_set-s and plugin_set's hash table */
	if ((result = init_plugin_set()) != 0)
		goto failed_init_plugin_set;

	/* initialize caches of txn_atom-s and txn_handle-s */
	if ((result = init_txnmgr_static()) != 0)
		goto failed_init_txnmgr_static;

	/* initialize cache of jnodes */
	if ((result = init_jnodes()) != 0)
		goto failed_init_jnodes;

	/* initialize cache of flush queues */
	if ((result = reiser4_init_fqs()) != 0)
		goto failed_init_fqs;

	/* initialize cache of structures attached to dentry->d_fsdata */
	if ((result = reiser4_init_dentry_fsdata()) != 0)
		goto failed_init_dentry_fsdata;

	/* initialize cache of structures attached to file->private_data */
	if ((result = reiser4_init_file_fsdata()) != 0)
		goto failed_init_file_fsdata;

	/*
	 * initialize cache of d_cursors. See plugin/file_ops_readdir.c for
	 * more details
	 */
	if ((result = reiser4_init_d_cursor()) != 0)
		goto failed_init_d_cursor;

	if ((result = register_filesystem(&reiser4_fs_type)) == 0) {
		reiser4_debugfs_root = debugfs_create_dir("reiser4", NULL);
		return 0;
	}

	reiser4_done_d_cursor();
 failed_init_d_cursor:
	reiser4_done_file_fsdata();
 failed_init_file_fsdata:
	reiser4_done_dentry_fsdata();
 failed_init_dentry_fsdata:
	reiser4_done_fqs();
 failed_init_fqs:
	done_jnodes();
 failed_init_jnodes:
	done_txnmgr_static();
 failed_init_txnmgr_static:
	done_plugin_set();
 failed_init_plugin_set:
 failed_init_plugins:
	done_znodes();
 failed_init_znodes:
	done_inodes();
 failed_inode_cache:
	return result;
}

/**
 * done_reiser4 - reiser4 exit entry point
 *
 * Unregister reiser4 filesystem type, deletes caches. It is called on shutdown
 * or at module unload.
 */
static void __exit done_reiser4(void)
{
	int result;

	debugfs_remove(reiser4_debugfs_root);
	result = unregister_filesystem(&reiser4_fs_type);
	BUG_ON(result != 0);
	reiser4_done_d_cursor();
	reiser4_done_file_fsdata();
	reiser4_done_dentry_fsdata();
	reiser4_done_fqs();
	done_jnodes();
	done_txnmgr_static();
	done_plugin_set();
	done_znodes();
	destroy_reiser4_cache(&inode_cache);
}

module_init(init_reiser4);
module_exit(done_reiser4);

MODULE_DESCRIPTION("Reiser4 filesystem");
MODULE_AUTHOR("Hans Reiser <Reiser@Namesys.COM>");

MODULE_LICENSE("GPL");

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
