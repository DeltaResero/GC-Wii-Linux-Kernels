/*
 * 
 * Common boot and setup code.
 *
 * Copyright (C) 2001 PPC64 Team, IBM Corp
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#undef DEBUG

#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/ide.h>
#include <linux/seq_file.h>
#include <linux/ioport.h>
#include <linux/console.h>
#include <linux/utsname.h>
#include <linux/tty.h>
#include <linux/root_dev.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/unistd.h>
#include <linux/serial.h>
#include <linux/serial_8250.h>
#include <linux/bootmem.h>
#include <asm/io.h>
#include <asm/kdump.h>
#include <asm/prom.h>
#include <asm/processor.h>
#include <asm/pgtable.h>
#include <asm/smp.h>
#include <asm/elf.h>
#include <asm/machdep.h>
#include <asm/paca.h>
#include <asm/time.h>
#include <asm/cputable.h>
#include <asm/sections.h>
#include <asm/btext.h>
#include <asm/nvram.h>
#include <asm/setup.h>
#include <asm/system.h>
#include <asm/rtas.h>
#include <asm/iommu.h>
#include <asm/serial.h>
#include <asm/cache.h>
#include <asm/page.h>
#include <asm/mmu.h>
#include <asm/lmb.h>
#include <asm/iseries/it_lp_naca.h>
#include <asm/firmware.h>
#include <asm/xmon.h>
#include <asm/udbg.h>
#include <asm/kexec.h>

#include "setup.h"

#ifdef DEBUG
#define DBG(fmt...) udbg_printf(fmt)
#else
#define DBG(fmt...)
#endif

int have_of = 1;
int boot_cpuid = 0;
int boot_cpuid_phys = 0;
dev_t boot_dev;
u64 ppc64_pft_size;

/* Pick defaults since we might want to patch instructions
 * before we've read this from the device tree.
 */
struct ppc64_caches ppc64_caches = {
	.dline_size = 0x80,
	.log_dline_size = 7,
	.iline_size = 0x80,
	.log_iline_size = 7
};
EXPORT_SYMBOL_GPL(ppc64_caches);

/*
 * These are used in binfmt_elf.c to put aux entries on the stack
 * for each elf executable being started.
 */
int dcache_bsize;
int icache_bsize;
int ucache_bsize;

/* The main machine-dep calls structure
 */
struct machdep_calls ppc_md;
EXPORT_SYMBOL(ppc_md);

#ifdef CONFIG_MAGIC_SYSRQ
unsigned long SYSRQ_KEY;
#endif /* CONFIG_MAGIC_SYSRQ */


static int ppc64_panic_event(struct notifier_block *, unsigned long, void *);
static struct notifier_block ppc64_panic_block = {
	.notifier_call = ppc64_panic_event,
	.priority = INT_MIN /* may not return; must be done last */
};

#ifdef CONFIG_SMP

static int smt_enabled_cmdline;

/* Look for ibm,smt-enabled OF option */
static void check_smt_enabled(void)
{
	struct device_node *dn;
	char *smt_option;

	/* Allow the command line to overrule the OF option */
	if (smt_enabled_cmdline)
		return;

	dn = of_find_node_by_path("/options");

	if (dn) {
		smt_option = (char *)get_property(dn, "ibm,smt-enabled", NULL);

                if (smt_option) {
			if (!strcmp(smt_option, "on"))
				smt_enabled_at_boot = 1;
			else if (!strcmp(smt_option, "off"))
				smt_enabled_at_boot = 0;
                }
        }
}

/* Look for smt-enabled= cmdline option */
static int __init early_smt_enabled(char *p)
{
	smt_enabled_cmdline = 1;

	if (!p)
		return 0;

	if (!strcmp(p, "on") || !strcmp(p, "1"))
		smt_enabled_at_boot = 1;
	else if (!strcmp(p, "off") || !strcmp(p, "0"))
		smt_enabled_at_boot = 0;

	return 0;
}
early_param("smt-enabled", early_smt_enabled);

#else
#define check_smt_enabled()
#endif /* CONFIG_SMP */

extern struct machdep_calls pSeries_md;
extern struct machdep_calls pmac_md;
extern struct machdep_calls maple_md;
extern struct machdep_calls cell_md;
extern struct machdep_calls iseries_md;

/* Ultimately, stuff them in an elf section like initcalls... */
static struct machdep_calls __initdata *machines[] = {
#ifdef CONFIG_PPC_PSERIES
	&pSeries_md,
#endif /* CONFIG_PPC_PSERIES */
#ifdef CONFIG_PPC_PMAC
	&pmac_md,
#endif /* CONFIG_PPC_PMAC */
#ifdef CONFIG_PPC_MAPLE
	&maple_md,
#endif /* CONFIG_PPC_MAPLE */
#ifdef CONFIG_PPC_CELL
	&cell_md,
#endif
#ifdef CONFIG_PPC_ISERIES
	&iseries_md,
#endif
	NULL
};

/*
 * Early initialization entry point. This is called by head.S
 * with MMU translation disabled. We rely on the "feature" of
 * the CPU that ignores the top 2 bits of the address in real
 * mode so we can access kernel globals normally provided we
 * only toy with things in the RMO region. From here, we do
 * some early parsing of the device-tree to setup out LMB
 * data structures, and allocate & initialize the hash table
 * and segment tables so we can start running with translation
 * enabled.
 *
 * It is this function which will call the probe() callback of
 * the various platform types and copy the matching one to the
 * global ppc_md structure. Your platform can eventually do
 * some very early initializations from the probe() routine, but
 * this is not recommended, be very careful as, for example, the
 * device-tree is not accessible via normal means at this point.
 */

void __init early_setup(unsigned long dt_ptr)
{
	struct paca_struct *lpaca = get_paca();
	static struct machdep_calls **mach;

	/* Enable early debugging if any specified (see udbg.h) */
	udbg_early_init();

	DBG(" -> early_setup()\n");

	/*
	 * Do early initializations using the flattened device
	 * tree, like retreiving the physical memory map or
	 * calculating/retreiving the hash table size
	 */
	early_init_devtree(__va(dt_ptr));

	/*
	 * Iterate all ppc_md structures until we find the proper
	 * one for the current machine type
	 */
	DBG("Probing machine type for platform %x...\n", _machine);

	for (mach = machines; *mach; mach++) {
		if ((*mach)->probe(_machine))
			break;
	}
	/* What can we do if we didn't find ? */
	if (*mach == NULL) {
		DBG("No suitable machine found !\n");
		for (;;);
	}
	ppc_md = **mach;

#ifdef CONFIG_CRASH_DUMP
	kdump_setup();
#endif

	DBG("Found, Initializing memory management...\n");

	/*
	 * Initialize the MMU Hash table and create the linear mapping
	 * of memory. Has to be done before stab/slb initialization as
	 * this is currently where the page size encoding is obtained
	 */
	htab_initialize();

	/*
	 * Initialize stab / SLB management except on iSeries
	 */
	if (cpu_has_feature(CPU_FTR_SLB))
		slb_initialize();
	else if (!firmware_has_feature(FW_FEATURE_ISERIES))
		stab_initialize(lpaca->stab_real);

	DBG(" <- early_setup()\n");
}

#ifdef CONFIG_SMP
void early_setup_secondary(void)
{
	struct paca_struct *lpaca = get_paca();

	/* Mark enabled in PACA */
	lpaca->proc_enabled = 0;

	/* Initialize hash table for that CPU */
	htab_initialize_secondary();

	/* Initialize STAB/SLB. We use a virtual address as it works
	 * in real mode on pSeries and we want a virutal address on
	 * iSeries anyway
	 */
	if (cpu_has_feature(CPU_FTR_SLB))
		slb_initialize();
	else
		stab_initialize(lpaca->stab_addr);
}

#endif /* CONFIG_SMP */

#if defined(CONFIG_SMP) || defined(CONFIG_KEXEC)
void smp_release_cpus(void)
{
	extern unsigned long __secondary_hold_spinloop;
	unsigned long *ptr;

	DBG(" -> smp_release_cpus()\n");

	/* All secondary cpus are spinning on a common spinloop, release them
	 * all now so they can start to spin on their individual paca
	 * spinloops. For non SMP kernels, the secondary cpus never get out
	 * of the common spinloop.
	 * This is useless but harmless on iSeries, secondaries are already
	 * waiting on their paca spinloops. */

	ptr  = (unsigned long *)((unsigned long)&__secondary_hold_spinloop
			- PHYSICAL_START);
	*ptr = 1;
	mb();

	DBG(" <- smp_release_cpus()\n");
}
#endif /* CONFIG_SMP || CONFIG_KEXEC */

/*
 * Initialize some remaining members of the ppc64_caches and systemcfg
 * structures
 * (at least until we get rid of them completely). This is mostly some
 * cache informations about the CPU that will be used by cache flush
 * routines and/or provided to userland
 */
static void __init initialize_cache_info(void)
{
	struct device_node *np;
	unsigned long num_cpus = 0;

	DBG(" -> initialize_cache_info()\n");

	for (np = NULL; (np = of_find_node_by_type(np, "cpu"));) {
		num_cpus += 1;

		/* We're assuming *all* of the CPUs have the same
		 * d-cache and i-cache sizes... -Peter
		 */

		if ( num_cpus == 1 ) {
			u32 *sizep, *lsizep;
			u32 size, lsize;
			const char *dc, *ic;

			/* Then read cache informations */
			if (_machine == PLATFORM_POWERMAC) {
				dc = "d-cache-block-size";
				ic = "i-cache-block-size";
			} else {
				dc = "d-cache-line-size";
				ic = "i-cache-line-size";
			}

			size = 0;
			lsize = cur_cpu_spec->dcache_bsize;
			sizep = (u32 *)get_property(np, "d-cache-size", NULL);
			if (sizep != NULL)
				size = *sizep;
			lsizep = (u32 *) get_property(np, dc, NULL);
			if (lsizep != NULL)
				lsize = *lsizep;
			if (sizep == 0 || lsizep == 0)
				DBG("Argh, can't find dcache properties ! "
				    "sizep: %p, lsizep: %p\n", sizep, lsizep);

			ppc64_caches.dsize = size;
			ppc64_caches.dline_size = lsize;
			ppc64_caches.log_dline_size = __ilog2(lsize);
			ppc64_caches.dlines_per_page = PAGE_SIZE / lsize;

			size = 0;
			lsize = cur_cpu_spec->icache_bsize;
			sizep = (u32 *)get_property(np, "i-cache-size", NULL);
			if (sizep != NULL)
				size = *sizep;
			lsizep = (u32 *)get_property(np, ic, NULL);
			if (lsizep != NULL)
				lsize = *lsizep;
			if (sizep == 0 || lsizep == 0)
				DBG("Argh, can't find icache properties ! "
				    "sizep: %p, lsizep: %p\n", sizep, lsizep);

			ppc64_caches.isize = size;
			ppc64_caches.iline_size = lsize;
			ppc64_caches.log_iline_size = __ilog2(lsize);
			ppc64_caches.ilines_per_page = PAGE_SIZE / lsize;
		}
	}

	DBG(" <- initialize_cache_info()\n");
}


/*
 * Do some initial setup of the system.  The parameters are those which 
 * were passed in from the bootloader.
 */
void __init setup_system(void)
{
	DBG(" -> setup_system()\n");

#ifdef CONFIG_KEXEC
	kdump_move_device_tree();
#endif
	/*
	 * Unflatten the device-tree passed by prom_init or kexec
	 */
	unflatten_device_tree();

#ifdef CONFIG_KEXEC
	kexec_setup();	/* requires unflattened device tree. */
#endif

	/*
	 * Fill the ppc64_caches & systemcfg structures with informations
	 * retrieved from the device-tree. Need to be called before
	 * finish_device_tree() since the later requires some of the
	 * informations filled up here to properly parse the interrupt
	 * tree.
	 * It also sets up the cache line sizes which allows to call
	 * routines like flush_icache_range (used by the hash init
	 * later on).
	 */
	initialize_cache_info();

#ifdef CONFIG_PPC_RTAS
	/*
	 * Initialize RTAS if available
	 */
	rtas_initialize();
#endif /* CONFIG_PPC_RTAS */

	/*
	 * Check if we have an initrd provided via the device-tree
	 */
	check_for_initrd();

	/*
	 * Do some platform specific early initializations, that includes
	 * setting up the hash table pointers. It also sets up some interrupt-mapping
	 * related options that will be used by finish_device_tree()
	 */
	ppc_md.init_early();

 	/*
	 * We can discover serial ports now since the above did setup the
	 * hash table management for us, thus ioremap works. We do that early
	 * so that further code can be debugged
	 */
	find_legacy_serial_ports();

	/*
	 * "Finish" the device-tree, that is do the actual parsing of
	 * some of the properties like the interrupt map
	 */
	finish_device_tree();

	/*
	 * Initialize xmon
	 */
#ifdef CONFIG_XMON_DEFAULT
	xmon_init(1);
#endif
	/*
	 * Register early console
	 */
	register_early_udbg_console();

	/* Save unparsed command line copy for /proc/cmdline */
	strlcpy(saved_command_line, cmd_line, COMMAND_LINE_SIZE);

	parse_early_param();

	check_smt_enabled();
	smp_setup_cpu_maps();

#ifdef CONFIG_SMP
	/* Release secondary cpus out of their spinloops at 0x60 now that
	 * we can map physical -> logical CPU ids
	 */
	smp_release_cpus();
#endif

	printk("Starting Linux PPC64 %s\n", system_utsname.version);

	printk("-----------------------------------------------------\n");
	printk("ppc64_pft_size                = 0x%lx\n", ppc64_pft_size);
	printk("ppc64_interrupt_controller    = 0x%ld\n",
	       ppc64_interrupt_controller);
	printk("platform                      = 0x%x\n", _machine);
	printk("physicalMemorySize            = 0x%lx\n", lmb_phys_mem_size());
	printk("ppc64_caches.dcache_line_size = 0x%x\n",
	       ppc64_caches.dline_size);
	printk("ppc64_caches.icache_line_size = 0x%x\n",
	       ppc64_caches.iline_size);
	printk("htab_address                  = 0x%p\n", htab_address);
	printk("htab_hash_mask                = 0x%lx\n", htab_hash_mask);
#if PHYSICAL_START > 0
	printk("physical_start                = 0x%x\n", PHYSICAL_START);
#endif
	printk("-----------------------------------------------------\n");

	mm_init_ppc64();

	DBG(" <- setup_system()\n");
}

static int ppc64_panic_event(struct notifier_block *this,
                             unsigned long event, void *ptr)
{
	ppc_md.panic((char *)ptr);  /* May not return */
	return NOTIFY_DONE;
}

#ifdef CONFIG_IRQSTACKS
static void __init irqstack_early_init(void)
{
	unsigned int i;

	/*
	 * interrupt stacks must be under 256MB, we cannot afford to take
	 * SLB misses on them.
	 */
	for_each_cpu(i) {
		softirq_ctx[i] = (struct thread_info *)
			__va(lmb_alloc_base(THREAD_SIZE,
					    THREAD_SIZE, 0x10000000));
		hardirq_ctx[i] = (struct thread_info *)
			__va(lmb_alloc_base(THREAD_SIZE,
					    THREAD_SIZE, 0x10000000));
	}
}
#else
#define irqstack_early_init()
#endif

/*
 * Stack space used when we detect a bad kernel stack pointer, and
 * early in SMP boots before relocation is enabled.
 */
static void __init emergency_stack_init(void)
{
	unsigned long limit;
	unsigned int i;

	/*
	 * Emergency stacks must be under 256MB, we cannot afford to take
	 * SLB misses on them. The ABI also requires them to be 128-byte
	 * aligned.
	 *
	 * Since we use these as temporary stacks during secondary CPU
	 * bringup, we need to get at them in real mode. This means they
	 * must also be within the RMO region.
	 */
	limit = min(0x10000000UL, lmb.rmo_size);

	for_each_cpu(i)
		paca[i].emergency_sp =
		__va(lmb_alloc_base(HW_PAGE_SIZE, 128, limit)) + HW_PAGE_SIZE;
}

/*
 * Called into from start_kernel, after lock_kernel has been called.
 * Initializes bootmem, which is unsed to manage page allocation until
 * mem_init is called.
 */
void __init setup_arch(char **cmdline_p)
{
	extern void do_init_bootmem(void);

	ppc64_boot_msg(0x12, "Setup Arch");

	*cmdline_p = cmd_line;

	/*
	 * Set cache line size based on type of cpu as a default.
	 * Systems with OF can look in the properties on the cpu node(s)
	 * for a possibly more accurate value.
	 */
	dcache_bsize = ppc64_caches.dline_size;
	icache_bsize = ppc64_caches.iline_size;

	/* reboot on panic */
	panic_timeout = 180;

	if (ppc_md.panic)
		notifier_chain_register(&panic_notifier_list, &ppc64_panic_block);

	init_mm.start_code = PAGE_OFFSET;
	init_mm.end_code = (unsigned long) _etext;
	init_mm.end_data = (unsigned long) _edata;
	init_mm.brk = klimit;
	
	irqstack_early_init();
	emergency_stack_init();

	stabs_alloc();

	/* set up the bootmem stuff with available memory */
	do_init_bootmem();
	sparse_init();

#ifdef CONFIG_DUMMY_CONSOLE
	conswitchp = &dummy_con;
#endif

	ppc_md.setup_arch();

	/* Use the default idle loop if the platform hasn't provided one. */
	if (NULL == ppc_md.idle_loop) {
		ppc_md.idle_loop = default_idle;
		printk(KERN_INFO "Using default idle loop\n");
	}

	paging_init();
	ppc64_boot_msg(0x15, "Setup Done");
}


/* ToDo: do something useful if ppc_md is not yet setup. */
#define PPC64_LINUX_FUNCTION 0x0f000000
#define PPC64_IPL_MESSAGE 0xc0000000
#define PPC64_TERM_MESSAGE 0xb0000000

static void ppc64_do_msg(unsigned int src, const char *msg)
{
	if (ppc_md.progress) {
		char buf[128];

		sprintf(buf, "%08X\n", src);
		ppc_md.progress(buf, 0);
		snprintf(buf, 128, "%s", msg);
		ppc_md.progress(buf, 0);
	}
}

/* Print a boot progress message. */
void ppc64_boot_msg(unsigned int src, const char *msg)
{
	ppc64_do_msg(PPC64_LINUX_FUNCTION|PPC64_IPL_MESSAGE|src, msg);
	printk("[boot]%04x %s\n", src, msg);
}

/* Print a termination message (print only -- does not stop the kernel) */
void ppc64_terminate_msg(unsigned int src, const char *msg)
{
	ppc64_do_msg(PPC64_LINUX_FUNCTION|PPC64_TERM_MESSAGE|src, msg);
	printk("[terminate]%04x %s\n", src, msg);
}

int check_legacy_ioport(unsigned long base_port)
{
	if (ppc_md.check_legacy_ioport == NULL)
		return 0;
	return ppc_md.check_legacy_ioport(base_port);
}
EXPORT_SYMBOL(check_legacy_ioport);

void cpu_die(void)
{
	if (ppc_md.cpu_die)
		ppc_md.cpu_die();
}

#ifdef CONFIG_SMP
void __init setup_per_cpu_areas(void)
{
	int i;
	unsigned long size;
	char *ptr;

	/* Copy section for each CPU (we discard the original) */
	size = ALIGN(__per_cpu_end - __per_cpu_start, SMP_CACHE_BYTES);
#ifdef CONFIG_MODULES
	if (size < PERCPU_ENOUGH_ROOM)
		size = PERCPU_ENOUGH_ROOM;
#endif

	for_each_cpu(i) {
		ptr = alloc_bootmem_node(NODE_DATA(cpu_to_node(i)), size);
		if (!ptr)
			panic("Cannot allocate cpu data for CPU %d\n", i);

		paca[i].data_offset = ptr - __per_cpu_start;
		memcpy(ptr, __per_cpu_start, __per_cpu_end - __per_cpu_start);
	}
}
#endif
