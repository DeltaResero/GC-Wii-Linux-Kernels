/*
 * linux/mm/slab.c
 * Written by Mark Hemment, 1996/97.
 * (markhe@nextd.demon.co.uk)
 *
 * kmem_cache_destroy() + some cleanup - 1999 Andrea Arcangeli
 *
 * Major cleanup, different bufctl logic, per-cpu arrays
 *	(c) 2000 Manfred Spraul
 *
 * Cleanup, make the head arrays unconditional, preparation for NUMA
 * 	(c) 2002 Manfred Spraul
 *
 * An implementation of the Slab Allocator as described in outline in;
 *	UNIX Internals: The New Frontiers by Uresh Vahalia
 *	Pub: Prentice Hall	ISBN 0-13-101908-2
 * or with a little more detail in;
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator
 *	Jeff Bonwick (Sun Microsystems).
 *	Presented at: USENIX Summer 1994 Technical Conference
 *
 * The memory is organized in caches, one cache for each object type.
 * (e.g. inode_cache, dentry_cache, buffer_head, vm_area_struct)
 * Each cache consists out of many slabs (they are small (usually one
 * page long) and always contiguous), and each slab contains multiple
 * initialized objects.
 *
 * This means, that your constructor is used only for newly allocated
 * slabs and you must pass objects with the same intializations to
 * kmem_cache_free.
 *
 * Each cache can only support one memory type (GFP_DMA, GFP_HIGHMEM,
 * normal). If you need a special memory type, then must create a new
 * cache for that memory type.
 *
 * In order to reduce fragmentation, the slabs are sorted in 3 groups:
 *   full slabs with 0 free objects
 *   partial slabs
 *   empty slabs with no allocated objects
 *
 * If partial slabs exist, then new allocations come from these slabs,
 * otherwise from empty slabs or new slabs are allocated.
 *
 * kmem_cache_destroy() CAN CRASH if you try to allocate from the cache
 * during kmem_cache_destroy(). The caller must prevent concurrent allocs.
 *
 * Each cache has a short per-cpu head array, most allocs
 * and frees go into that array, and if that array overflows, then 1/2
 * of the entries in the array are given back into the global cache.
 * The head array is strictly LIFO and should improve the cache hit rates.
 * On SMP, it additionally reduces the spinlock operations.
 *
 * The c_cpuarray may not be read with enabled local interrupts - 
 * it's changed with a smp_call_function().
 *
 * SMP synchronization:
 *  constructors and destructors are called without any locking.
 *  Several members in struct kmem_cache and struct slab never change, they
 *	are accessed without any locking.
 *  The per-cpu arrays are never accessed from the wrong cpu, no locking,
 *  	and local interrupts are disabled so slab code is preempt-safe.
 *  The non-constant members are protected with a per-cache irq spinlock.
 *
 * Many thanks to Mark Hemment, who wrote another per-cpu slab patch
 * in 2000 - many ideas in the current implementation are derived from
 * his patch.
 *
 * Further notes from the original documentation:
 *
 * 11 April '97.  Started multi-threading - markhe
 *	The global cache-chain is protected by the mutex 'cache_chain_mutex'.
 *	The sem is only needed when accessing/extending the cache-chain, which
 *	can never happen inside an interrupt (kmem_cache_create(),
 *	kmem_cache_shrink() and kmem_cache_reap()).
 *
 *	At present, each engine can be growing a cache.  This should be blocked.
 *
 * 15 March 2005. NUMA slab allocator.
 *	Shai Fultheim <shai@scalex86.org>.
 *	Shobhit Dayal <shobhit@calsoftinc.com>
 *	Alok N Kataria <alokk@calsoftinc.com>
 *	Christoph Lameter <christoph@lameter.com>
 *
 *	Modified the slab allocator to be node aware on NUMA systems.
 *	Each node has its own list of partial, free and full slabs.
 *	All object allocations for a node occur from node specific slab lists.
 */

#include	<linux/config.h>
#include	<linux/slab.h>
#include	<linux/mm.h>
#include	<linux/swap.h>
#include	<linux/cache.h>
#include	<linux/interrupt.h>
#include	<linux/init.h>
#include	<linux/compiler.h>
#include	<linux/seq_file.h>
#include	<linux/notifier.h>
#include	<linux/kallsyms.h>
#include	<linux/cpu.h>
#include	<linux/sysctl.h>
#include	<linux/module.h>
#include	<linux/rcupdate.h>
#include	<linux/string.h>
#include	<linux/nodemask.h>
#include	<linux/mempolicy.h>
#include	<linux/mutex.h>

#include	<asm/uaccess.h>
#include	<asm/cacheflush.h>
#include	<asm/tlbflush.h>
#include	<asm/page.h>

/*
 * DEBUG	- 1 for kmem_cache_create() to honour; SLAB_DEBUG_INITIAL,
 *		  SLAB_RED_ZONE & SLAB_POISON.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * STATS	- 1 to collect stats for /proc/slabinfo.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * FORCED_DEBUG	- 1 enables SLAB_RED_ZONE and SLAB_POISON (if possible)
 */

#ifdef CONFIG_DEBUG_SLAB
#define	DEBUG		1
#define	STATS		1
#define	FORCED_DEBUG	1
#else
#define	DEBUG		0
#define	STATS		0
#define	FORCED_DEBUG	0
#endif

/* Shouldn't this be in a header file somewhere? */
#define	BYTES_PER_WORD		sizeof(void *)

#ifndef cache_line_size
#define cache_line_size()	L1_CACHE_BYTES
#endif

#ifndef ARCH_KMALLOC_MINALIGN
/*
 * Enforce a minimum alignment for the kmalloc caches.
 * Usually, the kmalloc caches are cache_line_size() aligned, except when
 * DEBUG and FORCED_DEBUG are enabled, then they are BYTES_PER_WORD aligned.
 * Some archs want to perform DMA into kmalloc caches and need a guaranteed
 * alignment larger than BYTES_PER_WORD. ARCH_KMALLOC_MINALIGN allows that.
 * Note that this flag disables some debug features.
 */
#define ARCH_KMALLOC_MINALIGN 0
#endif

#ifndef ARCH_SLAB_MINALIGN
/*
 * Enforce a minimum alignment for all caches.
 * Intended for archs that get misalignment faults even for BYTES_PER_WORD
 * aligned buffers. Includes ARCH_KMALLOC_MINALIGN.
 * If possible: Do not enable this flag for CONFIG_DEBUG_SLAB, it disables
 * some debug features.
 */
#define ARCH_SLAB_MINALIGN 0
#endif

#ifndef ARCH_KMALLOC_FLAGS
#define ARCH_KMALLOC_FLAGS SLAB_HWCACHE_ALIGN
#endif

/* Legal flag mask for kmem_cache_create(). */
#if DEBUG
# define CREATE_MASK	(SLAB_DEBUG_INITIAL | SLAB_RED_ZONE | \
			 SLAB_POISON | SLAB_HWCACHE_ALIGN | \
			 SLAB_NO_REAP | SLAB_CACHE_DMA | \
			 SLAB_MUST_HWCACHE_ALIGN | SLAB_STORE_USER | \
			 SLAB_RECLAIM_ACCOUNT | SLAB_PANIC | \
			 SLAB_DESTROY_BY_RCU)
#else
# define CREATE_MASK	(SLAB_HWCACHE_ALIGN | SLAB_NO_REAP | \
			 SLAB_CACHE_DMA | SLAB_MUST_HWCACHE_ALIGN | \
			 SLAB_RECLAIM_ACCOUNT | SLAB_PANIC | \
			 SLAB_DESTROY_BY_RCU)
#endif

/*
 * kmem_bufctl_t:
 *
 * Bufctl's are used for linking objs within a slab
 * linked offsets.
 *
 * This implementation relies on "struct page" for locating the cache &
 * slab an object belongs to.
 * This allows the bufctl structure to be small (one int), but limits
 * the number of objects a slab (not a cache) can contain when off-slab
 * bufctls are used. The limit is the size of the largest general cache
 * that does not use off-slab slabs.
 * For 32bit archs with 4 kB pages, is this 56.
 * This is not serious, as it is only for large objects, when it is unwise
 * to have too many per slab.
 * Note: This limit can be raised by introducing a general cache whose size
 * is less than 512 (PAGE_SIZE<<3), but greater than 256.
 */

typedef unsigned int kmem_bufctl_t;
#define BUFCTL_END	(((kmem_bufctl_t)(~0U))-0)
#define BUFCTL_FREE	(((kmem_bufctl_t)(~0U))-1)
#define	SLAB_LIMIT	(((kmem_bufctl_t)(~0U))-2)

/* Max number of objs-per-slab for caches which use off-slab slabs.
 * Needed to avoid a possible looping condition in cache_grow().
 */
static unsigned long offslab_limit;

/*
 * struct slab
 *
 * Manages the objs in a slab. Placed either at the beginning of mem allocated
 * for a slab, or allocated from an general cache.
 * Slabs are chained into three list: fully used, partial, fully free slabs.
 */
struct slab {
	struct list_head list;
	unsigned long colouroff;
	void *s_mem;		/* including colour offset */
	unsigned int inuse;	/* num of objs active in slab */
	kmem_bufctl_t free;
	unsigned short nodeid;
};

/*
 * struct slab_rcu
 *
 * slab_destroy on a SLAB_DESTROY_BY_RCU cache uses this structure to
 * arrange for kmem_freepages to be called via RCU.  This is useful if
 * we need to approach a kernel structure obliquely, from its address
 * obtained without the usual locking.  We can lock the structure to
 * stabilize it and check it's still at the given address, only if we
 * can be sure that the memory has not been meanwhile reused for some
 * other kind of object (which our subsystem's lock might corrupt).
 *
 * rcu_read_lock before reading the address, then rcu_read_unlock after
 * taking the spinlock within the structure expected at that address.
 *
 * We assume struct slab_rcu can overlay struct slab when destroying.
 */
struct slab_rcu {
	struct rcu_head head;
	struct kmem_cache *cachep;
	void *addr;
};

/*
 * struct array_cache
 *
 * Purpose:
 * - LIFO ordering, to hand out cache-warm objects from _alloc
 * - reduce the number of linked list operations
 * - reduce spinlock operations
 *
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 *
 */
struct array_cache {
	unsigned int avail;
	unsigned int limit;
	unsigned int batchcount;
	unsigned int touched;
	spinlock_t lock;
	void *entry[0];		/*
				 * Must have this definition in here for the proper
				 * alignment of array_cache. Also simplifies accessing
				 * the entries.
				 * [0] is for gcc 2.95. It should really be [].
				 */
};

/* bootstrap: The caches do not work without cpuarrays anymore,
 * but the cpuarrays are allocated from the generic caches...
 */
#define BOOT_CPUCACHE_ENTRIES	1
struct arraycache_init {
	struct array_cache cache;
	void *entries[BOOT_CPUCACHE_ENTRIES];
};

/*
 * The slab lists for all objects.
 */
struct kmem_list3 {
	struct list_head slabs_partial;	/* partial list first, better asm code */
	struct list_head slabs_full;
	struct list_head slabs_free;
	unsigned long free_objects;
	unsigned long next_reap;
	int free_touched;
	unsigned int free_limit;
	unsigned int colour_next;	/* Per-node cache coloring */
	spinlock_t list_lock;
	struct array_cache *shared;	/* shared per node */
	struct array_cache **alien;	/* on other nodes */
};

/*
 * Need this for bootstrapping a per node allocator.
 */
#define NUM_INIT_LISTS (2 * MAX_NUMNODES + 1)
struct kmem_list3 __initdata initkmem_list3[NUM_INIT_LISTS];
#define	CACHE_CACHE 0
#define	SIZE_AC 1
#define	SIZE_L3 (1 + MAX_NUMNODES)

/*
 * This function must be completely optimized away if
 * a constant is passed to it. Mostly the same as
 * what is in linux/slab.h except it returns an
 * index.
 */
static __always_inline int index_of(const size_t size)
{
	extern void __bad_size(void);

	if (__builtin_constant_p(size)) {
		int i = 0;

#define CACHE(x) \
	if (size <=x) \
		return i; \
	else \
		i++;
#include "linux/kmalloc_sizes.h"
#undef CACHE
		__bad_size();
	} else
		__bad_size();
	return 0;
}

#define INDEX_AC index_of(sizeof(struct arraycache_init))
#define INDEX_L3 index_of(sizeof(struct kmem_list3))

static void kmem_list3_init(struct kmem_list3 *parent)
{
	INIT_LIST_HEAD(&parent->slabs_full);
	INIT_LIST_HEAD(&parent->slabs_partial);
	INIT_LIST_HEAD(&parent->slabs_free);
	parent->shared = NULL;
	parent->alien = NULL;
	parent->colour_next = 0;
	spin_lock_init(&parent->list_lock);
	parent->free_objects = 0;
	parent->free_touched = 0;
}

#define MAKE_LIST(cachep, listp, slab, nodeid)	\
	do {	\
		INIT_LIST_HEAD(listp);		\
		list_splice(&(cachep->nodelists[nodeid]->slab), listp); \
	} while (0)

#define	MAKE_ALL_LISTS(cachep, ptr, nodeid)			\
	do {					\
	MAKE_LIST((cachep), (&(ptr)->slabs_full), slabs_full, nodeid);	\
	MAKE_LIST((cachep), (&(ptr)->slabs_partial), slabs_partial, nodeid); \
	MAKE_LIST((cachep), (&(ptr)->slabs_free), slabs_free, nodeid);	\
	} while (0)

/*
 * struct kmem_cache
 *
 * manages a cache.
 */

struct kmem_cache {
/* 1) per-cpu data, touched during every alloc/free */
	struct array_cache *array[NR_CPUS];
	unsigned int batchcount;
	unsigned int limit;
	unsigned int shared;
	unsigned int buffer_size;
/* 2) touched by every alloc & free from the backend */
	struct kmem_list3 *nodelists[MAX_NUMNODES];
	unsigned int flags;	/* constant flags */
	unsigned int num;	/* # of objs per slab */
	spinlock_t spinlock;

/* 3) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	unsigned int gfporder;

	/* force GFP flags, e.g. GFP_DMA */
	gfp_t gfpflags;

	size_t colour;		/* cache colouring range */
	unsigned int colour_off;	/* colour offset */
	struct kmem_cache *slabp_cache;
	unsigned int slab_size;
	unsigned int dflags;	/* dynamic flags */

	/* constructor func */
	void (*ctor) (void *, struct kmem_cache *, unsigned long);

	/* de-constructor func */
	void (*dtor) (void *, struct kmem_cache *, unsigned long);

/* 4) cache creation/removal */
	const char *name;
	struct list_head next;

/* 5) statistics */
#if STATS
	unsigned long num_active;
	unsigned long num_allocations;
	unsigned long high_mark;
	unsigned long grown;
	unsigned long reaped;
	unsigned long errors;
	unsigned long max_freeable;
	unsigned long node_allocs;
	unsigned long node_frees;
	atomic_t allochit;
	atomic_t allocmiss;
	atomic_t freehit;
	atomic_t freemiss;
#endif
#if DEBUG
	/*
	 * If debugging is enabled, then the allocator can add additional
	 * fields and/or padding to every object. buffer_size contains the total
	 * object size including these internal fields, the following two
	 * variables contain the offset to the user object and its size.
	 */
	int obj_offset;
	int obj_size;
#endif
};

#define CFLGS_OFF_SLAB		(0x80000000UL)
#define	OFF_SLAB(x)	((x)->flags & CFLGS_OFF_SLAB)

#define BATCHREFILL_LIMIT	16
/* Optimization question: fewer reaps means less 
 * probability for unnessary cpucache drain/refill cycles.
 *
 * OTOH the cpuarrays can contain lots of objects,
 * which could lock up otherwise freeable slabs.
 */
#define REAPTIMEOUT_CPUC	(2*HZ)
#define REAPTIMEOUT_LIST3	(4*HZ)

#if STATS
#define	STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	STATS_INC_GROWN(x)	((x)->grown++)
#define	STATS_INC_REAPED(x)	((x)->reaped++)
#define	STATS_SET_HIGH(x)	do { if ((x)->num_active > (x)->high_mark) \
					(x)->high_mark = (x)->num_active; \
				} while (0)
#define	STATS_INC_ERR(x)	((x)->errors++)
#define	STATS_INC_NODEALLOCS(x)	((x)->node_allocs++)
#define	STATS_INC_NODEFREES(x)	((x)->node_frees++)
#define	STATS_SET_FREEABLE(x, i) \
				do { if ((x)->max_freeable < i) \
					(x)->max_freeable = i; \
				} while (0)

#define STATS_INC_ALLOCHIT(x)	atomic_inc(&(x)->allochit)
#define STATS_INC_ALLOCMISS(x)	atomic_inc(&(x)->allocmiss)
#define STATS_INC_FREEHIT(x)	atomic_inc(&(x)->freehit)
#define STATS_INC_FREEMISS(x)	atomic_inc(&(x)->freemiss)
#else
#define	STATS_INC_ACTIVE(x)	do { } while (0)
#define	STATS_DEC_ACTIVE(x)	do { } while (0)
#define	STATS_INC_ALLOCED(x)	do { } while (0)
#define	STATS_INC_GROWN(x)	do { } while (0)
#define	STATS_INC_REAPED(x)	do { } while (0)
#define	STATS_SET_HIGH(x)	do { } while (0)
#define	STATS_INC_ERR(x)	do { } while (0)
#define	STATS_INC_NODEALLOCS(x)	do { } while (0)
#define	STATS_INC_NODEFREES(x)	do { } while (0)
#define	STATS_SET_FREEABLE(x, i) \
				do { } while (0)

#define STATS_INC_ALLOCHIT(x)	do { } while (0)
#define STATS_INC_ALLOCMISS(x)	do { } while (0)
#define STATS_INC_FREEHIT(x)	do { } while (0)
#define STATS_INC_FREEMISS(x)	do { } while (0)
#endif

#if DEBUG
/* Magic nums for obj red zoning.
 * Placed in the first word before and the first word after an obj.
 */
#define	RED_INACTIVE	0x5A2CF071UL	/* when obj is inactive */
#define	RED_ACTIVE	0x170FC2A5UL	/* when obj is active */

/* ...and for poisoning */
#define	POISON_INUSE	0x5a	/* for use-uninitialised poisoning */
#define POISON_FREE	0x6b	/* for use-after-free poisoning */
#define	POISON_END	0xa5	/* end-byte of poisoning */

/* memory layout of objects:
 * 0		: objp
 * 0 .. cachep->obj_offset - BYTES_PER_WORD - 1: padding. This ensures that
 * 		the end of an object is aligned with the end of the real
 * 		allocation. Catches writes behind the end of the allocation.
 * cachep->obj_offset - BYTES_PER_WORD .. cachep->obj_offset - 1:
 * 		redzone word.
 * cachep->obj_offset: The real object.
 * cachep->buffer_size - 2* BYTES_PER_WORD: redzone word [BYTES_PER_WORD long]
 * cachep->buffer_size - 1* BYTES_PER_WORD: last caller address [BYTES_PER_WORD long]
 */
static int obj_offset(struct kmem_cache *cachep)
{
	return cachep->obj_offset;
}

static int obj_size(struct kmem_cache *cachep)
{
	return cachep->obj_size;
}

static unsigned long *dbg_redzone1(struct kmem_cache *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_RED_ZONE));
	return (unsigned long*) (objp+obj_offset(cachep)-BYTES_PER_WORD);
}

static unsigned long *dbg_redzone2(struct kmem_cache *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_RED_ZONE));
	if (cachep->flags & SLAB_STORE_USER)
		return (unsigned long *)(objp + cachep->buffer_size -
					 2 * BYTES_PER_WORD);
	return (unsigned long *)(objp + cachep->buffer_size - BYTES_PER_WORD);
}

static void **dbg_userword(struct kmem_cache *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_STORE_USER));
	return (void **)(objp + cachep->buffer_size - BYTES_PER_WORD);
}

#else

#define obj_offset(x)			0
#define obj_size(cachep)		(cachep->buffer_size)
#define dbg_redzone1(cachep, objp)	({BUG(); (unsigned long *)NULL;})
#define dbg_redzone2(cachep, objp)	({BUG(); (unsigned long *)NULL;})
#define dbg_userword(cachep, objp)	({BUG(); (void **)NULL;})

#endif

/*
 * Maximum size of an obj (in 2^order pages)
 * and absolute limit for the gfp order.
 */
#if defined(CONFIG_LARGE_ALLOCS)
#define	MAX_OBJ_ORDER	13	/* up to 32Mb */
#define	MAX_GFP_ORDER	13	/* up to 32Mb */
#elif defined(CONFIG_MMU)
#define	MAX_OBJ_ORDER	5	/* 32 pages */
#define	MAX_GFP_ORDER	5	/* 32 pages */
#else
#define	MAX_OBJ_ORDER	8	/* up to 1Mb */
#define	MAX_GFP_ORDER	8	/* up to 1Mb */
#endif

/*
 * Do not go above this order unless 0 objects fit into the slab.
 */
#define	BREAK_GFP_ORDER_HI	1
#define	BREAK_GFP_ORDER_LO	0
static int slab_break_gfp_order = BREAK_GFP_ORDER_LO;

/* Functions for storing/retrieving the cachep and or slab from the
 * global 'mem_map'. These are used to find the slab an obj belongs to.
 * With kfree(), these are used to find the cache which an obj belongs to.
 */
static inline void page_set_cache(struct page *page, struct kmem_cache *cache)
{
	page->lru.next = (struct list_head *)cache;
}

static inline struct kmem_cache *page_get_cache(struct page *page)
{
	return (struct kmem_cache *)page->lru.next;
}

static inline void page_set_slab(struct page *page, struct slab *slab)
{
	page->lru.prev = (struct list_head *)slab;
}

static inline struct slab *page_get_slab(struct page *page)
{
	return (struct slab *)page->lru.prev;
}

static inline struct kmem_cache *virt_to_cache(const void *obj)
{
	struct page *page = virt_to_page(obj);
	return page_get_cache(page);
}

static inline struct slab *virt_to_slab(const void *obj)
{
	struct page *page = virt_to_page(obj);
	return page_get_slab(page);
}

/* These are the default caches for kmalloc. Custom caches can have other sizes. */
struct cache_sizes malloc_sizes[] = {
#define CACHE(x) { .cs_size = (x) },
#include <linux/kmalloc_sizes.h>
	CACHE(ULONG_MAX)
#undef CACHE
};
EXPORT_SYMBOL(malloc_sizes);

/* Must match cache_sizes above. Out of line to keep cache footprint low. */
struct cache_names {
	char *name;
	char *name_dma;
};

static struct cache_names __initdata cache_names[] = {
#define CACHE(x) { .name = "size-" #x, .name_dma = "size-" #x "(DMA)" },
#include <linux/kmalloc_sizes.h>
	{NULL,}
#undef CACHE
};

static struct arraycache_init initarray_cache __initdata =
    { {0, BOOT_CPUCACHE_ENTRIES, 1, 0} };
static struct arraycache_init initarray_generic =
    { {0, BOOT_CPUCACHE_ENTRIES, 1, 0} };

/* internal cache of cache description objs */
static struct kmem_cache cache_cache = {
	.batchcount = 1,
	.limit = BOOT_CPUCACHE_ENTRIES,
	.shared = 1,
	.buffer_size = sizeof(struct kmem_cache),
	.flags = SLAB_NO_REAP,
	.spinlock = SPIN_LOCK_UNLOCKED,
	.name = "kmem_cache",
#if DEBUG
	.obj_size = sizeof(struct kmem_cache),
#endif
};

/* Guard access to the cache-chain. */
static DEFINE_MUTEX(cache_chain_mutex);
static struct list_head cache_chain;

/*
 * vm_enough_memory() looks at this to determine how many
 * slab-allocated pages are possibly freeable under pressure
 *
 * SLAB_RECLAIM_ACCOUNT turns this on per-slab
 */
atomic_t slab_reclaim_pages;

/*
 * chicken and egg problem: delay the per-cpu array allocation
 * until the general caches are up.
 */
static enum {
	NONE,
	PARTIAL_AC,
	PARTIAL_L3,
	FULL
} g_cpucache_up;

static DEFINE_PER_CPU(struct work_struct, reap_work);

static void free_block(struct kmem_cache *cachep, void **objpp, int len, int node);
static void enable_cpucache(struct kmem_cache *cachep);
static void cache_reap(void *unused);
static int __node_shrink(struct kmem_cache *cachep, int node);

static inline struct array_cache *cpu_cache_get(struct kmem_cache *cachep)
{
	return cachep->array[smp_processor_id()];
}

static inline struct kmem_cache *__find_general_cachep(size_t size, gfp_t gfpflags)
{
	struct cache_sizes *csizep = malloc_sizes;

#if DEBUG
	/* This happens if someone tries to call
	 * kmem_cache_create(), or __kmalloc(), before
	 * the generic caches are initialized.
	 */
	BUG_ON(malloc_sizes[INDEX_AC].cs_cachep == NULL);
#endif
	while (size > csizep->cs_size)
		csizep++;

	/*
	 * Really subtle: The last entry with cs->cs_size==ULONG_MAX
	 * has cs_{dma,}cachep==NULL. Thus no special case
	 * for large kmalloc calls required.
	 */
	if (unlikely(gfpflags & GFP_DMA))
		return csizep->cs_dmacachep;
	return csizep->cs_cachep;
}

struct kmem_cache *kmem_find_general_cachep(size_t size, gfp_t gfpflags)
{
	return __find_general_cachep(size, gfpflags);
}
EXPORT_SYMBOL(kmem_find_general_cachep);

static size_t slab_mgmt_size(size_t nr_objs, size_t align)
{
	return ALIGN(sizeof(struct slab)+nr_objs*sizeof(kmem_bufctl_t), align);
}

/* Calculate the number of objects and left-over bytes for a given
   buffer size. */
static void cache_estimate(unsigned long gfporder, size_t buffer_size,
			   size_t align, int flags, size_t *left_over,
			   unsigned int *num)
{
	int nr_objs;
	size_t mgmt_size;
	size_t slab_size = PAGE_SIZE << gfporder;

	/*
	 * The slab management structure can be either off the slab or
	 * on it. For the latter case, the memory allocated for a
	 * slab is used for:
	 *
	 * - The struct slab
	 * - One kmem_bufctl_t for each object
	 * - Padding to respect alignment of @align
	 * - @buffer_size bytes for each object
	 *
	 * If the slab management structure is off the slab, then the
	 * alignment will already be calculated into the size. Because
	 * the slabs are all pages aligned, the objects will be at the
	 * correct alignment when allocated.
	 */
	if (flags & CFLGS_OFF_SLAB) {
		mgmt_size = 0;
		nr_objs = slab_size / buffer_size;

		if (nr_objs > SLAB_LIMIT)
			nr_objs = SLAB_LIMIT;
	} else {
		/*
		 * Ignore padding for the initial guess. The padding
		 * is at most @align-1 bytes, and @buffer_size is at
		 * least @align. In the worst case, this result will
		 * be one greater than the number of objects that fit
		 * into the memory allocation when taking the padding
		 * into account.
		 */
		nr_objs = (slab_size - sizeof(struct slab)) /
			  (buffer_size + sizeof(kmem_bufctl_t));

		/*
		 * This calculated number will be either the right
		 * amount, or one greater than what we want.
		 */
		if (slab_mgmt_size(nr_objs, align) + nr_objs*buffer_size
		       > slab_size)
			nr_objs--;

		if (nr_objs > SLAB_LIMIT)
			nr_objs = SLAB_LIMIT;

		mgmt_size = slab_mgmt_size(nr_objs, align);
	}
	*num = nr_objs;
	*left_over = slab_size - nr_objs*buffer_size - mgmt_size;
}

#define slab_error(cachep, msg) __slab_error(__FUNCTION__, cachep, msg)

static void __slab_error(const char *function, struct kmem_cache *cachep, char *msg)
{
	printk(KERN_ERR "slab error in %s(): cache `%s': %s\n",
	       function, cachep->name, msg);
	dump_stack();
}

#ifdef CONFIG_NUMA
/*
 * Special reaping functions for NUMA systems called from cache_reap().
 * These take care of doing round robin flushing of alien caches (containing
 * objects freed on different nodes from which they were allocated) and the
 * flushing of remote pcps by calling drain_node_pages.
 */
static DEFINE_PER_CPU(unsigned long, reap_node);

static void init_reap_node(int cpu)
{
	int node;

	node = next_node(cpu_to_node(cpu), node_online_map);
	if (node == MAX_NUMNODES)
		node = 0;

	per_cpu(reap_node, cpu) = node;
}

static void next_reap_node(void)
{
	int node = __get_cpu_var(reap_node);

	/*
	 * Also drain per cpu pages on remote zones
	 */
	if (node != numa_node_id())
		drain_node_pages(node);

	node = next_node(node, node_online_map);
	if (unlikely(node >= MAX_NUMNODES))
		node = first_node(node_online_map);
	__get_cpu_var(reap_node) = node;
}

#else
#define init_reap_node(cpu) do { } while (0)
#define next_reap_node(void) do { } while (0)
#endif

/*
 * Initiate the reap timer running on the target CPU.  We run at around 1 to 2Hz
 * via the workqueue/eventd.
 * Add the CPU number into the expiration time to minimize the possibility of
 * the CPUs getting into lockstep and contending for the global cache chain
 * lock.
 */
static void __devinit start_cpu_timer(int cpu)
{
	struct work_struct *reap_work = &per_cpu(reap_work, cpu);

	/*
	 * When this gets called from do_initcalls via cpucache_init(),
	 * init_workqueues() has already run, so keventd will be setup
	 * at that time.
	 */
	if (keventd_up() && reap_work->func == NULL) {
		init_reap_node(cpu);
		INIT_WORK(reap_work, cache_reap, NULL);
		schedule_delayed_work_on(cpu, reap_work, HZ + 3 * cpu);
	}
}

static struct array_cache *alloc_arraycache(int node, int entries,
					    int batchcount)
{
	int memsize = sizeof(void *) * entries + sizeof(struct array_cache);
	struct array_cache *nc = NULL;

	nc = kmalloc_node(memsize, GFP_KERNEL, node);
	if (nc) {
		nc->avail = 0;
		nc->limit = entries;
		nc->batchcount = batchcount;
		nc->touched = 0;
		spin_lock_init(&nc->lock);
	}
	return nc;
}

#ifdef CONFIG_NUMA
static void *__cache_alloc_node(struct kmem_cache *, gfp_t, int);

static struct array_cache **alloc_alien_cache(int node, int limit)
{
	struct array_cache **ac_ptr;
	int memsize = sizeof(void *) * MAX_NUMNODES;
	int i;

	if (limit > 1)
		limit = 12;
	ac_ptr = kmalloc_node(memsize, GFP_KERNEL, node);
	if (ac_ptr) {
		for_each_node(i) {
			if (i == node || !node_online(i)) {
				ac_ptr[i] = NULL;
				continue;
			}
			ac_ptr[i] = alloc_arraycache(node, limit, 0xbaadf00d);
			if (!ac_ptr[i]) {
				for (i--; i <= 0; i--)
					kfree(ac_ptr[i]);
				kfree(ac_ptr);
				return NULL;
			}
		}
	}
	return ac_ptr;
}

static void free_alien_cache(struct array_cache **ac_ptr)
{
	int i;

	if (!ac_ptr)
		return;

	for_each_node(i)
	    kfree(ac_ptr[i]);

	kfree(ac_ptr);
}

static void __drain_alien_cache(struct kmem_cache *cachep,
				struct array_cache *ac, int node)
{
	struct kmem_list3 *rl3 = cachep->nodelists[node];

	if (ac->avail) {
		spin_lock(&rl3->list_lock);
		free_block(cachep, ac->entry, ac->avail, node);
		ac->avail = 0;
		spin_unlock(&rl3->list_lock);
	}
}

/*
 * Called from cache_reap() to regularly drain alien caches round robin.
 */
static void reap_alien(struct kmem_cache *cachep, struct kmem_list3 *l3)
{
	int node = __get_cpu_var(reap_node);

	if (l3->alien) {
		struct array_cache *ac = l3->alien[node];
		if (ac && ac->avail) {
			spin_lock_irq(&ac->lock);
			__drain_alien_cache(cachep, ac, node);
			spin_unlock_irq(&ac->lock);
		}
	}
}

static void drain_alien_cache(struct kmem_cache *cachep, struct array_cache **alien)
{
	int i = 0;
	struct array_cache *ac;
	unsigned long flags;

	for_each_online_node(i) {
		ac = alien[i];
		if (ac) {
			spin_lock_irqsave(&ac->lock, flags);
			__drain_alien_cache(cachep, ac, i);
			spin_unlock_irqrestore(&ac->lock, flags);
		}
	}
}
#else

#define drain_alien_cache(cachep, alien) do { } while (0)
#define reap_alien(cachep, l3) do { } while (0)

static inline struct array_cache **alloc_alien_cache(int node, int limit)
{
	return (struct array_cache **) 0x01020304ul;
}

static inline void free_alien_cache(struct array_cache **ac_ptr)
{
}

#endif

static int __devinit cpuup_callback(struct notifier_block *nfb,
				    unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	struct kmem_cache *cachep;
	struct kmem_list3 *l3 = NULL;
	int node = cpu_to_node(cpu);
	int memsize = sizeof(struct kmem_list3);

	switch (action) {
	case CPU_UP_PREPARE:
		mutex_lock(&cache_chain_mutex);
		/* we need to do this right in the beginning since
		 * alloc_arraycache's are going to use this list.
		 * kmalloc_node allows us to add the slab to the right
		 * kmem_list3 and not this cpu's kmem_list3
		 */

		list_for_each_entry(cachep, &cache_chain, next) {
			/* setup the size64 kmemlist for cpu before we can
			 * begin anything. Make sure some other cpu on this
			 * node has not already allocated this
			 */
			if (!cachep->nodelists[node]) {
				if (!(l3 = kmalloc_node(memsize,
							GFP_KERNEL, node)))
					goto bad;
				kmem_list3_init(l3);
				l3->next_reap = jiffies + REAPTIMEOUT_LIST3 +
				    ((unsigned long)cachep) % REAPTIMEOUT_LIST3;

				/*
				 * The l3s don't come and go as CPUs come and
				 * go.  cache_chain_mutex is sufficient
				 * protection here.
				 */
				cachep->nodelists[node] = l3;
			}

			spin_lock_irq(&cachep->nodelists[node]->list_lock);
			cachep->nodelists[node]->free_limit =
			    (1 + nr_cpus_node(node)) *
			    cachep->batchcount + cachep->num;
			spin_unlock_irq(&cachep->nodelists[node]->list_lock);
		}

		/* Now we can go ahead with allocating the shared array's
		   & array cache's */
		list_for_each_entry(cachep, &cache_chain, next) {
			struct array_cache *nc;
			struct array_cache *shared;
			struct array_cache **alien;

			nc = alloc_arraycache(node, cachep->limit,
						cachep->batchcount);
			if (!nc)
				goto bad;
			shared = alloc_arraycache(node,
					cachep->shared * cachep->batchcount,
					0xbaadf00d);
			if (!shared)
				goto bad;

			alien = alloc_alien_cache(node, cachep->limit);
			if (!alien)
				goto bad;
			cachep->array[cpu] = nc;

			l3 = cachep->nodelists[node];
			BUG_ON(!l3);

			spin_lock_irq(&l3->list_lock);
			if (!l3->shared) {
				/*
				 * We are serialised from CPU_DEAD or
				 * CPU_UP_CANCELLED by the cpucontrol lock
				 */
				l3->shared = shared;
				shared = NULL;
			}
#ifdef CONFIG_NUMA
			if (!l3->alien) {
				l3->alien = alien;
				alien = NULL;
			}
#endif
			spin_unlock_irq(&l3->list_lock);

			kfree(shared);
			free_alien_cache(alien);
		}
		mutex_unlock(&cache_chain_mutex);
		break;
	case CPU_ONLINE:
		start_cpu_timer(cpu);
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DEAD:
		/*
		 * Even if all the cpus of a node are down, we don't free the
		 * kmem_list3 of any cache. This to avoid a race between
		 * cpu_down, and a kmalloc allocation from another cpu for
		 * memory from the node of the cpu going down.  The list3
		 * structure is usually allocated from kmem_cache_create() and
		 * gets destroyed at kmem_cache_destroy().
		 */
		/* fall thru */
	case CPU_UP_CANCELED:
		mutex_lock(&cache_chain_mutex);

		list_for_each_entry(cachep, &cache_chain, next) {
			struct array_cache *nc;
			struct array_cache *shared;
			struct array_cache **alien;
			cpumask_t mask;

			mask = node_to_cpumask(node);
			/* cpu is dead; no one can alloc from it. */
			nc = cachep->array[cpu];
			cachep->array[cpu] = NULL;
			l3 = cachep->nodelists[node];

			if (!l3)
				goto free_array_cache;

			spin_lock_irq(&l3->list_lock);

			/* Free limit for this kmem_list3 */
			l3->free_limit -= cachep->batchcount;
			if (nc)
				free_block(cachep, nc->entry, nc->avail, node);

			if (!cpus_empty(mask)) {
				spin_unlock_irq(&l3->list_lock);
				goto free_array_cache;
			}

			shared = l3->shared;
			if (shared) {
				free_block(cachep, l3->shared->entry,
					   l3->shared->avail, node);
				l3->shared = NULL;
			}

			alien = l3->alien;
			l3->alien = NULL;

			spin_unlock_irq(&l3->list_lock);

			kfree(shared);
			if (alien) {
				drain_alien_cache(cachep, alien);
				free_alien_cache(alien);
			}
free_array_cache:
			kfree(nc);
		}
		/*
		 * In the previous loop, all the objects were freed to
		 * the respective cache's slabs,  now we can go ahead and
		 * shrink each nodelist to its limit.
		 */
		list_for_each_entry(cachep, &cache_chain, next) {
			l3 = cachep->nodelists[node];
			if (!l3)
				continue;
			spin_lock_irq(&l3->list_lock);
			/* free slabs belonging to this node */
			__node_shrink(cachep, node);
			spin_unlock_irq(&l3->list_lock);
		}
		mutex_unlock(&cache_chain_mutex);
		break;
#endif
	}
	return NOTIFY_OK;
      bad:
	mutex_unlock(&cache_chain_mutex);
	return NOTIFY_BAD;
}

static struct notifier_block cpucache_notifier = { &cpuup_callback, NULL, 0 };

/*
 * swap the static kmem_list3 with kmalloced memory
 */
static void init_list(struct kmem_cache *cachep, struct kmem_list3 *list, int nodeid)
{
	struct kmem_list3 *ptr;

	BUG_ON(cachep->nodelists[nodeid] != list);
	ptr = kmalloc_node(sizeof(struct kmem_list3), GFP_KERNEL, nodeid);
	BUG_ON(!ptr);

	local_irq_disable();
	memcpy(ptr, list, sizeof(struct kmem_list3));
	MAKE_ALL_LISTS(cachep, ptr, nodeid);
	cachep->nodelists[nodeid] = ptr;
	local_irq_enable();
}

/* Initialisation.
 * Called after the gfp() functions have been enabled, and before smp_init().
 */
void __init kmem_cache_init(void)
{
	size_t left_over;
	struct cache_sizes *sizes;
	struct cache_names *names;
	int i;
	int order;

	for (i = 0; i < NUM_INIT_LISTS; i++) {
		kmem_list3_init(&initkmem_list3[i]);
		if (i < MAX_NUMNODES)
			cache_cache.nodelists[i] = NULL;
	}

	/*
	 * Fragmentation resistance on low memory - only use bigger
	 * page orders on machines with more than 32MB of memory.
	 */
	if (num_physpages > (32 << 20) >> PAGE_SHIFT)
		slab_break_gfp_order = BREAK_GFP_ORDER_HI;

	/* Bootstrap is tricky, because several objects are allocated
	 * from caches that do not exist yet:
	 * 1) initialize the cache_cache cache: it contains the struct kmem_cache
	 *    structures of all caches, except cache_cache itself: cache_cache
	 *    is statically allocated.
	 *    Initially an __init data area is used for the head array and the
	 *    kmem_list3 structures, it's replaced with a kmalloc allocated
	 *    array at the end of the bootstrap.
	 * 2) Create the first kmalloc cache.
	 *    The struct kmem_cache for the new cache is allocated normally.
	 *    An __init data area is used for the head array.
	 * 3) Create the remaining kmalloc caches, with minimally sized
	 *    head arrays.
	 * 4) Replace the __init data head arrays for cache_cache and the first
	 *    kmalloc cache with kmalloc allocated arrays.
	 * 5) Replace the __init data for kmem_list3 for cache_cache and
	 *    the other cache's with kmalloc allocated memory.
	 * 6) Resize the head arrays of the kmalloc caches to their final sizes.
	 */

	/* 1) create the cache_cache */
	INIT_LIST_HEAD(&cache_chain);
	list_add(&cache_cache.next, &cache_chain);
	cache_cache.colour_off = cache_line_size();
	cache_cache.array[smp_processor_id()] = &initarray_cache.cache;
	cache_cache.nodelists[numa_node_id()] = &initkmem_list3[CACHE_CACHE];

	cache_cache.buffer_size = ALIGN(cache_cache.buffer_size, cache_line_size());

	for (order = 0; order < MAX_ORDER; order++) {
		cache_estimate(order, cache_cache.buffer_size,
			cache_line_size(), 0, &left_over, &cache_cache.num);
		if (cache_cache.num)
			break;
	}
	if (!cache_cache.num)
		BUG();
	cache_cache.gfporder = order;
	cache_cache.colour = left_over / cache_cache.colour_off;
	cache_cache.slab_size = ALIGN(cache_cache.num * sizeof(kmem_bufctl_t) +
				      sizeof(struct slab), cache_line_size());

	/* 2+3) create the kmalloc caches */
	sizes = malloc_sizes;
	names = cache_names;

	/* Initialize the caches that provide memory for the array cache
	 * and the kmem_list3 structures first.
	 * Without this, further allocations will bug
	 */

	sizes[INDEX_AC].cs_cachep = kmem_cache_create(names[INDEX_AC].name,
						      sizes[INDEX_AC].cs_size,
						      ARCH_KMALLOC_MINALIGN,
						      (ARCH_KMALLOC_FLAGS |
						       SLAB_PANIC), NULL, NULL);

	if (INDEX_AC != INDEX_L3)
		sizes[INDEX_L3].cs_cachep =
		    kmem_cache_create(names[INDEX_L3].name,
				      sizes[INDEX_L3].cs_size,
				      ARCH_KMALLOC_MINALIGN,
				      (ARCH_KMALLOC_FLAGS | SLAB_PANIC), NULL,
				      NULL);

	while (sizes->cs_size != ULONG_MAX) {
		/*
		 * For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter packing of the smaller caches.
		 */
		if (!sizes->cs_cachep)
			sizes->cs_cachep = kmem_cache_create(names->name,
							     sizes->cs_size,
							     ARCH_KMALLOC_MINALIGN,
							     (ARCH_KMALLOC_FLAGS
							      | SLAB_PANIC),
							     NULL, NULL);

		/* Inc off-slab bufctl limit until the ceiling is hit. */
		if (!(OFF_SLAB(sizes->cs_cachep))) {
			offslab_limit = sizes->cs_size - sizeof(struct slab);
			offslab_limit /= sizeof(kmem_bufctl_t);
		}

		sizes->cs_dmacachep = kmem_cache_create(names->name_dma,
							sizes->cs_size,
							ARCH_KMALLOC_MINALIGN,
							(ARCH_KMALLOC_FLAGS |
							 SLAB_CACHE_DMA |
							 SLAB_PANIC), NULL,
							NULL);

		sizes++;
		names++;
	}
	/* 4) Replace the bootstrap head arrays */
	{
		void *ptr;

		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);

		local_irq_disable();
		BUG_ON(cpu_cache_get(&cache_cache) != &initarray_cache.cache);
		memcpy(ptr, cpu_cache_get(&cache_cache),
		       sizeof(struct arraycache_init));
		cache_cache.array[smp_processor_id()] = ptr;
		local_irq_enable();

		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);

		local_irq_disable();
		BUG_ON(cpu_cache_get(malloc_sizes[INDEX_AC].cs_cachep)
		       != &initarray_generic.cache);
		memcpy(ptr, cpu_cache_get(malloc_sizes[INDEX_AC].cs_cachep),
		       sizeof(struct arraycache_init));
		malloc_sizes[INDEX_AC].cs_cachep->array[smp_processor_id()] =
		    ptr;
		local_irq_enable();
	}
	/* 5) Replace the bootstrap kmem_list3's */
	{
		int node;
		/* Replace the static kmem_list3 structures for the boot cpu */
		init_list(&cache_cache, &initkmem_list3[CACHE_CACHE],
			  numa_node_id());

		for_each_online_node(node) {
			init_list(malloc_sizes[INDEX_AC].cs_cachep,
				  &initkmem_list3[SIZE_AC + node], node);

			if (INDEX_AC != INDEX_L3) {
				init_list(malloc_sizes[INDEX_L3].cs_cachep,
					  &initkmem_list3[SIZE_L3 + node],
					  node);
			}
		}
	}

	/* 6) resize the head arrays to their final sizes */
	{
		struct kmem_cache *cachep;
		mutex_lock(&cache_chain_mutex);
		list_for_each_entry(cachep, &cache_chain, next)
		    enable_cpucache(cachep);
		mutex_unlock(&cache_chain_mutex);
	}

	/* Done! */
	g_cpucache_up = FULL;

	/* Register a cpu startup notifier callback
	 * that initializes cpu_cache_get for all new cpus
	 */
	register_cpu_notifier(&cpucache_notifier);

	/* The reap timers are started later, with a module init call:
	 * That part of the kernel is not yet operational.
	 */
}

static int __init cpucache_init(void)
{
	int cpu;

	/* 
	 * Register the timers that return unneeded
	 * pages to gfp.
	 */
	for_each_online_cpu(cpu)
	    start_cpu_timer(cpu);

	return 0;
}

__initcall(cpucache_init);

/*
 * Interface to system's page allocator. No need to hold the cache-lock.
 *
 * If we requested dmaable memory, we will get it. Even if we
 * did not request dmaable memory, we might get it, but that
 * would be relatively rare and ignorable.
 */
static void *kmem_getpages(struct kmem_cache *cachep, gfp_t flags, int nodeid)
{
	struct page *page;
	void *addr;
	int i;

	flags |= cachep->gfpflags;
	page = alloc_pages_node(nodeid, flags, cachep->gfporder);
	if (!page)
		return NULL;
	addr = page_address(page);

	i = (1 << cachep->gfporder);
	if (cachep->flags & SLAB_RECLAIM_ACCOUNT)
		atomic_add(i, &slab_reclaim_pages);
	add_page_state(nr_slab, i);
	while (i--) {
		SetPageSlab(page);
		page++;
	}
	return addr;
}

/*
 * Interface to system's page release.
 */
static void kmem_freepages(struct kmem_cache *cachep, void *addr)
{
	unsigned long i = (1 << cachep->gfporder);
	struct page *page = virt_to_page(addr);
	const unsigned long nr_freed = i;

	while (i--) {
		if (!TestClearPageSlab(page))
			BUG();
		page++;
	}
	sub_page_state(nr_slab, nr_freed);
	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += nr_freed;
	free_pages((unsigned long)addr, cachep->gfporder);
	if (cachep->flags & SLAB_RECLAIM_ACCOUNT)
		atomic_sub(1 << cachep->gfporder, &slab_reclaim_pages);
}

static void kmem_rcu_free(struct rcu_head *head)
{
	struct slab_rcu *slab_rcu = (struct slab_rcu *)head;
	struct kmem_cache *cachep = slab_rcu->cachep;

	kmem_freepages(cachep, slab_rcu->addr);
	if (OFF_SLAB(cachep))
		kmem_cache_free(cachep->slabp_cache, slab_rcu);
}

#if DEBUG

#ifdef CONFIG_DEBUG_PAGEALLOC
static void store_stackinfo(struct kmem_cache *cachep, unsigned long *addr,
			    unsigned long caller)
{
	int size = obj_size(cachep);

	addr = (unsigned long *)&((char *)addr)[obj_offset(cachep)];

	if (size < 5 * sizeof(unsigned long))
		return;

	*addr++ = 0x12345678;
	*addr++ = caller;
	*addr++ = smp_processor_id();
	size -= 3 * sizeof(unsigned long);
	{
		unsigned long *sptr = &caller;
		unsigned long svalue;

		while (!kstack_end(sptr)) {
			svalue = *sptr++;
			if (kernel_text_address(svalue)) {
				*addr++ = svalue;
				size -= sizeof(unsigned long);
				if (size <= sizeof(unsigned long))
					break;
			}
		}

	}
	*addr++ = 0x87654321;
}
#endif

static void poison_obj(struct kmem_cache *cachep, void *addr, unsigned char val)
{
	int size = obj_size(cachep);
	addr = &((char *)addr)[obj_offset(cachep)];

	memset(addr, val, size);
	*(unsigned char *)(addr + size - 1) = POISON_END;
}

static void dump_line(char *data, int offset, int limit)
{
	int i;
	printk(KERN_ERR "%03x:", offset);
	for (i = 0; i < limit; i++) {
		printk(" %02x", (unsigned char)data[offset + i]);
	}
	printk("\n");
}
#endif

#if DEBUG

static void print_objinfo(struct kmem_cache *cachep, void *objp, int lines)
{
	int i, size;
	char *realobj;

	if (cachep->flags & SLAB_RED_ZONE) {
		printk(KERN_ERR "Redzone: 0x%lx/0x%lx.\n",
		       *dbg_redzone1(cachep, objp),
		       *dbg_redzone2(cachep, objp));
	}

	if (cachep->flags & SLAB_STORE_USER) {
		printk(KERN_ERR "Last user: [<%p>]",
		       *dbg_userword(cachep, objp));
		print_symbol("(%s)",
			     (unsigned long)*dbg_userword(cachep, objp));
		printk("\n");
	}
	realobj = (char *)objp + obj_offset(cachep);
	size = obj_size(cachep);
	for (i = 0; i < size && lines; i += 16, lines--) {
		int limit;
		limit = 16;
		if (i + limit > size)
			limit = size - i;
		dump_line(realobj, i, limit);
	}
}

static void check_poison_obj(struct kmem_cache *cachep, void *objp)
{
	char *realobj;
	int size, i;
	int lines = 0;

	realobj = (char *)objp + obj_offset(cachep);
	size = obj_size(cachep);

	for (i = 0; i < size; i++) {
		char exp = POISON_FREE;
		if (i == size - 1)
			exp = POISON_END;
		if (realobj[i] != exp) {
			int limit;
			/* Mismatch ! */
			/* Print header */
			if (lines == 0) {
				printk(KERN_ERR
				       "Slab corruption: start=%p, len=%d\n",
				       realobj, size);
				print_objinfo(cachep, objp, 0);
			}
			/* Hexdump the affected line */
			i = (i / 16) * 16;
			limit = 16;
			if (i + limit > size)
				limit = size - i;
			dump_line(realobj, i, limit);
			i += 16;
			lines++;
			/* Limit to 5 lines */
			if (lines > 5)
				break;
		}
	}
	if (lines != 0) {
		/* Print some data about the neighboring objects, if they
		 * exist:
		 */
		struct slab *slabp = virt_to_slab(objp);
		int objnr;

		objnr = (unsigned)(objp - slabp->s_mem) / cachep->buffer_size;
		if (objnr) {
			objp = slabp->s_mem + (objnr - 1) * cachep->buffer_size;
			realobj = (char *)objp + obj_offset(cachep);
			printk(KERN_ERR "Prev obj: start=%p, len=%d\n",
			       realobj, size);
			print_objinfo(cachep, objp, 2);
		}
		if (objnr + 1 < cachep->num) {
			objp = slabp->s_mem + (objnr + 1) * cachep->buffer_size;
			realobj = (char *)objp + obj_offset(cachep);
			printk(KERN_ERR "Next obj: start=%p, len=%d\n",
			       realobj, size);
			print_objinfo(cachep, objp, 2);
		}
	}
}
#endif

#if DEBUG
/**
 * slab_destroy_objs - call the registered destructor for each object in
 *      a slab that is to be destroyed.
 */
static void slab_destroy_objs(struct kmem_cache *cachep, struct slab *slabp)
{
	int i;
	for (i = 0; i < cachep->num; i++) {
		void *objp = slabp->s_mem + cachep->buffer_size * i;

		if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
			if ((cachep->buffer_size % PAGE_SIZE) == 0
			    && OFF_SLAB(cachep))
				kernel_map_pages(virt_to_page(objp),
						 cachep->buffer_size / PAGE_SIZE,
						 1);
			else
				check_poison_obj(cachep, objp);
#else
			check_poison_obj(cachep, objp);
#endif
		}
		if (cachep->flags & SLAB_RED_ZONE) {
			if (*dbg_redzone1(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "start of a freed object "
					   "was overwritten");
			if (*dbg_redzone2(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "end of a freed object "
					   "was overwritten");
		}
		if (cachep->dtor && !(cachep->flags & SLAB_POISON))
			(cachep->dtor) (objp + obj_offset(cachep), cachep, 0);
	}
}
#else
static void slab_destroy_objs(struct kmem_cache *cachep, struct slab *slabp)
{
	if (cachep->dtor) {
		int i;
		for (i = 0; i < cachep->num; i++) {
			void *objp = slabp->s_mem + cachep->buffer_size * i;
			(cachep->dtor) (objp, cachep, 0);
		}
	}
}
#endif

/**
 * Destroy all the objs in a slab, and release the mem back to the system.
 * Before calling the slab must have been unlinked from the cache.
 * The cache-lock is not held/needed.
 */
static void slab_destroy(struct kmem_cache *cachep, struct slab *slabp)
{
	void *addr = slabp->s_mem - slabp->colouroff;

	slab_destroy_objs(cachep, slabp);
	if (unlikely(cachep->flags & SLAB_DESTROY_BY_RCU)) {
		struct slab_rcu *slab_rcu;

		slab_rcu = (struct slab_rcu *)slabp;
		slab_rcu->cachep = cachep;
		slab_rcu->addr = addr;
		call_rcu(&slab_rcu->head, kmem_rcu_free);
	} else {
		kmem_freepages(cachep, addr);
		if (OFF_SLAB(cachep))
			kmem_cache_free(cachep->slabp_cache, slabp);
	}
}

/* For setting up all the kmem_list3s for cache whose buffer_size is same
   as size of kmem_list3. */
static void set_up_list3s(struct kmem_cache *cachep, int index)
{
	int node;

	for_each_online_node(node) {
		cachep->nodelists[node] = &initkmem_list3[index + node];
		cachep->nodelists[node]->next_reap = jiffies +
		    REAPTIMEOUT_LIST3 +
		    ((unsigned long)cachep) % REAPTIMEOUT_LIST3;
	}
}

/**
 * calculate_slab_order - calculate size (page order) of slabs
 * @cachep: pointer to the cache that is being created
 * @size: size of objects to be created in this cache.
 * @align: required alignment for the objects.
 * @flags: slab allocation flags
 *
 * Also calculates the number of objects per slab.
 *
 * This could be made much more intelligent.  For now, try to avoid using
 * high order pages for slabs.  When the gfp() functions are more friendly
 * towards high-order requests, this should be changed.
 */
static inline size_t calculate_slab_order(struct kmem_cache *cachep,
			size_t size, size_t align, unsigned long flags)
{
	size_t left_over = 0;
	int gfporder;

	for (gfporder = 0 ; gfporder <= MAX_GFP_ORDER; gfporder++) {
		unsigned int num;
		size_t remainder;

		cache_estimate(gfporder, size, align, flags, &remainder, &num);
		if (!num)
			continue;

		/* More than offslab_limit objects will cause problems */
		if ((flags & CFLGS_OFF_SLAB) && num > offslab_limit)
			break;

		/* Found something acceptable - save it away */
		cachep->num = num;
		cachep->gfporder = gfporder;
		left_over = remainder;

		/*
		 * A VFS-reclaimable slab tends to have most allocations
		 * as GFP_NOFS and we really don't want to have to be allocating
		 * higher-order pages when we are unable to shrink dcache.
		 */
		if (flags & SLAB_RECLAIM_ACCOUNT)
			break;

		/*
		 * Large number of objects is good, but very large slabs are
		 * currently bad for the gfp()s.
		 */
		if (gfporder >= slab_break_gfp_order)
			break;

		/*
		 * Acceptable internal fragmentation?
		 */
		if ((left_over * 8) <= (PAGE_SIZE << gfporder))
			break;
	}
	return left_over;
}

/**
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @align: The required alignment for the objects.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a int, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 *
 * @name must be valid until the cache is destroyed. This implies that
 * the module calling this has to destroy the cache before getting 
 * unloaded.
 * 
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_NO_REAP - Don't automatically reap this cache when we're under
 * memory pressure.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
struct kmem_cache *
kmem_cache_create (const char *name, size_t size, size_t align,
	unsigned long flags, void (*ctor)(void*, struct kmem_cache *, unsigned long),
	void (*dtor)(void*, struct kmem_cache *, unsigned long))
{
	size_t left_over, slab_size, ralign;
	struct kmem_cache *cachep = NULL;
	struct list_head *p;

	/*
	 * Sanity checks... these are all serious usage bugs.
	 */
	if ((!name) ||
	    in_interrupt() ||
	    (size < BYTES_PER_WORD) ||
	    (size > (1 << MAX_OBJ_ORDER) * PAGE_SIZE) || (dtor && !ctor)) {
		printk(KERN_ERR "%s: Early error in slab %s\n",
		       __FUNCTION__, name);
		BUG();
	}

	/*
	 * Prevent CPUs from coming and going.
	 * lock_cpu_hotplug() nests outside cache_chain_mutex
	 */
	lock_cpu_hotplug();

	mutex_lock(&cache_chain_mutex);

	list_for_each(p, &cache_chain) {
		struct kmem_cache *pc = list_entry(p, struct kmem_cache, next);
		mm_segment_t old_fs = get_fs();
		char tmp;
		int res;

		/*
		 * This happens when the module gets unloaded and doesn't
		 * destroy its slab cache and no-one else reuses the vmalloc
		 * area of the module.  Print a warning.
		 */
		set_fs(KERNEL_DS);
		res = __get_user(tmp, pc->name);
		set_fs(old_fs);
		if (res) {
			printk("SLAB: cache with size %d has lost its name\n",
			       pc->buffer_size);
			continue;
		}

		if (!strcmp(pc->name, name)) {
			printk("kmem_cache_create: duplicate cache %s\n", name);
			dump_stack();
			goto oops;
		}
	}

#if DEBUG
	WARN_ON(strchr(name, ' '));	/* It confuses parsers */
	if ((flags & SLAB_DEBUG_INITIAL) && !ctor) {
		/* No constructor, but inital state check requested */
		printk(KERN_ERR "%s: No con, but init state check "
		       "requested - %s\n", __FUNCTION__, name);
		flags &= ~SLAB_DEBUG_INITIAL;
	}
#if FORCED_DEBUG
	/*
	 * Enable redzoning and last user accounting, except for caches with
	 * large objects, if the increased size would increase the object size
	 * above the next power of two: caches with object sizes just above a
	 * power of two have a significant amount of internal fragmentation.
	 */
	if ((size < 4096
	     || fls(size - 1) == fls(size - 1 + 3 * BYTES_PER_WORD)))
		flags |= SLAB_RED_ZONE | SLAB_STORE_USER;
	if (!(flags & SLAB_DESTROY_BY_RCU))
		flags |= SLAB_POISON;
#endif
	if (flags & SLAB_DESTROY_BY_RCU)
		BUG_ON(flags & SLAB_POISON);
#endif
	if (flags & SLAB_DESTROY_BY_RCU)
		BUG_ON(dtor);

	/*
	 * Always checks flags, a caller might be expecting debug
	 * support which isn't available.
	 */
	if (flags & ~CREATE_MASK)
		BUG();

	/* Check that size is in terms of words.  This is needed to avoid
	 * unaligned accesses for some archs when redzoning is used, and makes
	 * sure any on-slab bufctl's are also correctly aligned.
	 */
	if (size & (BYTES_PER_WORD - 1)) {
		size += (BYTES_PER_WORD - 1);
		size &= ~(BYTES_PER_WORD - 1);
	}

	/* calculate out the final buffer alignment: */
	/* 1) arch recommendation: can be overridden for debug */
	if (flags & SLAB_HWCACHE_ALIGN) {
		/* Default alignment: as specified by the arch code.
		 * Except if an object is really small, then squeeze multiple
		 * objects into one cacheline.
		 */
		ralign = cache_line_size();
		while (size <= ralign / 2)
			ralign /= 2;
	} else {
		ralign = BYTES_PER_WORD;
	}
	/* 2) arch mandated alignment: disables debug if necessary */
	if (ralign < ARCH_SLAB_MINALIGN) {
		ralign = ARCH_SLAB_MINALIGN;
		if (ralign > BYTES_PER_WORD)
			flags &= ~(SLAB_RED_ZONE | SLAB_STORE_USER);
	}
	/* 3) caller mandated alignment: disables debug if necessary */
	if (ralign < align) {
		ralign = align;
		if (ralign > BYTES_PER_WORD)
			flags &= ~(SLAB_RED_ZONE | SLAB_STORE_USER);
	}
	/* 4) Store it. Note that the debug code below can reduce
	 *    the alignment to BYTES_PER_WORD.
	 */
	align = ralign;

	/* Get cache's description obj. */
	cachep = kmem_cache_alloc(&cache_cache, SLAB_KERNEL);
	if (!cachep)
		goto oops;
	memset(cachep, 0, sizeof(struct kmem_cache));

#if DEBUG
	cachep->obj_size = size;

	if (flags & SLAB_RED_ZONE) {
		/* redzoning only works with word aligned caches */
		align = BYTES_PER_WORD;

		/* add space for red zone words */
		cachep->obj_offset += BYTES_PER_WORD;
		size += 2 * BYTES_PER_WORD;
	}
	if (flags & SLAB_STORE_USER) {
		/* user store requires word alignment and
		 * one word storage behind the end of the real
		 * object.
		 */
		align = BYTES_PER_WORD;
		size += BYTES_PER_WORD;
	}
#if FORCED_DEBUG && defined(CONFIG_DEBUG_PAGEALLOC)
	if (size >= malloc_sizes[INDEX_L3 + 1].cs_size
	    && cachep->obj_size > cache_line_size() && size < PAGE_SIZE) {
		cachep->obj_offset += PAGE_SIZE - size;
		size = PAGE_SIZE;
	}
#endif
#endif

	/* Determine if the slab management is 'on' or 'off' slab. */
	if (size >= (PAGE_SIZE >> 3))
		/*
		 * Size is large, assume best to place the slab management obj
		 * off-slab (should allow better packing of objs).
		 */
		flags |= CFLGS_OFF_SLAB;

	size = ALIGN(size, align);

	left_over = calculate_slab_order(cachep, size, align, flags);

	if (!cachep->num) {
		printk("kmem_cache_create: couldn't create cache %s.\n", name);
		kmem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto oops;
	}
	slab_size = ALIGN(cachep->num * sizeof(kmem_bufctl_t)
			  + sizeof(struct slab), align);

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~CFLGS_OFF_SLAB;
		left_over -= slab_size;
	}

	if (flags & CFLGS_OFF_SLAB) {
		/* really off slab. No need for manual alignment */
		slab_size =
		    cachep->num * sizeof(kmem_bufctl_t) + sizeof(struct slab);
	}

	cachep->colour_off = cache_line_size();
	/* Offset must be a multiple of the alignment. */
	if (cachep->colour_off < align)
		cachep->colour_off = align;
	cachep->colour = left_over / cachep->colour_off;
	cachep->slab_size = slab_size;
	cachep->flags = flags;
	cachep->gfpflags = 0;
	if (flags & SLAB_CACHE_DMA)
		cachep->gfpflags |= GFP_DMA;
	spin_lock_init(&cachep->spinlock);
	cachep->buffer_size = size;

	if (flags & CFLGS_OFF_SLAB)
		cachep->slabp_cache = kmem_find_general_cachep(slab_size, 0u);
	cachep->ctor = ctor;
	cachep->dtor = dtor;
	cachep->name = name;


	if (g_cpucache_up == FULL) {
		enable_cpucache(cachep);
	} else {
		if (g_cpucache_up == NONE) {
			/* Note: the first kmem_cache_create must create
			 * the cache that's used by kmalloc(24), otherwise
			 * the creation of further caches will BUG().
			 */
			cachep->array[smp_processor_id()] =
			    &initarray_generic.cache;

			/* If the cache that's used by
			 * kmalloc(sizeof(kmem_list3)) is the first cache,
			 * then we need to set up all its list3s, otherwise
			 * the creation of further caches will BUG().
			 */
			set_up_list3s(cachep, SIZE_AC);
			if (INDEX_AC == INDEX_L3)
				g_cpucache_up = PARTIAL_L3;
			else
				g_cpucache_up = PARTIAL_AC;
		} else {
			cachep->array[smp_processor_id()] =
			    kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);

			if (g_cpucache_up == PARTIAL_AC) {
				set_up_list3s(cachep, SIZE_L3);
				g_cpucache_up = PARTIAL_L3;
			} else {
				int node;
				for_each_online_node(node) {

					cachep->nodelists[node] =
					    kmalloc_node(sizeof
							 (struct kmem_list3),
							 GFP_KERNEL, node);
					BUG_ON(!cachep->nodelists[node]);
					kmem_list3_init(cachep->
							nodelists[node]);
				}
			}
		}
		cachep->nodelists[numa_node_id()]->next_reap =
		    jiffies + REAPTIMEOUT_LIST3 +
		    ((unsigned long)cachep) % REAPTIMEOUT_LIST3;

		BUG_ON(!cpu_cache_get(cachep));
		cpu_cache_get(cachep)->avail = 0;
		cpu_cache_get(cachep)->limit = BOOT_CPUCACHE_ENTRIES;
		cpu_cache_get(cachep)->batchcount = 1;
		cpu_cache_get(cachep)->touched = 0;
		cachep->batchcount = 1;
		cachep->limit = BOOT_CPUCACHE_ENTRIES;
	}

	/* cache setup completed, link it into the list */
	list_add(&cachep->next, &cache_chain);
      oops:
	if (!cachep && (flags & SLAB_PANIC))
		panic("kmem_cache_create(): failed to create slab `%s'\n",
		      name);
	mutex_unlock(&cache_chain_mutex);
	unlock_cpu_hotplug();
	return cachep;
}
EXPORT_SYMBOL(kmem_cache_create);

#if DEBUG
static void check_irq_off(void)
{
	BUG_ON(!irqs_disabled());
}

static void check_irq_on(void)
{
	BUG_ON(irqs_disabled());
}

static void check_spinlock_acquired(struct kmem_cache *cachep)
{
#ifdef CONFIG_SMP
	check_irq_off();
	assert_spin_locked(&cachep->nodelists[numa_node_id()]->list_lock);
#endif
}

static void check_spinlock_acquired_node(struct kmem_cache *cachep, int node)
{
#ifdef CONFIG_SMP
	check_irq_off();
	assert_spin_locked(&cachep->nodelists[node]->list_lock);
#endif
}

#else
#define check_irq_off()	do { } while(0)
#define check_irq_on()	do { } while(0)
#define check_spinlock_acquired(x) do { } while(0)
#define check_spinlock_acquired_node(x, y) do { } while(0)
#endif

/*
 * Waits for all CPUs to execute func().
 */
static void smp_call_function_all_cpus(void (*func)(void *arg), void *arg)
{
	check_irq_on();
	preempt_disable();

	local_irq_disable();
	func(arg);
	local_irq_enable();

	if (smp_call_function(func, arg, 1, 1))
		BUG();

	preempt_enable();
}

static void drain_array_locked(struct kmem_cache *cachep, struct array_cache *ac,
				int force, int node);

static void do_drain(void *arg)
{
	struct kmem_cache *cachep = (struct kmem_cache *) arg;
	struct array_cache *ac;
	int node = numa_node_id();

	check_irq_off();
	ac = cpu_cache_get(cachep);
	spin_lock(&cachep->nodelists[node]->list_lock);
	free_block(cachep, ac->entry, ac->avail, node);
	spin_unlock(&cachep->nodelists[node]->list_lock);
	ac->avail = 0;
}

static void drain_cpu_caches(struct kmem_cache *cachep)
{
	struct kmem_list3 *l3;
	int node;

	smp_call_function_all_cpus(do_drain, cachep);
	check_irq_on();
	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (l3) {
			spin_lock_irq(&l3->list_lock);
			drain_array_locked(cachep, l3->shared, 1, node);
			spin_unlock_irq(&l3->list_lock);
			if (l3->alien)
				drain_alien_cache(cachep, l3->alien);
		}
	}
}

static int __node_shrink(struct kmem_cache *cachep, int node)
{
	struct slab *slabp;
	struct kmem_list3 *l3 = cachep->nodelists[node];
	int ret;

	for (;;) {
		struct list_head *p;

		p = l3->slabs_free.prev;
		if (p == &l3->slabs_free)
			break;

		slabp = list_entry(l3->slabs_free.prev, struct slab, list);
#if DEBUG
		if (slabp->inuse)
			BUG();
#endif
		list_del(&slabp->list);

		l3->free_objects -= cachep->num;
		spin_unlock_irq(&l3->list_lock);
		slab_destroy(cachep, slabp);
		spin_lock_irq(&l3->list_lock);
	}
	ret = !list_empty(&l3->slabs_full) || !list_empty(&l3->slabs_partial);
	return ret;
}

static int __cache_shrink(struct kmem_cache *cachep)
{
	int ret = 0, i = 0;
	struct kmem_list3 *l3;

	drain_cpu_caches(cachep);

	check_irq_on();
	for_each_online_node(i) {
		l3 = cachep->nodelists[i];
		if (l3) {
			spin_lock_irq(&l3->list_lock);
			ret += __node_shrink(cachep, i);
			spin_unlock_irq(&l3->list_lock);
		}
	}
	return (ret ? 1 : 0);
}

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(struct kmem_cache *cachep)
{
	if (!cachep || in_interrupt())
		BUG();

	return __cache_shrink(cachep);
}
EXPORT_SYMBOL(kmem_cache_shrink);

/**
 * kmem_cache_destroy - delete a cache
 * @cachep: the cache to destroy
 *
 * Remove a struct kmem_cache object from the slab cache.
 * Returns 0 on success.
 *
 * It is expected this function will be called by a module when it is
 * unloaded.  This will remove the cache completely, and avoid a duplicate
 * cache being allocated each time a module is loaded and unloaded, if the
 * module doesn't have persistent in-kernel storage across loads and unloads.
 *
 * The cache must be empty before calling this function.
 *
 * The caller must guarantee that noone will allocate memory from the cache
 * during the kmem_cache_destroy().
 */
int kmem_cache_destroy(struct kmem_cache *cachep)
{
	int i;
	struct kmem_list3 *l3;

	if (!cachep || in_interrupt())
		BUG();

	/* Don't let CPUs to come and go */
	lock_cpu_hotplug();

	/* Find the cache in the chain of caches. */
	mutex_lock(&cache_chain_mutex);
	/*
	 * the chain is never empty, cache_cache is never destroyed
	 */
	list_del(&cachep->next);
	mutex_unlock(&cache_chain_mutex);

	if (__cache_shrink(cachep)) {
		slab_error(cachep, "Can't free all objects");
		mutex_lock(&cache_chain_mutex);
		list_add(&cachep->next, &cache_chain);
		mutex_unlock(&cache_chain_mutex);
		unlock_cpu_hotplug();
		return 1;
	}

	if (unlikely(cachep->flags & SLAB_DESTROY_BY_RCU))
		synchronize_rcu();

	for_each_online_cpu(i)
	    kfree(cachep->array[i]);

	/* NUMA: free the list3 structures */
	for_each_online_node(i) {
		if ((l3 = cachep->nodelists[i])) {
			kfree(l3->shared);
			free_alien_cache(l3->alien);
			kfree(l3);
		}
	}
	kmem_cache_free(&cache_cache, cachep);

	unlock_cpu_hotplug();

	return 0;
}
EXPORT_SYMBOL(kmem_cache_destroy);

/* Get the memory for a slab management obj. */
static struct slab *alloc_slabmgmt(struct kmem_cache *cachep, void *objp,
				   int colour_off, gfp_t local_flags)
{
	struct slab *slabp;

	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		slabp = kmem_cache_alloc(cachep->slabp_cache, local_flags);
		if (!slabp)
			return NULL;
	} else {
		slabp = objp + colour_off;
		colour_off += cachep->slab_size;
	}
	slabp->inuse = 0;
	slabp->colouroff = colour_off;
	slabp->s_mem = objp + colour_off;

	return slabp;
}

static inline kmem_bufctl_t *slab_bufctl(struct slab *slabp)
{
	return (kmem_bufctl_t *) (slabp + 1);
}

static void cache_init_objs(struct kmem_cache *cachep,
			    struct slab *slabp, unsigned long ctor_flags)
{
	int i;

	for (i = 0; i < cachep->num; i++) {
		void *objp = slabp->s_mem + cachep->buffer_size * i;
#if DEBUG
		/* need to poison the objs? */
		if (cachep->flags & SLAB_POISON)
			poison_obj(cachep, objp, POISON_FREE);
		if (cachep->flags & SLAB_STORE_USER)
			*dbg_userword(cachep, objp) = NULL;

		if (cachep->flags & SLAB_RED_ZONE) {
			*dbg_redzone1(cachep, objp) = RED_INACTIVE;
			*dbg_redzone2(cachep, objp) = RED_INACTIVE;
		}
		/*
		 * Constructors are not allowed to allocate memory from
		 * the same cache which they are a constructor for.
		 * Otherwise, deadlock. They must also be threaded.
		 */
		if (cachep->ctor && !(cachep->flags & SLAB_POISON))
			cachep->ctor(objp + obj_offset(cachep), cachep,
				     ctor_flags);

		if (cachep->flags & SLAB_RED_ZONE) {
			if (*dbg_redzone2(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "constructor overwrote the"
					   " end of an object");
			if (*dbg_redzone1(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "constructor overwrote the"
					   " start of an object");
		}
		if ((cachep->buffer_size % PAGE_SIZE) == 0 && OFF_SLAB(cachep)
		    && cachep->flags & SLAB_POISON)
			kernel_map_pages(virt_to_page(objp),
					 cachep->buffer_size / PAGE_SIZE, 0);
#else
		if (cachep->ctor)
			cachep->ctor(objp, cachep, ctor_flags);
#endif
		slab_bufctl(slabp)[i] = i + 1;
	}
	slab_bufctl(slabp)[i - 1] = BUFCTL_END;
	slabp->free = 0;
}

static void kmem_flagcheck(struct kmem_cache *cachep, gfp_t flags)
{
	if (flags & SLAB_DMA) {
		if (!(cachep->gfpflags & GFP_DMA))
			BUG();
	} else {
		if (cachep->gfpflags & GFP_DMA)
			BUG();
	}
}

static void *slab_get_obj(struct kmem_cache *cachep, struct slab *slabp, int nodeid)
{
	void *objp = slabp->s_mem + (slabp->free * cachep->buffer_size);
	kmem_bufctl_t next;

	slabp->inuse++;
	next = slab_bufctl(slabp)[slabp->free];
#if DEBUG
	slab_bufctl(slabp)[slabp->free] = BUFCTL_FREE;
	WARN_ON(slabp->nodeid != nodeid);
#endif
	slabp->free = next;

	return objp;
}

static void slab_put_obj(struct kmem_cache *cachep, struct slab *slabp, void *objp,
			  int nodeid)
{
	unsigned int objnr = (unsigned)(objp-slabp->s_mem) / cachep->buffer_size;

#if DEBUG
	/* Verify that the slab belongs to the intended node */
	WARN_ON(slabp->nodeid != nodeid);

	if (slab_bufctl(slabp)[objnr] != BUFCTL_FREE) {
		printk(KERN_ERR "slab: double free detected in cache "
		       "'%s', objp %p\n", cachep->name, objp);
		BUG();
	}
#endif
	slab_bufctl(slabp)[objnr] = slabp->free;
	slabp->free = objnr;
	slabp->inuse--;
}

static void set_slab_attr(struct kmem_cache *cachep, struct slab *slabp, void *objp)
{
	int i;
	struct page *page;

	/* Nasty!!!!!! I hope this is OK. */
	i = 1 << cachep->gfporder;
	page = virt_to_page(objp);
	do {
		page_set_cache(page, cachep);
		page_set_slab(page, slabp);
		page++;
	} while (--i);
}

/*
 * Grow (by 1) the number of slabs within a cache.  This is called by
 * kmem_cache_alloc() when there are no active objs left in a cache.
 */
static int cache_grow(struct kmem_cache *cachep, gfp_t flags, int nodeid)
{
	struct slab *slabp;
	void *objp;
	size_t offset;
	gfp_t local_flags;
	unsigned long ctor_flags;
	struct kmem_list3 *l3;

	/* Be lazy and only check for valid flags here,
	 * keeping it out of the critical path in kmem_cache_alloc().
	 */
	if (flags & ~(SLAB_DMA | SLAB_LEVEL_MASK | SLAB_NO_GROW))
		BUG();
	if (flags & SLAB_NO_GROW)
		return 0;

	ctor_flags = SLAB_CTOR_CONSTRUCTOR;
	local_flags = (flags & SLAB_LEVEL_MASK);
	if (!(local_flags & __GFP_WAIT))
		/*
		 * Not allowed to sleep.  Need to tell a constructor about
		 * this - it might need to know...
		 */
		ctor_flags |= SLAB_CTOR_ATOMIC;

	/* Take the l3 list lock to change the colour_next on this node */
	check_irq_off();
	l3 = cachep->nodelists[nodeid];
	spin_lock(&l3->list_lock);

	/* Get colour for the slab, and cal the next value. */
	offset = l3->colour_next;
	l3->colour_next++;
	if (l3->colour_next >= cachep->colour)
		l3->colour_next = 0;
	spin_unlock(&l3->list_lock);

	offset *= cachep->colour_off;

	if (local_flags & __GFP_WAIT)
		local_irq_enable();

	/*
	 * The test for missing atomic flag is performed here, rather than
	 * the more obvious place, simply to reduce the critical path length
	 * in kmem_cache_alloc(). If a caller is seriously mis-behaving they
	 * will eventually be caught here (where it matters).
	 */
	kmem_flagcheck(cachep, flags);

	/* Get mem for the objs.
	 * Attempt to allocate a physical page from 'nodeid',
	 */
	if (!(objp = kmem_getpages(cachep, flags, nodeid)))
		goto failed;

	/* Get slab management. */
	if (!(slabp = alloc_slabmgmt(cachep, objp, offset, local_flags)))
		goto opps1;

	slabp->nodeid = nodeid;
	set_slab_attr(cachep, slabp, objp);

	cache_init_objs(cachep, slabp, ctor_flags);

	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	check_irq_off();
	spin_lock(&l3->list_lock);

	/* Make slab active. */
	list_add_tail(&slabp->list, &(l3->slabs_free));
	STATS_INC_GROWN(cachep);
	l3->free_objects += cachep->num;
	spin_unlock(&l3->list_lock);
	return 1;
      opps1:
	kmem_freepages(cachep, objp);
      failed:
	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	return 0;
}

#if DEBUG

/*
 * Perform extra freeing checks:
 * - detect bad pointers.
 * - POISON/RED_ZONE checking
 * - destructor calls, for caches with POISON+dtor
 */
static void kfree_debugcheck(const void *objp)
{
	struct page *page;

	if (!virt_addr_valid(objp)) {
		printk(KERN_ERR "kfree_debugcheck: out of range ptr %lxh.\n",
		       (unsigned long)objp);
		BUG();
	}
	page = virt_to_page(objp);
	if (!PageSlab(page)) {
		printk(KERN_ERR "kfree_debugcheck: bad ptr %lxh.\n",
		       (unsigned long)objp);
		BUG();
	}
}

static void *cache_free_debugcheck(struct kmem_cache *cachep, void *objp,
				   void *caller)
{
	struct page *page;
	unsigned int objnr;
	struct slab *slabp;

	objp -= obj_offset(cachep);
	kfree_debugcheck(objp);
	page = virt_to_page(objp);

	if (page_get_cache(page) != cachep) {
		printk(KERN_ERR
		       "mismatch in kmem_cache_free: expected cache %p, got %p\n",
		       page_get_cache(page), cachep);
		printk(KERN_ERR "%p is %s.\n", cachep, cachep->name);
		printk(KERN_ERR "%p is %s.\n", page_get_cache(page),
		       page_get_cache(page)->name);
		WARN_ON(1);
	}
	slabp = page_get_slab(page);

	if (cachep->flags & SLAB_RED_ZONE) {
		if (*dbg_redzone1(cachep, objp) != RED_ACTIVE
		    || *dbg_redzone2(cachep, objp) != RED_ACTIVE) {
			slab_error(cachep,
				   "double free, or memory outside"
				   " object was overwritten");
			printk(KERN_ERR
			       "%p: redzone 1: 0x%lx, redzone 2: 0x%lx.\n",
			       objp, *dbg_redzone1(cachep, objp),
			       *dbg_redzone2(cachep, objp));
		}
		*dbg_redzone1(cachep, objp) = RED_INACTIVE;
		*dbg_redzone2(cachep, objp) = RED_INACTIVE;
	}
	if (cachep->flags & SLAB_STORE_USER)
		*dbg_userword(cachep, objp) = caller;

	objnr = (unsigned)(objp - slabp->s_mem) / cachep->buffer_size;

	BUG_ON(objnr >= cachep->num);
	BUG_ON(objp != slabp->s_mem + objnr * cachep->buffer_size);

	if (cachep->flags & SLAB_DEBUG_INITIAL) {
		/* Need to call the slab's constructor so the
		 * caller can perform a verify of its state (debugging).
		 * Called without the cache-lock held.
		 */
		cachep->ctor(objp + obj_offset(cachep),
			     cachep, SLAB_CTOR_CONSTRUCTOR | SLAB_CTOR_VERIFY);
	}
	if (cachep->flags & SLAB_POISON && cachep->dtor) {
		/* we want to cache poison the object,
		 * call the destruction callback
		 */
		cachep->dtor(objp + obj_offset(cachep), cachep, 0);
	}
	if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
		if ((cachep->buffer_size % PAGE_SIZE) == 0 && OFF_SLAB(cachep)) {
			store_stackinfo(cachep, objp, (unsigned long)caller);
			kernel_map_pages(virt_to_page(objp),
					 cachep->buffer_size / PAGE_SIZE, 0);
		} else {
			poison_obj(cachep, objp, POISON_FREE);
		}
#else
		poison_obj(cachep, objp, POISON_FREE);
#endif
	}
	return objp;
}

static void check_slabp(struct kmem_cache *cachep, struct slab *slabp)
{
	kmem_bufctl_t i;
	int entries = 0;

	/* Check slab's freelist to see if this obj is there. */
	for (i = slabp->free; i != BUFCTL_END; i = slab_bufctl(slabp)[i]) {
		entries++;
		if (entries > cachep->num || i >= cachep->num)
			goto bad;
	}
	if (entries != cachep->num - slabp->inuse) {
	      bad:
		printk(KERN_ERR
		       "slab: Internal list corruption detected in cache '%s'(%d), slabp %p(%d). Hexdump:\n",
		       cachep->name, cachep->num, slabp, slabp->inuse);
		for (i = 0;
		     i < sizeof(*slabp) + cachep->num * sizeof(kmem_bufctl_t);
		     i++) {
			if ((i % 16) == 0)
				printk("\n%03x:", i);
			printk(" %02x", ((unsigned char *)slabp)[i]);
		}
		printk("\n");
		BUG();
	}
}
#else
#define kfree_debugcheck(x) do { } while(0)
#define cache_free_debugcheck(x,objp,z) (objp)
#define check_slabp(x,y) do { } while(0)
#endif

static void *cache_alloc_refill(struct kmem_cache *cachep, gfp_t flags)
{
	int batchcount;
	struct kmem_list3 *l3;
	struct array_cache *ac;

	check_irq_off();
	ac = cpu_cache_get(cachep);
      retry:
	batchcount = ac->batchcount;
	if (!ac->touched && batchcount > BATCHREFILL_LIMIT) {
		/* if there was little recent activity on this
		 * cache, then perform only a partial refill.
		 * Otherwise we could generate refill bouncing.
		 */
		batchcount = BATCHREFILL_LIMIT;
	}
	l3 = cachep->nodelists[numa_node_id()];

	BUG_ON(ac->avail > 0 || !l3);
	spin_lock(&l3->list_lock);

	if (l3->shared) {
		struct array_cache *shared_array = l3->shared;
		if (shared_array->avail) {
			if (batchcount > shared_array->avail)
				batchcount = shared_array->avail;
			shared_array->avail -= batchcount;
			ac->avail = batchcount;
			memcpy(ac->entry,
			       &(shared_array->entry[shared_array->avail]),
			       sizeof(void *) * batchcount);
			shared_array->touched = 1;
			goto alloc_done;
		}
	}
	while (batchcount > 0) {
		struct list_head *entry;
		struct slab *slabp;
		/* Get slab alloc is to come from. */
		entry = l3->slabs_partial.next;
		if (entry == &l3->slabs_partial) {
			l3->free_touched = 1;
			entry = l3->slabs_free.next;
			if (entry == &l3->slabs_free)
				goto must_grow;
		}

		slabp = list_entry(entry, struct slab, list);
		check_slabp(cachep, slabp);
		check_spinlock_acquired(cachep);
		while (slabp->inuse < cachep->num && batchcount--) {
			STATS_INC_ALLOCED(cachep);
			STATS_INC_ACTIVE(cachep);
			STATS_SET_HIGH(cachep);

			ac->entry[ac->avail++] = slab_get_obj(cachep, slabp,
							    numa_node_id());
		}
		check_slabp(cachep, slabp);

		/* move slabp to correct slabp list: */
		list_del(&slabp->list);
		if (slabp->free == BUFCTL_END)
			list_add(&slabp->list, &l3->slabs_full);
		else
			list_add(&slabp->list, &l3->slabs_partial);
	}

      must_grow:
	l3->free_objects -= ac->avail;
      alloc_done:
	spin_unlock(&l3->list_lock);

	if (unlikely(!ac->avail)) {
		int x;
		x = cache_grow(cachep, flags, numa_node_id());

		// cache_grow can reenable interrupts, then ac could change.
		ac = cpu_cache_get(cachep);
		if (!x && ac->avail == 0)	// no objects in sight? abort
			return NULL;

		if (!ac->avail)	// objects refilled by interrupt?
			goto retry;
	}
	ac->touched = 1;
	return ac->entry[--ac->avail];
}

static inline void
cache_alloc_debugcheck_before(struct kmem_cache *cachep, gfp_t flags)
{
	might_sleep_if(flags & __GFP_WAIT);
#if DEBUG
	kmem_flagcheck(cachep, flags);
#endif
}

#if DEBUG
static void *cache_alloc_debugcheck_after(struct kmem_cache *cachep, gfp_t flags,
					void *objp, void *caller)
{
	if (!objp)
		return objp;
	if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
		if ((cachep->buffer_size % PAGE_SIZE) == 0 && OFF_SLAB(cachep))
			kernel_map_pages(virt_to_page(objp),
					 cachep->buffer_size / PAGE_SIZE, 1);
		else
			check_poison_obj(cachep, objp);
#else
		check_poison_obj(cachep, objp);
#endif
		poison_obj(cachep, objp, POISON_INUSE);
	}
	if (cachep->flags & SLAB_STORE_USER)
		*dbg_userword(cachep, objp) = caller;

	if (cachep->flags & SLAB_RED_ZONE) {
		if (*dbg_redzone1(cachep, objp) != RED_INACTIVE
		    || *dbg_redzone2(cachep, objp) != RED_INACTIVE) {
			slab_error(cachep,
				   "double free, or memory outside"
				   " object was overwritten");
			printk(KERN_ERR
			       "%p: redzone 1: 0x%lx, redzone 2: 0x%lx.\n",
			       objp, *dbg_redzone1(cachep, objp),
			       *dbg_redzone2(cachep, objp));
		}
		*dbg_redzone1(cachep, objp) = RED_ACTIVE;
		*dbg_redzone2(cachep, objp) = RED_ACTIVE;
	}
	objp += obj_offset(cachep);
	if (cachep->ctor && cachep->flags & SLAB_POISON) {
		unsigned long ctor_flags = SLAB_CTOR_CONSTRUCTOR;

		if (!(flags & __GFP_WAIT))
			ctor_flags |= SLAB_CTOR_ATOMIC;

		cachep->ctor(objp, cachep, ctor_flags);
	}
	return objp;
}
#else
#define cache_alloc_debugcheck_after(a,b,objp,d) (objp)
#endif

static inline void *____cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	void *objp;
	struct array_cache *ac;

#ifdef CONFIG_NUMA
	if (unlikely(current->mempolicy && !in_interrupt())) {
		int nid = slab_node(current->mempolicy);

		if (nid != numa_node_id())
			return __cache_alloc_node(cachep, flags, nid);
	}
#endif

	check_irq_off();
	ac = cpu_cache_get(cachep);
	if (likely(ac->avail)) {
		STATS_INC_ALLOCHIT(cachep);
		ac->touched = 1;
		objp = ac->entry[--ac->avail];
	} else {
		STATS_INC_ALLOCMISS(cachep);
		objp = cache_alloc_refill(cachep, flags);
	}
	return objp;
}

static __always_inline void *
__cache_alloc(struct kmem_cache *cachep, gfp_t flags, void *caller)
{
	unsigned long save_flags;
	void *objp;

	cache_alloc_debugcheck_before(cachep, flags);

	local_irq_save(save_flags);
	objp = ____cache_alloc(cachep, flags);
	local_irq_restore(save_flags);
	objp = cache_alloc_debugcheck_after(cachep, flags, objp,
					    caller);
	prefetchw(objp);
	return objp;
}

#ifdef CONFIG_NUMA
/*
 * A interface to enable slab creation on nodeid
 */
static void *__cache_alloc_node(struct kmem_cache *cachep, gfp_t flags, int nodeid)
{
	struct list_head *entry;
	struct slab *slabp;
	struct kmem_list3 *l3;
	void *obj;
	int x;

	l3 = cachep->nodelists[nodeid];
	BUG_ON(!l3);

      retry:
	check_irq_off();
	spin_lock(&l3->list_lock);
	entry = l3->slabs_partial.next;
	if (entry == &l3->slabs_partial) {
		l3->free_touched = 1;
		entry = l3->slabs_free.next;
		if (entry == &l3->slabs_free)
			goto must_grow;
	}

	slabp = list_entry(entry, struct slab, list);
	check_spinlock_acquired_node(cachep, nodeid);
	check_slabp(cachep, slabp);

	STATS_INC_NODEALLOCS(cachep);
	STATS_INC_ACTIVE(cachep);
	STATS_SET_HIGH(cachep);

	BUG_ON(slabp->inuse == cachep->num);

	obj = slab_get_obj(cachep, slabp, nodeid);
	check_slabp(cachep, slabp);
	l3->free_objects--;
	/* move slabp to correct slabp list: */
	list_del(&slabp->list);

	if (slabp->free == BUFCTL_END) {
		list_add(&slabp->list, &l3->slabs_full);
	} else {
		list_add(&slabp->list, &l3->slabs_partial);
	}

	spin_unlock(&l3->list_lock);
	goto done;

      must_grow:
	spin_unlock(&l3->list_lock);
	x = cache_grow(cachep, flags, nodeid);

	if (!x)
		return NULL;

	goto retry;
      done:
	return obj;
}
#endif

/*
 * Caller needs to acquire correct kmem_list's list_lock
 */
static void free_block(struct kmem_cache *cachep, void **objpp, int nr_objects,
		       int node)
{
	int i;
	struct kmem_list3 *l3;

	for (i = 0; i < nr_objects; i++) {
		void *objp = objpp[i];
		struct slab *slabp;

		slabp = virt_to_slab(objp);
		l3 = cachep->nodelists[node];
		list_del(&slabp->list);
		check_spinlock_acquired_node(cachep, node);
		check_slabp(cachep, slabp);
		slab_put_obj(cachep, slabp, objp, node);
		STATS_DEC_ACTIVE(cachep);
		l3->free_objects++;
		check_slabp(cachep, slabp);

		/* fixup slab chains */
		if (slabp->inuse == 0) {
			if (l3->free_objects > l3->free_limit) {
				l3->free_objects -= cachep->num;
				slab_destroy(cachep, slabp);
			} else {
				list_add(&slabp->list, &l3->slabs_free);
			}
		} else {
			/* Unconditionally move a slab to the end of the
			 * partial list on free - maximum time for the
			 * other objects to be freed, too.
			 */
			list_add_tail(&slabp->list, &l3->slabs_partial);
		}
	}
}

static void cache_flusharray(struct kmem_cache *cachep, struct array_cache *ac)
{
	int batchcount;
	struct kmem_list3 *l3;
	int node = numa_node_id();

	batchcount = ac->batchcount;
#if DEBUG
	BUG_ON(!batchcount || batchcount > ac->avail);
#endif
	check_irq_off();
	l3 = cachep->nodelists[node];
	spin_lock(&l3->list_lock);
	if (l3->shared) {
		struct array_cache *shared_array = l3->shared;
		int max = shared_array->limit - shared_array->avail;
		if (max) {
			if (batchcount > max)
				batchcount = max;
			memcpy(&(shared_array->entry[shared_array->avail]),
			       ac->entry, sizeof(void *) * batchcount);
			shared_array->avail += batchcount;
			goto free_done;
		}
	}

	free_block(cachep, ac->entry, batchcount, node);
      free_done:
#if STATS
	{
		int i = 0;
		struct list_head *p;

		p = l3->slabs_free.next;
		while (p != &(l3->slabs_free)) {
			struct slab *slabp;

			slabp = list_entry(p, struct slab, list);
			BUG_ON(slabp->inuse);

			i++;
			p = p->next;
		}
		STATS_SET_FREEABLE(cachep, i);
	}
#endif
	spin_unlock(&l3->list_lock);
	ac->avail -= batchcount;
	memmove(ac->entry, &(ac->entry[batchcount]),
		sizeof(void *) * ac->avail);
}

/*
 * __cache_free
 * Release an obj back to its cache. If the obj has a constructed
 * state, it must be in this state _before_ it is released.
 *
 * Called with disabled ints.
 */
static inline void __cache_free(struct kmem_cache *cachep, void *objp)
{
	struct array_cache *ac = cpu_cache_get(cachep);

	check_irq_off();
	objp = cache_free_debugcheck(cachep, objp, __builtin_return_address(0));

	/* Make sure we are not freeing a object from another
	 * node to the array cache on this cpu.
	 */
#ifdef CONFIG_NUMA
	{
		struct slab *slabp;
		slabp = virt_to_slab(objp);
		if (unlikely(slabp->nodeid != numa_node_id())) {
			struct array_cache *alien = NULL;
			int nodeid = slabp->nodeid;
			struct kmem_list3 *l3 =
			    cachep->nodelists[numa_node_id()];

			STATS_INC_NODEFREES(cachep);
			if (l3->alien && l3->alien[nodeid]) {
				alien = l3->alien[nodeid];
				spin_lock(&alien->lock);
				if (unlikely(alien->avail == alien->limit))
					__drain_alien_cache(cachep,
							    alien, nodeid);
				alien->entry[alien->avail++] = objp;
				spin_unlock(&alien->lock);
			} else {
				spin_lock(&(cachep->nodelists[nodeid])->
					  list_lock);
				free_block(cachep, &objp, 1, nodeid);
				spin_unlock(&(cachep->nodelists[nodeid])->
					    list_lock);
			}
			return;
		}
	}
#endif
	if (likely(ac->avail < ac->limit)) {
		STATS_INC_FREEHIT(cachep);
		ac->entry[ac->avail++] = objp;
		return;
	} else {
		STATS_INC_FREEMISS(cachep);
		cache_flusharray(cachep, ac);
		ac->entry[ac->avail++] = objp;
	}
}

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.  The flags are only relevant
 * if the cache has no available objects.
 */
void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags)
{
	return __cache_alloc(cachep, flags, __builtin_return_address(0));
}
EXPORT_SYMBOL(kmem_cache_alloc);

/**
 * kmem_ptr_validate - check if an untrusted pointer might
 *	be a slab entry.
 * @cachep: the cache we're checking against
 * @ptr: pointer to validate
 *
 * This verifies that the untrusted pointer looks sane:
 * it is _not_ a guarantee that the pointer is actually
 * part of the slab cache in question, but it at least
 * validates that the pointer can be dereferenced and
 * looks half-way sane.
 *
 * Currently only used for dentry validation.
 */
int fastcall kmem_ptr_validate(struct kmem_cache *cachep, void *ptr)
{
	unsigned long addr = (unsigned long)ptr;
	unsigned long min_addr = PAGE_OFFSET;
	unsigned long align_mask = BYTES_PER_WORD - 1;
	unsigned long size = cachep->buffer_size;
	struct page *page;

	if (unlikely(addr < min_addr))
		goto out;
	if (unlikely(addr > (unsigned long)high_memory - size))
		goto out;
	if (unlikely(addr & align_mask))
		goto out;
	if (unlikely(!kern_addr_valid(addr)))
		goto out;
	if (unlikely(!kern_addr_valid(addr + size - 1)))
		goto out;
	page = virt_to_page(ptr);
	if (unlikely(!PageSlab(page)))
		goto out;
	if (unlikely(page_get_cache(page) != cachep))
		goto out;
	return 1;
      out:
	return 0;
}

#ifdef CONFIG_NUMA
/**
 * kmem_cache_alloc_node - Allocate an object on the specified node
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 * @nodeid: node number of the target node.
 *
 * Identical to kmem_cache_alloc, except that this function is slow
 * and can sleep. And it will allocate memory on the given node, which
 * can improve the performance for cpu bound structures.
 * New and improved: it will now make sure that the object gets
 * put on the correct node list so that there is no false sharing.
 */
void *kmem_cache_alloc_node(struct kmem_cache *cachep, gfp_t flags, int nodeid)
{
	unsigned long save_flags;
	void *ptr;

	cache_alloc_debugcheck_before(cachep, flags);
	local_irq_save(save_flags);

	if (nodeid == -1 || nodeid == numa_node_id() ||
	    !cachep->nodelists[nodeid])
		ptr = ____cache_alloc(cachep, flags);
	else
		ptr = __cache_alloc_node(cachep, flags, nodeid);
	local_irq_restore(save_flags);

	ptr = cache_alloc_debugcheck_after(cachep, flags, ptr,
					   __builtin_return_address(0));

	return ptr;
}
EXPORT_SYMBOL(kmem_cache_alloc_node);

void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	struct kmem_cache *cachep;

	cachep = kmem_find_general_cachep(size, flags);
	if (unlikely(cachep == NULL))
		return NULL;
	return kmem_cache_alloc_node(cachep, flags, node);
}
EXPORT_SYMBOL(kmalloc_node);
#endif

/**
 * kmalloc - allocate memory
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * kmalloc is the normal method of allocating memory
 * in the kernel.
 *
 * The @flags argument may be one of:
 *
 * %GFP_USER - Allocate memory on behalf of user.  May sleep.
 *
 * %GFP_KERNEL - Allocate normal kernel ram.  May sleep.
 *
 * %GFP_ATOMIC - Allocation will not sleep.  Use inside interrupt handlers.
 *
 * Additionally, the %GFP_DMA flag may be set to indicate the memory
 * must be suitable for DMA.  This can mean different things on different
 * platforms.  For example, on i386, it means that the memory must come
 * from the first 16MB.
 */
static __always_inline void *__do_kmalloc(size_t size, gfp_t flags,
					  void *caller)
{
	struct kmem_cache *cachep;

	/* If you want to save a few bytes .text space: replace
	 * __ with kmem_.
	 * Then kmalloc uses the uninlined functions instead of the inline
	 * functions.
	 */
	cachep = __find_general_cachep(size, flags);
	if (unlikely(cachep == NULL))
		return NULL;
	return __cache_alloc(cachep, flags, caller);
}

#ifndef CONFIG_DEBUG_SLAB

void *__kmalloc(size_t size, gfp_t flags)
{
	return __do_kmalloc(size, flags, NULL);
}
EXPORT_SYMBOL(__kmalloc);

#else

void *__kmalloc_track_caller(size_t size, gfp_t flags, void *caller)
{
	return __do_kmalloc(size, flags, caller);
}
EXPORT_SYMBOL(__kmalloc_track_caller);

#endif

#ifdef CONFIG_SMP
/**
 * __alloc_percpu - allocate one copy of the object for every present
 * cpu in the system, zeroing them.
 * Objects should be dereferenced using the per_cpu_ptr macro only.
 *
 * @size: how many bytes of memory are required.
 */
void *__alloc_percpu(size_t size)
{
	int i;
	struct percpu_data *pdata = kmalloc(sizeof(*pdata), GFP_KERNEL);

	if (!pdata)
		return NULL;

	/*
	 * Cannot use for_each_online_cpu since a cpu may come online
	 * and we have no way of figuring out how to fix the array
	 * that we have allocated then....
	 */
	for_each_cpu(i) {
		int node = cpu_to_node(i);

		if (node_online(node))
			pdata->ptrs[i] = kmalloc_node(size, GFP_KERNEL, node);
		else
			pdata->ptrs[i] = kmalloc(size, GFP_KERNEL);

		if (!pdata->ptrs[i])
			goto unwind_oom;
		memset(pdata->ptrs[i], 0, size);
	}

	/* Catch derefs w/o wrappers */
	return (void *)(~(unsigned long)pdata);

      unwind_oom:
	while (--i >= 0) {
		if (!cpu_possible(i))
			continue;
		kfree(pdata->ptrs[i]);
	}
	kfree(pdata);
	return NULL;
}
EXPORT_SYMBOL(__alloc_percpu);
#endif

/**
 * kmem_cache_free - Deallocate an object
 * @cachep: The cache the allocation was from.
 * @objp: The previously allocated object.
 *
 * Free an object which was previously allocated from this
 * cache.
 */
void kmem_cache_free(struct kmem_cache *cachep, void *objp)
{
	unsigned long flags;

	local_irq_save(flags);
	__cache_free(cachep, objp);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(kmem_cache_free);

/**
 * kfree - free previously allocated memory
 * @objp: pointer returned by kmalloc.
 *
 * If @objp is NULL, no operation is performed.
 *
 * Don't free memory not originally allocated by kmalloc()
 * or you will run into trouble.
 */
void kfree(const void *objp)
{
	struct kmem_cache *c;
	unsigned long flags;

	if (unlikely(!objp))
		return;
	local_irq_save(flags);
	kfree_debugcheck(objp);
	c = virt_to_cache(objp);
	mutex_debug_check_no_locks_freed(objp, obj_size(c));
	__cache_free(c, (void *)objp);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(kfree);

#ifdef CONFIG_SMP
/**
 * free_percpu - free previously allocated percpu memory
 * @objp: pointer returned by alloc_percpu.
 *
 * Don't free memory not originally allocated by alloc_percpu()
 * The complemented objp is to check for that.
 */
void free_percpu(const void *objp)
{
	int i;
	struct percpu_data *p = (struct percpu_data *)(~(unsigned long)objp);

	/*
	 * We allocate for all cpus so we cannot use for online cpu here.
	 */
	for_each_cpu(i)
	    kfree(p->ptrs[i]);
	kfree(p);
}
EXPORT_SYMBOL(free_percpu);
#endif

unsigned int kmem_cache_size(struct kmem_cache *cachep)
{
	return obj_size(cachep);
}
EXPORT_SYMBOL(kmem_cache_size);

const char *kmem_cache_name(struct kmem_cache *cachep)
{
	return cachep->name;
}
EXPORT_SYMBOL_GPL(kmem_cache_name);

/*
 * This initializes kmem_list3 for all nodes.
 */
static int alloc_kmemlist(struct kmem_cache *cachep)
{
	int node;
	struct kmem_list3 *l3;
	int err = 0;

	for_each_online_node(node) {
		struct array_cache *nc = NULL, *new;
		struct array_cache **new_alien = NULL;
#ifdef CONFIG_NUMA
		if (!(new_alien = alloc_alien_cache(node, cachep->limit)))
			goto fail;
#endif
		if (!(new = alloc_arraycache(node, (cachep->shared *
						    cachep->batchcount),
					     0xbaadf00d)))
			goto fail;
		if ((l3 = cachep->nodelists[node])) {

			spin_lock_irq(&l3->list_lock);

			if ((nc = cachep->nodelists[node]->shared))
				free_block(cachep, nc->entry, nc->avail, node);

			l3->shared = new;
			if (!cachep->nodelists[node]->alien) {
				l3->alien = new_alien;
				new_alien = NULL;
			}
			l3->free_limit = (1 + nr_cpus_node(node)) *
			    cachep->batchcount + cachep->num;
			spin_unlock_irq(&l3->list_lock);
			kfree(nc);
			free_alien_cache(new_alien);
			continue;
		}
		if (!(l3 = kmalloc_node(sizeof(struct kmem_list3),
					GFP_KERNEL, node)))
			goto fail;

		kmem_list3_init(l3);
		l3->next_reap = jiffies + REAPTIMEOUT_LIST3 +
		    ((unsigned long)cachep) % REAPTIMEOUT_LIST3;
		l3->shared = new;
		l3->alien = new_alien;
		l3->free_limit = (1 + nr_cpus_node(node)) *
		    cachep->batchcount + cachep->num;
		cachep->nodelists[node] = l3;
	}
	return err;
      fail:
	err = -ENOMEM;
	return err;
}

struct ccupdate_struct {
	struct kmem_cache *cachep;
	struct array_cache *new[NR_CPUS];
};

static void do_ccupdate_local(void *info)
{
	struct ccupdate_struct *new = (struct ccupdate_struct *)info;
	struct array_cache *old;

	check_irq_off();
	old = cpu_cache_get(new->cachep);

	new->cachep->array[smp_processor_id()] = new->new[smp_processor_id()];
	new->new[smp_processor_id()] = old;
}

static int do_tune_cpucache(struct kmem_cache *cachep, int limit, int batchcount,
			    int shared)
{
	struct ccupdate_struct new;
	int i, err;

	memset(&new.new, 0, sizeof(new.new));
	for_each_online_cpu(i) {
		new.new[i] =
		    alloc_arraycache(cpu_to_node(i), limit, batchcount);
		if (!new.new[i]) {
			for (i--; i >= 0; i--)
				kfree(new.new[i]);
			return -ENOMEM;
		}
	}
	new.cachep = cachep;

	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	check_irq_on();
	spin_lock(&cachep->spinlock);
	cachep->batchcount = batchcount;
	cachep->limit = limit;
	cachep->shared = shared;
	spin_unlock(&cachep->spinlock);

	for_each_online_cpu(i) {
		struct array_cache *ccold = new.new[i];
		if (!ccold)
			continue;
		spin_lock_irq(&cachep->nodelists[cpu_to_node(i)]->list_lock);
		free_block(cachep, ccold->entry, ccold->avail, cpu_to_node(i));
		spin_unlock_irq(&cachep->nodelists[cpu_to_node(i)]->list_lock);
		kfree(ccold);
	}

	err = alloc_kmemlist(cachep);
	if (err) {
		printk(KERN_ERR "alloc_kmemlist failed for %s, error %d.\n",
		       cachep->name, -err);
		BUG();
	}
	return 0;
}

static void enable_cpucache(struct kmem_cache *cachep)
{
	int err;
	int limit, shared;

	/* The head array serves three purposes:
	 * - create a LIFO ordering, i.e. return objects that are cache-warm
	 * - reduce the number of spinlock operations.
	 * - reduce the number of linked list operations on the slab and 
	 *   bufctl chains: array operations are cheaper.
	 * The numbers are guessed, we should auto-tune as described by
	 * Bonwick.
	 */
	if (cachep->buffer_size > 131072)
		limit = 1;
	else if (cachep->buffer_size > PAGE_SIZE)
		limit = 8;
	else if (cachep->buffer_size > 1024)
		limit = 24;
	else if (cachep->buffer_size > 256)
		limit = 54;
	else
		limit = 120;

	/* Cpu bound tasks (e.g. network routing) can exhibit cpu bound
	 * allocation behaviour: Most allocs on one cpu, most free operations
	 * on another cpu. For these cases, an efficient object passing between
	 * cpus is necessary. This is provided by a shared array. The array
	 * replaces Bonwick's magazine layer.
	 * On uniprocessor, it's functionally equivalent (but less efficient)
	 * to a larger limit. Thus disabled by default.
	 */
	shared = 0;
#ifdef CONFIG_SMP
	if (cachep->buffer_size <= PAGE_SIZE)
		shared = 8;
#endif

#if DEBUG
	/* With debugging enabled, large batchcount lead to excessively
	 * long periods with disabled local interrupts. Limit the 
	 * batchcount
	 */
	if (limit > 32)
		limit = 32;
#endif
	err = do_tune_cpucache(cachep, limit, (limit + 1) / 2, shared);
	if (err)
		printk(KERN_ERR "enable_cpucache failed for %s, error %d.\n",
		       cachep->name, -err);
}

static void drain_array_locked(struct kmem_cache *cachep, struct array_cache *ac,
				int force, int node)
{
	int tofree;

	check_spinlock_acquired_node(cachep, node);
	if (ac->touched && !force) {
		ac->touched = 0;
	} else if (ac->avail) {
		tofree = force ? ac->avail : (ac->limit + 4) / 5;
		if (tofree > ac->avail) {
			tofree = (ac->avail + 1) / 2;
		}
		free_block(cachep, ac->entry, tofree, node);
		ac->avail -= tofree;
		memmove(ac->entry, &(ac->entry[tofree]),
			sizeof(void *) * ac->avail);
	}
}

/**
 * cache_reap - Reclaim memory from caches.
 * @unused: unused parameter
 *
 * Called from workqueue/eventd every few seconds.
 * Purpose:
 * - clear the per-cpu caches for this CPU.
 * - return freeable pages to the main free memory pool.
 *
 * If we cannot acquire the cache chain mutex then just give up - we'll
 * try again on the next iteration.
 */
static void cache_reap(void *unused)
{
	struct list_head *walk;
	struct kmem_list3 *l3;

	if (!mutex_trylock(&cache_chain_mutex)) {
		/* Give up. Setup the next iteration. */
		schedule_delayed_work(&__get_cpu_var(reap_work),
				      REAPTIMEOUT_CPUC);
		return;
	}

	list_for_each(walk, &cache_chain) {
		struct kmem_cache *searchp;
		struct list_head *p;
		int tofree;
		struct slab *slabp;

		searchp = list_entry(walk, struct kmem_cache, next);

		if (searchp->flags & SLAB_NO_REAP)
			goto next;

		check_irq_on();

		l3 = searchp->nodelists[numa_node_id()];
		reap_alien(searchp, l3);
		spin_lock_irq(&l3->list_lock);

		drain_array_locked(searchp, cpu_cache_get(searchp), 0,
				   numa_node_id());

		if (time_after(l3->next_reap, jiffies))
			goto next_unlock;

		l3->next_reap = jiffies + REAPTIMEOUT_LIST3;

		if (l3->shared)
			drain_array_locked(searchp, l3->shared, 0,
					   numa_node_id());

		if (l3->free_touched) {
			l3->free_touched = 0;
			goto next_unlock;
		}

		tofree =
		    (l3->free_limit + 5 * searchp->num -
		     1) / (5 * searchp->num);
		do {
			p = l3->slabs_free.next;
			if (p == &(l3->slabs_free))
				break;

			slabp = list_entry(p, struct slab, list);
			BUG_ON(slabp->inuse);
			list_del(&slabp->list);
			STATS_INC_REAPED(searchp);

			/* Safe to drop the lock. The slab is no longer
			 * linked to the cache.
			 * searchp cannot disappear, we hold
			 * cache_chain_lock
			 */
			l3->free_objects -= searchp->num;
			spin_unlock_irq(&l3->list_lock);
			slab_destroy(searchp, slabp);
			spin_lock_irq(&l3->list_lock);
		} while (--tofree > 0);
	      next_unlock:
		spin_unlock_irq(&l3->list_lock);
	      next:
		cond_resched();
	}
	check_irq_on();
	mutex_unlock(&cache_chain_mutex);
	next_reap_node();
	/* Setup the next iteration */
	schedule_delayed_work(&__get_cpu_var(reap_work), REAPTIMEOUT_CPUC);
}

#ifdef CONFIG_PROC_FS

static void print_slabinfo_header(struct seq_file *m)
{
	/*
	 * Output format version, so at least we can change it
	 * without _too_ many complaints.
	 */
#if STATS
	seq_puts(m, "slabinfo - version: 2.1 (statistics)\n");
#else
	seq_puts(m, "slabinfo - version: 2.1\n");
#endif
	seq_puts(m, "# name            <active_objs> <num_objs> <objsize> "
		 "<objperslab> <pagesperslab>");
	seq_puts(m, " : tunables <limit> <batchcount> <sharedfactor>");
	seq_puts(m, " : slabdata <active_slabs> <num_slabs> <sharedavail>");
#if STATS
	seq_puts(m, " : globalstat <listallocs> <maxobjs> <grown> <reaped> "
		 "<error> <maxfreeable> <nodeallocs> <remotefrees>");
	seq_puts(m, " : cpustat <allochit> <allocmiss> <freehit> <freemiss>");
#endif
	seq_putc(m, '\n');
}

static void *s_start(struct seq_file *m, loff_t *pos)
{
	loff_t n = *pos;
	struct list_head *p;

	mutex_lock(&cache_chain_mutex);
	if (!n)
		print_slabinfo_header(m);
	p = cache_chain.next;
	while (n--) {
		p = p->next;
		if (p == &cache_chain)
			return NULL;
	}
	return list_entry(p, struct kmem_cache, next);
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct kmem_cache *cachep = p;
	++*pos;
	return cachep->next.next == &cache_chain ? NULL
	    : list_entry(cachep->next.next, struct kmem_cache, next);
}

static void s_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&cache_chain_mutex);
}

static int s_show(struct seq_file *m, void *p)
{
	struct kmem_cache *cachep = p;
	struct list_head *q;
	struct slab *slabp;
	unsigned long active_objs;
	unsigned long num_objs;
	unsigned long active_slabs = 0;
	unsigned long num_slabs, free_objects = 0, shared_avail = 0;
	const char *name;
	char *error = NULL;
	int node;
	struct kmem_list3 *l3;

	spin_lock(&cachep->spinlock);
	active_objs = 0;
	num_slabs = 0;
	for_each_online_node(node) {
		l3 = cachep->nodelists[node];
		if (!l3)
			continue;

		check_irq_on();
		spin_lock_irq(&l3->list_lock);

		list_for_each(q, &l3->slabs_full) {
			slabp = list_entry(q, struct slab, list);
			if (slabp->inuse != cachep->num && !error)
				error = "slabs_full accounting error";
			active_objs += cachep->num;
			active_slabs++;
		}
		list_for_each(q, &l3->slabs_partial) {
			slabp = list_entry(q, struct slab, list);
			if (slabp->inuse == cachep->num && !error)
				error = "slabs_partial inuse accounting error";
			if (!slabp->inuse && !error)
				error = "slabs_partial/inuse accounting error";
			active_objs += slabp->inuse;
			active_slabs++;
		}
		list_for_each(q, &l3->slabs_free) {
			slabp = list_entry(q, struct slab, list);
			if (slabp->inuse && !error)
				error = "slabs_free/inuse accounting error";
			num_slabs++;
		}
		free_objects += l3->free_objects;
		if (l3->shared)
			shared_avail += l3->shared->avail;

		spin_unlock_irq(&l3->list_lock);
	}
	num_slabs += active_slabs;
	num_objs = num_slabs * cachep->num;
	if (num_objs - active_objs != free_objects && !error)
		error = "free_objects accounting error";

	name = cachep->name;
	if (error)
		printk(KERN_ERR "slab: cache %s error: %s\n", name, error);

	seq_printf(m, "%-17s %6lu %6lu %6u %4u %4d",
		   name, active_objs, num_objs, cachep->buffer_size,
		   cachep->num, (1 << cachep->gfporder));
	seq_printf(m, " : tunables %4u %4u %4u",
		   cachep->limit, cachep->batchcount, cachep->shared);
	seq_printf(m, " : slabdata %6lu %6lu %6lu",
		   active_slabs, num_slabs, shared_avail);
#if STATS
	{			/* list3 stats */
		unsigned long high = cachep->high_mark;
		unsigned long allocs = cachep->num_allocations;
		unsigned long grown = cachep->grown;
		unsigned long reaped = cachep->reaped;
		unsigned long errors = cachep->errors;
		unsigned long max_freeable = cachep->max_freeable;
		unsigned long node_allocs = cachep->node_allocs;
		unsigned long node_frees = cachep->node_frees;

		seq_printf(m, " : globalstat %7lu %6lu %5lu %4lu \
				%4lu %4lu %4lu %4lu", allocs, high, grown, reaped, errors, max_freeable, node_allocs, node_frees);
	}
	/* cpu stats */
	{
		unsigned long allochit = atomic_read(&cachep->allochit);
		unsigned long allocmiss = atomic_read(&cachep->allocmiss);
		unsigned long freehit = atomic_read(&cachep->freehit);
		unsigned long freemiss = atomic_read(&cachep->freemiss);

		seq_printf(m, " : cpustat %6lu %6lu %6lu %6lu",
			   allochit, allocmiss, freehit, freemiss);
	}
#endif
	seq_putc(m, '\n');
	spin_unlock(&cachep->spinlock);
	return 0;
}

/*
 * slabinfo_op - iterator that generates /proc/slabinfo
 *
 * Output layout:
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */

struct seq_operations slabinfo_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show,
};

#define MAX_SLABINFO_WRITE 128
/**
 * slabinfo_write - Tuning for the slab allocator
 * @file: unused
 * @buffer: user buffer
 * @count: data length
 * @ppos: unused
 */
ssize_t slabinfo_write(struct file *file, const char __user * buffer,
		       size_t count, loff_t *ppos)
{
	char kbuf[MAX_SLABINFO_WRITE + 1], *tmp;
	int limit, batchcount, shared, res;
	struct list_head *p;

	if (count > MAX_SLABINFO_WRITE)
		return -EINVAL;
	if (copy_from_user(&kbuf, buffer, count))
		return -EFAULT;
	kbuf[MAX_SLABINFO_WRITE] = '\0';

	tmp = strchr(kbuf, ' ');
	if (!tmp)
		return -EINVAL;
	*tmp = '\0';
	tmp++;
	if (sscanf(tmp, " %d %d %d", &limit, &batchcount, &shared) != 3)
		return -EINVAL;

	/* Find the cache in the chain of caches. */
	mutex_lock(&cache_chain_mutex);
	res = -EINVAL;
	list_for_each(p, &cache_chain) {
		struct kmem_cache *cachep = list_entry(p, struct kmem_cache,
						       next);

		if (!strcmp(cachep->name, kbuf)) {
			if (limit < 1 ||
			    batchcount < 1 ||
			    batchcount > limit || shared < 0) {
				res = 0;
			} else {
				res = do_tune_cpucache(cachep, limit,
						       batchcount, shared);
			}
			break;
		}
	}
	mutex_unlock(&cache_chain_mutex);
	if (res >= 0)
		res = count;
	return res;
}
#endif

/**
 * ksize - get the actual amount of memory allocated for a given object
 * @objp: Pointer to the object
 *
 * kmalloc may internally round up allocations and return more memory
 * than requested. ksize() can be used to determine the actual amount of
 * memory allocated. The caller may use this additional memory, even though
 * a smaller amount of memory was initially specified with the kmalloc call.
 * The caller must guarantee that objp points to a valid object previously
 * allocated with either kmalloc() or kmem_cache_alloc(). The object
 * must not be freed during the duration of the call.
 */
unsigned int ksize(const void *objp)
{
	if (unlikely(objp == NULL))
		return 0;

	return obj_size(virt_to_cache(objp));
}
