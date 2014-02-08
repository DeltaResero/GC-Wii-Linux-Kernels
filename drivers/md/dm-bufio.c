/*
 * Copyright (C) 2009 Red Hat Czech, s.r.o.
 *
 * Mikulas Patocka <mpatocka@redhat.com>
 *
 * This file is released under the GPL.
 */

#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "dm-bufio.h"

/*
 * dm_bufio_client_create --- create a buffered IO cache on a given device
 * dm_bufio_client_destroy --- release a buffered IO cache
 *
 * dm_bufio_read --- read a given block from disk. Returns pointer to data.
 *	Returns a pointer to dm_buffer that can be used to release the buffer
 *	or to make it dirty.
 * dm_bufio_new --- like dm_bufio_read, but don't read anything from the disk.
 *	It is expected that the caller initializes the buffer and marks it
 *	dirty.
 * dm_bufio_release --- release a reference obtained with dm_bufio_read or
 *	dm_bufio_new. The data pointer and dm_buffer pointer is no longer valid
 *	after this call.
 *
 * WARNING: to avoid deadlocks, the thread can hold at most one buffer. Multiple
 *	threads can hold each one buffer simultaneously.
 *
 * dm_bufio_mark_buffer_dirty --- mark a buffer dirty. It should be called after
 *	the buffer is modified.
 * dm_bufio_write_dirty_buffers --- write all dirty buffers. Guarantees that all
 *	dirty buffers created prior to this call are on disk when this call
 *	exits.
 * dm_bufio_issue_flush --- send an empty write barrier to the device to flush
 *	hardware disk cache.
 *
 * In case of memory pressure, the buffer may be written after
 *	dm_bufio_mark_buffer_dirty, but before dm_bufio_write_dirty_buffers.
 *	So, dm_bufio_write_dirty_buffers guarantees that the buffer is on-disk,
 *	but the actual writing may occur earlier.
 *
 * dm_bufio_release_move --- like dm_bufio_release, and also move the buffer to
 *	the new block. dm_bufio_write_dirty_buffers is needed to commit the new
 *	block.
 * dm_bufio_drop_buffers --- clear all buffers.
 */

/*
 * Memory management policy:
 *	When we're above threshold, start asynchronous writing of dirty buffers
 *	and memory reclaiming --- but still allow new allocations.
 *	When we're above limit, don't allocate any more space and synchronously
 *	wait until existing buffers are freed.
 *
 * These default parameters can be overriden with parameters to
 * dm_bufio_client_create.
 */
#define DM_BUFIO_THRESHOLD_MEMORY	(8 * 1048576)
#define DM_BUFIO_LIMIT_MEMORY		(9 * 1048576)

/*
 * The number of bvec entries that are embedded directly in the buffer.
 * If the chunk size is larger, dm-io is used to do the io.
 */
#define DM_BUFIO_INLINE_VECS		16

/*
 * Buffer hash
 */
#define DM_BUFIO_HASH_SIZE	(PAGE_SIZE / sizeof(struct hlist_head) / 2)
#define DM_BUFIO_HASH(block)	((block) & (DM_BUFIO_HASH_SIZE - 1))

/*
 * Don't try to kmalloc blocks larger than this.
 * For exaplanation, see dm_bufio_alloc_buffer_data below.
 */
#define DM_BUFIO_BLOCK_SIZE_KMALLOC_LIMIT	PAGE_SIZE

/*
 * Buffer state bits.
 */
#define B_READING	0
#define B_WRITING	1
#define B_DIRTY		2

struct dm_bufio_client {
	/*
	 * Linking of buffers:
	 *	all buffers are linked to cache_hash with their hash_list field.
	 *	clean buffers that are not being written (B_WRITING not set)
	 *		are linked to lru with their lru_list field.
	 *	dirty and clean buffers that are being written are linked
	 *		to dirty_lru with their	lru_list field. When the write
	 *		finishes, the buffer cannot be immediatelly relinked
	 *		(because we are in an interrupt context and relinking
	 *		requires process context), so some clean-not-writing
	 *		buffers	can be held on dirty_lru too. They are later
	 *		added to
	 *		lru in the process context.
	 */
	struct list_head lru;
	struct list_head dirty_lru;
	struct mutex lock;
	struct block_device *bdev;
	unsigned block_size;
	unsigned char sectors_per_block_bits;
	unsigned char pages_per_block_bits;

	unsigned long n_buffers;
	unsigned threshold_buffers;
	unsigned limit_buffers;

	struct dm_io_client *dm_io;

	struct dm_buffer *reserved_buffer;
	struct hlist_head cache_hash[DM_BUFIO_HASH_SIZE];
	wait_queue_head_t free_buffer_wait;

	int async_write_error;
};

/*
 * A method, with wich the data is allocated:
 * kmalloc(), __get_free_pages() or vmalloc().
 * See the comment at dm_bufio_alloc_buffer_data.
 */
#define DATA_MODE_KMALLOC		1
#define DATA_MODE_GET_FREE_PAGES	2
#define DATA_MODE_VMALLOC		3

struct dm_buffer {
	struct hlist_node hash_list;
	struct list_head lru_list;
	sector_t block;
	void *data;
	char data_mode;		/* DATA_MODE_* */
	unsigned hold_count;
	int read_error;
	int write_error;
	unsigned long state;
	struct dm_bufio_client *c;
	struct bio bio;
	struct bio_vec bio_vec[DM_BUFIO_INLINE_VECS];
};

/*
 * Allocating buffer data.
 *
 * Small buffers are allocated with kmalloc, to use space optimally.
 *
 * Large buffers:
 * We use get_free_pages or vmalloc, both have their advantages and
 * disadvantages.
 * __get_free_pages can randomly fail, if the memory is fragmented.
 * __vmalloc won't randomly fail, but vmalloc space is limited (it may be
 *	as low as 128M) --- so using it for caching is not appropriate.
 * If the allocation may fail, we use __get_free_pages, memory fragmentation
 *	won't have fatal effect here, it just causes flushes of some other
 *	buffers and more I/O will be performed.
 * If the allocation shouldn't fail, we use __vmalloc. This is only for
 *	the initial reserve allocation, so there's no risk of wasting
 *	all vmalloc space.
 */

static void *dm_bufio_alloc_buffer_data(struct dm_bufio_client *c, gfp_t gfp_mask, char *data_mode)
{
	if (c->block_size <= DM_BUFIO_BLOCK_SIZE_KMALLOC_LIMIT) {
		*data_mode = DATA_MODE_KMALLOC;
		return kmalloc(c->block_size, gfp_mask);
	} else if (gfp_mask & __GFP_NORETRY) {
		*data_mode = DATA_MODE_GET_FREE_PAGES;
		return (void *)__get_free_pages(gfp_mask, c->pages_per_block_bits);
	} else {
		*data_mode = DATA_MODE_VMALLOC;
		return __vmalloc(c->block_size, gfp_mask, PAGE_KERNEL);
	}
}

/*
 * Free buffer's data.
 */

static void dm_bufio_free_buffer_data(struct dm_bufio_client *c, void *data, char data_mode)
{
	switch (data_mode) {

	case DATA_MODE_KMALLOC:
		kfree(data);
		break;
	case DATA_MODE_GET_FREE_PAGES:
		free_pages((unsigned long)data, c->pages_per_block_bits);
		break;
	case DATA_MODE_VMALLOC:
		vfree(data);
		break;
	default:
		printk(KERN_CRIT "dm_bufio_free_buffer_data: bad data mode: %d", data_mode);
		BUG();

	}
}


/*
 * Allocate buffer and its data.
 */

static struct dm_buffer *alloc_buffer(struct dm_bufio_client *c, gfp_t gfp_mask)
{
	struct dm_buffer *b;
	b = kmalloc(sizeof(struct dm_buffer), gfp_mask);
	if (!b)
		return NULL;
	b->c = c;
	b->data = dm_bufio_alloc_buffer_data(c, gfp_mask, &b->data_mode);
	if (!b->data) {
		kfree(b);
		return NULL;
	}
	return b;
}

/*
 * Free buffer and its data.
 */

static void free_buffer(struct dm_buffer *b)
{
	dm_bufio_free_buffer_data(b->c, b->data, b->data_mode);
	kfree(b);
}


/*
 * Link buffer to the hash list and clean or dirty queue.
 */

static void link_buffer(struct dm_buffer *b, sector_t block, int dirty)
{
	struct dm_bufio_client *c = b->c;
	c->n_buffers++;
	b->block = block;
	list_add(&b->lru_list, dirty ? &c->dirty_lru : &c->lru);
	hlist_add_head(&b->hash_list, &c->cache_hash[DM_BUFIO_HASH(block)]);
}

/*
 * Unlink buffer from the hash list and dirty or clean queue.
 */

static void unlink_buffer(struct dm_buffer *b)
{
	BUG_ON(!b->c->n_buffers);
	b->c->n_buffers--;
	hlist_del(&b->hash_list);
	list_del(&b->lru_list);
}

/*
 * Place the buffer to the head of dirty or clean LRU queue.
 */

static void relink_lru(struct dm_buffer *b, int dirty)
{
	struct dm_bufio_client *c = b->c;
	list_del(&b->lru_list);
	list_add(&b->lru_list, dirty ? &c->dirty_lru : &c->lru);
}

/*
 * This function is called when wait_on_bit is actually waiting.
 * It unplugs the underlying block device, so that coalesced I/Os in
 * the request queue are dispatched to the device.
 */

static int do_io_schedule(void *word)
{
	struct dm_buffer *b = container_of(word, struct dm_buffer, state);
	struct dm_bufio_client *c = b->c;

	blk_run_address_space(c->bdev->bd_inode->i_mapping);

	io_schedule();

	return 0;
}

static void write_dirty_buffer(struct dm_buffer *b);

/*
 * Wait until any activity on the buffer finishes.
 * Possibly write the buffer if it is dirty.
 * When this function finishes, there is no I/O running on the buffer
 * and the buffer is not dirty.
 */

static void make_buffer_clean(struct dm_buffer *b)
{
	BUG_ON(b->hold_count);
	if (likely(!b->state))	/* fast case */
		return;
	wait_on_bit(&b->state, B_READING, do_io_schedule, TASK_UNINTERRUPTIBLE);
	write_dirty_buffer(b);
	wait_on_bit(&b->state, B_WRITING, do_io_schedule, TASK_UNINTERRUPTIBLE);
}

/*
 * Find some buffer that is not held by anybody, clean it, unlink it and
 * return it.
 * If "wait" is zero, try less harder and don't block.
 */

static struct dm_buffer *get_unclaimed_buffer(struct dm_bufio_client *c, int wait)
{
	struct dm_buffer *b;
	list_for_each_entry_reverse(b, &c->lru, lru_list) {
		cond_resched();
		BUG_ON(test_bit(B_WRITING, &b->state));
		BUG_ON(test_bit(B_DIRTY, &b->state));
		if (!b->hold_count) {
			if (!wait && unlikely(test_bit(B_READING, &b->state)))
				continue;
			make_buffer_clean(b);
			unlink_buffer(b);
			return b;
		}
	}
	list_for_each_entry_reverse(b, &c->dirty_lru, lru_list) {
		cond_resched();
		BUG_ON(test_bit(B_READING, &b->state));
		if (!b->hold_count) {
			if (!wait && (unlikely(test_bit(B_DIRTY, &b->state)) ||
				      unlikely(test_bit(B_WRITING, &b->state)))) {
				if (!test_bit(B_WRITING, &b->state))
					write_dirty_buffer(b);
				continue;
			}
			make_buffer_clean(b);
			unlink_buffer(b);
			return b;
		}
	}
	return NULL;
}

/*
 * Wait until some other threads free some buffer or release hold count
 * on some buffer.
 *
 * This function is entered with c->lock held, drops it and regains it before
 * exiting.
 */

static void wait_for_free_buffer(struct dm_bufio_client *c)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&c->free_buffer_wait, &wait);
	set_task_state(current, TASK_UNINTERRUPTIBLE);
	mutex_unlock(&c->lock);

	io_schedule();

	set_task_state(current, TASK_RUNNING);
	remove_wait_queue(&c->free_buffer_wait, &wait);

	mutex_lock(&c->lock);
}

/*
 * Allocate a new buffer. If the allocation is not possible, wait until some
 * other thread frees a buffer.
 *
 * May drop the lock and regain it.
 */

static struct dm_buffer *alloc_buffer_wait(struct dm_bufio_client *c)
{
	struct dm_buffer *b;

retry:
	/*
	 * dm-bufio is resistant to allocation failures (it just keeps
	 * one buffer reserved in caes all the allocations fail).
	 * So set flags to not try too hard:
	 *	GFP_NOIO: don't recurse into the I/O layer
	 *	__GFP_NOMEMALLOC: don't use emergency reserves
	 *	__GFP_NORETRY: don't retry and rather return failure
	 *	__GFP_NOWARN: don't print a warning in case of failure
	 */
	b = alloc_buffer(c, GFP_NOIO | __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN);
	if (b)
		return b;

	if (c->reserved_buffer) {
		b = c->reserved_buffer;
		c->reserved_buffer = NULL;
		return b;
	}

	b = get_unclaimed_buffer(c, 1);
	if (b)
		return b;

	wait_for_free_buffer(c);
	goto retry;
}

/*
 * Free a buffer and wake other threads waiting for free buffers.
 */

static void free_buffer_wake(struct dm_buffer *b)
{
	struct dm_bufio_client *c = b->c;

	if (unlikely(!c->reserved_buffer))
		c->reserved_buffer = b;
	else
		free_buffer(b);

	wake_up(&c->free_buffer_wait);

	cond_resched();
}

/*
 * Check if we're over watermark.
 * If we are over threshold_buffers, start freeing buffers.
 * If we're over "limit_buffers", blocks until we get under the limit.
 */

static void check_watermark(struct dm_bufio_client *c)
{
	while (c->n_buffers > c->threshold_buffers) {
		struct dm_buffer *b;
		b = get_unclaimed_buffer(c, c->n_buffers > c->limit_buffers);
		if (!b)
			return;
		free_buffer_wake(b);
	}
}

static void dm_bufio_dmio_complete(unsigned long error, void *context);

/*
 * Submit I/O on the buffer.
 *
 * Bio interface is faster but it has some problems:
 *	- the vector list is limited (increasing this limit increases
 *		memory-consumption per buffer, so it is not viable)
 *	- the memory must be direct-mapped, not vmallocated
 *	- the I/O driver can spuriously reject requests if it thinks that
 *		the requests are too big for the device or if they cross a
 *		controller-defined memory boundary
 *
 * If the buffer is small enough (up to DM_BUFIO_INLINE_VECS pages) and
 * it is not vmalloc()ated, try using the bio interface.
 *
 * If the buffer is big, if it is vmalloc()ated or if the underlying device
 * rejects the bio because it is too large, use dmio layer to do the I/O.
 * dmio layer splits the I/O to multiple requests, solving the above
 * shorcomings.
 */

static void dm_bufio_submit_io(struct dm_buffer *b, int rw, sector_t block, bio_end_io_t *end_io)
{
	if (b->c->block_size <= DM_BUFIO_INLINE_VECS * PAGE_SIZE && b->data_mode != DATA_MODE_VMALLOC) {
		char *ptr;
		int len;
		bio_init(&b->bio);
		b->bio.bi_io_vec = b->bio_vec;
		b->bio.bi_max_vecs = DM_BUFIO_INLINE_VECS;
		b->bio.bi_sector = b->block << b->c->sectors_per_block_bits;
		b->bio.bi_bdev = b->c->bdev;
		b->bio.bi_end_io = end_io;

		/*
		 * we assume that if len >= PAGE_SIZE, ptr is page-aligned,
		 * if len < PAGE_SIZE, the buffer doesn't cross page boundary.
		 */
		ptr = b->data;
		len = b->c->block_size;
		do {
			if (!bio_add_page(&b->bio, virt_to_page(ptr), len < PAGE_SIZE ? len : PAGE_SIZE, virt_to_phys(ptr) & (PAGE_SIZE - 1))) {
				BUG_ON(b->c->block_size <= PAGE_SIZE);
				goto use_dmio;
			}
			len -= PAGE_SIZE;
			ptr += PAGE_SIZE;
		} while (len > 0);
		submit_bio(rw, &b->bio);
	} else
use_dmio : {
		int r;
		struct dm_io_request io_req = {
			.bi_rw = rw,
			.notify.fn = dm_bufio_dmio_complete,
			.notify.context = b,
			.client = b->c->dm_io,
		};
		struct dm_io_region region = {
			.bdev = b->c->bdev,
			.sector = b->block << b->c->sectors_per_block_bits,
			.count = b->c->block_size >> SECTOR_SHIFT,
		};
		if (b->data_mode != DATA_MODE_VMALLOC) {
			io_req.mem.type = DM_IO_KMEM;
			io_req.mem.ptr.addr = b->data;
		} else {
			io_req.mem.type = DM_IO_VMA;
			io_req.mem.ptr.vma = b->data;
		}
		b->bio.bi_end_io = end_io;
		r = dm_io(&io_req, 1, &region, NULL);
		if (unlikely(r))
			end_io(&b->bio, r);
	}
}

/*
 * dm-io completion routine. It just calls b->bio.bi_end_io, pretending
 * that the request was handled directly with bio interface.
 */

static void dm_bufio_dmio_complete(unsigned long error, void *context)
{
	struct dm_buffer *b = context;
	int err = 0;
	if (unlikely(error != 0))
		err = -EIO;
	b->bio.bi_end_io(&b->bio, err);
}

/* Find a buffer in the hash. */

static struct dm_buffer *dm_bufio_find(struct dm_bufio_client *c, sector_t block)
{
	struct dm_buffer *b;
	struct hlist_node *hn;
	hlist_for_each_entry(b, hn, &c->cache_hash[DM_BUFIO_HASH(block)], hash_list) {
		cond_resched();
		if (b->block == block)
			return b;
	}

	return NULL;
}

static void read_endio(struct bio *bio, int error);

/*
 * A common routine for dm_bufio_new and dm_bufio_read.
 * Operation of these function is very similar, except that dm_bufio_new
 * doesn't read the buffer from the disk (assuming that the caller overwrites
 * all the data and uses dm_bufio_mark_buffer_dirty to write new data back).
 */

static void *dm_bufio_new_read(struct dm_bufio_client *c, sector_t block, struct dm_buffer **bp, int read)
{
	struct dm_buffer *b, *new_b = NULL;

	cond_resched();
	mutex_lock(&c->lock);
retry_search:
	b = dm_bufio_find(c, block);
	if (b) {
		if (new_b)
			free_buffer_wake(new_b);
		b->hold_count++;
		relink_lru(b, test_bit(B_DIRTY, &b->state) || test_bit(B_WRITING, &b->state));
unlock_wait_ret:
		mutex_unlock(&c->lock);
wait_ret:
		wait_on_bit(&b->state, B_READING, do_io_schedule, TASK_UNINTERRUPTIBLE);
		if (b->read_error) {
			int error = b->read_error;
			dm_bufio_release(b);
			return ERR_PTR(error);
		}
		*bp = b;
		return b->data;
	}
	if (!new_b) {
		new_b = alloc_buffer_wait(c);
		goto retry_search;
	}

	check_watermark(c);

	b = new_b;
	b->hold_count = 1;
	b->read_error = 0;
	b->write_error = 0;
	link_buffer(b, block, 0);

	if (!read) {
		b->state = 0;
		goto unlock_wait_ret;
	}

	b->state = 1 << B_READING;

	mutex_unlock(&c->lock);

	dm_bufio_submit_io(b, READ, b->block, read_endio);

	goto wait_ret;
}

/* Read the buffer and hold reference on it */

void *dm_bufio_read(struct dm_bufio_client *c, sector_t block, struct dm_buffer **bp)
{
	return dm_bufio_new_read(c, block, bp, 1);
}
EXPORT_SYMBOL(dm_bufio_read);

/* Get the buffer with possibly invalid data and hold reference on it */

void *dm_bufio_new(struct dm_bufio_client *c, sector_t block, struct dm_buffer **bp)
{
	return dm_bufio_new_read(c, block, bp, 0);
}
EXPORT_SYMBOL(dm_bufio_new);

/*
 * The endio routine for reading: set the error, clear the bit and wake up
 * anyone waiting on the buffer.
 */

static void read_endio(struct bio *bio, int error)
{
	struct dm_buffer *b = container_of(bio, struct dm_buffer, bio);
	b->read_error = error;
	BUG_ON(!test_bit(B_READING, &b->state));
	smp_mb__before_clear_bit();
	clear_bit(B_READING, &b->state);
	smp_mb__after_clear_bit();
	wake_up_bit(&b->state, B_READING);
}

/*
 * Release the reference held on the buffer.
 */

void dm_bufio_release(struct dm_buffer *b)
{
	struct dm_bufio_client *c = b->c;
	mutex_lock(&c->lock);
	BUG_ON(!b->hold_count);
	BUG_ON(test_bit(B_READING, &b->state));
	b->hold_count--;
	if (!b->hold_count) {
		wake_up(&c->free_buffer_wait);
		/*
		 * If there were errors on the buffer, and the buffer is not
		 * to be written, free the buffer. There is no point in caching
		 * invalid buffer.
		 */
		if ((b->read_error || b->write_error) &&
		    !test_bit(B_WRITING, &b->state) &&
		    !test_bit(B_DIRTY, &b->state)) {
			unlink_buffer(b);
			free_buffer_wake(b);
		}
	}
	mutex_unlock(&c->lock);
}
EXPORT_SYMBOL(dm_bufio_release);

/*
 * Mark that the data in the buffer were modified and the buffer needs to
 * be written back.
 */

void dm_bufio_mark_buffer_dirty(struct dm_buffer *b)
{
	struct dm_bufio_client *c = b->c;

	mutex_lock(&c->lock);

	if (!test_and_set_bit(B_DIRTY, &b->state))
		relink_lru(b, 1);

	mutex_unlock(&c->lock);
}
EXPORT_SYMBOL(dm_bufio_mark_buffer_dirty);

static void write_endio(struct bio *bio, int error);

/*
 * Initiate a write on a dirty buffer, but don't wait for it.
 * If the buffer is not dirty, exit.
 * If there some previous write going on, wait for it to finish (we can't
 * have two writes on the same buffer simultaneously).
 * Finally, submit our write and don't wait on it. We set B_WRITING indicating
 * that there is a write in progress.
 */

static void write_dirty_buffer(struct dm_buffer *b)
{
	if (!test_bit(B_DIRTY, &b->state))
		return;
	clear_bit(B_DIRTY, &b->state);
	wait_on_bit_lock(&b->state, B_WRITING, do_io_schedule, TASK_UNINTERRUPTIBLE);
	dm_bufio_submit_io(b, WRITE, b->block, write_endio);
}

/*
 * The endio routine for write.
 * Set the error, clear B_WRITING bit and wake anyone who was waiting on it.
 */

static void write_endio(struct bio *bio, int error)
{
	struct dm_buffer *b = container_of(bio, struct dm_buffer, bio);
	b->write_error = error;
	if (unlikely(error)) {
		struct dm_bufio_client *c = b->c;
		cmpxchg(&c->async_write_error, 0, error);
	}
	BUG_ON(!test_bit(B_WRITING, &b->state));
	smp_mb__before_clear_bit();
	clear_bit(B_WRITING, &b->state);
	smp_mb__after_clear_bit();
	wake_up_bit(&b->state, B_WRITING);
}

/*
 * Write all the dirty buffers asynchronously.
 */

static void write_dirty_buffers_async(struct dm_bufio_client *c)
{
	struct dm_buffer *b;
	list_for_each_entry_reverse(b, &c->dirty_lru, lru_list) {
		cond_resched();
		BUG_ON(test_bit(B_READING, &b->state));
		write_dirty_buffer(b);
	}
}

/*
 * Write all the dirty buffers synchronously.
 * For performance, it is essential that the buffers are written asynchronously
 * and simultaneously (so that the block layer can merge the writes) and then
 * waited upon.
 *
 * Finally, we flush hardware disk cache.
 */

int dm_bufio_write_dirty_buffers(struct dm_bufio_client *c)
{
	int a, f;

	struct dm_buffer *b, *tmp;
	mutex_lock(&c->lock);
	write_dirty_buffers_async(c);
	mutex_unlock(&c->lock);
	mutex_lock(&c->lock);
	list_for_each_entry_safe_reverse(b, tmp, &c->dirty_lru, lru_list) {
		cond_resched();
		BUG_ON(test_bit(B_READING, &b->state));
		if (test_bit(B_WRITING, &b->state)) {
			b->hold_count++;
			mutex_unlock(&c->lock);
			wait_on_bit(&b->state, B_WRITING, do_io_schedule, TASK_UNINTERRUPTIBLE);
			mutex_lock(&c->lock);
			b->hold_count--;
		}
		if (!test_bit(B_DIRTY, &b->state) && !test_bit(B_WRITING, &b->state))
			relink_lru(b, 0);
	}
	wake_up(&c->free_buffer_wait);
	mutex_unlock(&c->lock);

	a = xchg(&c->async_write_error, 0);
	f = dm_bufio_issue_flush(c);
	if (unlikely(a))
		return a;
	return f;
}
EXPORT_SYMBOL(dm_bufio_write_dirty_buffers);

/*
 * Use dm-io to send and empty barrier flush the device.
 */

int dm_bufio_issue_flush(struct dm_bufio_client *c)
{
	struct dm_io_request io_req = {
		.bi_rw = WRITE_BARRIER,
		.mem.type = DM_IO_KMEM,
		.mem.ptr.bvec = NULL,
		.client = c->dm_io,
	};
	struct dm_io_region io_reg = {
		.bdev = c->bdev,
		.sector = 0,
		.count = 0,
	};
	return dm_io(&io_req, 1, &io_reg, NULL);
}
EXPORT_SYMBOL(dm_bufio_issue_flush);

/*
 * Release the buffer and copy it to the new location.
 *
 * We first delete any other buffer that may be at that new location.
 *
 * Then, we write the buffer to the original location if it was dirty.
 *
 * Then, if we are the only one who is holding the buffer, relink the buffer
 * in the hash queue for the new location.
 *
 * If there was someone other holding the buffer, we write it to the new
 * location but not relink it, because that other user needs to have the buffer
 * at the same place.
 */

void dm_bufio_release_move(struct dm_buffer *b, sector_t new_block)
{
	struct dm_bufio_client *c = b->c;
	struct dm_buffer *underlying;

	mutex_lock(&c->lock);

retry:
	underlying = dm_bufio_find(c, new_block);
	if (unlikely(underlying != NULL)) {
		if (underlying->hold_count) {
			wait_for_free_buffer(c);
			goto retry;
		}
		make_buffer_clean(underlying);
		unlink_buffer(underlying);
		free_buffer_wake(underlying);
	}

	BUG_ON(!b->hold_count);
	BUG_ON(test_bit(B_READING, &b->state));
	write_dirty_buffer(b);
	if (b->hold_count == 1) {
		wait_on_bit(&b->state, B_WRITING, do_io_schedule, TASK_UNINTERRUPTIBLE);
		set_bit(B_DIRTY, &b->state);
		unlink_buffer(b);
		link_buffer(b, new_block, 1);
	} else {
		wait_on_bit_lock(&b->state, B_WRITING, do_io_schedule, TASK_UNINTERRUPTIBLE);
		dm_bufio_submit_io(b, WRITE, new_block, write_endio);
		wait_on_bit(&b->state, B_WRITING, do_io_schedule, TASK_UNINTERRUPTIBLE);
	}
	mutex_unlock(&c->lock);
	dm_bufio_release(b);
}
EXPORT_SYMBOL(dm_bufio_release_move);

/*
 * Free all the buffers (and possibly write them if they were dirty)
 * It is required that the calling theread doesn't have any reference on
 * any buffer.
 */

void dm_bufio_drop_buffers(struct dm_bufio_client *c)
{
	struct dm_buffer *b;
	mutex_lock(&c->lock);
	write_dirty_buffers_async(c);
	while ((b = get_unclaimed_buffer(c, 1)))
		free_buffer_wake(b);
	BUG_ON(!list_empty(&c->lru));
	BUG_ON(!list_empty(&c->dirty_lru));
	mutex_unlock(&c->lock);
}
EXPORT_SYMBOL(dm_bufio_drop_buffers);

/* Create the buffering interface */

struct dm_bufio_client *dm_bufio_client_create(struct block_device *bdev, unsigned block_size, unsigned flags, __u64 cache_threshold, __u64 cache_limit)
{
	int r;
	struct dm_bufio_client *c;
	unsigned i;

	BUG_ON(block_size < 1 << SECTOR_SHIFT || (block_size & (block_size - 1)));

	c = kmalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		r = -ENOMEM;
		goto bad_client;
	}

	c->bdev = bdev;
	c->block_size = block_size;
	c->sectors_per_block_bits = ffs(block_size) - 1 - SECTOR_SHIFT;
	c->pages_per_block_bits = ffs(block_size) - 1 >= PAGE_SHIFT ? ffs(block_size) - 1 - PAGE_SHIFT : 0;
	INIT_LIST_HEAD(&c->lru);
	INIT_LIST_HEAD(&c->dirty_lru);
	for (i = 0; i < DM_BUFIO_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&c->cache_hash[i]);
	mutex_init(&c->lock);
	c->n_buffers = 0;

	if (!cache_limit)
		cache_limit = DM_BUFIO_LIMIT_MEMORY;
	c->limit_buffers = cache_limit >> (c->sectors_per_block_bits + SECTOR_SHIFT);
	if (!c->limit_buffers)
		c->limit_buffers = 1;

	if (!cache_threshold)
		cache_threshold = DM_BUFIO_THRESHOLD_MEMORY;
	if (cache_threshold > cache_limit)
		cache_threshold = cache_limit;
	c->threshold_buffers = cache_threshold >> (c->sectors_per_block_bits + SECTOR_SHIFT);
	if (!c->threshold_buffers)
		c->threshold_buffers = 1;

	/*printk("%d %d\n", c->limit_buffers, c->threshold_buffers);*/

	init_waitqueue_head(&c->free_buffer_wait);
	c->async_write_error = 0;

	/* Number of pages is not really hard limit, just a mempool size */
	c->dm_io = dm_io_client_create((block_size + PAGE_SIZE - 1) / PAGE_SIZE);
	if (IS_ERR(c->dm_io)) {
		r = PTR_ERR(c->dm_io);
		goto bad_dm_io;
	}

	c->reserved_buffer = alloc_buffer(c, GFP_KERNEL);
	if (!c->reserved_buffer) {
		r = -ENOMEM;
		goto bad_buffer;
	}

	return c;

bad_buffer:
	dm_io_client_destroy(c->dm_io);
bad_dm_io:
	kfree(c);
bad_client:
	return ERR_PTR(r);
}
EXPORT_SYMBOL(dm_bufio_client_create);

/*
 * Free the buffering interface.
 * It is required that there are no references on any buffers.
 */

void dm_bufio_client_destroy(struct dm_bufio_client *c)
{
	unsigned i;
	dm_bufio_drop_buffers(c);
	for (i = 0; i < DM_BUFIO_HASH_SIZE; i++)
		BUG_ON(!hlist_empty(&c->cache_hash[i]));
	BUG_ON(!c->reserved_buffer);
	free_buffer(c->reserved_buffer);
	BUG_ON(c->n_buffers != 0);
	dm_io_client_destroy(c->dm_io);
	kfree(c);
}
EXPORT_SYMBOL(dm_bufio_client_destroy);
