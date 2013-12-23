/* Support for MMIO probes.
 * Benfit many code from kprobes
 * (C) 2002 Louis Zhuang <louis.zhuang@intel.com>.
 *     2007 Alexander Eichner
 *     2008 Pekka Paalanen <pq@iki.fi>
 */

#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/preempt.h>
#include <linux/percpu.h>
#include <linux/kdebug.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <linux/errno.h>
#include <asm/debugreg.h>
#include <linux/mmiotrace.h>

#define KMMIO_PAGE_HASH_BITS 4
#define KMMIO_PAGE_TABLE_SIZE (1 << KMMIO_PAGE_HASH_BITS)

struct kmmio_fault_page {
	struct list_head list;
	struct kmmio_fault_page *release_next;
	unsigned long page; /* location of the fault page */
	bool old_presence; /* page presence prior to arming */
	bool armed;

	/*
	 * Number of times this page has been registered as a part
	 * of a probe. If zero, page is disarmed and this may be freed.
	 * Used only by writers (RCU) and post_kmmio_handler().
	 * Protected by kmmio_lock, when linked into kmmio_page_table.
	 */
	int count;
};

struct kmmio_delayed_release {
	struct rcu_head rcu;
	struct kmmio_fault_page *release_list;
};

struct kmmio_context {
	struct kmmio_fault_page *fpage;
	struct kmmio_probe *probe;
	unsigned long saved_flags;
	unsigned long addr;
	int active;
};

static DEFINE_SPINLOCK(kmmio_lock);

/* Protected by kmmio_lock */
unsigned int kmmio_count;

/* Read-protected by RCU, write-protected by kmmio_lock. */
static struct list_head kmmio_page_table[KMMIO_PAGE_TABLE_SIZE];
static LIST_HEAD(kmmio_probes);

static struct list_head *kmmio_page_list(unsigned long page)
{
	return &kmmio_page_table[hash_long(page, KMMIO_PAGE_HASH_BITS)];
}

/* Accessed per-cpu */
static DEFINE_PER_CPU(struct kmmio_context, kmmio_ctx);

/*
 * this is basically a dynamic stabbing problem:
 * Could use the existing prio tree code or
 * Possible better implementations:
 * The Interval Skip List: A Data Structure for Finding All Intervals That
 * Overlap a Point (might be simple)
 * Space Efficient Dynamic Stabbing with Fast Queries - Mikkel Thorup
 */
/* Get the kmmio at this addr (if any). You must be holding RCU read lock. */
static struct kmmio_probe *get_kmmio_probe(unsigned long addr)
{
	struct kmmio_probe *p;
	list_for_each_entry_rcu(p, &kmmio_probes, list) {
		if (addr >= p->addr && addr < (p->addr + p->len))
			return p;
	}
	return NULL;
}

/* You must be holding RCU read lock. */
static struct kmmio_fault_page *get_kmmio_fault_page(unsigned long page)
{
	struct list_head *head;
	struct kmmio_fault_page *p;

	page &= PAGE_MASK;
	head = kmmio_page_list(page);
	list_for_each_entry_rcu(p, head, list) {
		if (p->page == page)
			return p;
	}
	return NULL;
}

static void set_pmd_presence(pmd_t *pmd, bool present, bool *old)
{
	pmdval_t v = pmd_val(*pmd);
	*old = !!(v & _PAGE_PRESENT);
	v &= ~_PAGE_PRESENT;
	if (present)
		v |= _PAGE_PRESENT;
	set_pmd(pmd, __pmd(v));
}

static void set_pte_presence(pte_t *pte, bool present, bool *old)
{
	pteval_t v = pte_val(*pte);
	*old = !!(v & _PAGE_PRESENT);
	v &= ~_PAGE_PRESENT;
	if (present)
		v |= _PAGE_PRESENT;
	set_pte_atomic(pte, __pte(v));
}

static int set_page_presence(unsigned long addr, bool present, bool *old)
{
	unsigned int level;
	pte_t *pte = lookup_address(addr, &level);

	if (!pte) {
		pr_err("kmmio: no pte for page 0x%08lx\n", addr);
		return -1;
	}

	switch (level) {
	case PG_LEVEL_2M:
		set_pmd_presence((pmd_t *)pte, present, old);
		break;
	case PG_LEVEL_4K:
		set_pte_presence(pte, present, old);
		break;
	default:
		pr_err("kmmio: unexpected page level 0x%x.\n", level);
		return -1;
	}

	__flush_tlb_one(addr);
	return 0;
}

/*
 * Mark the given page as not present. Access to it will trigger a fault.
 *
 * Struct kmmio_fault_page is protected by RCU and kmmio_lock, but the
 * protection is ignored here. RCU read lock is assumed held, so the struct
 * will not disappear unexpectedly. Furthermore, the caller must guarantee,
 * that double arming the same virtual address (page) cannot occur.
 *
 * Double disarming on the other hand is allowed, and may occur when a fault
 * and mmiotrace shutdown happen simultaneously.
 */
static int arm_kmmio_fault_page(struct kmmio_fault_page *f)
{
	int ret;
	WARN_ONCE(f->armed, KERN_ERR "kmmio page already armed.\n");
	if (f->armed) {
		pr_warning("kmmio double-arm: page 0x%08lx, ref %d, old %d\n",
					f->page, f->count, f->old_presence);
	}
	ret = set_page_presence(f->page, false, &f->old_presence);
	WARN_ONCE(ret < 0, KERN_ERR "kmmio arming 0x%08lx failed.\n", f->page);
	f->armed = true;
	return ret;
}

/** Restore the given page to saved presence state. */
static void disarm_kmmio_fault_page(struct kmmio_fault_page *f)
{
	bool tmp;
	int ret = set_page_presence(f->page, f->old_presence, &tmp);
	WARN_ONCE(ret < 0,
			KERN_ERR "kmmio disarming 0x%08lx failed.\n", f->page);
	f->armed = false;
}

/*
 * This is being called from do_page_fault().
 *
 * We may be in an interrupt or a critical section. Also prefecthing may
 * trigger a page fault. We may be in the middle of process switch.
 * We cannot take any locks, because we could be executing especially
 * within a kmmio critical section.
 *
 * Local interrupts are disabled, so preemption cannot happen.
 * Do not enable interrupts, do not sleep, and watch out for other CPUs.
 */
/*
 * Interrupts are disabled on entry as trap3 is an interrupt gate
 * and they remain disabled thorough out this function.
 */
int kmmio_handler(struct pt_regs *regs, unsigned long addr)
{
	struct kmmio_context *ctx;
	struct kmmio_fault_page *faultpage;
	int ret = 0; /* default to fault not handled */

	/*
	 * Preemption is now disabled to prevent process switch during
	 * single stepping. We can only handle one active kmmio trace
	 * per cpu, so ensure that we finish it before something else
	 * gets to run. We also hold the RCU read lock over single
	 * stepping to avoid looking up the probe and kmmio_fault_page
	 * again.
	 */
	preempt_disable();
	rcu_read_lock();

	faultpage = get_kmmio_fault_page(addr);
	if (!faultpage) {
		/*
		 * Either this page fault is not caused by kmmio, or
		 * another CPU just pulled the kmmio probe from under
		 * our feet. The latter case should not be possible.
		 */
		goto no_kmmio;
	}

	ctx = &get_cpu_var(kmmio_ctx);
	if (ctx->active) {
		if (addr == ctx->addr) {
			/*
			 * A second fault on the same page means some other
			 * condition needs handling by do_page_fault(), the
			 * page really not being present is the most common.
			 */
			pr_debug("kmmio: secondary hit for 0x%08lx CPU %d.\n",
					addr, smp_processor_id());

			if (!faultpage->old_presence)
				pr_info("kmmio: unexpected secondary hit for "
					"address 0x%08lx on CPU %d.\n", addr,
					smp_processor_id());
		} else {
			/*
			 * Prevent overwriting already in-flight context.
			 * This should not happen, let's hope disarming at
			 * least prevents a panic.
			 */
			pr_emerg("kmmio: recursive probe hit on CPU %d, "
					"for address 0x%08lx. Ignoring.\n",
					smp_processor_id(), addr);
			pr_emerg("kmmio: previous hit was at 0x%08lx.\n",
						ctx->addr);
			disarm_kmmio_fault_page(faultpage);
		}
		goto no_kmmio_ctx;
	}
	ctx->active++;

	ctx->fpage = faultpage;
	ctx->probe = get_kmmio_probe(addr);
	ctx->saved_flags = (regs->flags & (X86_EFLAGS_TF | X86_EFLAGS_IF));
	ctx->addr = addr;

	if (ctx->probe && ctx->probe->pre_handler)
		ctx->probe->pre_handler(ctx->probe, regs, addr);

	/*
	 * Enable single-stepping and disable interrupts for the faulting
	 * context. Local interrupts must not get enabled during stepping.
	 */
	regs->flags |= X86_EFLAGS_TF;
	regs->flags &= ~X86_EFLAGS_IF;

	/* Now we set present bit in PTE and single step. */
	disarm_kmmio_fault_page(ctx->fpage);

	/*
	 * If another cpu accesses the same page while we are stepping,
	 * the access will not be caught. It will simply succeed and the
	 * only downside is we lose the event. If this becomes a problem,
	 * the user should drop to single cpu before tracing.
	 */

	put_cpu_var(kmmio_ctx);
	return 1; /* fault handled */

no_kmmio_ctx:
	put_cpu_var(kmmio_ctx);
no_kmmio:
	rcu_read_unlock();
	preempt_enable_no_resched();
	return ret;
}

/*
 * Interrupts are disabled on entry as trap1 is an interrupt gate
 * and they remain disabled thorough out this function.
 * This must always get called as the pair to kmmio_handler().
 */
static int post_kmmio_handler(unsigned long condition, struct pt_regs *regs)
{
	int ret = 0;
	struct kmmio_context *ctx = &get_cpu_var(kmmio_ctx);

	if (!ctx->active) {
		pr_warning("kmmio: spurious debug trap on CPU %d.\n",
							smp_processor_id());
		goto out;
	}

	if (ctx->probe && ctx->probe->post_handler)
		ctx->probe->post_handler(ctx->probe, condition, regs);

	/* Prevent racing against release_kmmio_fault_page(). */
	spin_lock(&kmmio_lock);
	if (ctx->fpage->count)
		arm_kmmio_fault_page(ctx->fpage);
	spin_unlock(&kmmio_lock);

	regs->flags &= ~X86_EFLAGS_TF;
	regs->flags |= ctx->saved_flags;

	/* These were acquired in kmmio_handler(). */
	ctx->active--;
	BUG_ON(ctx->active);
	rcu_read_unlock();
	preempt_enable_no_resched();

	/*
	 * if somebody else is singlestepping across a probe point, flags
	 * will have TF set, in which case, continue the remaining processing
	 * of do_debug, as if this is not a probe hit.
	 */
	if (!(regs->flags & X86_EFLAGS_TF))
		ret = 1;
out:
	put_cpu_var(kmmio_ctx);
	return ret;
}

/* You must be holding kmmio_lock. */
static int add_kmmio_fault_page(unsigned long page)
{
	struct kmmio_fault_page *f;

	page &= PAGE_MASK;
	f = get_kmmio_fault_page(page);
	if (f) {
		if (!f->count)
			arm_kmmio_fault_page(f);
		f->count++;
		return 0;
	}

	f = kzalloc(sizeof(*f), GFP_ATOMIC);
	if (!f)
		return -1;

	f->count = 1;
	f->page = page;

	if (arm_kmmio_fault_page(f)) {
		kfree(f);
		return -1;
	}

	list_add_rcu(&f->list, kmmio_page_list(f->page));

	return 0;
}

/* You must be holding kmmio_lock. */
static void release_kmmio_fault_page(unsigned long page,
				struct kmmio_fault_page **release_list)
{
	struct kmmio_fault_page *f;

	page &= PAGE_MASK;
	f = get_kmmio_fault_page(page);
	if (!f)
		return;

	f->count--;
	BUG_ON(f->count < 0);
	if (!f->count) {
		disarm_kmmio_fault_page(f);
		f->release_next = *release_list;
		*release_list = f;
	}
}

/*
 * With page-unaligned ioremaps, one or two armed pages may contain
 * addresses from outside the intended mapping. Events for these addresses
 * are currently silently dropped. The events may result only from programming
 * mistakes by accessing addresses before the beginning or past the end of a
 * mapping.
 */
int register_kmmio_probe(struct kmmio_probe *p)
{
	unsigned long flags;
	int ret = 0;
	unsigned long size = 0;
	const unsigned long size_lim = p->len + (p->addr & ~PAGE_MASK);

	spin_lock_irqsave(&kmmio_lock, flags);
	if (get_kmmio_probe(p->addr)) {
		ret = -EEXIST;
		goto out;
	}
	kmmio_count++;
	list_add_rcu(&p->list, &kmmio_probes);
	while (size < size_lim) {
		if (add_kmmio_fault_page(p->addr + size))
			pr_err("kmmio: Unable to set page fault.\n");
		size += PAGE_SIZE;
	}
out:
	spin_unlock_irqrestore(&kmmio_lock, flags);
	/*
	 * XXX: What should I do here?
	 * Here was a call to global_flush_tlb(), but it does not exist
	 * anymore. It seems it's not needed after all.
	 */
	return ret;
}
EXPORT_SYMBOL(register_kmmio_probe);

static void rcu_free_kmmio_fault_pages(struct rcu_head *head)
{
	struct kmmio_delayed_release *dr = container_of(
						head,
						struct kmmio_delayed_release,
						rcu);
	struct kmmio_fault_page *p = dr->release_list;
	while (p) {
		struct kmmio_fault_page *next = p->release_next;
		BUG_ON(p->count);
		kfree(p);
		p = next;
	}
	kfree(dr);
}

static void remove_kmmio_fault_pages(struct rcu_head *head)
{
	struct kmmio_delayed_release *dr =
		container_of(head, struct kmmio_delayed_release, rcu);
	struct kmmio_fault_page *p = dr->release_list;
	struct kmmio_fault_page **prevp = &dr->release_list;
	unsigned long flags;

	spin_lock_irqsave(&kmmio_lock, flags);
	while (p) {
		if (!p->count) {
			list_del_rcu(&p->list);
			prevp = &p->release_next;
		} else {
			*prevp = p->release_next;
		}
		p = p->release_next;
	}
	spin_unlock_irqrestore(&kmmio_lock, flags);

	/* This is the real RCU destroy call. */
	call_rcu(&dr->rcu, rcu_free_kmmio_fault_pages);
}

/*
 * Remove a kmmio probe. You have to synchronize_rcu() before you can be
 * sure that the callbacks will not be called anymore. Only after that
 * you may actually release your struct kmmio_probe.
 *
 * Unregistering a kmmio fault page has three steps:
 * 1. release_kmmio_fault_page()
 *    Disarm the page, wait a grace period to let all faults finish.
 * 2. remove_kmmio_fault_pages()
 *    Remove the pages from kmmio_page_table.
 * 3. rcu_free_kmmio_fault_pages()
 *    Actally free the kmmio_fault_page structs as with RCU.
 */
void unregister_kmmio_probe(struct kmmio_probe *p)
{
	unsigned long flags;
	unsigned long size = 0;
	const unsigned long size_lim = p->len + (p->addr & ~PAGE_MASK);
	struct kmmio_fault_page *release_list = NULL;
	struct kmmio_delayed_release *drelease;

	spin_lock_irqsave(&kmmio_lock, flags);
	while (size < size_lim) {
		release_kmmio_fault_page(p->addr + size, &release_list);
		size += PAGE_SIZE;
	}
	list_del_rcu(&p->list);
	kmmio_count--;
	spin_unlock_irqrestore(&kmmio_lock, flags);

	drelease = kmalloc(sizeof(*drelease), GFP_ATOMIC);
	if (!drelease) {
		pr_crit("kmmio: leaking kmmio_fault_page objects.\n");
		return;
	}
	drelease->release_list = release_list;

	/*
	 * This is not really RCU here. We have just disarmed a set of
	 * pages so that they cannot trigger page faults anymore. However,
	 * we cannot remove the pages from kmmio_page_table,
	 * because a probe hit might be in flight on another CPU. The
	 * pages are collected into a list, and they will be removed from
	 * kmmio_page_table when it is certain that no probe hit related to
	 * these pages can be in flight. RCU grace period sounds like a
	 * good choice.
	 *
	 * If we removed the pages too early, kmmio page fault handler might
	 * not find the respective kmmio_fault_page and determine it's not
	 * a kmmio fault, when it actually is. This would lead to madness.
	 */
	call_rcu(&drelease->rcu, remove_kmmio_fault_pages);
}
EXPORT_SYMBOL(unregister_kmmio_probe);

static int kmmio_die_notifier(struct notifier_block *nb, unsigned long val,
								void *args)
{
	struct die_args *arg = args;

	if (val == DIE_DEBUG && (arg->err & DR_STEP))
		if (post_kmmio_handler(arg->err, arg->regs) == 1)
			return NOTIFY_STOP;

	return NOTIFY_DONE;
}

static struct notifier_block nb_die = {
	.notifier_call = kmmio_die_notifier
};

static int __init init_kmmio(void)
{
	int i;
	for (i = 0; i < KMMIO_PAGE_TABLE_SIZE; i++)
		INIT_LIST_HEAD(&kmmio_page_table[i]);
	return register_die_notifier(&nb_die);
}
fs_initcall(init_kmmio); /* should be before device_initcall() */
