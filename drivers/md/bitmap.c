/*
 * bitmap.c two-level bitmap (C) Peter T. Breuer (ptb@ot.uc3m.es) 2003
 *
 * bitmap_create  - sets up the bitmap structure
 * bitmap_destroy - destroys the bitmap structure
 *
 * additions, Copyright (C) 2003-2004, Paul Clements, SteelEye Technology, Inc.:
 * - added disk storage for bitmap
 * - changes to allow various bitmap chunk sizes
 * - added bitmap daemon (to asynchronously clear bitmap bits from disk)
 */

/*
 * Still to do:
 *
 * flush after percent set rather than just time based. (maybe both).
 * wait if count gets too high, wake when it drops to half.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/config.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/buffer_head.h>
#include <linux/raid/md.h>
#include <linux/raid/bitmap.h>

/* debug macros */

#define DEBUG 0

#if DEBUG
/* these are for debugging purposes only! */

/* define one and only one of these */
#define INJECT_FAULTS_1 0 /* cause bitmap_alloc_page to fail always */
#define INJECT_FAULTS_2 0 /* cause bitmap file to be kicked when first bit set*/
#define INJECT_FAULTS_3 0 /* treat bitmap file as kicked at init time */
#define INJECT_FAULTS_4 0 /* undef */
#define INJECT_FAULTS_5 0 /* undef */
#define INJECT_FAULTS_6 0

/* if these are defined, the driver will fail! debug only */
#define INJECT_FATAL_FAULT_1 0 /* fail kmalloc, causing bitmap_create to fail */
#define INJECT_FATAL_FAULT_2 0 /* undef */
#define INJECT_FATAL_FAULT_3 0 /* undef */
#endif

//#define DPRINTK PRINTK /* set this NULL to avoid verbose debug output */
#define DPRINTK(x...) do { } while(0)

#ifndef PRINTK
#  if DEBUG > 0
#    define PRINTK(x...) printk(KERN_DEBUG x)
#  else
#    define PRINTK(x...)
#  endif
#endif

static inline char * bmname(struct bitmap *bitmap)
{
	return bitmap->mddev ? mdname(bitmap->mddev) : "mdX";
}

#define WRITE_POOL_SIZE 256
/* mempool for queueing pending writes on the bitmap file */
static void *write_pool_alloc(gfp_t gfp_flags, void *data)
{
	return kmalloc(sizeof(struct page_list), gfp_flags);
}

static void write_pool_free(void *ptr, void *data)
{
	kfree(ptr);
}

/*
 * just a placeholder - calls kmalloc for bitmap pages
 */
static unsigned char *bitmap_alloc_page(struct bitmap *bitmap)
{
	unsigned char *page;

#ifdef INJECT_FAULTS_1
	page = NULL;
#else
	page = kmalloc(PAGE_SIZE, GFP_NOIO);
#endif
	if (!page)
		printk("%s: bitmap_alloc_page FAILED\n", bmname(bitmap));
	else
		PRINTK("%s: bitmap_alloc_page: allocated page at %p\n",
			bmname(bitmap), page);
	return page;
}

/*
 * for now just a placeholder -- just calls kfree for bitmap pages
 */
static void bitmap_free_page(struct bitmap *bitmap, unsigned char *page)
{
	PRINTK("%s: bitmap_free_page: free page %p\n", bmname(bitmap), page);
	kfree(page);
}

/*
 * check a page and, if necessary, allocate it (or hijack it if the alloc fails)
 *
 * 1) check to see if this page is allocated, if it's not then try to alloc
 * 2) if the alloc fails, set the page's hijacked flag so we'll use the
 *    page pointer directly as a counter
 *
 * if we find our page, we increment the page's refcount so that it stays
 * allocated while we're using it
 */
static int bitmap_checkpage(struct bitmap *bitmap, unsigned long page, int create)
{
	unsigned char *mappage;

	if (page >= bitmap->pages) {
		printk(KERN_ALERT
			"%s: invalid bitmap page request: %lu (> %lu)\n",
			bmname(bitmap), page, bitmap->pages-1);
		return -EINVAL;
	}


	if (bitmap->bp[page].hijacked) /* it's hijacked, don't try to alloc */
		return 0;

	if (bitmap->bp[page].map) /* page is already allocated, just return */
		return 0;

	if (!create)
		return -ENOENT;

	spin_unlock_irq(&bitmap->lock);

	/* this page has not been allocated yet */

	if ((mappage = bitmap_alloc_page(bitmap)) == NULL) {
		PRINTK("%s: bitmap map page allocation failed, hijacking\n",
			bmname(bitmap));
		/* failed - set the hijacked flag so that we can use the
		 * pointer as a counter */
		spin_lock_irq(&bitmap->lock);
		if (!bitmap->bp[page].map)
			bitmap->bp[page].hijacked = 1;
		goto out;
	}

	/* got a page */

	spin_lock_irq(&bitmap->lock);

	/* recheck the page */

	if (bitmap->bp[page].map || bitmap->bp[page].hijacked) {
		/* somebody beat us to getting the page */
		bitmap_free_page(bitmap, mappage);
		return 0;
	}

	/* no page was in place and we have one, so install it */

	memset(mappage, 0, PAGE_SIZE);
	bitmap->bp[page].map = mappage;
	bitmap->missing_pages--;
out:
	return 0;
}


/* if page is completely empty, put it back on the free list, or dealloc it */
/* if page was hijacked, unmark the flag so it might get alloced next time */
/* Note: lock should be held when calling this */
static void bitmap_checkfree(struct bitmap *bitmap, unsigned long page)
{
	char *ptr;

	if (bitmap->bp[page].count) /* page is still busy */
		return;

	/* page is no longer in use, it can be released */

	if (bitmap->bp[page].hijacked) { /* page was hijacked, undo this now */
		bitmap->bp[page].hijacked = 0;
		bitmap->bp[page].map = NULL;
		return;
	}

	/* normal case, free the page */

#if 0
/* actually ... let's not.  We will probably need the page again exactly when
 * memory is tight and we are flusing to disk
 */
	return;
#else
	ptr = bitmap->bp[page].map;
	bitmap->bp[page].map = NULL;
	bitmap->missing_pages++;
	bitmap_free_page(bitmap, ptr);
	return;
#endif
}


/*
 * bitmap file handling - read and write the bitmap file and its superblock
 */

/* copy the pathname of a file to a buffer */
char *file_path(struct file *file, char *buf, int count)
{
	struct dentry *d;
	struct vfsmount *v;

	if (!buf)
		return NULL;

	d = file->f_dentry;
	v = file->f_vfsmnt;

	buf = d_path(d, v, buf, count);

	return IS_ERR(buf) ? NULL : buf;
}

/*
 * basic page I/O operations
 */

/* IO operations when bitmap is stored near all superblocks */
static struct page *read_sb_page(mddev_t *mddev, long offset, unsigned long index)
{
	/* choose a good rdev and read the page from there */

	mdk_rdev_t *rdev;
	struct list_head *tmp;
	struct page *page = alloc_page(GFP_KERNEL);
	sector_t target;

	if (!page)
		return ERR_PTR(-ENOMEM);

	ITERATE_RDEV(mddev, rdev, tmp) {
		if (! test_bit(In_sync, &rdev->flags)
		    || test_bit(Faulty, &rdev->flags))
			continue;

		target = (rdev->sb_offset << 1) + offset + index * (PAGE_SIZE/512);

		if (sync_page_io(rdev->bdev, target, PAGE_SIZE, page, READ)) {
			page->index = index;
			return page;
		}
	}
	return ERR_PTR(-EIO);

}

static int write_sb_page(mddev_t *mddev, long offset, struct page *page, int wait)
{
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	ITERATE_RDEV(mddev, rdev, tmp)
		if (test_bit(In_sync, &rdev->flags)
		    && !test_bit(Faulty, &rdev->flags))
			md_super_write(mddev, rdev,
				       (rdev->sb_offset<<1) + offset
				       + page->index * (PAGE_SIZE/512),
				       PAGE_SIZE,
				       page);

	if (wait)
		md_super_wait(mddev);
	return 0;
}

/*
 * write out a page to a file
 */
static int write_page(struct bitmap *bitmap, struct page *page, int wait)
{
	int ret = -ENOMEM;

	if (bitmap->file == NULL)
		return write_sb_page(bitmap->mddev, bitmap->offset, page, wait);

	flush_dcache_page(page); /* make sure visible to anyone reading the file */

	if (wait)
		lock_page(page);
	else {
		if (TestSetPageLocked(page))
			return -EAGAIN; /* already locked */
		if (PageWriteback(page)) {
			unlock_page(page);
			return -EAGAIN;
		}
	}

	ret = page->mapping->a_ops->prepare_write(bitmap->file, page, 0, PAGE_SIZE);
	if (!ret)
		ret = page->mapping->a_ops->commit_write(bitmap->file, page, 0,
			PAGE_SIZE);
	if (ret) {
		unlock_page(page);
		return ret;
	}

	set_page_dirty(page); /* force it to be written out */

	if (!wait) {
		/* add to list to be waited for by daemon */
		struct page_list *item = mempool_alloc(bitmap->write_pool, GFP_NOIO);
		item->page = page;
		spin_lock(&bitmap->write_lock);
		list_add(&item->list, &bitmap->complete_pages);
		spin_unlock(&bitmap->write_lock);
		md_wakeup_thread(bitmap->writeback_daemon);
	}
	return write_one_page(page, wait);
}

/* read a page from a file, pinning it into cache, and return bytes_read */
static struct page *read_page(struct file *file, unsigned long index,
					unsigned long *bytes_read)
{
	struct inode *inode = file->f_mapping->host;
	struct page *page = NULL;
	loff_t isize = i_size_read(inode);
	unsigned long end_index = isize >> PAGE_SHIFT;

	PRINTK("read bitmap file (%dB @ %Lu)\n", (int)PAGE_SIZE,
			(unsigned long long)index << PAGE_SHIFT);

	page = read_cache_page(inode->i_mapping, index,
			(filler_t *)inode->i_mapping->a_ops->readpage, file);
	if (IS_ERR(page))
		goto out;
	wait_on_page_locked(page);
	if (!PageUptodate(page) || PageError(page)) {
		put_page(page);
		page = ERR_PTR(-EIO);
		goto out;
	}

	if (index > end_index) /* we have read beyond EOF */
		*bytes_read = 0;
	else if (index == end_index) /* possible short read */
		*bytes_read = isize & ~PAGE_MASK;
	else
		*bytes_read = PAGE_SIZE; /* got a full page */
out:
	if (IS_ERR(page))
		printk(KERN_ALERT "md: bitmap read error: (%dB @ %Lu): %ld\n",
			(int)PAGE_SIZE,
			(unsigned long long)index << PAGE_SHIFT,
			PTR_ERR(page));
	return page;
}

/*
 * bitmap file superblock operations
 */

/* update the event counter and sync the superblock to disk */
int bitmap_update_sb(struct bitmap *bitmap)
{
	bitmap_super_t *sb;
	unsigned long flags;

	if (!bitmap || !bitmap->mddev) /* no bitmap for this array */
		return 0;
	spin_lock_irqsave(&bitmap->lock, flags);
	if (!bitmap->sb_page) { /* no superblock */
		spin_unlock_irqrestore(&bitmap->lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&bitmap->lock, flags);
	sb = (bitmap_super_t *)kmap_atomic(bitmap->sb_page, KM_USER0);
	sb->events = cpu_to_le64(bitmap->mddev->events);
	if (!bitmap->mddev->degraded)
		sb->events_cleared = cpu_to_le64(bitmap->mddev->events);
	kunmap_atomic(sb, KM_USER0);
	return write_page(bitmap, bitmap->sb_page, 1);
}

/* print out the bitmap file superblock */
void bitmap_print_sb(struct bitmap *bitmap)
{
	bitmap_super_t *sb;

	if (!bitmap || !bitmap->sb_page)
		return;
	sb = (bitmap_super_t *)kmap_atomic(bitmap->sb_page, KM_USER0);
	printk(KERN_DEBUG "%s: bitmap file superblock:\n", bmname(bitmap));
	printk(KERN_DEBUG "         magic: %08x\n", le32_to_cpu(sb->magic));
	printk(KERN_DEBUG "       version: %d\n", le32_to_cpu(sb->version));
	printk(KERN_DEBUG "          uuid: %08x.%08x.%08x.%08x\n",
					*(__u32 *)(sb->uuid+0),
					*(__u32 *)(sb->uuid+4),
					*(__u32 *)(sb->uuid+8),
					*(__u32 *)(sb->uuid+12));
	printk(KERN_DEBUG "        events: %llu\n",
			(unsigned long long) le64_to_cpu(sb->events));
	printk(KERN_DEBUG "events cleared: %llu\n",
			(unsigned long long) le64_to_cpu(sb->events_cleared));
	printk(KERN_DEBUG "         state: %08x\n", le32_to_cpu(sb->state));
	printk(KERN_DEBUG "     chunksize: %d B\n", le32_to_cpu(sb->chunksize));
	printk(KERN_DEBUG "  daemon sleep: %ds\n", le32_to_cpu(sb->daemon_sleep));
	printk(KERN_DEBUG "     sync size: %llu KB\n",
			(unsigned long long)le64_to_cpu(sb->sync_size)/2);
	printk(KERN_DEBUG "max write behind: %d\n", le32_to_cpu(sb->write_behind));
	kunmap_atomic(sb, KM_USER0);
}

/* read the superblock from the bitmap file and initialize some bitmap fields */
static int bitmap_read_sb(struct bitmap *bitmap)
{
	char *reason = NULL;
	bitmap_super_t *sb;
	unsigned long chunksize, daemon_sleep, write_behind;
	unsigned long bytes_read;
	unsigned long long events;
	int err = -EINVAL;

	/* page 0 is the superblock, read it... */
	if (bitmap->file)
		bitmap->sb_page = read_page(bitmap->file, 0, &bytes_read);
	else {
		bitmap->sb_page = read_sb_page(bitmap->mddev, bitmap->offset, 0);
		bytes_read = PAGE_SIZE;
	}
	if (IS_ERR(bitmap->sb_page)) {
		err = PTR_ERR(bitmap->sb_page);
		bitmap->sb_page = NULL;
		return err;
	}

	sb = (bitmap_super_t *)kmap_atomic(bitmap->sb_page, KM_USER0);

	if (bytes_read < sizeof(*sb)) { /* short read */
		printk(KERN_INFO "%s: bitmap file superblock truncated\n",
			bmname(bitmap));
		err = -ENOSPC;
		goto out;
	}

	chunksize = le32_to_cpu(sb->chunksize);
	daemon_sleep = le32_to_cpu(sb->daemon_sleep);
	write_behind = le32_to_cpu(sb->write_behind);

	/* verify that the bitmap-specific fields are valid */
	if (sb->magic != cpu_to_le32(BITMAP_MAGIC))
		reason = "bad magic";
	else if (le32_to_cpu(sb->version) < BITMAP_MAJOR_LO ||
		 le32_to_cpu(sb->version) > BITMAP_MAJOR_HI)
		reason = "unrecognized superblock version";
	else if (chunksize < PAGE_SIZE)
		reason = "bitmap chunksize too small";
	else if ((1 << ffz(~chunksize)) != chunksize)
		reason = "bitmap chunksize not a power of 2";
	else if (daemon_sleep < 1 || daemon_sleep > MAX_SCHEDULE_TIMEOUT / HZ)
		reason = "daemon sleep period out of range";
	else if (write_behind > COUNTER_MAX)
		reason = "write-behind limit out of range (0 - 16383)";
	if (reason) {
		printk(KERN_INFO "%s: invalid bitmap file superblock: %s\n",
			bmname(bitmap), reason);
		goto out;
	}

	/* keep the array size field of the bitmap superblock up to date */
	sb->sync_size = cpu_to_le64(bitmap->mddev->resync_max_sectors);

	if (!bitmap->mddev->persistent)
		goto success;

	/*
	 * if we have a persistent array superblock, compare the
	 * bitmap's UUID and event counter to the mddev's
	 */
	if (memcmp(sb->uuid, bitmap->mddev->uuid, 16)) {
		printk(KERN_INFO "%s: bitmap superblock UUID mismatch\n",
			bmname(bitmap));
		goto out;
	}
	events = le64_to_cpu(sb->events);
	if (events < bitmap->mddev->events) {
		printk(KERN_INFO "%s: bitmap file is out of date (%llu < %llu) "
			"-- forcing full recovery\n", bmname(bitmap), events,
			(unsigned long long) bitmap->mddev->events);
		sb->state |= BITMAP_STALE;
	}
success:
	/* assign fields using values from superblock */
	bitmap->chunksize = chunksize;
	bitmap->daemon_sleep = daemon_sleep;
	bitmap->daemon_lastrun = jiffies;
	bitmap->max_write_behind = write_behind;
	bitmap->flags |= sb->state;
	if (le32_to_cpu(sb->version) == BITMAP_MAJOR_HOSTENDIAN)
		bitmap->flags |= BITMAP_HOSTENDIAN;
	bitmap->events_cleared = le64_to_cpu(sb->events_cleared);
	if (sb->state & BITMAP_STALE)
		bitmap->events_cleared = bitmap->mddev->events;
	err = 0;
out:
	kunmap_atomic(sb, KM_USER0);
	if (err)
		bitmap_print_sb(bitmap);
	return err;
}

enum bitmap_mask_op {
	MASK_SET,
	MASK_UNSET
};

/* record the state of the bitmap in the superblock */
static void bitmap_mask_state(struct bitmap *bitmap, enum bitmap_state bits,
				enum bitmap_mask_op op)
{
	bitmap_super_t *sb;
	unsigned long flags;

	spin_lock_irqsave(&bitmap->lock, flags);
	if (!bitmap || !bitmap->sb_page) { /* can't set the state */
		spin_unlock_irqrestore(&bitmap->lock, flags);
		return;
	}
	spin_unlock_irqrestore(&bitmap->lock, flags);
	sb = (bitmap_super_t *)kmap_atomic(bitmap->sb_page, KM_USER0);
	switch (op) {
		case MASK_SET: sb->state |= bits;
				break;
		case MASK_UNSET: sb->state &= ~bits;
				break;
		default: BUG();
	}
	kunmap_atomic(sb, KM_USER0);
}

/*
 * general bitmap file operations
 */

/* calculate the index of the page that contains this bit */
static inline unsigned long file_page_index(unsigned long chunk)
{
	return CHUNK_BIT_OFFSET(chunk) >> PAGE_BIT_SHIFT;
}

/* calculate the (bit) offset of this bit within a page */
static inline unsigned long file_page_offset(unsigned long chunk)
{
	return CHUNK_BIT_OFFSET(chunk) & (PAGE_BITS - 1);
}

/*
 * return a pointer to the page in the filemap that contains the given bit
 *
 * this lookup is complicated by the fact that the bitmap sb might be exactly
 * 1 page (e.g., x86) or less than 1 page -- so the bitmap might start on page
 * 0 or page 1
 */
static inline struct page *filemap_get_page(struct bitmap *bitmap,
					unsigned long chunk)
{
	return bitmap->filemap[file_page_index(chunk) - file_page_index(0)];
}


static void bitmap_file_unmap(struct bitmap *bitmap)
{
	struct page **map, *sb_page;
	unsigned long *attr;
	int pages;
	unsigned long flags;

	spin_lock_irqsave(&bitmap->lock, flags);
	map = bitmap->filemap;
	bitmap->filemap = NULL;
	attr = bitmap->filemap_attr;
	bitmap->filemap_attr = NULL;
	pages = bitmap->file_pages;
	bitmap->file_pages = 0;
	sb_page = bitmap->sb_page;
	bitmap->sb_page = NULL;
	spin_unlock_irqrestore(&bitmap->lock, flags);

	while (pages--)
		if (map[pages]->index != 0) /* 0 is sb_page, release it below */
			put_page(map[pages]);
	kfree(map);
	kfree(attr);

	safe_put_page(sb_page);
}

static void bitmap_stop_daemon(struct bitmap *bitmap);

/* dequeue the next item in a page list -- don't call from irq context */
static struct page_list *dequeue_page(struct bitmap *bitmap)
{
	struct page_list *item = NULL;
	struct list_head *head = &bitmap->complete_pages;

	spin_lock(&bitmap->write_lock);
	if (list_empty(head))
		goto out;
	item = list_entry(head->prev, struct page_list, list);
	list_del(head->prev);
out:
	spin_unlock(&bitmap->write_lock);
	return item;
}

static void drain_write_queues(struct bitmap *bitmap)
{
	struct page_list *item;

	while ((item = dequeue_page(bitmap))) {
		/* don't bother to wait */
		mempool_free(item, bitmap->write_pool);
	}

	wake_up(&bitmap->write_wait);
}

static void bitmap_file_put(struct bitmap *bitmap)
{
	struct file *file;
	unsigned long flags;

	spin_lock_irqsave(&bitmap->lock, flags);
	file = bitmap->file;
	bitmap->file = NULL;
	spin_unlock_irqrestore(&bitmap->lock, flags);

	bitmap_stop_daemon(bitmap);

	drain_write_queues(bitmap);

	bitmap_file_unmap(bitmap);

	if (file)
		fput(file);
}


/*
 * bitmap_file_kick - if an error occurs while manipulating the bitmap file
 * then it is no longer reliable, so we stop using it and we mark the file
 * as failed in the superblock
 */
static void bitmap_file_kick(struct bitmap *bitmap)
{
	char *path, *ptr = NULL;

	bitmap_mask_state(bitmap, BITMAP_STALE, MASK_SET);
	bitmap_update_sb(bitmap);

	if (bitmap->file) {
		path = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (path)
			ptr = file_path(bitmap->file, path, PAGE_SIZE);

		printk(KERN_ALERT "%s: kicking failed bitmap file %s from array!\n",
		       bmname(bitmap), ptr ? ptr : "");

		kfree(path);
	}

	bitmap_file_put(bitmap);

	return;
}

enum bitmap_page_attr {
	BITMAP_PAGE_DIRTY = 0, // there are set bits that need to be synced
	BITMAP_PAGE_CLEAN = 1, // there are bits that might need to be cleared
	BITMAP_PAGE_NEEDWRITE=2, // there are cleared bits that need to be synced
};

static inline void set_page_attr(struct bitmap *bitmap, struct page *page,
				enum bitmap_page_attr attr)
{
	__set_bit((page->index<<2) + attr, bitmap->filemap_attr);
}

static inline void clear_page_attr(struct bitmap *bitmap, struct page *page,
				enum bitmap_page_attr attr)
{
	__clear_bit((page->index<<2) + attr, bitmap->filemap_attr);
}

static inline unsigned long test_page_attr(struct bitmap *bitmap, struct page *page,
					   enum bitmap_page_attr attr)
{
	return test_bit((page->index<<2) + attr, bitmap->filemap_attr);
}

/*
 * bitmap_file_set_bit -- called before performing a write to the md device
 * to set (and eventually sync) a particular bit in the bitmap file
 *
 * we set the bit immediately, then we record the page number so that
 * when an unplug occurs, we can flush the dirty pages out to disk
 */
static void bitmap_file_set_bit(struct bitmap *bitmap, sector_t block)
{
	unsigned long bit;
	struct page *page;
	void *kaddr;
	unsigned long chunk = block >> CHUNK_BLOCK_SHIFT(bitmap);

	if (!bitmap->filemap) {
		return;
	}

	page = filemap_get_page(bitmap, chunk);
	bit = file_page_offset(chunk);

 	/* set the bit */
	kaddr = kmap_atomic(page, KM_USER0);
	if (bitmap->flags & BITMAP_HOSTENDIAN)
		set_bit(bit, kaddr);
	else
		ext2_set_bit(bit, kaddr);
	kunmap_atomic(kaddr, KM_USER0);
	PRINTK("set file bit %lu page %lu\n", bit, page->index);

	/* record page number so it gets flushed to disk when unplug occurs */
	set_page_attr(bitmap, page, BITMAP_PAGE_DIRTY);

}

/* this gets called when the md device is ready to unplug its underlying
 * (slave) device queues -- before we let any writes go down, we need to
 * sync the dirty pages of the bitmap file to disk */
int bitmap_unplug(struct bitmap *bitmap)
{
	unsigned long i, flags;
	int dirty, need_write;
	struct page *page;
	int wait = 0;
	int err;

	if (!bitmap)
		return 0;

	/* look at each page to see if there are any set bits that need to be
	 * flushed out to disk */
	for (i = 0; i < bitmap->file_pages; i++) {
		spin_lock_irqsave(&bitmap->lock, flags);
		if (!bitmap->filemap) {
			spin_unlock_irqrestore(&bitmap->lock, flags);
			return 0;
		}
		page = bitmap->filemap[i];
		dirty = test_page_attr(bitmap, page, BITMAP_PAGE_DIRTY);
		need_write = test_page_attr(bitmap, page, BITMAP_PAGE_NEEDWRITE);
		clear_page_attr(bitmap, page, BITMAP_PAGE_DIRTY);
		clear_page_attr(bitmap, page, BITMAP_PAGE_NEEDWRITE);
		if (dirty)
			wait = 1;
		spin_unlock_irqrestore(&bitmap->lock, flags);

		if (dirty | need_write) {
			err = write_page(bitmap, page, 0);
			if (err == -EAGAIN) {
				if (dirty)
					err = write_page(bitmap, page, 1);
				else
					err = 0;
			}
			if (err)
				return 1;
		}
	}
	if (wait) { /* if any writes were performed, we need to wait on them */
		if (bitmap->file) {
			spin_lock_irq(&bitmap->write_lock);
			wait_event_lock_irq(bitmap->write_wait,
					    list_empty(&bitmap->complete_pages), bitmap->write_lock,
					    wake_up_process(bitmap->writeback_daemon->tsk));
			spin_unlock_irq(&bitmap->write_lock);
		} else
			md_super_wait(bitmap->mddev);
	}
	return 0;
}

static void bitmap_set_memory_bits(struct bitmap *bitmap, sector_t offset, int needed);
/* * bitmap_init_from_disk -- called at bitmap_create time to initialize
 * the in-memory bitmap from the on-disk bitmap -- also, sets up the
 * memory mapping of the bitmap file
 * Special cases:
 *   if there's no bitmap file, or if the bitmap file had been
 *   previously kicked from the array, we mark all the bits as
 *   1's in order to cause a full resync.
 *
 * We ignore all bits for sectors that end earlier than 'start'.
 * This is used when reading an out-of-date bitmap...
 */
static int bitmap_init_from_disk(struct bitmap *bitmap, sector_t start)
{
	unsigned long i, chunks, index, oldindex, bit;
	struct page *page = NULL, *oldpage = NULL;
	unsigned long num_pages, bit_cnt = 0;
	struct file *file;
	unsigned long bytes, offset, dummy;
	int outofdate;
	int ret = -ENOSPC;
	void *paddr;

	chunks = bitmap->chunks;
	file = bitmap->file;

	BUG_ON(!file && !bitmap->offset);

#ifdef INJECT_FAULTS_3
	outofdate = 1;
#else
	outofdate = bitmap->flags & BITMAP_STALE;
#endif
	if (outofdate)
		printk(KERN_INFO "%s: bitmap file is out of date, doing full "
			"recovery\n", bmname(bitmap));

	bytes = (chunks + 7) / 8;

	num_pages = (bytes + sizeof(bitmap_super_t) + PAGE_SIZE - 1) / PAGE_SIZE;

	if (file && i_size_read(file->f_mapping->host) < bytes + sizeof(bitmap_super_t)) {
		printk(KERN_INFO "%s: bitmap file too short %lu < %lu\n",
			bmname(bitmap),
			(unsigned long) i_size_read(file->f_mapping->host),
			bytes + sizeof(bitmap_super_t));
		goto out;
	}

	ret = -ENOMEM;

	bitmap->filemap = kmalloc(sizeof(struct page *) * num_pages, GFP_KERNEL);
	if (!bitmap->filemap)
		goto out;

	/* We need 4 bits per page, rounded up to a multiple of sizeof(unsigned long) */
	bitmap->filemap_attr = kzalloc(
		(((num_pages*4/8)+sizeof(unsigned long))
		 /sizeof(unsigned long))
		*sizeof(unsigned long),
		GFP_KERNEL);
	if (!bitmap->filemap_attr)
		goto out;

	oldindex = ~0L;

	for (i = 0; i < chunks; i++) {
		int b;
		index = file_page_index(i);
		bit = file_page_offset(i);
		if (index != oldindex) { /* this is a new page, read it in */
			/* unmap the old page, we're done with it */
			if (index == 0) {
				/*
				 * if we're here then the superblock page
				 * contains some bits (PAGE_SIZE != sizeof sb)
				 * we've already read it in, so just use it
				 */
				page = bitmap->sb_page;
				offset = sizeof(bitmap_super_t);
			} else if (file) {
				page = read_page(file, index, &dummy);
				offset = 0;
			} else {
				page = read_sb_page(bitmap->mddev, bitmap->offset, index);
				offset = 0;
			}
			if (IS_ERR(page)) { /* read error */
				ret = PTR_ERR(page);
				goto out;
			}

			oldindex = index;
			oldpage = page;

			if (outofdate) {
				/*
				 * if bitmap is out of date, dirty the
			 	 * whole page and write it out
				 */
				paddr = kmap_atomic(page, KM_USER0);
				memset(paddr + offset, 0xff,
				       PAGE_SIZE - offset);
				kunmap_atomic(paddr, KM_USER0);
				ret = write_page(bitmap, page, 1);
				if (ret) {
					/* release, page not in filemap yet */
					put_page(page);
					goto out;
				}
			}

			bitmap->filemap[bitmap->file_pages++] = page;
		}
		paddr = kmap_atomic(page, KM_USER0);
		if (bitmap->flags & BITMAP_HOSTENDIAN)
			b = test_bit(bit, paddr);
		else
			b = ext2_test_bit(bit, paddr);
		kunmap_atomic(paddr, KM_USER0);
		if (b) {
			/* if the disk bit is set, set the memory bit */
			bitmap_set_memory_bits(bitmap, i << CHUNK_BLOCK_SHIFT(bitmap),
					       ((i+1) << (CHUNK_BLOCK_SHIFT(bitmap)) >= start)
				);
			bit_cnt++;
			set_page_attr(bitmap, page, BITMAP_PAGE_CLEAN);
		}
	}

 	/* everything went OK */
	ret = 0;
	bitmap_mask_state(bitmap, BITMAP_STALE, MASK_UNSET);

	if (bit_cnt) { /* Kick recovery if any bits were set */
		set_bit(MD_RECOVERY_NEEDED, &bitmap->mddev->recovery);
		md_wakeup_thread(bitmap->mddev->thread);
	}

out:
	printk(KERN_INFO "%s: bitmap initialized from disk: "
		"read %lu/%lu pages, set %lu bits, status: %d\n",
		bmname(bitmap), bitmap->file_pages, num_pages, bit_cnt, ret);

	return ret;
}

void bitmap_write_all(struct bitmap *bitmap)
{
	/* We don't actually write all bitmap blocks here,
	 * just flag them as needing to be written
	 */
	int i;

	for (i=0; i < bitmap->file_pages; i++)
		set_page_attr(bitmap, bitmap->filemap[i],
			      BITMAP_PAGE_NEEDWRITE);
}


static void bitmap_count_page(struct bitmap *bitmap, sector_t offset, int inc)
{
	sector_t chunk = offset >> CHUNK_BLOCK_SHIFT(bitmap);
	unsigned long page = chunk >> PAGE_COUNTER_SHIFT;
	bitmap->bp[page].count += inc;
/*
	if (page == 0) printk("count page 0, offset %llu: %d gives %d\n",
			      (unsigned long long)offset, inc, bitmap->bp[page].count);
*/
	bitmap_checkfree(bitmap, page);
}
static bitmap_counter_t *bitmap_get_counter(struct bitmap *bitmap,
					    sector_t offset, int *blocks,
					    int create);

/*
 * bitmap daemon -- periodically wakes up to clean bits and flush pages
 *			out to disk
 */

int bitmap_daemon_work(struct bitmap *bitmap)
{
	unsigned long j;
	unsigned long flags;
	struct page *page = NULL, *lastpage = NULL;
	int err = 0;
	int blocks;
	void *paddr;

	if (bitmap == NULL)
		return 0;
	if (time_before(jiffies, bitmap->daemon_lastrun + bitmap->daemon_sleep*HZ))
		return 0;
	bitmap->daemon_lastrun = jiffies;

	for (j = 0; j < bitmap->chunks; j++) {
		bitmap_counter_t *bmc;
		spin_lock_irqsave(&bitmap->lock, flags);
		if (!bitmap->filemap) {
			/* error or shutdown */
			spin_unlock_irqrestore(&bitmap->lock, flags);
			break;
		}

		page = filemap_get_page(bitmap, j);

		if (page != lastpage) {
			/* skip this page unless it's marked as needing cleaning */
			if (!test_page_attr(bitmap, page, BITMAP_PAGE_CLEAN)) {
				int need_write = test_page_attr(bitmap, page,
								BITMAP_PAGE_NEEDWRITE);
				if (need_write)
					clear_page_attr(bitmap, page, BITMAP_PAGE_NEEDWRITE);

				spin_unlock_irqrestore(&bitmap->lock, flags);
				if (need_write) {
					switch (write_page(bitmap, page, 0)) {
					case -EAGAIN:
						set_page_attr(bitmap, page, BITMAP_PAGE_NEEDWRITE);
						break;
					case 0:
						break;
					default:
						bitmap_file_kick(bitmap);
					}
				}
				continue;
			}

			/* grab the new page, sync and release the old */
			if (lastpage != NULL) {
				if (test_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE)) {
					clear_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE);
					spin_unlock_irqrestore(&bitmap->lock, flags);
					err = write_page(bitmap, lastpage, 0);
					if (err == -EAGAIN) {
						err = 0;
						set_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE);
					}
				} else {
					set_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE);
					spin_unlock_irqrestore(&bitmap->lock, flags);
				}
				if (err)
					bitmap_file_kick(bitmap);
			} else
				spin_unlock_irqrestore(&bitmap->lock, flags);
			lastpage = page;
/*
			printk("bitmap clean at page %lu\n", j);
*/
			spin_lock_irqsave(&bitmap->lock, flags);
			clear_page_attr(bitmap, page, BITMAP_PAGE_CLEAN);
		}
		bmc = bitmap_get_counter(bitmap, j << CHUNK_BLOCK_SHIFT(bitmap),
					&blocks, 0);
		if (bmc) {
/*
  if (j < 100) printk("bitmap: j=%lu, *bmc = 0x%x\n", j, *bmc);
*/
			if (*bmc == 2) {
				*bmc=1; /* maybe clear the bit next time */
				set_page_attr(bitmap, page, BITMAP_PAGE_CLEAN);
			} else if (*bmc == 1) {
				/* we can clear the bit */
				*bmc = 0;
				bitmap_count_page(bitmap, j << CHUNK_BLOCK_SHIFT(bitmap),
						  -1);

				/* clear the bit */
				paddr = kmap_atomic(page, KM_USER0);
				if (bitmap->flags & BITMAP_HOSTENDIAN)
					clear_bit(file_page_offset(j), paddr);
				else
					ext2_clear_bit(file_page_offset(j), paddr);
				kunmap_atomic(paddr, KM_USER0);
			}
		}
		spin_unlock_irqrestore(&bitmap->lock, flags);
	}

	/* now sync the final page */
	if (lastpage != NULL) {
		spin_lock_irqsave(&bitmap->lock, flags);
		if (test_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE)) {
			clear_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE);
			spin_unlock_irqrestore(&bitmap->lock, flags);
			err = write_page(bitmap, lastpage, 0);
			if (err == -EAGAIN) {
				set_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE);
				err = 0;
			}
		} else {
			set_page_attr(bitmap, lastpage, BITMAP_PAGE_NEEDWRITE);
			spin_unlock_irqrestore(&bitmap->lock, flags);
		}
	}

	return err;
}

static void daemon_exit(struct bitmap *bitmap, mdk_thread_t **daemon)
{
	mdk_thread_t *dmn;
	unsigned long flags;

	/* if no one is waiting on us, we'll free the md thread struct
	 * and exit, otherwise we let the waiter clean things up */
	spin_lock_irqsave(&bitmap->lock, flags);
	if ((dmn = *daemon)) { /* no one is waiting, cleanup and exit */
		*daemon = NULL;
		spin_unlock_irqrestore(&bitmap->lock, flags);
		kfree(dmn);
		complete_and_exit(NULL, 0); /* do_exit not exported */
	}
	spin_unlock_irqrestore(&bitmap->lock, flags);
}

static void bitmap_writeback_daemon(mddev_t *mddev)
{
	struct bitmap *bitmap = mddev->bitmap;
	struct page *page;
	struct page_list *item;
	int err = 0;

	if (signal_pending(current)) {
		printk(KERN_INFO
		       "%s: bitmap writeback daemon got signal, exiting...\n",
		       bmname(bitmap));
		err = -EINTR;
		goto out;
	}
	if (bitmap == NULL)
		/* about to be stopped. */
		return;

	PRINTK("%s: bitmap writeback daemon woke up...\n", bmname(bitmap));
	/* wait on bitmap page writebacks */
	while ((item = dequeue_page(bitmap))) {
		page = item->page;
		mempool_free(item, bitmap->write_pool);
		PRINTK("wait on page writeback: %p\n", page);
		wait_on_page_writeback(page);
		PRINTK("finished page writeback: %p\n", page);

		err = PageError(page);
		if (err) {
			printk(KERN_WARNING "%s: bitmap file writeback "
			       "failed (page %lu): %d\n",
			       bmname(bitmap), page->index, err);
			bitmap_file_kick(bitmap);
			goto out;
		}
	}
 out:
	wake_up(&bitmap->write_wait);
	if (err) {
		printk(KERN_INFO "%s: bitmap writeback daemon exiting (%d)\n",
		       bmname(bitmap), err);
		daemon_exit(bitmap, &bitmap->writeback_daemon);
	}
}

static mdk_thread_t *bitmap_start_daemon(struct bitmap *bitmap,
				void (*func)(mddev_t *), char *name)
{
	mdk_thread_t *daemon;
	char namebuf[32];

#ifdef INJECT_FATAL_FAULT_2
	daemon = NULL;
#else
	sprintf(namebuf, "%%s_%s", name);
	daemon = md_register_thread(func, bitmap->mddev, namebuf);
#endif
	if (!daemon) {
		printk(KERN_ERR "%s: failed to start bitmap daemon\n",
			bmname(bitmap));
		return ERR_PTR(-ECHILD);
	}

	md_wakeup_thread(daemon); /* start it running */

	PRINTK("%s: %s daemon (pid %d) started...\n",
		bmname(bitmap), name, daemon->tsk->pid);

	return daemon;
}

static void bitmap_stop_daemon(struct bitmap *bitmap)
{
	/* the daemon can't stop itself... it'll just exit instead... */
	if (bitmap->writeback_daemon && ! IS_ERR(bitmap->writeback_daemon) &&
	    current->pid != bitmap->writeback_daemon->tsk->pid) {
		mdk_thread_t *daemon;
		unsigned long flags;

		spin_lock_irqsave(&bitmap->lock, flags);
		daemon = bitmap->writeback_daemon;
		bitmap->writeback_daemon = NULL;
		spin_unlock_irqrestore(&bitmap->lock, flags);
		if (daemon && ! IS_ERR(daemon))
			md_unregister_thread(daemon); /* destroy the thread */
	}
}

static bitmap_counter_t *bitmap_get_counter(struct bitmap *bitmap,
					    sector_t offset, int *blocks,
					    int create)
{
	/* If 'create', we might release the lock and reclaim it.
	 * The lock must have been taken with interrupts enabled.
	 * If !create, we don't release the lock.
	 */
	sector_t chunk = offset >> CHUNK_BLOCK_SHIFT(bitmap);
	unsigned long page = chunk >> PAGE_COUNTER_SHIFT;
	unsigned long pageoff = (chunk & PAGE_COUNTER_MASK) << COUNTER_BYTE_SHIFT;
	sector_t csize;

	if (bitmap_checkpage(bitmap, page, create) < 0) {
		csize = ((sector_t)1) << (CHUNK_BLOCK_SHIFT(bitmap));
		*blocks = csize - (offset & (csize- 1));
		return NULL;
	}
	/* now locked ... */

	if (bitmap->bp[page].hijacked) { /* hijacked pointer */
		/* should we use the first or second counter field
		 * of the hijacked pointer? */
		int hi = (pageoff > PAGE_COUNTER_MASK);
		csize = ((sector_t)1) << (CHUNK_BLOCK_SHIFT(bitmap) +
					  PAGE_COUNTER_SHIFT - 1);
		*blocks = csize - (offset & (csize- 1));
		return  &((bitmap_counter_t *)
			  &bitmap->bp[page].map)[hi];
	} else { /* page is allocated */
		csize = ((sector_t)1) << (CHUNK_BLOCK_SHIFT(bitmap));
		*blocks = csize - (offset & (csize- 1));
		return (bitmap_counter_t *)
			&(bitmap->bp[page].map[pageoff]);
	}
}

int bitmap_startwrite(struct bitmap *bitmap, sector_t offset, unsigned long sectors, int behind)
{
	if (!bitmap) return 0;

	if (behind) {
		atomic_inc(&bitmap->behind_writes);
		PRINTK(KERN_DEBUG "inc write-behind count %d/%d\n",
		  atomic_read(&bitmap->behind_writes), bitmap->max_write_behind);
	}

	while (sectors) {
		int blocks;
		bitmap_counter_t *bmc;

		spin_lock_irq(&bitmap->lock);
		bmc = bitmap_get_counter(bitmap, offset, &blocks, 1);
		if (!bmc) {
			spin_unlock_irq(&bitmap->lock);
			return 0;
		}

		if (unlikely((*bmc & COUNTER_MAX) == COUNTER_MAX)) {
			DEFINE_WAIT(__wait);
			/* note that it is safe to do the prepare_to_wait
			 * after the test as long as we do it before dropping
			 * the spinlock.
			 */
			prepare_to_wait(&bitmap->overflow_wait, &__wait,
					TASK_UNINTERRUPTIBLE);
			spin_unlock_irq(&bitmap->lock);
			bitmap->mddev->queue
				->unplug_fn(bitmap->mddev->queue);
			schedule();
			finish_wait(&bitmap->overflow_wait, &__wait);
			continue;
		}

		switch(*bmc) {
		case 0:
			bitmap_file_set_bit(bitmap, offset);
			bitmap_count_page(bitmap,offset, 1);
			blk_plug_device(bitmap->mddev->queue);
			/* fall through */
		case 1:
			*bmc = 2;
		}

		(*bmc)++;

		spin_unlock_irq(&bitmap->lock);

		offset += blocks;
		if (sectors > blocks)
			sectors -= blocks;
		else sectors = 0;
	}
	return 0;
}

void bitmap_endwrite(struct bitmap *bitmap, sector_t offset, unsigned long sectors,
		     int success, int behind)
{
	if (!bitmap) return;
	if (behind) {
		atomic_dec(&bitmap->behind_writes);
		PRINTK(KERN_DEBUG "dec write-behind count %d/%d\n",
		  atomic_read(&bitmap->behind_writes), bitmap->max_write_behind);
	}

	while (sectors) {
		int blocks;
		unsigned long flags;
		bitmap_counter_t *bmc;

		spin_lock_irqsave(&bitmap->lock, flags);
		bmc = bitmap_get_counter(bitmap, offset, &blocks, 0);
		if (!bmc) {
			spin_unlock_irqrestore(&bitmap->lock, flags);
			return;
		}

		if (!success && ! (*bmc & NEEDED_MASK))
			*bmc |= NEEDED_MASK;

		if ((*bmc & COUNTER_MAX) == COUNTER_MAX)
			wake_up(&bitmap->overflow_wait);

		(*bmc)--;
		if (*bmc <= 2) {
			set_page_attr(bitmap,
				      filemap_get_page(bitmap, offset >> CHUNK_BLOCK_SHIFT(bitmap)),
				      BITMAP_PAGE_CLEAN);
		}
		spin_unlock_irqrestore(&bitmap->lock, flags);
		offset += blocks;
		if (sectors > blocks)
			sectors -= blocks;
		else sectors = 0;
	}
}

int bitmap_start_sync(struct bitmap *bitmap, sector_t offset, int *blocks,
			int degraded)
{
	bitmap_counter_t *bmc;
	int rv;
	if (bitmap == NULL) {/* FIXME or bitmap set as 'failed' */
		*blocks = 1024;
		return 1; /* always resync if no bitmap */
	}
	spin_lock_irq(&bitmap->lock);
	bmc = bitmap_get_counter(bitmap, offset, blocks, 0);
	rv = 0;
	if (bmc) {
		/* locked */
		if (RESYNC(*bmc))
			rv = 1;
		else if (NEEDED(*bmc)) {
			rv = 1;
			if (!degraded) { /* don't set/clear bits if degraded */
				*bmc |= RESYNC_MASK;
				*bmc &= ~NEEDED_MASK;
			}
		}
	}
	spin_unlock_irq(&bitmap->lock);
	return rv;
}

void bitmap_end_sync(struct bitmap *bitmap, sector_t offset, int *blocks, int aborted)
{
	bitmap_counter_t *bmc;
	unsigned long flags;
/*
	if (offset == 0) printk("bitmap_end_sync 0 (%d)\n", aborted);
*/	if (bitmap == NULL) {
		*blocks = 1024;
		return;
	}
	spin_lock_irqsave(&bitmap->lock, flags);
	bmc = bitmap_get_counter(bitmap, offset, blocks, 0);
	if (bmc == NULL)
		goto unlock;
	/* locked */
/*
	if (offset == 0) printk("bitmap_end sync found 0x%x, blocks %d\n", *bmc, *blocks);
*/
	if (RESYNC(*bmc)) {
		*bmc &= ~RESYNC_MASK;

		if (!NEEDED(*bmc) && aborted)
			*bmc |= NEEDED_MASK;
		else {
			if (*bmc <= 2) {
				set_page_attr(bitmap,
					      filemap_get_page(bitmap, offset >> CHUNK_BLOCK_SHIFT(bitmap)),
					      BITMAP_PAGE_CLEAN);
			}
		}
	}
 unlock:
	spin_unlock_irqrestore(&bitmap->lock, flags);
}

void bitmap_close_sync(struct bitmap *bitmap)
{
	/* Sync has finished, and any bitmap chunks that weren't synced
	 * properly have been aborted.  It remains to us to clear the
	 * RESYNC bit wherever it is still on
	 */
	sector_t sector = 0;
	int blocks;
	if (!bitmap) return;
	while (sector < bitmap->mddev->resync_max_sectors) {
		bitmap_end_sync(bitmap, sector, &blocks, 0);
/*
		if (sector < 500) printk("bitmap_close_sync: sec %llu blks %d\n",
					 (unsigned long long)sector, blocks);
*/		sector += blocks;
	}
}

static void bitmap_set_memory_bits(struct bitmap *bitmap, sector_t offset, int needed)
{
	/* For each chunk covered by any of these sectors, set the
	 * counter to 1 and set resync_needed.  They should all
	 * be 0 at this point
	 */

	int secs;
	bitmap_counter_t *bmc;
	spin_lock_irq(&bitmap->lock);
	bmc = bitmap_get_counter(bitmap, offset, &secs, 1);
	if (!bmc) {
		spin_unlock_irq(&bitmap->lock);
		return;
	}
	if (! *bmc) {
		struct page *page;
		*bmc = 1 | (needed?NEEDED_MASK:0);
		bitmap_count_page(bitmap, offset, 1);
		page = filemap_get_page(bitmap, offset >> CHUNK_BLOCK_SHIFT(bitmap));
		set_page_attr(bitmap, page, BITMAP_PAGE_CLEAN);
	}
	spin_unlock_irq(&bitmap->lock);

}

/*
 * flush out any pending updates
 */
void bitmap_flush(mddev_t *mddev)
{
	struct bitmap *bitmap = mddev->bitmap;
	int sleep;

	if (!bitmap) /* there was no bitmap */
		return;

	/* run the daemon_work three time to ensure everything is flushed
	 * that can be
	 */
	sleep = bitmap->daemon_sleep;
	bitmap->daemon_sleep = 0;
	bitmap_daemon_work(bitmap);
	bitmap_daemon_work(bitmap);
	bitmap_daemon_work(bitmap);
	bitmap->daemon_sleep = sleep;
	bitmap_update_sb(bitmap);
}

/*
 * free memory that was allocated
 */
static void bitmap_free(struct bitmap *bitmap)
{
	unsigned long k, pages;
	struct bitmap_page *bp;

	if (!bitmap) /* there was no bitmap */
		return;

	/* release the bitmap file and kill the daemon */
	bitmap_file_put(bitmap);

	bp = bitmap->bp;
	pages = bitmap->pages;

	/* free all allocated memory */

	mempool_destroy(bitmap->write_pool);

	if (bp) /* deallocate the page memory */
		for (k = 0; k < pages; k++)
			if (bp[k].map && !bp[k].hijacked)
				kfree(bp[k].map);
	kfree(bp);
	kfree(bitmap);
}
void bitmap_destroy(mddev_t *mddev)
{
	struct bitmap *bitmap = mddev->bitmap;

	if (!bitmap) /* there was no bitmap */
		return;

	mddev->bitmap = NULL; /* disconnect from the md device */
	if (mddev->thread)
		mddev->thread->timeout = MAX_SCHEDULE_TIMEOUT;

	bitmap_free(bitmap);
}

/*
 * initialize the bitmap structure
 * if this returns an error, bitmap_destroy must be called to do clean up
 */
int bitmap_create(mddev_t *mddev)
{
	struct bitmap *bitmap;
	unsigned long blocks = mddev->resync_max_sectors;
	unsigned long chunks;
	unsigned long pages;
	struct file *file = mddev->bitmap_file;
	int err;
	sector_t start;

	BUG_ON(sizeof(bitmap_super_t) != 256);

	if (!file && !mddev->bitmap_offset) /* bitmap disabled, nothing to do */
		return 0;

	BUG_ON(file && mddev->bitmap_offset);

	bitmap = kzalloc(sizeof(*bitmap), GFP_KERNEL);
	if (!bitmap)
		return -ENOMEM;

	spin_lock_init(&bitmap->lock);
	init_waitqueue_head(&bitmap->overflow_wait);
	bitmap->mddev = mddev;

	spin_lock_init(&bitmap->write_lock);
	INIT_LIST_HEAD(&bitmap->complete_pages);
	init_waitqueue_head(&bitmap->write_wait);
	bitmap->write_pool = mempool_create(WRITE_POOL_SIZE, write_pool_alloc,
				write_pool_free, NULL);
	err = -ENOMEM;
	if (!bitmap->write_pool)
		goto error;

	bitmap->file = file;
	bitmap->offset = mddev->bitmap_offset;
	if (file) get_file(file);
	/* read superblock from bitmap file (this sets bitmap->chunksize) */
	err = bitmap_read_sb(bitmap);
	if (err)
		goto error;

	bitmap->chunkshift = find_first_bit(&bitmap->chunksize,
					sizeof(bitmap->chunksize));

	/* now that chunksize and chunkshift are set, we can use these macros */
 	chunks = (blocks + CHUNK_BLOCK_RATIO(bitmap) - 1) /
			CHUNK_BLOCK_RATIO(bitmap);
 	pages = (chunks + PAGE_COUNTER_RATIO - 1) / PAGE_COUNTER_RATIO;

	BUG_ON(!pages);

	bitmap->chunks = chunks;
	bitmap->pages = pages;
	bitmap->missing_pages = pages;
	bitmap->counter_bits = COUNTER_BITS;

	bitmap->syncchunk = ~0UL;

#ifdef INJECT_FATAL_FAULT_1
	bitmap->bp = NULL;
#else
	bitmap->bp = kzalloc(pages * sizeof(*bitmap->bp), GFP_KERNEL);
#endif
	err = -ENOMEM;
	if (!bitmap->bp)
		goto error;

	/* now that we have some pages available, initialize the in-memory
	 * bitmap from the on-disk bitmap */
	start = 0;
	if (mddev->degraded == 0
	    || bitmap->events_cleared == mddev->events)
		/* no need to keep dirty bits to optimise a re-add of a missing device */
		start = mddev->recovery_cp;
	err = bitmap_init_from_disk(bitmap, start);

	if (err)
		goto error;

	printk(KERN_INFO "created bitmap (%lu pages) for device %s\n",
		pages, bmname(bitmap));

	mddev->bitmap = bitmap;

	if (file)
		/* kick off the bitmap writeback daemon */
		bitmap->writeback_daemon =
			bitmap_start_daemon(bitmap,
					    bitmap_writeback_daemon,
					    "bitmap_wb");

	if (IS_ERR(bitmap->writeback_daemon))
		return PTR_ERR(bitmap->writeback_daemon);
	mddev->thread->timeout = bitmap->daemon_sleep * HZ;

	return bitmap_update_sb(bitmap);

 error:
	bitmap_free(bitmap);
	return err;
}

/* the bitmap API -- for raid personalities */
EXPORT_SYMBOL(bitmap_startwrite);
EXPORT_SYMBOL(bitmap_endwrite);
EXPORT_SYMBOL(bitmap_start_sync);
EXPORT_SYMBOL(bitmap_end_sync);
EXPORT_SYMBOL(bitmap_unplug);
EXPORT_SYMBOL(bitmap_close_sync);
EXPORT_SYMBOL(bitmap_daemon_work);
