/*
 * Generic ring buffer
 *
 * Copyright (C) 2008 Steven Rostedt <srostedt@redhat.com>
 */
#include <linux/ring_buffer.h>
#include <linux/trace_clock.h>
#include <linux/ftrace_irq.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/cpu.h>
#include <linux/fs.h>

#include "trace.h"

/*
 * The ring buffer is made up of a list of pages. A separate list of pages is
 * allocated for each CPU. A writer may only write to a buffer that is
 * associated with the CPU it is currently executing on.  A reader may read
 * from any per cpu buffer.
 *
 * The reader is special. For each per cpu buffer, the reader has its own
 * reader page. When a reader has read the entire reader page, this reader
 * page is swapped with another page in the ring buffer.
 *
 * Now, as long as the writer is off the reader page, the reader can do what
 * ever it wants with that page. The writer will never write to that page
 * again (as long as it is out of the ring buffer).
 *
 * Here's some silly ASCII art.
 *
 *   +------+
 *   |reader|          RING BUFFER
 *   |page  |
 *   +------+        +---+   +---+   +---+
 *                   |   |-->|   |-->|   |
 *                   +---+   +---+   +---+
 *                     ^               |
 *                     |               |
 *                     +---------------+
 *
 *
 *   +------+
 *   |reader|          RING BUFFER
 *   |page  |------------------v
 *   +------+        +---+   +---+   +---+
 *                   |   |-->|   |-->|   |
 *                   +---+   +---+   +---+
 *                     ^               |
 *                     |               |
 *                     +---------------+
 *
 *
 *   +------+
 *   |reader|          RING BUFFER
 *   |page  |------------------v
 *   +------+        +---+   +---+   +---+
 *      ^            |   |-->|   |-->|   |
 *      |            +---+   +---+   +---+
 *      |                              |
 *      |                              |
 *      +------------------------------+
 *
 *
 *   +------+
 *   |buffer|          RING BUFFER
 *   |page  |------------------v
 *   +------+        +---+   +---+   +---+
 *      ^            |   |   |   |-->|   |
 *      |   New      +---+   +---+   +---+
 *      |  Reader------^               |
 *      |   page                       |
 *      +------------------------------+
 *
 *
 * After we make this swap, the reader can hand this page off to the splice
 * code and be done with it. It can even allocate a new page if it needs to
 * and swap that into the ring buffer.
 *
 * We will be using cmpxchg soon to make all this lockless.
 *
 */

/*
 * A fast way to enable or disable all ring buffers is to
 * call tracing_on or tracing_off. Turning off the ring buffers
 * prevents all ring buffers from being recorded to.
 * Turning this switch on, makes it OK to write to the
 * ring buffer, if the ring buffer is enabled itself.
 *
 * There's three layers that must be on in order to write
 * to the ring buffer.
 *
 * 1) This global flag must be set.
 * 2) The ring buffer must be enabled for recording.
 * 3) The per cpu buffer must be enabled for recording.
 *
 * In case of an anomaly, this global flag has a bit set that
 * will permantly disable all ring buffers.
 */

/*
 * Global flag to disable all recording to ring buffers
 *  This has two bits: ON, DISABLED
 *
 *  ON   DISABLED
 * ---- ----------
 *   0      0        : ring buffers are off
 *   1      0        : ring buffers are on
 *   X      1        : ring buffers are permanently disabled
 */

enum {
	RB_BUFFERS_ON_BIT	= 0,
	RB_BUFFERS_DISABLED_BIT	= 1,
};

enum {
	RB_BUFFERS_ON		= 1 << RB_BUFFERS_ON_BIT,
	RB_BUFFERS_DISABLED	= 1 << RB_BUFFERS_DISABLED_BIT,
};

static unsigned long ring_buffer_flags __read_mostly = RB_BUFFERS_ON;

#define BUF_PAGE_HDR_SIZE offsetof(struct buffer_data_page, data)

/**
 * tracing_on - enable all tracing buffers
 *
 * This function enables all tracing buffers that may have been
 * disabled with tracing_off.
 */
void tracing_on(void)
{
	set_bit(RB_BUFFERS_ON_BIT, &ring_buffer_flags);
}
EXPORT_SYMBOL_GPL(tracing_on);

/**
 * tracing_off - turn off all tracing buffers
 *
 * This function stops all tracing buffers from recording data.
 * It does not disable any overhead the tracers themselves may
 * be causing. This function simply causes all recording to
 * the ring buffers to fail.
 */
void tracing_off(void)
{
	clear_bit(RB_BUFFERS_ON_BIT, &ring_buffer_flags);
}
EXPORT_SYMBOL_GPL(tracing_off);

/**
 * tracing_off_permanent - permanently disable ring buffers
 *
 * This function, once called, will disable all ring buffers
 * permanently.
 */
void tracing_off_permanent(void)
{
	set_bit(RB_BUFFERS_DISABLED_BIT, &ring_buffer_flags);
}

/**
 * tracing_is_on - show state of ring buffers enabled
 */
int tracing_is_on(void)
{
	return ring_buffer_flags == RB_BUFFERS_ON;
}
EXPORT_SYMBOL_GPL(tracing_is_on);

#include "trace.h"

#define RB_EVNT_HDR_SIZE (offsetof(struct ring_buffer_event, array))
#define RB_ALIGNMENT		4U
#define RB_MAX_SMALL_DATA	28

enum {
	RB_LEN_TIME_EXTEND = 8,
	RB_LEN_TIME_STAMP = 16,
};

static inline int rb_null_event(struct ring_buffer_event *event)
{
	return event->type == RINGBUF_TYPE_PADDING && event->time_delta == 0;
}

static inline int rb_discarded_event(struct ring_buffer_event *event)
{
	return event->type == RINGBUF_TYPE_PADDING && event->time_delta;
}

static void rb_event_set_padding(struct ring_buffer_event *event)
{
	event->type = RINGBUF_TYPE_PADDING;
	event->time_delta = 0;
}

/**
 * ring_buffer_event_discard - discard an event in the ring buffer
 * @buffer: the ring buffer
 * @event: the event to discard
 *
 * Sometimes a event that is in the ring buffer needs to be ignored.
 * This function lets the user discard an event in the ring buffer
 * and then that event will not be read later.
 *
 * Note, it is up to the user to be careful with this, and protect
 * against races. If the user discards an event that has been consumed
 * it is possible that it could corrupt the ring buffer.
 */
void ring_buffer_event_discard(struct ring_buffer_event *event)
{
	event->type = RINGBUF_TYPE_PADDING;
	/* time delta must be non zero */
	if (!event->time_delta)
		event->time_delta = 1;
}

static unsigned
rb_event_data_length(struct ring_buffer_event *event)
{
	unsigned length;

	if (event->len)
		length = event->len * RB_ALIGNMENT;
	else
		length = event->array[0];
	return length + RB_EVNT_HDR_SIZE;
}

/* inline for ring buffer fast paths */
static unsigned
rb_event_length(struct ring_buffer_event *event)
{
	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		if (rb_null_event(event))
			/* undefined */
			return -1;
		return rb_event_data_length(event);

	case RINGBUF_TYPE_TIME_EXTEND:
		return RB_LEN_TIME_EXTEND;

	case RINGBUF_TYPE_TIME_STAMP:
		return RB_LEN_TIME_STAMP;

	case RINGBUF_TYPE_DATA:
		return rb_event_data_length(event);
	default:
		BUG();
	}
	/* not hit */
	return 0;
}

/**
 * ring_buffer_event_length - return the length of the event
 * @event: the event to get the length of
 */
unsigned ring_buffer_event_length(struct ring_buffer_event *event)
{
	unsigned length = rb_event_length(event);
	if (event->type != RINGBUF_TYPE_DATA)
		return length;
	length -= RB_EVNT_HDR_SIZE;
	if (length > RB_MAX_SMALL_DATA + sizeof(event->array[0]))
                length -= sizeof(event->array[0]);
	return length;
}
EXPORT_SYMBOL_GPL(ring_buffer_event_length);

/* inline for ring buffer fast paths */
static void *
rb_event_data(struct ring_buffer_event *event)
{
	BUG_ON(event->type != RINGBUF_TYPE_DATA);
	/* If length is in len field, then array[0] has the data */
	if (event->len)
		return (void *)&event->array[0];
	/* Otherwise length is in array[0] and array[1] has the data */
	return (void *)&event->array[1];
}

/**
 * ring_buffer_event_data - return the data of the event
 * @event: the event to get the data from
 */
void *ring_buffer_event_data(struct ring_buffer_event *event)
{
	return rb_event_data(event);
}
EXPORT_SYMBOL_GPL(ring_buffer_event_data);

#define for_each_buffer_cpu(buffer, cpu)		\
	for_each_cpu(cpu, buffer->cpumask)

#define TS_SHIFT	27
#define TS_MASK		((1ULL << TS_SHIFT) - 1)
#define TS_DELTA_TEST	(~TS_MASK)

struct buffer_data_page {
	u64		 time_stamp;	/* page time stamp */
	local_t		 commit;	/* write committed index */
	unsigned char	 data[];	/* data of buffer page */
};

struct buffer_page {
	local_t		 write;		/* index for next write */
	unsigned	 read;		/* index for next read */
	struct list_head list;		/* list of free pages */
	struct buffer_data_page *page;	/* Actual data page */
};

static void rb_init_page(struct buffer_data_page *bpage)
{
	local_set(&bpage->commit, 0);
}

/**
 * ring_buffer_page_len - the size of data on the page.
 * @page: The page to read
 *
 * Returns the amount of data on the page, including buffer page header.
 */
size_t ring_buffer_page_len(void *page)
{
	return local_read(&((struct buffer_data_page *)page)->commit)
		+ BUF_PAGE_HDR_SIZE;
}

/*
 * Also stolen from mm/slob.c. Thanks to Mathieu Desnoyers for pointing
 * this issue out.
 */
static void free_buffer_page(struct buffer_page *bpage)
{
	free_page((unsigned long)bpage->page);
	kfree(bpage);
}

/*
 * We need to fit the time_stamp delta into 27 bits.
 */
static inline int test_time_stamp(u64 delta)
{
	if (delta & TS_DELTA_TEST)
		return 1;
	return 0;
}

#define BUF_PAGE_SIZE (PAGE_SIZE - BUF_PAGE_HDR_SIZE)

/*
 * head_page == tail_page && head == tail then buffer is empty.
 */
struct ring_buffer_per_cpu {
	int				cpu;
	struct ring_buffer		*buffer;
	spinlock_t			reader_lock; /* serialize readers */
	raw_spinlock_t			lock;
	struct lock_class_key		lock_key;
	struct list_head		pages;
	struct buffer_page		*head_page;	/* read from head */
	struct buffer_page		*tail_page;	/* write to tail */
	struct buffer_page		*commit_page;	/* committed pages */
	struct buffer_page		*reader_page;
	unsigned long			overrun;
	unsigned long			entries;
	u64				write_stamp;
	u64				read_stamp;
	atomic_t			record_disabled;
};

struct ring_buffer {
	unsigned			pages;
	unsigned			flags;
	int				cpus;
	atomic_t			record_disabled;
	cpumask_var_t			cpumask;

	struct mutex			mutex;

	struct ring_buffer_per_cpu	**buffers;

#ifdef CONFIG_HOTPLUG_CPU
	struct notifier_block		cpu_notify;
#endif
	u64				(*clock)(void);
};

struct ring_buffer_iter {
	struct ring_buffer_per_cpu	*cpu_buffer;
	unsigned long			head;
	struct buffer_page		*head_page;
	u64				read_stamp;
};

/* buffer may be either ring_buffer or ring_buffer_per_cpu */
#define RB_WARN_ON(buffer, cond)				\
	({							\
		int _____ret = unlikely(cond);			\
		if (_____ret) {					\
			atomic_inc(&buffer->record_disabled);	\
			WARN_ON(1);				\
		}						\
		_____ret;					\
	})

/* Up this if you want to test the TIME_EXTENTS and normalization */
#define DEBUG_SHIFT 0

u64 ring_buffer_time_stamp(struct ring_buffer *buffer, int cpu)
{
	u64 time;

	preempt_disable_notrace();
	/* shift to debug/test normalization and TIME_EXTENTS */
	time = buffer->clock() << DEBUG_SHIFT;
	preempt_enable_no_resched_notrace();

	return time;
}
EXPORT_SYMBOL_GPL(ring_buffer_time_stamp);

void ring_buffer_normalize_time_stamp(struct ring_buffer *buffer,
				      int cpu, u64 *ts)
{
	/* Just stupid testing the normalize function and deltas */
	*ts >>= DEBUG_SHIFT;
}
EXPORT_SYMBOL_GPL(ring_buffer_normalize_time_stamp);

/**
 * check_pages - integrity check of buffer pages
 * @cpu_buffer: CPU buffer with pages to test
 *
 * As a safety measure we check to make sure the data pages have not
 * been corrupted.
 */
static int rb_check_pages(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct list_head *head = &cpu_buffer->pages;
	struct buffer_page *bpage, *tmp;

	if (RB_WARN_ON(cpu_buffer, head->next->prev != head))
		return -1;
	if (RB_WARN_ON(cpu_buffer, head->prev->next != head))
		return -1;

	list_for_each_entry_safe(bpage, tmp, head, list) {
		if (RB_WARN_ON(cpu_buffer,
			       bpage->list.next->prev != &bpage->list))
			return -1;
		if (RB_WARN_ON(cpu_buffer,
			       bpage->list.prev->next != &bpage->list))
			return -1;
	}

	return 0;
}

static int rb_allocate_pages(struct ring_buffer_per_cpu *cpu_buffer,
			     unsigned nr_pages)
{
	struct list_head *head = &cpu_buffer->pages;
	struct buffer_page *bpage, *tmp;
	unsigned long addr;
	LIST_HEAD(pages);
	unsigned i;

	for (i = 0; i < nr_pages; i++) {
		bpage = kzalloc_node(ALIGN(sizeof(*bpage), cache_line_size()),
				    GFP_KERNEL, cpu_to_node(cpu_buffer->cpu));
		if (!bpage)
			goto free_pages;
		list_add(&bpage->list, &pages);

		addr = __get_free_page(GFP_KERNEL);
		if (!addr)
			goto free_pages;
		bpage->page = (void *)addr;
		rb_init_page(bpage->page);
	}

	list_splice(&pages, head);

	rb_check_pages(cpu_buffer);

	return 0;

 free_pages:
	list_for_each_entry_safe(bpage, tmp, &pages, list) {
		list_del_init(&bpage->list);
		free_buffer_page(bpage);
	}
	return -ENOMEM;
}

static struct ring_buffer_per_cpu *
rb_allocate_cpu_buffer(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct buffer_page *bpage;
	unsigned long addr;
	int ret;

	cpu_buffer = kzalloc_node(ALIGN(sizeof(*cpu_buffer), cache_line_size()),
				  GFP_KERNEL, cpu_to_node(cpu));
	if (!cpu_buffer)
		return NULL;

	cpu_buffer->cpu = cpu;
	cpu_buffer->buffer = buffer;
	spin_lock_init(&cpu_buffer->reader_lock);
	cpu_buffer->lock = (raw_spinlock_t)__RAW_SPIN_LOCK_UNLOCKED;
	INIT_LIST_HEAD(&cpu_buffer->pages);

	bpage = kzalloc_node(ALIGN(sizeof(*bpage), cache_line_size()),
			    GFP_KERNEL, cpu_to_node(cpu));
	if (!bpage)
		goto fail_free_buffer;

	cpu_buffer->reader_page = bpage;
	addr = __get_free_page(GFP_KERNEL);
	if (!addr)
		goto fail_free_reader;
	bpage->page = (void *)addr;
	rb_init_page(bpage->page);

	INIT_LIST_HEAD(&cpu_buffer->reader_page->list);

	ret = rb_allocate_pages(cpu_buffer, buffer->pages);
	if (ret < 0)
		goto fail_free_reader;

	cpu_buffer->head_page
		= list_entry(cpu_buffer->pages.next, struct buffer_page, list);
	cpu_buffer->tail_page = cpu_buffer->commit_page = cpu_buffer->head_page;

	return cpu_buffer;

 fail_free_reader:
	free_buffer_page(cpu_buffer->reader_page);

 fail_free_buffer:
	kfree(cpu_buffer);
	return NULL;
}

static void rb_free_cpu_buffer(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct list_head *head = &cpu_buffer->pages;
	struct buffer_page *bpage, *tmp;

	free_buffer_page(cpu_buffer->reader_page);

	list_for_each_entry_safe(bpage, tmp, head, list) {
		list_del_init(&bpage->list);
		free_buffer_page(bpage);
	}
	kfree(cpu_buffer);
}

/*
 * Causes compile errors if the struct buffer_page gets bigger
 * than the struct page.
 */
extern int ring_buffer_page_too_big(void);

#ifdef CONFIG_HOTPLUG_CPU
static int rb_cpu_notify(struct notifier_block *self,
			 unsigned long action, void *hcpu);
#endif

/**
 * ring_buffer_alloc - allocate a new ring_buffer
 * @size: the size in bytes per cpu that is needed.
 * @flags: attributes to set for the ring buffer.
 *
 * Currently the only flag that is available is the RB_FL_OVERWRITE
 * flag. This flag means that the buffer will overwrite old data
 * when the buffer wraps. If this flag is not set, the buffer will
 * drop data when the tail hits the head.
 */
struct ring_buffer *ring_buffer_alloc(unsigned long size, unsigned flags)
{
	struct ring_buffer *buffer;
	int bsize;
	int cpu;

	/* Paranoid! Optimizes out when all is well */
	if (sizeof(struct buffer_page) > sizeof(struct page))
		ring_buffer_page_too_big();


	/* keep it in its own cache line */
	buffer = kzalloc(ALIGN(sizeof(*buffer), cache_line_size()),
			 GFP_KERNEL);
	if (!buffer)
		return NULL;

	if (!alloc_cpumask_var(&buffer->cpumask, GFP_KERNEL))
		goto fail_free_buffer;

	buffer->pages = DIV_ROUND_UP(size, BUF_PAGE_SIZE);
	buffer->flags = flags;
	buffer->clock = trace_clock_local;

	/* need at least two pages */
	if (buffer->pages == 1)
		buffer->pages++;

	/*
	 * In case of non-hotplug cpu, if the ring-buffer is allocated
	 * in early initcall, it will not be notified of secondary cpus.
	 * In that off case, we need to allocate for all possible cpus.
	 */
#ifdef CONFIG_HOTPLUG_CPU
	get_online_cpus();
	cpumask_copy(buffer->cpumask, cpu_online_mask);
#else
	cpumask_copy(buffer->cpumask, cpu_possible_mask);
#endif
	buffer->cpus = nr_cpu_ids;

	bsize = sizeof(void *) * nr_cpu_ids;
	buffer->buffers = kzalloc(ALIGN(bsize, cache_line_size()),
				  GFP_KERNEL);
	if (!buffer->buffers)
		goto fail_free_cpumask;

	for_each_buffer_cpu(buffer, cpu) {
		buffer->buffers[cpu] =
			rb_allocate_cpu_buffer(buffer, cpu);
		if (!buffer->buffers[cpu])
			goto fail_free_buffers;
	}

#ifdef CONFIG_HOTPLUG_CPU
	buffer->cpu_notify.notifier_call = rb_cpu_notify;
	buffer->cpu_notify.priority = 0;
	register_cpu_notifier(&buffer->cpu_notify);
#endif

	put_online_cpus();
	mutex_init(&buffer->mutex);

	return buffer;

 fail_free_buffers:
	for_each_buffer_cpu(buffer, cpu) {
		if (buffer->buffers[cpu])
			rb_free_cpu_buffer(buffer->buffers[cpu]);
	}
	kfree(buffer->buffers);

 fail_free_cpumask:
	free_cpumask_var(buffer->cpumask);
	put_online_cpus();

 fail_free_buffer:
	kfree(buffer);
	return NULL;
}
EXPORT_SYMBOL_GPL(ring_buffer_alloc);

/**
 * ring_buffer_free - free a ring buffer.
 * @buffer: the buffer to free.
 */
void
ring_buffer_free(struct ring_buffer *buffer)
{
	int cpu;

	get_online_cpus();

#ifdef CONFIG_HOTPLUG_CPU
	unregister_cpu_notifier(&buffer->cpu_notify);
#endif

	for_each_buffer_cpu(buffer, cpu)
		rb_free_cpu_buffer(buffer->buffers[cpu]);

	put_online_cpus();

	kfree(buffer->buffers);
	free_cpumask_var(buffer->cpumask);

	kfree(buffer);
}
EXPORT_SYMBOL_GPL(ring_buffer_free);

void ring_buffer_set_clock(struct ring_buffer *buffer,
			   u64 (*clock)(void))
{
	buffer->clock = clock;
}

static void rb_reset_cpu(struct ring_buffer_per_cpu *cpu_buffer);

static void
rb_remove_pages(struct ring_buffer_per_cpu *cpu_buffer, unsigned nr_pages)
{
	struct buffer_page *bpage;
	struct list_head *p;
	unsigned i;

	atomic_inc(&cpu_buffer->record_disabled);
	synchronize_sched();

	for (i = 0; i < nr_pages; i++) {
		if (RB_WARN_ON(cpu_buffer, list_empty(&cpu_buffer->pages)))
			return;
		p = cpu_buffer->pages.next;
		bpage = list_entry(p, struct buffer_page, list);
		list_del_init(&bpage->list);
		free_buffer_page(bpage);
	}
	if (RB_WARN_ON(cpu_buffer, list_empty(&cpu_buffer->pages)))
		return;

	rb_reset_cpu(cpu_buffer);

	rb_check_pages(cpu_buffer);

	atomic_dec(&cpu_buffer->record_disabled);

}

static void
rb_insert_pages(struct ring_buffer_per_cpu *cpu_buffer,
		struct list_head *pages, unsigned nr_pages)
{
	struct buffer_page *bpage;
	struct list_head *p;
	unsigned i;

	atomic_inc(&cpu_buffer->record_disabled);
	synchronize_sched();

	for (i = 0; i < nr_pages; i++) {
		if (RB_WARN_ON(cpu_buffer, list_empty(pages)))
			return;
		p = pages->next;
		bpage = list_entry(p, struct buffer_page, list);
		list_del_init(&bpage->list);
		list_add_tail(&bpage->list, &cpu_buffer->pages);
	}
	rb_reset_cpu(cpu_buffer);

	rb_check_pages(cpu_buffer);

	atomic_dec(&cpu_buffer->record_disabled);
}

/**
 * ring_buffer_resize - resize the ring buffer
 * @buffer: the buffer to resize.
 * @size: the new size.
 *
 * The tracer is responsible for making sure that the buffer is
 * not being used while changing the size.
 * Note: We may be able to change the above requirement by using
 *  RCU synchronizations.
 *
 * Minimum size is 2 * BUF_PAGE_SIZE.
 *
 * Returns -1 on failure.
 */
int ring_buffer_resize(struct ring_buffer *buffer, unsigned long size)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned nr_pages, rm_pages, new_pages;
	struct buffer_page *bpage, *tmp;
	unsigned long buffer_size;
	unsigned long addr;
	LIST_HEAD(pages);
	int i, cpu;

	/*
	 * Always succeed at resizing a non-existent buffer:
	 */
	if (!buffer)
		return size;

	size = DIV_ROUND_UP(size, BUF_PAGE_SIZE);
	size *= BUF_PAGE_SIZE;
	buffer_size = buffer->pages * BUF_PAGE_SIZE;

	/* we need a minimum of two pages */
	if (size < BUF_PAGE_SIZE * 2)
		size = BUF_PAGE_SIZE * 2;

	if (size == buffer_size)
		return size;

	mutex_lock(&buffer->mutex);
	get_online_cpus();

	nr_pages = DIV_ROUND_UP(size, BUF_PAGE_SIZE);

	if (size < buffer_size) {

		/* easy case, just free pages */
		if (RB_WARN_ON(buffer, nr_pages >= buffer->pages))
			goto out_fail;

		rm_pages = buffer->pages - nr_pages;

		for_each_buffer_cpu(buffer, cpu) {
			cpu_buffer = buffer->buffers[cpu];
			rb_remove_pages(cpu_buffer, rm_pages);
		}
		goto out;
	}

	/*
	 * This is a bit more difficult. We only want to add pages
	 * when we can allocate enough for all CPUs. We do this
	 * by allocating all the pages and storing them on a local
	 * link list. If we succeed in our allocation, then we
	 * add these pages to the cpu_buffers. Otherwise we just free
	 * them all and return -ENOMEM;
	 */
	if (RB_WARN_ON(buffer, nr_pages <= buffer->pages))
		goto out_fail;

	new_pages = nr_pages - buffer->pages;

	for_each_buffer_cpu(buffer, cpu) {
		for (i = 0; i < new_pages; i++) {
			bpage = kzalloc_node(ALIGN(sizeof(*bpage),
						  cache_line_size()),
					    GFP_KERNEL, cpu_to_node(cpu));
			if (!bpage)
				goto free_pages;
			list_add(&bpage->list, &pages);
			addr = __get_free_page(GFP_KERNEL);
			if (!addr)
				goto free_pages;
			bpage->page = (void *)addr;
			rb_init_page(bpage->page);
		}
	}

	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		rb_insert_pages(cpu_buffer, &pages, new_pages);
	}

	if (RB_WARN_ON(buffer, !list_empty(&pages)))
		goto out_fail;

 out:
	buffer->pages = nr_pages;
	put_online_cpus();
	mutex_unlock(&buffer->mutex);

	return size;

 free_pages:
	list_for_each_entry_safe(bpage, tmp, &pages, list) {
		list_del_init(&bpage->list);
		free_buffer_page(bpage);
	}
	put_online_cpus();
	mutex_unlock(&buffer->mutex);
	return -ENOMEM;

	/*
	 * Something went totally wrong, and we are too paranoid
	 * to even clean up the mess.
	 */
 out_fail:
	put_online_cpus();
	mutex_unlock(&buffer->mutex);
	return -1;
}
EXPORT_SYMBOL_GPL(ring_buffer_resize);

static inline void *
__rb_data_page_index(struct buffer_data_page *bpage, unsigned index)
{
	return bpage->data + index;
}

static inline void *__rb_page_index(struct buffer_page *bpage, unsigned index)
{
	return bpage->page->data + index;
}

static inline struct ring_buffer_event *
rb_reader_event(struct ring_buffer_per_cpu *cpu_buffer)
{
	return __rb_page_index(cpu_buffer->reader_page,
			       cpu_buffer->reader_page->read);
}

static inline struct ring_buffer_event *
rb_head_event(struct ring_buffer_per_cpu *cpu_buffer)
{
	return __rb_page_index(cpu_buffer->head_page,
			       cpu_buffer->head_page->read);
}

static inline struct ring_buffer_event *
rb_iter_head_event(struct ring_buffer_iter *iter)
{
	return __rb_page_index(iter->head_page, iter->head);
}

static inline unsigned rb_page_write(struct buffer_page *bpage)
{
	return local_read(&bpage->write);
}

static inline unsigned rb_page_commit(struct buffer_page *bpage)
{
	return local_read(&bpage->page->commit);
}

/* Size is determined by what has been commited */
static inline unsigned rb_page_size(struct buffer_page *bpage)
{
	return rb_page_commit(bpage);
}

static inline unsigned
rb_commit_index(struct ring_buffer_per_cpu *cpu_buffer)
{
	return rb_page_commit(cpu_buffer->commit_page);
}

static inline unsigned rb_head_size(struct ring_buffer_per_cpu *cpu_buffer)
{
	return rb_page_commit(cpu_buffer->head_page);
}

/*
 * When the tail hits the head and the buffer is in overwrite mode,
 * the head jumps to the next page and all content on the previous
 * page is discarded. But before doing so, we update the overrun
 * variable of the buffer.
 */
static void rb_update_overflow(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct ring_buffer_event *event;
	unsigned long head;

	for (head = 0; head < rb_head_size(cpu_buffer);
	     head += rb_event_length(event)) {

		event = __rb_page_index(cpu_buffer->head_page, head);
		if (RB_WARN_ON(cpu_buffer, rb_null_event(event)))
			return;
		/* Only count data entries */
		if (event->type != RINGBUF_TYPE_DATA)
			continue;
		cpu_buffer->overrun++;
		cpu_buffer->entries--;
	}
}

static inline void rb_inc_page(struct ring_buffer_per_cpu *cpu_buffer,
			       struct buffer_page **bpage)
{
	struct list_head *p = (*bpage)->list.next;

	if (p == &cpu_buffer->pages)
		p = p->next;

	*bpage = list_entry(p, struct buffer_page, list);
}

static inline unsigned
rb_event_index(struct ring_buffer_event *event)
{
	unsigned long addr = (unsigned long)event;

	return (addr & ~PAGE_MASK) - (PAGE_SIZE - BUF_PAGE_SIZE);
}

static int
rb_is_commit(struct ring_buffer_per_cpu *cpu_buffer,
	     struct ring_buffer_event *event)
{
	unsigned long addr = (unsigned long)event;
	unsigned long index;

	index = rb_event_index(event);
	addr &= PAGE_MASK;

	return cpu_buffer->commit_page->page == (void *)addr &&
		rb_commit_index(cpu_buffer) == index;
}

static void
rb_set_commit_event(struct ring_buffer_per_cpu *cpu_buffer,
		    struct ring_buffer_event *event)
{
	unsigned long addr = (unsigned long)event;
	unsigned long index;

	index = rb_event_index(event);
	addr &= PAGE_MASK;

	while (cpu_buffer->commit_page->page != (void *)addr) {
		if (RB_WARN_ON(cpu_buffer,
			  cpu_buffer->commit_page == cpu_buffer->tail_page))
			return;
		cpu_buffer->commit_page->page->commit =
			cpu_buffer->commit_page->write;
		rb_inc_page(cpu_buffer, &cpu_buffer->commit_page);
		cpu_buffer->write_stamp =
			cpu_buffer->commit_page->page->time_stamp;
	}

	/* Now set the commit to the event's index */
	local_set(&cpu_buffer->commit_page->page->commit, index);
}

static void
rb_set_commit_to_write(struct ring_buffer_per_cpu *cpu_buffer)
{
	/*
	 * We only race with interrupts and NMIs on this CPU.
	 * If we own the commit event, then we can commit
	 * all others that interrupted us, since the interruptions
	 * are in stack format (they finish before they come
	 * back to us). This allows us to do a simple loop to
	 * assign the commit to the tail.
	 */
 again:
	while (cpu_buffer->commit_page != cpu_buffer->tail_page) {
		cpu_buffer->commit_page->page->commit =
			cpu_buffer->commit_page->write;
		rb_inc_page(cpu_buffer, &cpu_buffer->commit_page);
		cpu_buffer->write_stamp =
			cpu_buffer->commit_page->page->time_stamp;
		/* add barrier to keep gcc from optimizing too much */
		barrier();
	}
	while (rb_commit_index(cpu_buffer) !=
	       rb_page_write(cpu_buffer->commit_page)) {
		cpu_buffer->commit_page->page->commit =
			cpu_buffer->commit_page->write;
		barrier();
	}

	/* again, keep gcc from optimizing */
	barrier();

	/*
	 * If an interrupt came in just after the first while loop
	 * and pushed the tail page forward, we will be left with
	 * a dangling commit that will never go forward.
	 */
	if (unlikely(cpu_buffer->commit_page != cpu_buffer->tail_page))
		goto again;
}

static void rb_reset_reader_page(struct ring_buffer_per_cpu *cpu_buffer)
{
	cpu_buffer->read_stamp = cpu_buffer->reader_page->page->time_stamp;
	cpu_buffer->reader_page->read = 0;
}

static void rb_inc_iter(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;

	/*
	 * The iterator could be on the reader page (it starts there).
	 * But the head could have moved, since the reader was
	 * found. Check for this case and assign the iterator
	 * to the head page instead of next.
	 */
	if (iter->head_page == cpu_buffer->reader_page)
		iter->head_page = cpu_buffer->head_page;
	else
		rb_inc_page(cpu_buffer, &iter->head_page);

	iter->read_stamp = iter->head_page->page->time_stamp;
	iter->head = 0;
}

/**
 * ring_buffer_update_event - update event type and data
 * @event: the even to update
 * @type: the type of event
 * @length: the size of the event field in the ring buffer
 *
 * Update the type and data fields of the event. The length
 * is the actual size that is written to the ring buffer,
 * and with this, we can determine what to place into the
 * data field.
 */
static void
rb_update_event(struct ring_buffer_event *event,
			 unsigned type, unsigned length)
{
	event->type = type;

	switch (type) {

	case RINGBUF_TYPE_PADDING:
		break;

	case RINGBUF_TYPE_TIME_EXTEND:
		event->len = DIV_ROUND_UP(RB_LEN_TIME_EXTEND, RB_ALIGNMENT);
		break;

	case RINGBUF_TYPE_TIME_STAMP:
		event->len = DIV_ROUND_UP(RB_LEN_TIME_STAMP, RB_ALIGNMENT);
		break;

	case RINGBUF_TYPE_DATA:
		length -= RB_EVNT_HDR_SIZE;
		if (length > RB_MAX_SMALL_DATA) {
			event->len = 0;
			event->array[0] = length;
		} else
			event->len = DIV_ROUND_UP(length, RB_ALIGNMENT);
		break;
	default:
		BUG();
	}
}

static unsigned rb_calculate_event_length(unsigned length)
{
	struct ring_buffer_event event; /* Used only for sizeof array */

	/* zero length can cause confusions */
	if (!length)
		length = 1;

	if (length > RB_MAX_SMALL_DATA)
		length += sizeof(event.array[0]);

	length += RB_EVNT_HDR_SIZE;
	length = ALIGN(length, RB_ALIGNMENT);

	return length;
}

static struct ring_buffer_event *
__rb_reserve_next(struct ring_buffer_per_cpu *cpu_buffer,
		  unsigned type, unsigned long length, u64 *ts)
{
	struct buffer_page *tail_page, *head_page, *reader_page, *commit_page;
	unsigned long tail, write;
	struct ring_buffer *buffer = cpu_buffer->buffer;
	struct ring_buffer_event *event;
	unsigned long flags;
	bool lock_taken = false;

	commit_page = cpu_buffer->commit_page;
	/* we just need to protect against interrupts */
	barrier();
	tail_page = cpu_buffer->tail_page;
	write = local_add_return(length, &tail_page->write);
	tail = write - length;

	/* See if we shot pass the end of this buffer page */
	if (write > BUF_PAGE_SIZE) {
		struct buffer_page *next_page = tail_page;

		local_irq_save(flags);
		/*
		 * Since the write to the buffer is still not
		 * fully lockless, we must be careful with NMIs.
		 * The locks in the writers are taken when a write
		 * crosses to a new page. The locks protect against
		 * races with the readers (this will soon be fixed
		 * with a lockless solution).
		 *
		 * Because we can not protect against NMIs, and we
		 * want to keep traces reentrant, we need to manage
		 * what happens when we are in an NMI.
		 *
		 * NMIs can happen after we take the lock.
		 * If we are in an NMI, only take the lock
		 * if it is not already taken. Otherwise
		 * simply fail.
		 */
		if (unlikely(in_nmi())) {
			if (!__raw_spin_trylock(&cpu_buffer->lock))
				goto out_reset;
		} else
			__raw_spin_lock(&cpu_buffer->lock);

		lock_taken = true;

		rb_inc_page(cpu_buffer, &next_page);

		head_page = cpu_buffer->head_page;
		reader_page = cpu_buffer->reader_page;

		/* we grabbed the lock before incrementing */
		if (RB_WARN_ON(cpu_buffer, next_page == reader_page))
			goto out_reset;

		/*
		 * If for some reason, we had an interrupt storm that made
		 * it all the way around the buffer, bail, and warn
		 * about it.
		 */
		if (unlikely(next_page == commit_page)) {
			WARN_ON_ONCE(1);
			goto out_reset;
		}

		if (next_page == head_page) {
			if (!(buffer->flags & RB_FL_OVERWRITE))
				goto out_reset;

			/* tail_page has not moved yet? */
			if (tail_page == cpu_buffer->tail_page) {
				/* count overflows */
				rb_update_overflow(cpu_buffer);

				rb_inc_page(cpu_buffer, &head_page);
				cpu_buffer->head_page = head_page;
				cpu_buffer->head_page->read = 0;
			}
		}

		/*
		 * If the tail page is still the same as what we think
		 * it is, then it is up to us to update the tail
		 * pointer.
		 */
		if (tail_page == cpu_buffer->tail_page) {
			local_set(&next_page->write, 0);
			local_set(&next_page->page->commit, 0);
			cpu_buffer->tail_page = next_page;

			/* reread the time stamp */
			*ts = ring_buffer_time_stamp(buffer, cpu_buffer->cpu);
			cpu_buffer->tail_page->page->time_stamp = *ts;
		}

		/*
		 * The actual tail page has moved forward.
		 */
		if (tail < BUF_PAGE_SIZE) {
			/* Mark the rest of the page with padding */
			event = __rb_page_index(tail_page, tail);
			rb_event_set_padding(event);
		}

		if (tail <= BUF_PAGE_SIZE)
			/* Set the write back to the previous setting */
			local_set(&tail_page->write, tail);

		/*
		 * If this was a commit entry that failed,
		 * increment that too
		 */
		if (tail_page == cpu_buffer->commit_page &&
		    tail == rb_commit_index(cpu_buffer)) {
			rb_set_commit_to_write(cpu_buffer);
		}

		__raw_spin_unlock(&cpu_buffer->lock);
		local_irq_restore(flags);

		/* fail and let the caller try again */
		return ERR_PTR(-EAGAIN);
	}

	/* We reserved something on the buffer */

	if (RB_WARN_ON(cpu_buffer, write > BUF_PAGE_SIZE))
		return NULL;

	event = __rb_page_index(tail_page, tail);
	rb_update_event(event, type, length);

	/*
	 * If this is a commit and the tail is zero, then update
	 * this page's time stamp.
	 */
	if (!tail && rb_is_commit(cpu_buffer, event))
		cpu_buffer->commit_page->page->time_stamp = *ts;

	return event;

 out_reset:
	/* reset write */
	if (tail <= BUF_PAGE_SIZE)
		local_set(&tail_page->write, tail);

	if (likely(lock_taken))
		__raw_spin_unlock(&cpu_buffer->lock);
	local_irq_restore(flags);
	return NULL;
}

static int
rb_add_time_stamp(struct ring_buffer_per_cpu *cpu_buffer,
		  u64 *ts, u64 *delta)
{
	struct ring_buffer_event *event;
	static int once;
	int ret;

	if (unlikely(*delta > (1ULL << 59) && !once++)) {
		printk(KERN_WARNING "Delta way too big! %llu"
		       " ts=%llu write stamp = %llu\n",
		       (unsigned long long)*delta,
		       (unsigned long long)*ts,
		       (unsigned long long)cpu_buffer->write_stamp);
		WARN_ON(1);
	}

	/*
	 * The delta is too big, we to add a
	 * new timestamp.
	 */
	event = __rb_reserve_next(cpu_buffer,
				  RINGBUF_TYPE_TIME_EXTEND,
				  RB_LEN_TIME_EXTEND,
				  ts);
	if (!event)
		return -EBUSY;

	if (PTR_ERR(event) == -EAGAIN)
		return -EAGAIN;

	/* Only a commited time event can update the write stamp */
	if (rb_is_commit(cpu_buffer, event)) {
		/*
		 * If this is the first on the page, then we need to
		 * update the page itself, and just put in a zero.
		 */
		if (rb_event_index(event)) {
			event->time_delta = *delta & TS_MASK;
			event->array[0] = *delta >> TS_SHIFT;
		} else {
			cpu_buffer->commit_page->page->time_stamp = *ts;
			event->time_delta = 0;
			event->array[0] = 0;
		}
		cpu_buffer->write_stamp = *ts;
		/* let the caller know this was the commit */
		ret = 1;
	} else {
		/* Darn, this is just wasted space */
		event->time_delta = 0;
		event->array[0] = 0;
		ret = 0;
	}

	*delta = 0;

	return ret;
}

static struct ring_buffer_event *
rb_reserve_next_event(struct ring_buffer_per_cpu *cpu_buffer,
		      unsigned type, unsigned long length)
{
	struct ring_buffer_event *event;
	u64 ts, delta;
	int commit = 0;
	int nr_loops = 0;

 again:
	/*
	 * We allow for interrupts to reenter here and do a trace.
	 * If one does, it will cause this original code to loop
	 * back here. Even with heavy interrupts happening, this
	 * should only happen a few times in a row. If this happens
	 * 1000 times in a row, there must be either an interrupt
	 * storm or we have something buggy.
	 * Bail!
	 */
	if (RB_WARN_ON(cpu_buffer, ++nr_loops > 1000))
		return NULL;

	ts = ring_buffer_time_stamp(cpu_buffer->buffer, cpu_buffer->cpu);

	/*
	 * Only the first commit can update the timestamp.
	 * Yes there is a race here. If an interrupt comes in
	 * just after the conditional and it traces too, then it
	 * will also check the deltas. More than one timestamp may
	 * also be made. But only the entry that did the actual
	 * commit will be something other than zero.
	 */
	if (cpu_buffer->tail_page == cpu_buffer->commit_page &&
	    rb_page_write(cpu_buffer->tail_page) ==
	    rb_commit_index(cpu_buffer)) {

		delta = ts - cpu_buffer->write_stamp;

		/* make sure this delta is calculated here */
		barrier();

		/* Did the write stamp get updated already? */
		if (unlikely(ts < cpu_buffer->write_stamp))
			delta = 0;

		if (test_time_stamp(delta)) {

			commit = rb_add_time_stamp(cpu_buffer, &ts, &delta);

			if (commit == -EBUSY)
				return NULL;

			if (commit == -EAGAIN)
				goto again;

			RB_WARN_ON(cpu_buffer, commit < 0);
		}
	} else
		/* Non commits have zero deltas */
		delta = 0;

	event = __rb_reserve_next(cpu_buffer, type, length, &ts);
	if (PTR_ERR(event) == -EAGAIN)
		goto again;

	if (!event) {
		if (unlikely(commit))
			/*
			 * Ouch! We needed a timestamp and it was commited. But
			 * we didn't get our event reserved.
			 */
			rb_set_commit_to_write(cpu_buffer);
		return NULL;
	}

	/*
	 * If the timestamp was commited, make the commit our entry
	 * now so that we will update it when needed.
	 */
	if (commit)
		rb_set_commit_event(cpu_buffer, event);
	else if (!rb_is_commit(cpu_buffer, event))
		delta = 0;

	event->time_delta = delta;

	return event;
}

static DEFINE_PER_CPU(int, rb_need_resched);

/**
 * ring_buffer_lock_reserve - reserve a part of the buffer
 * @buffer: the ring buffer to reserve from
 * @length: the length of the data to reserve (excluding event header)
 *
 * Returns a reseverd event on the ring buffer to copy directly to.
 * The user of this interface will need to get the body to write into
 * and can use the ring_buffer_event_data() interface.
 *
 * The length is the length of the data needed, not the event length
 * which also includes the event header.
 *
 * Must be paired with ring_buffer_unlock_commit, unless NULL is returned.
 * If NULL is returned, then nothing has been allocated or locked.
 */
struct ring_buffer_event *
ring_buffer_lock_reserve(struct ring_buffer *buffer, unsigned long length)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	int cpu, resched;

	if (ring_buffer_flags != RB_BUFFERS_ON)
		return NULL;

	if (atomic_read(&buffer->record_disabled))
		return NULL;

	/* If we are tracing schedule, we don't want to recurse */
	resched = ftrace_preempt_disable();

	cpu = raw_smp_processor_id();

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		goto out;

	cpu_buffer = buffer->buffers[cpu];

	if (atomic_read(&cpu_buffer->record_disabled))
		goto out;

	length = rb_calculate_event_length(length);
	if (length > BUF_PAGE_SIZE)
		goto out;

	event = rb_reserve_next_event(cpu_buffer, RINGBUF_TYPE_DATA, length);
	if (!event)
		goto out;

	/*
	 * Need to store resched state on this cpu.
	 * Only the first needs to.
	 */

	if (preempt_count() == 1)
		per_cpu(rb_need_resched, cpu) = resched;

	return event;

 out:
	ftrace_preempt_enable(resched);
	return NULL;
}
EXPORT_SYMBOL_GPL(ring_buffer_lock_reserve);

static void rb_commit(struct ring_buffer_per_cpu *cpu_buffer,
		      struct ring_buffer_event *event)
{
	cpu_buffer->entries++;

	/* Only process further if we own the commit */
	if (!rb_is_commit(cpu_buffer, event))
		return;

	cpu_buffer->write_stamp += event->time_delta;

	rb_set_commit_to_write(cpu_buffer);
}

/**
 * ring_buffer_unlock_commit - commit a reserved
 * @buffer: The buffer to commit to
 * @event: The event pointer to commit.
 *
 * This commits the data to the ring buffer, and releases any locks held.
 *
 * Must be paired with ring_buffer_lock_reserve.
 */
int ring_buffer_unlock_commit(struct ring_buffer *buffer,
			      struct ring_buffer_event *event)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	int cpu = raw_smp_processor_id();

	cpu_buffer = buffer->buffers[cpu];

	rb_commit(cpu_buffer, event);

	/*
	 * Only the last preempt count needs to restore preemption.
	 */
	if (preempt_count() == 1)
		ftrace_preempt_enable(per_cpu(rb_need_resched, cpu));
	else
		preempt_enable_no_resched_notrace();

	return 0;
}
EXPORT_SYMBOL_GPL(ring_buffer_unlock_commit);

/**
 * ring_buffer_write - write data to the buffer without reserving
 * @buffer: The ring buffer to write to.
 * @length: The length of the data being written (excluding the event header)
 * @data: The data to write to the buffer.
 *
 * This is like ring_buffer_lock_reserve and ring_buffer_unlock_commit as
 * one function. If you already have the data to write to the buffer, it
 * may be easier to simply call this function.
 *
 * Note, like ring_buffer_lock_reserve, the length is the length of the data
 * and not the length of the event which would hold the header.
 */
int ring_buffer_write(struct ring_buffer *buffer,
			unsigned long length,
			void *data)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	unsigned long event_length;
	void *body;
	int ret = -EBUSY;
	int cpu, resched;

	if (ring_buffer_flags != RB_BUFFERS_ON)
		return -EBUSY;

	if (atomic_read(&buffer->record_disabled))
		return -EBUSY;

	resched = ftrace_preempt_disable();

	cpu = raw_smp_processor_id();

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		goto out;

	cpu_buffer = buffer->buffers[cpu];

	if (atomic_read(&cpu_buffer->record_disabled))
		goto out;

	event_length = rb_calculate_event_length(length);
	event = rb_reserve_next_event(cpu_buffer,
				      RINGBUF_TYPE_DATA, event_length);
	if (!event)
		goto out;

	body = rb_event_data(event);

	memcpy(body, data, length);

	rb_commit(cpu_buffer, event);

	ret = 0;
 out:
	ftrace_preempt_enable(resched);

	return ret;
}
EXPORT_SYMBOL_GPL(ring_buffer_write);

static int rb_per_cpu_empty(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct buffer_page *reader = cpu_buffer->reader_page;
	struct buffer_page *head = cpu_buffer->head_page;
	struct buffer_page *commit = cpu_buffer->commit_page;

	return reader->read == rb_page_commit(reader) &&
		(commit == reader ||
		 (commit == head &&
		  head->read == rb_page_commit(commit)));
}

/**
 * ring_buffer_record_disable - stop all writes into the buffer
 * @buffer: The ring buffer to stop writes to.
 *
 * This prevents all writes to the buffer. Any attempt to write
 * to the buffer after this will fail and return NULL.
 *
 * The caller should call synchronize_sched() after this.
 */
void ring_buffer_record_disable(struct ring_buffer *buffer)
{
	atomic_inc(&buffer->record_disabled);
}
EXPORT_SYMBOL_GPL(ring_buffer_record_disable);

/**
 * ring_buffer_record_enable - enable writes to the buffer
 * @buffer: The ring buffer to enable writes
 *
 * Note, multiple disables will need the same number of enables
 * to truely enable the writing (much like preempt_disable).
 */
void ring_buffer_record_enable(struct ring_buffer *buffer)
{
	atomic_dec(&buffer->record_disabled);
}
EXPORT_SYMBOL_GPL(ring_buffer_record_enable);

/**
 * ring_buffer_record_disable_cpu - stop all writes into the cpu_buffer
 * @buffer: The ring buffer to stop writes to.
 * @cpu: The CPU buffer to stop
 *
 * This prevents all writes to the buffer. Any attempt to write
 * to the buffer after this will fail and return NULL.
 *
 * The caller should call synchronize_sched() after this.
 */
void ring_buffer_record_disable_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return;

	cpu_buffer = buffer->buffers[cpu];
	atomic_inc(&cpu_buffer->record_disabled);
}
EXPORT_SYMBOL_GPL(ring_buffer_record_disable_cpu);

/**
 * ring_buffer_record_enable_cpu - enable writes to the buffer
 * @buffer: The ring buffer to enable writes
 * @cpu: The CPU to enable.
 *
 * Note, multiple disables will need the same number of enables
 * to truely enable the writing (much like preempt_disable).
 */
void ring_buffer_record_enable_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return;

	cpu_buffer = buffer->buffers[cpu];
	atomic_dec(&cpu_buffer->record_disabled);
}
EXPORT_SYMBOL_GPL(ring_buffer_record_enable_cpu);

/**
 * ring_buffer_entries_cpu - get the number of entries in a cpu buffer
 * @buffer: The ring buffer
 * @cpu: The per CPU buffer to get the entries from.
 */
unsigned long ring_buffer_entries_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long ret;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return 0;

	cpu_buffer = buffer->buffers[cpu];
	ret = cpu_buffer->entries;

	return ret;
}
EXPORT_SYMBOL_GPL(ring_buffer_entries_cpu);

/**
 * ring_buffer_overrun_cpu - get the number of overruns in a cpu_buffer
 * @buffer: The ring buffer
 * @cpu: The per CPU buffer to get the number of overruns from
 */
unsigned long ring_buffer_overrun_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long ret;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return 0;

	cpu_buffer = buffer->buffers[cpu];
	ret = cpu_buffer->overrun;

	return ret;
}
EXPORT_SYMBOL_GPL(ring_buffer_overrun_cpu);

/**
 * ring_buffer_entries - get the number of entries in a buffer
 * @buffer: The ring buffer
 *
 * Returns the total number of entries in the ring buffer
 * (all CPU entries)
 */
unsigned long ring_buffer_entries(struct ring_buffer *buffer)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long entries = 0;
	int cpu;

	/* if you care about this being correct, lock the buffer */
	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		entries += cpu_buffer->entries;
	}

	return entries;
}
EXPORT_SYMBOL_GPL(ring_buffer_entries);

/**
 * ring_buffer_overrun_cpu - get the number of overruns in buffer
 * @buffer: The ring buffer
 *
 * Returns the total number of overruns in the ring buffer
 * (all CPU entries)
 */
unsigned long ring_buffer_overruns(struct ring_buffer *buffer)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long overruns = 0;
	int cpu;

	/* if you care about this being correct, lock the buffer */
	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		overruns += cpu_buffer->overrun;
	}

	return overruns;
}
EXPORT_SYMBOL_GPL(ring_buffer_overruns);

static void rb_iter_reset(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;

	/* Iterator usage is expected to have record disabled */
	if (list_empty(&cpu_buffer->reader_page->list)) {
		iter->head_page = cpu_buffer->head_page;
		iter->head = cpu_buffer->head_page->read;
	} else {
		iter->head_page = cpu_buffer->reader_page;
		iter->head = cpu_buffer->reader_page->read;
	}
	if (iter->head)
		iter->read_stamp = cpu_buffer->read_stamp;
	else
		iter->read_stamp = iter->head_page->page->time_stamp;
}

/**
 * ring_buffer_iter_reset - reset an iterator
 * @iter: The iterator to reset
 *
 * Resets the iterator, so that it will start from the beginning
 * again.
 */
void ring_buffer_iter_reset(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	unsigned long flags;

	if (!iter)
		return;

	cpu_buffer = iter->cpu_buffer;

	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);
	rb_iter_reset(iter);
	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);
}
EXPORT_SYMBOL_GPL(ring_buffer_iter_reset);

/**
 * ring_buffer_iter_empty - check if an iterator has no more to read
 * @iter: The iterator to check
 */
int ring_buffer_iter_empty(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer;

	cpu_buffer = iter->cpu_buffer;

	return iter->head_page == cpu_buffer->commit_page &&
		iter->head == rb_commit_index(cpu_buffer);
}
EXPORT_SYMBOL_GPL(ring_buffer_iter_empty);

static void
rb_update_read_stamp(struct ring_buffer_per_cpu *cpu_buffer,
		     struct ring_buffer_event *event)
{
	u64 delta;

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		return;

	case RINGBUF_TYPE_TIME_EXTEND:
		delta = event->array[0];
		delta <<= TS_SHIFT;
		delta += event->time_delta;
		cpu_buffer->read_stamp += delta;
		return;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		return;

	case RINGBUF_TYPE_DATA:
		cpu_buffer->read_stamp += event->time_delta;
		return;

	default:
		BUG();
	}
	return;
}

static void
rb_update_iter_read_stamp(struct ring_buffer_iter *iter,
			  struct ring_buffer_event *event)
{
	u64 delta;

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		return;

	case RINGBUF_TYPE_TIME_EXTEND:
		delta = event->array[0];
		delta <<= TS_SHIFT;
		delta += event->time_delta;
		iter->read_stamp += delta;
		return;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		return;

	case RINGBUF_TYPE_DATA:
		iter->read_stamp += event->time_delta;
		return;

	default:
		BUG();
	}
	return;
}

static struct buffer_page *
rb_get_reader_page(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct buffer_page *reader = NULL;
	unsigned long flags;
	int nr_loops = 0;

	local_irq_save(flags);
	__raw_spin_lock(&cpu_buffer->lock);

 again:
	/*
	 * This should normally only loop twice. But because the
	 * start of the reader inserts an empty page, it causes
	 * a case where we will loop three times. There should be no
	 * reason to loop four times (that I know of).
	 */
	if (RB_WARN_ON(cpu_buffer, ++nr_loops > 3)) {
		reader = NULL;
		goto out;
	}

	reader = cpu_buffer->reader_page;

	/* If there's more to read, return this page */
	if (cpu_buffer->reader_page->read < rb_page_size(reader))
		goto out;

	/* Never should we have an index greater than the size */
	if (RB_WARN_ON(cpu_buffer,
		       cpu_buffer->reader_page->read > rb_page_size(reader)))
		goto out;

	/* check if we caught up to the tail */
	reader = NULL;
	if (cpu_buffer->commit_page == cpu_buffer->reader_page)
		goto out;

	/*
	 * Splice the empty reader page into the list around the head.
	 * Reset the reader page to size zero.
	 */

	reader = cpu_buffer->head_page;
	cpu_buffer->reader_page->list.next = reader->list.next;
	cpu_buffer->reader_page->list.prev = reader->list.prev;

	local_set(&cpu_buffer->reader_page->write, 0);
	local_set(&cpu_buffer->reader_page->page->commit, 0);

	/* Make the reader page now replace the head */
	reader->list.prev->next = &cpu_buffer->reader_page->list;
	reader->list.next->prev = &cpu_buffer->reader_page->list;

	/*
	 * If the tail is on the reader, then we must set the head
	 * to the inserted page, otherwise we set it one before.
	 */
	cpu_buffer->head_page = cpu_buffer->reader_page;

	if (cpu_buffer->commit_page != reader)
		rb_inc_page(cpu_buffer, &cpu_buffer->head_page);

	/* Finally update the reader page to the new head */
	cpu_buffer->reader_page = reader;
	rb_reset_reader_page(cpu_buffer);

	goto again;

 out:
	__raw_spin_unlock(&cpu_buffer->lock);
	local_irq_restore(flags);

	return reader;
}

static void rb_advance_reader(struct ring_buffer_per_cpu *cpu_buffer)
{
	struct ring_buffer_event *event;
	struct buffer_page *reader;
	unsigned length;

	reader = rb_get_reader_page(cpu_buffer);

	/* This function should not be called when buffer is empty */
	if (RB_WARN_ON(cpu_buffer, !reader))
		return;

	event = rb_reader_event(cpu_buffer);

	if (event->type == RINGBUF_TYPE_DATA || rb_discarded_event(event))
		cpu_buffer->entries--;

	rb_update_read_stamp(cpu_buffer, event);

	length = rb_event_length(event);
	cpu_buffer->reader_page->read += length;
}

static void rb_advance_iter(struct ring_buffer_iter *iter)
{
	struct ring_buffer *buffer;
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	unsigned length;

	cpu_buffer = iter->cpu_buffer;
	buffer = cpu_buffer->buffer;

	/*
	 * Check if we are at the end of the buffer.
	 */
	if (iter->head >= rb_page_size(iter->head_page)) {
		if (RB_WARN_ON(buffer,
			       iter->head_page == cpu_buffer->commit_page))
			return;
		rb_inc_iter(iter);
		return;
	}

	event = rb_iter_head_event(iter);

	length = rb_event_length(event);

	/*
	 * This should not be called to advance the header if we are
	 * at the tail of the buffer.
	 */
	if (RB_WARN_ON(cpu_buffer,
		       (iter->head_page == cpu_buffer->commit_page) &&
		       (iter->head + length > rb_commit_index(cpu_buffer))))
		return;

	rb_update_iter_read_stamp(iter, event);

	iter->head += length;

	/* check for end of page padding */
	if ((iter->head >= rb_page_size(iter->head_page)) &&
	    (iter->head_page != cpu_buffer->commit_page))
		rb_advance_iter(iter);
}

static struct ring_buffer_event *
rb_buffer_peek(struct ring_buffer *buffer, int cpu, u64 *ts)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	struct buffer_page *reader;
	int nr_loops = 0;

	cpu_buffer = buffer->buffers[cpu];

 again:
	/*
	 * We repeat when a timestamp is encountered. It is possible
	 * to get multiple timestamps from an interrupt entering just
	 * as one timestamp is about to be written. The max times
	 * that this can happen is the number of nested interrupts we
	 * can have.  Nesting 10 deep of interrupts is clearly
	 * an anomaly.
	 */
	if (RB_WARN_ON(cpu_buffer, ++nr_loops > 10))
		return NULL;

	reader = rb_get_reader_page(cpu_buffer);
	if (!reader)
		return NULL;

	event = rb_reader_event(cpu_buffer);

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		if (rb_null_event(event))
			RB_WARN_ON(cpu_buffer, 1);
		/*
		 * Because the writer could be discarding every
		 * event it creates (which would probably be bad)
		 * if we were to go back to "again" then we may never
		 * catch up, and will trigger the warn on, or lock
		 * the box. Return the padding, and we will release
		 * the current locks, and try again.
		 */
		return event;

	case RINGBUF_TYPE_TIME_EXTEND:
		/* Internal data, OK to advance */
		rb_advance_reader(cpu_buffer);
		goto again;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		rb_advance_reader(cpu_buffer);
		goto again;

	case RINGBUF_TYPE_DATA:
		if (ts) {
			*ts = cpu_buffer->read_stamp + event->time_delta;
			ring_buffer_normalize_time_stamp(buffer,
							 cpu_buffer->cpu, ts);
		}
		return event;

	default:
		BUG();
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ring_buffer_peek);

static struct ring_buffer_event *
rb_iter_peek(struct ring_buffer_iter *iter, u64 *ts)
{
	struct ring_buffer *buffer;
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event;
	int nr_loops = 0;

	if (ring_buffer_iter_empty(iter))
		return NULL;

	cpu_buffer = iter->cpu_buffer;
	buffer = cpu_buffer->buffer;

 again:
	/*
	 * We repeat when a timestamp is encountered. It is possible
	 * to get multiple timestamps from an interrupt entering just
	 * as one timestamp is about to be written. The max times
	 * that this can happen is the number of nested interrupts we
	 * can have. Nesting 10 deep of interrupts is clearly
	 * an anomaly.
	 */
	if (RB_WARN_ON(cpu_buffer, ++nr_loops > 10))
		return NULL;

	if (rb_per_cpu_empty(cpu_buffer))
		return NULL;

	event = rb_iter_head_event(iter);

	switch (event->type) {
	case RINGBUF_TYPE_PADDING:
		if (rb_null_event(event)) {
			rb_inc_iter(iter);
			goto again;
		}
		rb_advance_iter(iter);
		return event;

	case RINGBUF_TYPE_TIME_EXTEND:
		/* Internal data, OK to advance */
		rb_advance_iter(iter);
		goto again;

	case RINGBUF_TYPE_TIME_STAMP:
		/* FIXME: not implemented */
		rb_advance_iter(iter);
		goto again;

	case RINGBUF_TYPE_DATA:
		if (ts) {
			*ts = iter->read_stamp + event->time_delta;
			ring_buffer_normalize_time_stamp(buffer,
							 cpu_buffer->cpu, ts);
		}
		return event;

	default:
		BUG();
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ring_buffer_iter_peek);

/**
 * ring_buffer_peek - peek at the next event to be read
 * @buffer: The ring buffer to read
 * @cpu: The cpu to peak at
 * @ts: The timestamp counter of this event.
 *
 * This will return the event that will be read next, but does
 * not consume the data.
 */
struct ring_buffer_event *
ring_buffer_peek(struct ring_buffer *buffer, int cpu, u64 *ts)
{
	struct ring_buffer_per_cpu *cpu_buffer = buffer->buffers[cpu];
	struct ring_buffer_event *event;
	unsigned long flags;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return NULL;

 again:
	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);
	event = rb_buffer_peek(buffer, cpu, ts);
	if (event && event->type == RINGBUF_TYPE_PADDING)
		rb_advance_reader(cpu_buffer);
	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);

	if (event && event->type == RINGBUF_TYPE_PADDING) {
		cpu_relax();
		goto again;
	}

	return event;
}

/**
 * ring_buffer_iter_peek - peek at the next event to be read
 * @iter: The ring buffer iterator
 * @ts: The timestamp counter of this event.
 *
 * This will return the event that will be read next, but does
 * not increment the iterator.
 */
struct ring_buffer_event *
ring_buffer_iter_peek(struct ring_buffer_iter *iter, u64 *ts)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;
	struct ring_buffer_event *event;
	unsigned long flags;

 again:
	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);
	event = rb_iter_peek(iter, ts);
	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);

	if (event && event->type == RINGBUF_TYPE_PADDING) {
		cpu_relax();
		goto again;
	}

	return event;
}

/**
 * ring_buffer_consume - return an event and consume it
 * @buffer: The ring buffer to get the next event from
 *
 * Returns the next event in the ring buffer, and that event is consumed.
 * Meaning, that sequential reads will keep returning a different event,
 * and eventually empty the ring buffer if the producer is slower.
 */
struct ring_buffer_event *
ring_buffer_consume(struct ring_buffer *buffer, int cpu, u64 *ts)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_event *event = NULL;
	unsigned long flags;

 again:
	/* might be called in atomic */
	preempt_disable();

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		goto out;

	cpu_buffer = buffer->buffers[cpu];
	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);

	event = rb_buffer_peek(buffer, cpu, ts);
	if (event)
		rb_advance_reader(cpu_buffer);

	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);

 out:
	preempt_enable();

	if (event && event->type == RINGBUF_TYPE_PADDING) {
		cpu_relax();
		goto again;
	}

	return event;
}
EXPORT_SYMBOL_GPL(ring_buffer_consume);

/**
 * ring_buffer_read_start - start a non consuming read of the buffer
 * @buffer: The ring buffer to read from
 * @cpu: The cpu buffer to iterate over
 *
 * This starts up an iteration through the buffer. It also disables
 * the recording to the buffer until the reading is finished.
 * This prevents the reading from being corrupted. This is not
 * a consuming read, so a producer is not expected.
 *
 * Must be paired with ring_buffer_finish.
 */
struct ring_buffer_iter *
ring_buffer_read_start(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	struct ring_buffer_iter *iter;
	unsigned long flags;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return NULL;

	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return NULL;

	cpu_buffer = buffer->buffers[cpu];

	iter->cpu_buffer = cpu_buffer;

	atomic_inc(&cpu_buffer->record_disabled);
	synchronize_sched();

	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);
	__raw_spin_lock(&cpu_buffer->lock);
	rb_iter_reset(iter);
	__raw_spin_unlock(&cpu_buffer->lock);
	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);

	return iter;
}
EXPORT_SYMBOL_GPL(ring_buffer_read_start);

/**
 * ring_buffer_finish - finish reading the iterator of the buffer
 * @iter: The iterator retrieved by ring_buffer_start
 *
 * This re-enables the recording to the buffer, and frees the
 * iterator.
 */
void
ring_buffer_read_finish(struct ring_buffer_iter *iter)
{
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;

	atomic_dec(&cpu_buffer->record_disabled);
	kfree(iter);
}
EXPORT_SYMBOL_GPL(ring_buffer_read_finish);

/**
 * ring_buffer_read - read the next item in the ring buffer by the iterator
 * @iter: The ring buffer iterator
 * @ts: The time stamp of the event read.
 *
 * This reads the next event in the ring buffer and increments the iterator.
 */
struct ring_buffer_event *
ring_buffer_read(struct ring_buffer_iter *iter, u64 *ts)
{
	struct ring_buffer_event *event;
	struct ring_buffer_per_cpu *cpu_buffer = iter->cpu_buffer;
	unsigned long flags;

 again:
	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);
	event = rb_iter_peek(iter, ts);
	if (!event)
		goto out;

	rb_advance_iter(iter);
 out:
	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);

	if (event && event->type == RINGBUF_TYPE_PADDING) {
		cpu_relax();
		goto again;
	}

	return event;
}
EXPORT_SYMBOL_GPL(ring_buffer_read);

/**
 * ring_buffer_size - return the size of the ring buffer (in bytes)
 * @buffer: The ring buffer.
 */
unsigned long ring_buffer_size(struct ring_buffer *buffer)
{
	return BUF_PAGE_SIZE * buffer->pages;
}
EXPORT_SYMBOL_GPL(ring_buffer_size);

static void
rb_reset_cpu(struct ring_buffer_per_cpu *cpu_buffer)
{
	cpu_buffer->head_page
		= list_entry(cpu_buffer->pages.next, struct buffer_page, list);
	local_set(&cpu_buffer->head_page->write, 0);
	local_set(&cpu_buffer->head_page->page->commit, 0);

	cpu_buffer->head_page->read = 0;

	cpu_buffer->tail_page = cpu_buffer->head_page;
	cpu_buffer->commit_page = cpu_buffer->head_page;

	INIT_LIST_HEAD(&cpu_buffer->reader_page->list);
	local_set(&cpu_buffer->reader_page->write, 0);
	local_set(&cpu_buffer->reader_page->page->commit, 0);
	cpu_buffer->reader_page->read = 0;

	cpu_buffer->overrun = 0;
	cpu_buffer->entries = 0;

	cpu_buffer->write_stamp = 0;
	cpu_buffer->read_stamp = 0;
}

/**
 * ring_buffer_reset_cpu - reset a ring buffer per CPU buffer
 * @buffer: The ring buffer to reset a per cpu buffer of
 * @cpu: The CPU buffer to be reset
 */
void ring_buffer_reset_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer = buffer->buffers[cpu];
	unsigned long flags;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return;

	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);

	__raw_spin_lock(&cpu_buffer->lock);

	rb_reset_cpu(cpu_buffer);

	__raw_spin_unlock(&cpu_buffer->lock);

	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);
}
EXPORT_SYMBOL_GPL(ring_buffer_reset_cpu);

/**
 * ring_buffer_reset - reset a ring buffer
 * @buffer: The ring buffer to reset all cpu buffers
 */
void ring_buffer_reset(struct ring_buffer *buffer)
{
	int cpu;

	for_each_buffer_cpu(buffer, cpu)
		ring_buffer_reset_cpu(buffer, cpu);
}
EXPORT_SYMBOL_GPL(ring_buffer_reset);

/**
 * rind_buffer_empty - is the ring buffer empty?
 * @buffer: The ring buffer to test
 */
int ring_buffer_empty(struct ring_buffer *buffer)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	int cpu;

	/* yes this is racy, but if you don't like the race, lock the buffer */
	for_each_buffer_cpu(buffer, cpu) {
		cpu_buffer = buffer->buffers[cpu];
		if (!rb_per_cpu_empty(cpu_buffer))
			return 0;
	}

	return 1;
}
EXPORT_SYMBOL_GPL(ring_buffer_empty);

/**
 * ring_buffer_empty_cpu - is a cpu buffer of a ring buffer empty?
 * @buffer: The ring buffer
 * @cpu: The CPU buffer to test
 */
int ring_buffer_empty_cpu(struct ring_buffer *buffer, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer;
	int ret;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		return 1;

	cpu_buffer = buffer->buffers[cpu];
	ret = rb_per_cpu_empty(cpu_buffer);


	return ret;
}
EXPORT_SYMBOL_GPL(ring_buffer_empty_cpu);

/**
 * ring_buffer_swap_cpu - swap a CPU buffer between two ring buffers
 * @buffer_a: One buffer to swap with
 * @buffer_b: The other buffer to swap with
 *
 * This function is useful for tracers that want to take a "snapshot"
 * of a CPU buffer and has another back up buffer lying around.
 * it is expected that the tracer handles the cpu buffer not being
 * used at the moment.
 */
int ring_buffer_swap_cpu(struct ring_buffer *buffer_a,
			 struct ring_buffer *buffer_b, int cpu)
{
	struct ring_buffer_per_cpu *cpu_buffer_a;
	struct ring_buffer_per_cpu *cpu_buffer_b;
	int ret = -EINVAL;

	if (!cpumask_test_cpu(cpu, buffer_a->cpumask) ||
	    !cpumask_test_cpu(cpu, buffer_b->cpumask))
		goto out;

	/* At least make sure the two buffers are somewhat the same */
	if (buffer_a->pages != buffer_b->pages)
		goto out;

	ret = -EAGAIN;

	if (ring_buffer_flags != RB_BUFFERS_ON)
		goto out;

	if (atomic_read(&buffer_a->record_disabled))
		goto out;

	if (atomic_read(&buffer_b->record_disabled))
		goto out;

	cpu_buffer_a = buffer_a->buffers[cpu];
	cpu_buffer_b = buffer_b->buffers[cpu];

	if (atomic_read(&cpu_buffer_a->record_disabled))
		goto out;

	if (atomic_read(&cpu_buffer_b->record_disabled))
		goto out;

	/*
	 * We can't do a synchronize_sched here because this
	 * function can be called in atomic context.
	 * Normally this will be called from the same CPU as cpu.
	 * If not it's up to the caller to protect this.
	 */
	atomic_inc(&cpu_buffer_a->record_disabled);
	atomic_inc(&cpu_buffer_b->record_disabled);

	buffer_a->buffers[cpu] = cpu_buffer_b;
	buffer_b->buffers[cpu] = cpu_buffer_a;

	cpu_buffer_b->buffer = buffer_a;
	cpu_buffer_a->buffer = buffer_b;

	atomic_dec(&cpu_buffer_a->record_disabled);
	atomic_dec(&cpu_buffer_b->record_disabled);

	ret = 0;
out:
	return ret;
}
EXPORT_SYMBOL_GPL(ring_buffer_swap_cpu);

static void rb_remove_entries(struct ring_buffer_per_cpu *cpu_buffer,
			      struct buffer_data_page *bpage,
			      unsigned int offset)
{
	struct ring_buffer_event *event;
	unsigned long head;

	__raw_spin_lock(&cpu_buffer->lock);
	for (head = offset; head < local_read(&bpage->commit);
	     head += rb_event_length(event)) {

		event = __rb_data_page_index(bpage, head);
		if (RB_WARN_ON(cpu_buffer, rb_null_event(event)))
			return;
		/* Only count data entries */
		if (event->type != RINGBUF_TYPE_DATA)
			continue;
		cpu_buffer->entries--;
	}
	__raw_spin_unlock(&cpu_buffer->lock);
}

/**
 * ring_buffer_alloc_read_page - allocate a page to read from buffer
 * @buffer: the buffer to allocate for.
 *
 * This function is used in conjunction with ring_buffer_read_page.
 * When reading a full page from the ring buffer, these functions
 * can be used to speed up the process. The calling function should
 * allocate a few pages first with this function. Then when it
 * needs to get pages from the ring buffer, it passes the result
 * of this function into ring_buffer_read_page, which will swap
 * the page that was allocated, with the read page of the buffer.
 *
 * Returns:
 *  The page allocated, or NULL on error.
 */
void *ring_buffer_alloc_read_page(struct ring_buffer *buffer)
{
	struct buffer_data_page *bpage;
	unsigned long addr;

	addr = __get_free_page(GFP_KERNEL);
	if (!addr)
		return NULL;

	bpage = (void *)addr;

	rb_init_page(bpage);

	return bpage;
}

/**
 * ring_buffer_free_read_page - free an allocated read page
 * @buffer: the buffer the page was allocate for
 * @data: the page to free
 *
 * Free a page allocated from ring_buffer_alloc_read_page.
 */
void ring_buffer_free_read_page(struct ring_buffer *buffer, void *data)
{
	free_page((unsigned long)data);
}

/**
 * ring_buffer_read_page - extract a page from the ring buffer
 * @buffer: buffer to extract from
 * @data_page: the page to use allocated from ring_buffer_alloc_read_page
 * @len: amount to extract
 * @cpu: the cpu of the buffer to extract
 * @full: should the extraction only happen when the page is full.
 *
 * This function will pull out a page from the ring buffer and consume it.
 * @data_page must be the address of the variable that was returned
 * from ring_buffer_alloc_read_page. This is because the page might be used
 * to swap with a page in the ring buffer.
 *
 * for example:
 *	rpage = ring_buffer_alloc_read_page(buffer);
 *	if (!rpage)
 *		return error;
 *	ret = ring_buffer_read_page(buffer, &rpage, len, cpu, 0);
 *	if (ret >= 0)
 *		process_page(rpage, ret);
 *
 * When @full is set, the function will not return true unless
 * the writer is off the reader page.
 *
 * Note: it is up to the calling functions to handle sleeps and wakeups.
 *  The ring buffer can be used anywhere in the kernel and can not
 *  blindly call wake_up. The layer that uses the ring buffer must be
 *  responsible for that.
 *
 * Returns:
 *  >=0 if data has been transferred, returns the offset of consumed data.
 *  <0 if no data has been transferred.
 */
int ring_buffer_read_page(struct ring_buffer *buffer,
			  void **data_page, size_t len, int cpu, int full)
{
	struct ring_buffer_per_cpu *cpu_buffer = buffer->buffers[cpu];
	struct ring_buffer_event *event;
	struct buffer_data_page *bpage;
	struct buffer_page *reader;
	unsigned long flags;
	unsigned int commit;
	unsigned int read;
	u64 save_timestamp;
	int ret = -1;

	if (!cpumask_test_cpu(cpu, buffer->cpumask))
		goto out;

	/*
	 * If len is not big enough to hold the page header, then
	 * we can not copy anything.
	 */
	if (len <= BUF_PAGE_HDR_SIZE)
		goto out;

	len -= BUF_PAGE_HDR_SIZE;

	if (!data_page)
		goto out;

	bpage = *data_page;
	if (!bpage)
		goto out;

	spin_lock_irqsave(&cpu_buffer->reader_lock, flags);

	reader = rb_get_reader_page(cpu_buffer);
	if (!reader)
		goto out_unlock;

	event = rb_reader_event(cpu_buffer);

	read = reader->read;
	commit = rb_page_commit(reader);

	/*
	 * If this page has been partially read or
	 * if len is not big enough to read the rest of the page or
	 * a writer is still on the page, then
	 * we must copy the data from the page to the buffer.
	 * Otherwise, we can simply swap the page with the one passed in.
	 */
	if (read || (len < (commit - read)) ||
	    cpu_buffer->reader_page == cpu_buffer->commit_page) {
		struct buffer_data_page *rpage = cpu_buffer->reader_page->page;
		unsigned int rpos = read;
		unsigned int pos = 0;
		unsigned int size;

		if (full)
			goto out_unlock;

		if (len > (commit - read))
			len = (commit - read);

		size = rb_event_length(event);

		if (len < size)
			goto out_unlock;

		/* save the current timestamp, since the user will need it */
		save_timestamp = cpu_buffer->read_stamp;

		/* Need to copy one event at a time */
		do {
			memcpy(bpage->data + pos, rpage->data + rpos, size);

			len -= size;

			rb_advance_reader(cpu_buffer);
			rpos = reader->read;
			pos += size;

			event = rb_reader_event(cpu_buffer);
			size = rb_event_length(event);
		} while (len > size);

		/* update bpage */
		local_set(&bpage->commit, pos);
		bpage->time_stamp = save_timestamp;

		/* we copied everything to the beginning */
		read = 0;
	} else {
		/* swap the pages */
		rb_init_page(bpage);
		bpage = reader->page;
		reader->page = *data_page;
		local_set(&reader->write, 0);
		reader->read = 0;
		*data_page = bpage;

		/* update the entry counter */
		rb_remove_entries(cpu_buffer, bpage, read);
	}
	ret = read;

 out_unlock:
	spin_unlock_irqrestore(&cpu_buffer->reader_lock, flags);

 out:
	return ret;
}

static ssize_t
rb_simple_read(struct file *filp, char __user *ubuf,
	       size_t cnt, loff_t *ppos)
{
	unsigned long *p = filp->private_data;
	char buf[64];
	int r;

	if (test_bit(RB_BUFFERS_DISABLED_BIT, p))
		r = sprintf(buf, "permanently disabled\n");
	else
		r = sprintf(buf, "%d\n", test_bit(RB_BUFFERS_ON_BIT, p));

	return simple_read_from_buffer(ubuf, cnt, ppos, buf, r);
}

static ssize_t
rb_simple_write(struct file *filp, const char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	unsigned long *p = filp->private_data;
	char buf[64];
	unsigned long val;
	int ret;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	ret = strict_strtoul(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val)
		set_bit(RB_BUFFERS_ON_BIT, p);
	else
		clear_bit(RB_BUFFERS_ON_BIT, p);

	(*ppos)++;

	return cnt;
}

static const struct file_operations rb_simple_fops = {
	.open		= tracing_open_generic,
	.read		= rb_simple_read,
	.write		= rb_simple_write,
};


static __init int rb_init_debugfs(void)
{
	struct dentry *d_tracer;
	struct dentry *entry;

	d_tracer = tracing_init_dentry();

	entry = debugfs_create_file("tracing_on", 0644, d_tracer,
				    &ring_buffer_flags, &rb_simple_fops);
	if (!entry)
		pr_warning("Could not create debugfs 'tracing_on' entry\n");

	return 0;
}

fs_initcall(rb_init_debugfs);

#ifdef CONFIG_HOTPLUG_CPU
static int rb_cpu_notify(struct notifier_block *self,
			 unsigned long action, void *hcpu)
{
	struct ring_buffer *buffer =
		container_of(self, struct ring_buffer, cpu_notify);
	long cpu = (long)hcpu;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		if (cpu_isset(cpu, *buffer->cpumask))
			return NOTIFY_OK;

		buffer->buffers[cpu] =
			rb_allocate_cpu_buffer(buffer, cpu);
		if (!buffer->buffers[cpu]) {
			WARN(1, "failed to allocate ring buffer on CPU %ld\n",
			     cpu);
			return NOTIFY_OK;
		}
		smp_wmb();
		cpu_set(cpu, *buffer->cpumask);
		break;
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		/*
		 * Do nothing.
		 *  If we were to free the buffer, then the user would
		 *  lose any trace that was in the buffer.
		 */
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}
#endif
