#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/clocksource.h>
#include <linux/time.h>
#include <linux/acpi.h>
#include <linux/cpufreq.h>
#include <linux/acpi_pmtmr.h>

#include <asm/hpet.h>
#include <asm/timex.h>
#include <asm/timer.h>

static int notsc __initdata = 0;

unsigned int cpu_khz;		/* TSC clocks / usec, not used here */
EXPORT_SYMBOL(cpu_khz);
unsigned int tsc_khz;
EXPORT_SYMBOL(tsc_khz);

/* Accelerators for sched_clock()
 * convert from cycles(64bits) => nanoseconds (64bits)
 *  basic equation:
 *		ns = cycles / (freq / ns_per_sec)
 *		ns = cycles * (ns_per_sec / freq)
 *		ns = cycles * (10^9 / (cpu_khz * 10^3))
 *		ns = cycles * (10^6 / cpu_khz)
 *
 *	Then we use scaling math (suggested by george@mvista.com) to get:
 *		ns = cycles * (10^6 * SC / cpu_khz) / SC
 *		ns = cycles * cyc2ns_scale / SC
 *
 *	And since SC is a constant power of two, we can convert the div
 *  into a shift.
 *
 *  We can use khz divisor instead of mhz to keep a better precision, since
 *  cyc2ns_scale is limited to 10^6 * 2^10, which fits in 32 bits.
 *  (mathieu.desnoyers@polymtl.ca)
 *
 *			-johnstul@us.ibm.com "math is hard, lets go shopping!"
 */
DEFINE_PER_CPU(unsigned long, cyc2ns);

static void set_cyc2ns_scale(unsigned long cpu_khz, int cpu)
{
	unsigned long flags, prev_scale, *scale;
	unsigned long long tsc_now, ns_now;

	local_irq_save(flags);
	sched_clock_idle_sleep_event();

	scale = &per_cpu(cyc2ns, cpu);

	rdtscll(tsc_now);
	ns_now = __cycles_2_ns(tsc_now);

	prev_scale = *scale;
	if (cpu_khz)
		*scale = (NSEC_PER_MSEC << CYC2NS_SCALE_FACTOR)/cpu_khz;

	sched_clock_idle_wakeup_event(0);
	local_irq_restore(flags);
}

unsigned long long native_sched_clock(void)
{
	unsigned long a = 0;

	/* Could do CPU core sync here. Opteron can execute rdtsc speculatively,
	 * which means it is not completely exact and may not be monotonous
	 * between CPUs. But the errors should be too small to matter for
	 * scheduling purposes.
	 */

	rdtscll(a);
	return cycles_2_ns(a);
}

/* We need to define a real function for sched_clock, to override the
   weak default version */
#ifdef CONFIG_PARAVIRT
unsigned long long sched_clock(void)
{
	return paravirt_sched_clock();
}
#else
unsigned long long
sched_clock(void) __attribute__((alias("native_sched_clock")));
#endif


static int tsc_unstable;

int check_tsc_unstable(void)
{
	return tsc_unstable;
}
EXPORT_SYMBOL_GPL(check_tsc_unstable);

#ifdef CONFIG_CPU_FREQ

/* Frequency scaling support. Adjust the TSC based timer when the cpu frequency
 * changes.
 *
 * RED-PEN: On SMP we assume all CPUs run with the same frequency.  It's
 * not that important because current Opteron setups do not support
 * scaling on SMP anyroads.
 *
 * Should fix up last_tsc too. Currently gettimeofday in the
 * first tick after the change will be slightly wrong.
 */

static unsigned int  ref_freq;
static unsigned long loops_per_jiffy_ref;
static unsigned long tsc_khz_ref;

static int time_cpufreq_notifier(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct cpufreq_freqs *freq = data;
	unsigned long *lpj, dummy;

	if (cpu_has(&cpu_data(freq->cpu), X86_FEATURE_CONSTANT_TSC))
		return 0;

	lpj = &dummy;
	if (!(freq->flags & CPUFREQ_CONST_LOOPS))
#ifdef CONFIG_SMP
		lpj = &cpu_data(freq->cpu).loops_per_jiffy;
#else
		lpj = &boot_cpu_data.loops_per_jiffy;
#endif

	if (!ref_freq) {
		ref_freq = freq->old;
		loops_per_jiffy_ref = *lpj;
		tsc_khz_ref = tsc_khz;
	}
	if ((val == CPUFREQ_PRECHANGE  && freq->old < freq->new) ||
		(val == CPUFREQ_POSTCHANGE && freq->old > freq->new) ||
		(val == CPUFREQ_RESUMECHANGE)) {
		*lpj =
		cpufreq_scale(loops_per_jiffy_ref, ref_freq, freq->new);

		tsc_khz = cpufreq_scale(tsc_khz_ref, ref_freq, freq->new);
		if (!(freq->flags & CPUFREQ_CONST_LOOPS))
			mark_tsc_unstable("cpufreq changes");
	}

	set_cyc2ns_scale(tsc_khz_ref, freq->cpu);

	return 0;
}

static struct notifier_block time_cpufreq_notifier_block = {
	.notifier_call  = time_cpufreq_notifier
};

static int __init cpufreq_tsc(void)
{
	cpufreq_register_notifier(&time_cpufreq_notifier_block,
				  CPUFREQ_TRANSITION_NOTIFIER);
	return 0;
}

core_initcall(cpufreq_tsc);

#endif

#define MAX_RETRIES	5
#define SMI_TRESHOLD	50000

/*
 * Read TSC and the reference counters. Take care of SMI disturbance
 */
static unsigned long __init tsc_read_refs(unsigned long *pm,
					  unsigned long *hpet)
{
	unsigned long t1, t2;
	int i;

	for (i = 0; i < MAX_RETRIES; i++) {
		t1 = get_cycles();
		if (hpet)
			*hpet = hpet_readl(HPET_COUNTER) & 0xFFFFFFFF;
		else
			*pm = acpi_pm_read_early();
		t2 = get_cycles();
		if ((t2 - t1) < SMI_TRESHOLD)
			return t2;
	}
	return ULONG_MAX;
}

/**
 * tsc_calibrate - calibrate the tsc on boot
 */
void __init tsc_calibrate(void)
{
	unsigned long flags, tsc1, tsc2, tr1, tr2, pm1, pm2, hpet1, hpet2;
	int hpet = is_hpet_enabled(), cpu;

	local_irq_save(flags);

	tsc1 = tsc_read_refs(&pm1, hpet ? &hpet1 : NULL);

	outb((inb(0x61) & ~0x02) | 0x01, 0x61);

	outb(0xb0, 0x43);
	outb((CLOCK_TICK_RATE / (1000 / 50)) & 0xff, 0x42);
	outb((CLOCK_TICK_RATE / (1000 / 50)) >> 8, 0x42);
	tr1 = get_cycles();
	while ((inb(0x61) & 0x20) == 0);
	tr2 = get_cycles();

	tsc2 = tsc_read_refs(&pm2, hpet ? &hpet2 : NULL);

	local_irq_restore(flags);

	/*
	 * Preset the result with the raw and inaccurate PIT
	 * calibration value
	 */
	tsc_khz = (tr2 - tr1) / 50;

	/* hpet or pmtimer available ? */
	if (!hpet && !pm1 && !pm2) {
		printk(KERN_INFO "TSC calibrated against PIT\n");
		goto out;
	}

	/* Check, whether the sampling was disturbed by an SMI */
	if (tsc1 == ULONG_MAX || tsc2 == ULONG_MAX) {
		printk(KERN_WARNING "TSC calibration disturbed by SMI, "
		       "using PIT calibration result\n");
		goto out;
	}

	tsc2 = (tsc2 - tsc1) * 1000000L;

	if (hpet) {
		printk(KERN_INFO "TSC calibrated against HPET\n");
		if (hpet2 < hpet1)
			hpet2 += 0x100000000;
		hpet2 -= hpet1;
		tsc1 = (hpet2 * hpet_readl(HPET_PERIOD)) / 1000000;
	} else {
		printk(KERN_INFO "TSC calibrated against PM_TIMER\n");
		if (pm2 < pm1)
			pm2 += ACPI_PM_OVRRUN;
		pm2 -= pm1;
		tsc1 = (pm2 * 1000000000) / PMTMR_TICKS_PER_SEC;
	}

	tsc_khz = tsc2 / tsc1;

out:
	for_each_possible_cpu(cpu)
		set_cyc2ns_scale(tsc_khz, cpu);
}

/*
 * Make an educated guess if the TSC is trustworthy and synchronized
 * over all CPUs.
 */
__cpuinit int unsynchronized_tsc(void)
{
	if (tsc_unstable)
		return 1;

#ifdef CONFIG_SMP
	if (apic_is_clustered_box())
		return 1;
#endif

	if (boot_cpu_has(X86_FEATURE_CONSTANT_TSC))
		return 0;

	/* Assume multi socket systems are not synchronized */
	return num_present_cpus() > 1;
}

int __init notsc_setup(char *s)
{
	notsc = 1;
	return 1;
}

__setup("notsc", notsc_setup);


/* clock source code: */
static cycle_t read_tsc(void)
{
	cycle_t ret = (cycle_t)get_cycles();
	return ret;
}

static cycle_t __vsyscall_fn vread_tsc(void)
{
	cycle_t ret = (cycle_t)vget_cycles();
	return ret;
}

static struct clocksource clocksource_tsc = {
	.name			= "tsc",
	.rating			= 300,
	.read			= read_tsc,
	.mask			= CLOCKSOURCE_MASK(64),
	.shift			= 22,
	.flags			= CLOCK_SOURCE_IS_CONTINUOUS |
				  CLOCK_SOURCE_MUST_VERIFY,
	.vread			= vread_tsc,
};

void mark_tsc_unstable(char *reason)
{
	if (!tsc_unstable) {
		tsc_unstable = 1;
		printk("Marking TSC unstable due to %s\n", reason);
		/* Change only the rating, when not registered */
		if (clocksource_tsc.mult)
			clocksource_change_rating(&clocksource_tsc, 0);
		else
			clocksource_tsc.rating = 0;
	}
}
EXPORT_SYMBOL_GPL(mark_tsc_unstable);

void __init init_tsc_clocksource(void)
{
	if (!notsc) {
		clocksource_tsc.mult = clocksource_khz2mult(tsc_khz,
							clocksource_tsc.shift);
		if (check_tsc_unstable())
			clocksource_tsc.rating = 0;

		clocksource_register(&clocksource_tsc);
	}
}
