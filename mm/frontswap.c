/*
 * Frontswap frontend
 *
 * This code provides the generic "frontend" layer to call a matching
 * "backend" driver implementation of frontswap.  See
 * Documentation/vm/frontswap.txt for more information.
 *
 * Copyright (C) 2009-2010 Oracle Corp.  All rights reserved.
 * Author: Dan Magenheimer
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/proc_fs.h>
#include <linux/security.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/frontswap.h>
#include <linux/swapfile.h>

/*
 * frontswap_ops is set by frontswap_register_ops to contain the pointers
 * to the frontswap "backend" implementation functions.
 */
static struct frontswap_ops frontswap_ops __read_mostly;

/*
 * This global enablement flag reduces overhead on systems where frontswap_ops
 * has not been registered, so is preferred to the slower alternative: a
 * function call that checks a non-global.
 */
int frontswap_enabled __read_mostly;
EXPORT_SYMBOL(frontswap_enabled);

/*
 * Counters available via /sys/kernel/debug/frontswap (if debugfs is
 * properly configured.  These are for information only so are not protected
 * against increment races.
 */
static u64 frontswap_gets;
static u64 frontswap_succ_puts;
static u64 frontswap_failed_puts;
static u64 frontswap_invalidates;

/*
 * Register operations for frontswap, returning previous thus allowing
 * detection of multiple backends and possible nesting
 */
struct frontswap_ops frontswap_register_ops(struct frontswap_ops *ops)
{
	struct frontswap_ops old = frontswap_ops;

	frontswap_ops = *ops;
	frontswap_enabled = 1;
	return old;
}
EXPORT_SYMBOL(frontswap_register_ops);

/* Called when a swap device is swapon'd */
void __frontswap_init(unsigned type)
{
	struct swap_info_struct *sis = swap_info[type];

	BUG_ON(sis == NULL);
	if (sis->frontswap_map == NULL)
		return;
	if (frontswap_enabled)
		(*frontswap_ops.init)(type);
}
EXPORT_SYMBOL(__frontswap_init);

/*
 * "Put" data from a page to frontswap and associate it with the page's
 * swaptype and offset.  Page must be locked and in the swap cache.
 * If frontswap already contains a page with matching swaptype and
 * offset, the frontswap implmentation may either overwrite the data and
 * return success or invalidate the page from frontswap and return failure
 */
int __frontswap_put_page(struct page *page)
{
	int ret = -1, dup = 0;
	swp_entry_t entry = { .val = page_private(page), };
	int type = swp_type(entry);
	struct swap_info_struct *sis = swap_info[type];
	pgoff_t offset = swp_offset(entry);

	BUG_ON(!PageLocked(page));
	BUG_ON(sis == NULL);
	if (frontswap_test(sis, offset))
		dup = 1;
	ret = (*frontswap_ops.put_page)(type, offset, page);
	if (ret == 0) {
		frontswap_set(sis, offset);
		frontswap_succ_puts++;
		if (!dup)
			atomic_inc(&sis->frontswap_pages);
	} else if (dup) {
		/*
		  failed dup always results in automatic invalidate of
		  the (older) page from frontswap
		 */
		frontswap_clear(sis, offset);
		atomic_dec(&sis->frontswap_pages);
		frontswap_failed_puts++;
	} else
		frontswap_failed_puts++;
	return ret;
}
EXPORT_SYMBOL(__frontswap_put_page);

/*
 * "Get" data from frontswap associated with swaptype and offset that were
 * specified when the data was put to frontswap and use it to fill the
 * specified page with data. Page must be locked and in the swap cache
 */
int __frontswap_get_page(struct page *page)
{
	int ret = -1;
	swp_entry_t entry = { .val = page_private(page), };
	int type = swp_type(entry);
	struct swap_info_struct *sis = swap_info[type];
	pgoff_t offset = swp_offset(entry);

	BUG_ON(!PageLocked(page));
	BUG_ON(sis == NULL);
	if (frontswap_test(sis, offset))
		ret = (*frontswap_ops.get_page)(type, offset, page);
	if (ret == 0)
		frontswap_gets++;
	return ret;
}
EXPORT_SYMBOL(__frontswap_get_page);

/*
 * Invalidate any data from frontswap associated with the specified swaptype
 * and offset so that a subsequent "get" will fail.
 */
void __frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	struct swap_info_struct *sis = swap_info[type];

	BUG_ON(sis == NULL);
	if (frontswap_test(sis, offset)) {
		(*frontswap_ops.invalidate_page)(type, offset);
		atomic_dec(&sis->frontswap_pages);
		frontswap_clear(sis, offset);
		frontswap_invalidates++;
	}
}
EXPORT_SYMBOL(__frontswap_invalidate_page);

/*
 * Invalidate all data from frontswap associated with all offsets for the
 * specified swaptype.
 */
void __frontswap_invalidate_area(unsigned type)
{
	struct swap_info_struct *sis = swap_info[type];

	BUG_ON(sis == NULL);
	if (sis->frontswap_map == NULL)
		return;
	(*frontswap_ops.invalidate_area)(type);
	atomic_set(&sis->frontswap_pages, 0);
	memset(sis->frontswap_map, 0, sis->max / sizeof(long));
}
EXPORT_SYMBOL(__frontswap_invalidate_area);

/*
 * Frontswap, like a true swap device, may unnecessarily retain pages
 * under certain circumstances; "shrink" frontswap is essentially a
 * "partial swapoff" and works by calling try_to_unuse to attempt to
 * unuse enough frontswap pages to attempt to -- subject to memory
 * constraints -- reduce the number of pages in frontswap to the
 * number given in the parameter target_pages.
 */
void frontswap_shrink(unsigned long target_pages)
{
	struct swap_info_struct *si = NULL;
	int si_frontswap_pages;
	unsigned long total_pages = 0, total_pages_to_unuse;
	unsigned long pages = 0, pages_to_unuse = 0;
	int type;
	bool locked = false;

	/*
	 * we don't want to hold swap_lock while doing a very
	 * lengthy try_to_unuse, but swap_list may change
	 * so restart scan from swap_list.head each time
	 */
	spin_lock(&swap_lock);
	locked = true;
	total_pages = 0;
	for (type = swap_list.head; type >= 0; type = si->next) {
		si = swap_info[type];
		total_pages += atomic_read(&si->frontswap_pages);
	}
	if (total_pages <= target_pages)
		goto out;
	total_pages_to_unuse = total_pages - target_pages;
	for (type = swap_list.head; type >= 0; type = si->next) {
		si = swap_info[type];
		si_frontswap_pages = atomic_read(&si->frontswap_pages);
		if (total_pages_to_unuse < si_frontswap_pages)
			pages = pages_to_unuse = total_pages_to_unuse;
		else {
			pages = si_frontswap_pages;
			pages_to_unuse = 0; /* unuse all */
		}
		/* ensure there is enough RAM to fetch pages from frontswap */
		if (security_vm_enough_memory_kern(pages))
			continue;
		vm_unacct_memory(pages);
		break;
	}
	if (type < 0)
		goto out;
	locked = false;
	spin_unlock(&swap_lock);
	try_to_unuse(type, true, pages_to_unuse);
out:
	if (locked)
		spin_unlock(&swap_lock);
	return;
}
EXPORT_SYMBOL(frontswap_shrink);

/*
 * Count and return the number of frontswap pages across all
 * swap devices.  This is exported so that backend drivers can
 * determine current usage without reading debugfs.
 */
unsigned long frontswap_curr_pages(void)
{
	int type;
	unsigned long totalpages = 0;
	struct swap_info_struct *si = NULL;

	spin_lock(&swap_lock);
	for (type = swap_list.head; type >= 0; type = si->next) {
		si = swap_info[type];
		totalpages += atomic_read(&si->frontswap_pages);
	}
	spin_unlock(&swap_lock);
	return totalpages;
}
EXPORT_SYMBOL(frontswap_curr_pages);

static int __init init_frontswap(void)
{
	int err = 0;

#ifdef CONFIG_DEBUG_FS
	struct dentry *root = debugfs_create_dir("frontswap", NULL);
	if (root == NULL)
		return -ENXIO;
	debugfs_create_u64("gets", S_IRUGO, root, &frontswap_gets);
	debugfs_create_u64("succ_puts", S_IRUGO, root, &frontswap_succ_puts);
	debugfs_create_u64("puts", S_IRUGO, root, &frontswap_failed_puts);
	debugfs_create_u64("invalidates", S_IRUGO,
				root, &frontswap_invalidates);
#endif
	return err;
}

module_init(init_frontswap);
