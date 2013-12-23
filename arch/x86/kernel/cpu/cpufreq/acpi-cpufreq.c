/*
 * acpi-cpufreq.c - ACPI Processor P-States Driver ($Revision: 1.4 $)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2002 - 2004 Dominik Brodowski <linux@brodo.de>
 *  Copyright (C) 2006       Denis Sadykov <denis.m.sadykov@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/compiler.h>
#include <linux/dmi.h>
#include <linux/ftrace.h>

#include <linux/acpi.h>
#include <acpi/processor.h>

#include <asm/io.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/cpufeature.h>
#include <asm/delay.h>
#include <asm/uaccess.h>

#define dprintk(msg...) cpufreq_debug_printk(CPUFREQ_DEBUG_DRIVER, "acpi-cpufreq", msg)

MODULE_AUTHOR("Paul Diefenbaugh, Dominik Brodowski");
MODULE_DESCRIPTION("ACPI Processor P-States Driver");
MODULE_LICENSE("GPL");

enum {
	UNDEFINED_CAPABLE = 0,
	SYSTEM_INTEL_MSR_CAPABLE,
	SYSTEM_IO_CAPABLE,
};

#define INTEL_MSR_RANGE		(0xffff)
#define CPUID_6_ECX_APERFMPERF_CAPABILITY	(0x1)

struct acpi_cpufreq_data {
	struct acpi_processor_performance *acpi_data;
	struct cpufreq_frequency_table *freq_table;
	unsigned int max_freq;
	unsigned int resume;
	unsigned int cpu_feature;
};

static DEFINE_PER_CPU(struct acpi_cpufreq_data *, drv_data);

/* acpi_perf_data is a pointer to percpu data. */
static struct acpi_processor_performance *acpi_perf_data;

static struct cpufreq_driver acpi_cpufreq_driver;

static unsigned int acpi_pstate_strict;

static int check_est_cpu(unsigned int cpuid)
{
	struct cpuinfo_x86 *cpu = &cpu_data(cpuid);

	if (cpu->x86_vendor != X86_VENDOR_INTEL ||
	    !cpu_has(cpu, X86_FEATURE_EST))
		return 0;

	return 1;
}

static unsigned extract_io(u32 value, struct acpi_cpufreq_data *data)
{
	struct acpi_processor_performance *perf;
	int i;

	perf = data->acpi_data;

	for (i=0; i<perf->state_count; i++) {
		if (value == perf->states[i].status)
			return data->freq_table[i].frequency;
	}
	return 0;
}

static unsigned extract_msr(u32 msr, struct acpi_cpufreq_data *data)
{
	int i;
	struct acpi_processor_performance *perf;

	msr &= INTEL_MSR_RANGE;
	perf = data->acpi_data;

	for (i=0; data->freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (msr == perf->states[data->freq_table[i].index].status)
			return data->freq_table[i].frequency;
	}
	return data->freq_table[0].frequency;
}

static unsigned extract_freq(u32 val, struct acpi_cpufreq_data *data)
{
	switch (data->cpu_feature) {
	case SYSTEM_INTEL_MSR_CAPABLE:
		return extract_msr(val, data);
	case SYSTEM_IO_CAPABLE:
		return extract_io(val, data);
	default:
		return 0;
	}
}

struct msr_addr {
	u32 reg;
};

struct io_addr {
	u16 port;
	u8 bit_width;
};

typedef union {
	struct msr_addr msr;
	struct io_addr io;
} drv_addr_union;

struct drv_cmd {
	unsigned int type;
	const struct cpumask *mask;
	drv_addr_union addr;
	u32 val;
};

static long do_drv_read(void *_cmd)
{
	struct drv_cmd *cmd = _cmd;
	u32 h;

	switch (cmd->type) {
	case SYSTEM_INTEL_MSR_CAPABLE:
		rdmsr(cmd->addr.msr.reg, cmd->val, h);
		break;
	case SYSTEM_IO_CAPABLE:
		acpi_os_read_port((acpi_io_address)cmd->addr.io.port,
				&cmd->val,
				(u32)cmd->addr.io.bit_width);
		break;
	default:
		break;
	}
	return 0;
}

static long do_drv_write(void *_cmd)
{
	struct drv_cmd *cmd = _cmd;
	u32 lo, hi;

	switch (cmd->type) {
	case SYSTEM_INTEL_MSR_CAPABLE:
		rdmsr(cmd->addr.msr.reg, lo, hi);
		lo = (lo & ~INTEL_MSR_RANGE) | (cmd->val & INTEL_MSR_RANGE);
		wrmsr(cmd->addr.msr.reg, lo, hi);
		break;
	case SYSTEM_IO_CAPABLE:
		acpi_os_write_port((acpi_io_address)cmd->addr.io.port,
				cmd->val,
				(u32)cmd->addr.io.bit_width);
		break;
	default:
		break;
	}
	return 0;
}

static void drv_read(struct drv_cmd *cmd)
{
	cmd->val = 0;

	work_on_cpu(cpumask_any(cmd->mask), do_drv_read, cmd);
}

static void drv_write(struct drv_cmd *cmd)
{
	unsigned int i;

	for_each_cpu(i, cmd->mask) {
		work_on_cpu(i, do_drv_write, cmd);
	}
}

static u32 get_cur_val(const struct cpumask *mask)
{
	struct acpi_processor_performance *perf;
	struct drv_cmd cmd;

	if (unlikely(cpumask_empty(mask)))
		return 0;

	switch (per_cpu(drv_data, cpumask_first(mask))->cpu_feature) {
	case SYSTEM_INTEL_MSR_CAPABLE:
		cmd.type = SYSTEM_INTEL_MSR_CAPABLE;
		cmd.addr.msr.reg = MSR_IA32_PERF_STATUS;
		break;
	case SYSTEM_IO_CAPABLE:
		cmd.type = SYSTEM_IO_CAPABLE;
		perf = per_cpu(drv_data, cpumask_first(mask))->acpi_data;
		cmd.addr.io.port = perf->control_register.address;
		cmd.addr.io.bit_width = perf->control_register.bit_width;
		break;
	default:
		return 0;
	}

	cmd.mask = mask;
	drv_read(&cmd);

	dprintk("get_cur_val = %u\n", cmd.val);

	return cmd.val;
}

struct perf_cur {
	union {
		struct {
			u32 lo;
			u32 hi;
		} split;
		u64 whole;
	} aperf_cur, mperf_cur;
};


static long read_measured_perf_ctrs(void *_cur)
{
	struct perf_cur *cur = _cur;

	rdmsr(MSR_IA32_APERF, cur->aperf_cur.split.lo, cur->aperf_cur.split.hi);
	rdmsr(MSR_IA32_MPERF, cur->mperf_cur.split.lo, cur->mperf_cur.split.hi);

	wrmsr(MSR_IA32_APERF, 0, 0);
	wrmsr(MSR_IA32_MPERF, 0, 0);

	return 0;
}

/*
 * Return the measured active (C0) frequency on this CPU since last call
 * to this function.
 * Input: cpu number
 * Return: Average CPU frequency in terms of max frequency (zero on error)
 *
 * We use IA32_MPERF and IA32_APERF MSRs to get the measured performance
 * over a period of time, while CPU is in C0 state.
 * IA32_MPERF counts at the rate of max advertised frequency
 * IA32_APERF counts at the rate of actual CPU frequency
 * Only IA32_APERF/IA32_MPERF ratio is architecturally defined and
 * no meaning should be associated with absolute values of these MSRs.
 */
static unsigned int get_measured_perf(struct cpufreq_policy *policy,
				      unsigned int cpu)
{
	struct perf_cur cur;
	unsigned int perf_percent;
	unsigned int retval;

	if (!work_on_cpu(cpu, read_measured_perf_ctrs, &cur))
		return 0;

#ifdef __i386__
	/*
	 * We dont want to do 64 bit divide with 32 bit kernel
	 * Get an approximate value. Return failure in case we cannot get
	 * an approximate value.
	 */
	if (unlikely(cur.aperf_cur.split.hi || cur.mperf_cur.split.hi)) {
		int shift_count;
		u32 h;

		h = max_t(u32, cur.aperf_cur.split.hi, cur.mperf_cur.split.hi);
		shift_count = fls(h);

		cur.aperf_cur.whole >>= shift_count;
		cur.mperf_cur.whole >>= shift_count;
	}

	if (((unsigned long)(-1) / 100) < cur.aperf_cur.split.lo) {
		int shift_count = 7;
		cur.aperf_cur.split.lo >>= shift_count;
		cur.mperf_cur.split.lo >>= shift_count;
	}

	if (cur.aperf_cur.split.lo && cur.mperf_cur.split.lo)
		perf_percent = (cur.aperf_cur.split.lo * 100) /
				cur.mperf_cur.split.lo;
	else
		perf_percent = 0;

#else
	if (unlikely(((unsigned long)(-1) / 100) < cur.aperf_cur.whole)) {
		int shift_count = 7;
		cur.aperf_cur.whole >>= shift_count;
		cur.mperf_cur.whole >>= shift_count;
	}

	if (cur.aperf_cur.whole && cur.mperf_cur.whole)
		perf_percent = (cur.aperf_cur.whole * 100) /
				cur.mperf_cur.whole;
	else
		perf_percent = 0;

#endif

	retval = per_cpu(drv_data, policy->cpu)->max_freq * perf_percent / 100;

	return retval;
}

static unsigned int get_cur_freq_on_cpu(unsigned int cpu)
{
	struct acpi_cpufreq_data *data = per_cpu(drv_data, cpu);
	unsigned int freq;
	unsigned int cached_freq;

	dprintk("get_cur_freq_on_cpu (%d)\n", cpu);

	if (unlikely(data == NULL ||
		     data->acpi_data == NULL || data->freq_table == NULL)) {
		return 0;
	}

	cached_freq = data->freq_table[data->acpi_data->state].frequency;
	freq = extract_freq(get_cur_val(cpumask_of(cpu)), data);
	if (freq != cached_freq) {
		/*
		 * The dreaded BIOS frequency change behind our back.
		 * Force set the frequency on next target call.
		 */
		data->resume = 1;
	}

	dprintk("cur freq = %u\n", freq);

	return freq;
}

static unsigned int check_freqs(const struct cpumask *mask, unsigned int freq,
				struct acpi_cpufreq_data *data)
{
	unsigned int cur_freq;
	unsigned int i;

	for (i=0; i<100; i++) {
		cur_freq = extract_freq(get_cur_val(mask), data);
		if (cur_freq == freq)
			return 1;
		udelay(10);
	}
	return 0;
}

static int acpi_cpufreq_target(struct cpufreq_policy *policy,
			       unsigned int target_freq, unsigned int relation)
{
	struct acpi_cpufreq_data *data = per_cpu(drv_data, policy->cpu);
	struct acpi_processor_performance *perf;
	struct cpufreq_freqs freqs;
	struct drv_cmd cmd;
	unsigned int next_state = 0; /* Index into freq_table */
	unsigned int next_perf_state = 0; /* Index into perf table */
	unsigned int i;
	int result = 0;
	struct power_trace it;

	dprintk("acpi_cpufreq_target %d (%d)\n", target_freq, policy->cpu);

	if (unlikely(data == NULL ||
	     data->acpi_data == NULL || data->freq_table == NULL)) {
		return -ENODEV;
	}

	perf = data->acpi_data;
	result = cpufreq_frequency_table_target(policy,
						data->freq_table,
						target_freq,
						relation, &next_state);
	if (unlikely(result)) {
		result = -ENODEV;
		goto out;
	}

	next_perf_state = data->freq_table[next_state].index;
	if (perf->state == next_perf_state) {
		if (unlikely(data->resume)) {
			dprintk("Called after resume, resetting to P%d\n",
				next_perf_state);
			data->resume = 0;
		} else {
			dprintk("Already at target state (P%d)\n",
				next_perf_state);
			goto out;
		}
	}

	trace_power_mark(&it, POWER_PSTATE, next_perf_state);

	switch (data->cpu_feature) {
	case SYSTEM_INTEL_MSR_CAPABLE:
		cmd.type = SYSTEM_INTEL_MSR_CAPABLE;
		cmd.addr.msr.reg = MSR_IA32_PERF_CTL;
		cmd.val = (u32) perf->states[next_perf_state].control;
		break;
	case SYSTEM_IO_CAPABLE:
		cmd.type = SYSTEM_IO_CAPABLE;
		cmd.addr.io.port = perf->control_register.address;
		cmd.addr.io.bit_width = perf->control_register.bit_width;
		cmd.val = (u32) perf->states[next_perf_state].control;
		break;
	default:
		result = -ENODEV;
		goto out;
	}

	/* cpufreq holds the hotplug lock, so we are safe from here on */
	if (policy->shared_type != CPUFREQ_SHARED_TYPE_ANY)
		cmd.mask = policy->cpus;
	else
		cmd.mask = cpumask_of(policy->cpu);

	freqs.old = perf->states[perf->state].core_frequency * 1000;
	freqs.new = data->freq_table[next_state].frequency;
	for_each_cpu(i, cmd.mask) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	}

	drv_write(&cmd);

	if (acpi_pstate_strict) {
		if (!check_freqs(cmd.mask, freqs.new, data)) {
			dprintk("acpi_cpufreq_target failed (%d)\n",
				policy->cpu);
			result = -EAGAIN;
			goto out;
		}
	}

	for_each_cpu(i, cmd.mask) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}
	perf->state = next_perf_state;

out:
	return result;
}

static int acpi_cpufreq_verify(struct cpufreq_policy *policy)
{
	struct acpi_cpufreq_data *data = per_cpu(drv_data, policy->cpu);

	dprintk("acpi_cpufreq_verify\n");

	return cpufreq_frequency_table_verify(policy, data->freq_table);
}

static unsigned long
acpi_cpufreq_guess_freq(struct acpi_cpufreq_data *data, unsigned int cpu)
{
	struct acpi_processor_performance *perf = data->acpi_data;

	if (cpu_khz) {
		/* search the closest match to cpu_khz */
		unsigned int i;
		unsigned long freq;
		unsigned long freqn = perf->states[0].core_frequency * 1000;

		for (i=0; i<(perf->state_count-1); i++) {
			freq = freqn;
			freqn = perf->states[i+1].core_frequency * 1000;
			if ((2 * cpu_khz) > (freqn + freq)) {
				perf->state = i;
				return freq;
			}
		}
		perf->state = perf->state_count-1;
		return freqn;
	} else {
		/* assume CPU is at P0... */
		perf->state = 0;
		return perf->states[0].core_frequency * 1000;
	}
}

static void free_acpi_perf_data(void)
{
	unsigned int i;

	/* Freeing a NULL pointer is OK, and alloc_percpu zeroes. */
	for_each_possible_cpu(i)
		free_cpumask_var(per_cpu_ptr(acpi_perf_data, i)
				 ->shared_cpu_map);
	free_percpu(acpi_perf_data);
}

/*
 * acpi_cpufreq_early_init - initialize ACPI P-States library
 *
 * Initialize the ACPI P-States library (drivers/acpi/processor_perflib.c)
 * in order to determine correct frequency and voltage pairings. We can
 * do _PDC and _PSD and find out the processor dependency for the
 * actual init that will happen later...
 */
static int __init acpi_cpufreq_early_init(void)
{
	unsigned int i;
	dprintk("acpi_cpufreq_early_init\n");

	acpi_perf_data = alloc_percpu(struct acpi_processor_performance);
	if (!acpi_perf_data) {
		dprintk("Memory allocation error for acpi_perf_data.\n");
		return -ENOMEM;
	}
	for_each_possible_cpu(i) {
		if (!alloc_cpumask_var_node(
			&per_cpu_ptr(acpi_perf_data, i)->shared_cpu_map,
			GFP_KERNEL, cpu_to_node(i))) {

			/* Freeing a NULL pointer is OK: alloc_percpu zeroes. */
			free_acpi_perf_data();
			return -ENOMEM;
		}
	}

	/* Do initialization in ACPI core */
	acpi_processor_preregister_performance(acpi_perf_data);
	return 0;
}

#ifdef CONFIG_SMP
/*
 * Some BIOSes do SW_ANY coordination internally, either set it up in hw
 * or do it in BIOS firmware and won't inform about it to OS. If not
 * detected, this has a side effect of making CPU run at a different speed
 * than OS intended it to run at. Detect it and handle it cleanly.
 */
static int bios_with_sw_any_bug;

static int sw_any_bug_found(const struct dmi_system_id *d)
{
	bios_with_sw_any_bug = 1;
	return 0;
}

static const struct dmi_system_id sw_any_bug_dmi_table[] = {
	{
		.callback = sw_any_bug_found,
		.ident = "Supermicro Server X6DLP",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Supermicro"),
			DMI_MATCH(DMI_BIOS_VERSION, "080010"),
			DMI_MATCH(DMI_PRODUCT_NAME, "X6DLP"),
		},
	},
	{ }
};
#endif

static int acpi_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	unsigned int i;
	unsigned int valid_states = 0;
	unsigned int cpu = policy->cpu;
	struct acpi_cpufreq_data *data;
	unsigned int result = 0;
	struct cpuinfo_x86 *c = &cpu_data(policy->cpu);
	struct acpi_processor_performance *perf;

	dprintk("acpi_cpufreq_cpu_init\n");

	data = kzalloc(sizeof(struct acpi_cpufreq_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->acpi_data = percpu_ptr(acpi_perf_data, cpu);
	per_cpu(drv_data, cpu) = data;

	if (cpu_has(c, X86_FEATURE_CONSTANT_TSC))
		acpi_cpufreq_driver.flags |= CPUFREQ_CONST_LOOPS;

	result = acpi_processor_register_performance(data->acpi_data, cpu);
	if (result)
		goto err_free;

	perf = data->acpi_data;
	policy->shared_type = perf->shared_type;

	/*
	 * Will let policy->cpus know about dependency only when software
	 * coordination is required.
	 */
	if (policy->shared_type == CPUFREQ_SHARED_TYPE_ALL ||
	    policy->shared_type == CPUFREQ_SHARED_TYPE_ANY) {
		cpumask_copy(policy->cpus, perf->shared_cpu_map);
	}
	cpumask_copy(policy->related_cpus, perf->shared_cpu_map);

#ifdef CONFIG_SMP
	dmi_check_system(sw_any_bug_dmi_table);
	if (bios_with_sw_any_bug && cpumask_weight(policy->cpus) == 1) {
		policy->shared_type = CPUFREQ_SHARED_TYPE_ALL;
		cpumask_copy(policy->cpus, cpu_core_mask(cpu));
	}
#endif

	/* capability check */
	if (perf->state_count <= 1) {
		dprintk("No P-States\n");
		result = -ENODEV;
		goto err_unreg;
	}

	if (perf->control_register.space_id != perf->status_register.space_id) {
		result = -ENODEV;
		goto err_unreg;
	}

	switch (perf->control_register.space_id) {
	case ACPI_ADR_SPACE_SYSTEM_IO:
		dprintk("SYSTEM IO addr space\n");
		data->cpu_feature = SYSTEM_IO_CAPABLE;
		break;
	case ACPI_ADR_SPACE_FIXED_HARDWARE:
		dprintk("HARDWARE addr space\n");
		if (!check_est_cpu(cpu)) {
			result = -ENODEV;
			goto err_unreg;
		}
		data->cpu_feature = SYSTEM_INTEL_MSR_CAPABLE;
		break;
	default:
		dprintk("Unknown addr space %d\n",
			(u32) (perf->control_register.space_id));
		result = -ENODEV;
		goto err_unreg;
	}

	data->freq_table = kmalloc(sizeof(struct cpufreq_frequency_table) *
		    (perf->state_count+1), GFP_KERNEL);
	if (!data->freq_table) {
		result = -ENOMEM;
		goto err_unreg;
	}

	/* detect transition latency */
	policy->cpuinfo.transition_latency = 0;
	for (i=0; i<perf->state_count; i++) {
		if ((perf->states[i].transition_latency * 1000) >
		    policy->cpuinfo.transition_latency)
			policy->cpuinfo.transition_latency =
			    perf->states[i].transition_latency * 1000;
	}

	/* Check for high latency (>20uS) from buggy BIOSes, like on T42 */
	if (perf->control_register.space_id == ACPI_ADR_SPACE_FIXED_HARDWARE &&
	    policy->cpuinfo.transition_latency > 20 * 1000) {
		static int print_once;
		policy->cpuinfo.transition_latency = 20 * 1000;
		if (!print_once) {
			print_once = 1;
			printk(KERN_INFO "Capping off P-state tranision latency"
				" at 20 uS\n");
		}
	}

	data->max_freq = perf->states[0].core_frequency * 1000;
	/* table init */
	for (i=0; i<perf->state_count; i++) {
		if (i>0 && perf->states[i].core_frequency >=
		    data->freq_table[valid_states-1].frequency / 1000)
			continue;

		data->freq_table[valid_states].index = i;
		data->freq_table[valid_states].frequency =
		    perf->states[i].core_frequency * 1000;
		valid_states++;
	}
	data->freq_table[valid_states].frequency = CPUFREQ_TABLE_END;
	perf->state = 0;

	result = cpufreq_frequency_table_cpuinfo(policy, data->freq_table);
	if (result)
		goto err_freqfree;

	switch (perf->control_register.space_id) {
	case ACPI_ADR_SPACE_SYSTEM_IO:
		/* Current speed is unknown and not detectable by IO port */
		policy->cur = acpi_cpufreq_guess_freq(data, policy->cpu);
		break;
	case ACPI_ADR_SPACE_FIXED_HARDWARE:
		acpi_cpufreq_driver.get = get_cur_freq_on_cpu;
		policy->cur = get_cur_freq_on_cpu(cpu);
		break;
	default:
		break;
	}

	/* notify BIOS that we exist */
	acpi_processor_notify_smm(THIS_MODULE);

	/* Check for APERF/MPERF support in hardware */
	if (c->x86_vendor == X86_VENDOR_INTEL && c->cpuid_level >= 6) {
		unsigned int ecx;
		ecx = cpuid_ecx(6);
		if (ecx & CPUID_6_ECX_APERFMPERF_CAPABILITY)
			acpi_cpufreq_driver.getavg = get_measured_perf;
	}

	dprintk("CPU%u - ACPI performance management activated.\n", cpu);
	for (i = 0; i < perf->state_count; i++)
		dprintk("     %cP%d: %d MHz, %d mW, %d uS\n",
			(i == perf->state ? '*' : ' '), i,
			(u32) perf->states[i].core_frequency,
			(u32) perf->states[i].power,
			(u32) perf->states[i].transition_latency);

	cpufreq_frequency_table_get_attr(data->freq_table, policy->cpu);

	/*
	 * the first call to ->target() should result in us actually
	 * writing something to the appropriate registers.
	 */
	data->resume = 1;

	return result;

err_freqfree:
	kfree(data->freq_table);
err_unreg:
	acpi_processor_unregister_performance(perf, cpu);
err_free:
	kfree(data);
	per_cpu(drv_data, cpu) = NULL;

	return result;
}

static int acpi_cpufreq_cpu_exit(struct cpufreq_policy *policy)
{
	struct acpi_cpufreq_data *data = per_cpu(drv_data, policy->cpu);

	dprintk("acpi_cpufreq_cpu_exit\n");

	if (data) {
		cpufreq_frequency_table_put_attr(policy->cpu);
		per_cpu(drv_data, policy->cpu) = NULL;
		acpi_processor_unregister_performance(data->acpi_data,
						      policy->cpu);
		kfree(data);
	}

	return 0;
}

static int acpi_cpufreq_resume(struct cpufreq_policy *policy)
{
	struct acpi_cpufreq_data *data = per_cpu(drv_data, policy->cpu);

	dprintk("acpi_cpufreq_resume\n");

	data->resume = 1;

	return 0;
}

static struct freq_attr *acpi_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver acpi_cpufreq_driver = {
	.verify = acpi_cpufreq_verify,
	.target = acpi_cpufreq_target,
	.init = acpi_cpufreq_cpu_init,
	.exit = acpi_cpufreq_cpu_exit,
	.resume = acpi_cpufreq_resume,
	.name = "acpi-cpufreq",
	.owner = THIS_MODULE,
	.attr = acpi_cpufreq_attr,
};

static int __init acpi_cpufreq_init(void)
{
	int ret;

	if (acpi_disabled)
		return 0;

	dprintk("acpi_cpufreq_init\n");

	ret = acpi_cpufreq_early_init();
	if (ret)
		return ret;

	ret = cpufreq_register_driver(&acpi_cpufreq_driver);
	if (ret)
		free_acpi_perf_data();

	return ret;
}

static void __exit acpi_cpufreq_exit(void)
{
	dprintk("acpi_cpufreq_exit\n");

	cpufreq_unregister_driver(&acpi_cpufreq_driver);

	free_percpu(acpi_perf_data);
}

module_param(acpi_pstate_strict, uint, 0644);
MODULE_PARM_DESC(acpi_pstate_strict,
	"value 0 or non-zero. non-zero -> strict ACPI checks are "
	"performed during frequency changes.");

late_initcall(acpi_cpufreq_init);
module_exit(acpi_cpufreq_exit);

MODULE_ALIAS("acpi");
