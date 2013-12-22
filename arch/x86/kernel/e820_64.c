/*
 * Handle the memory map.
 * The functions here do the job until bootmem takes over.
 *
 *  Getting sanitize_e820_map() in sync with i386 version by applying change:
 *  -  Provisions for empty E820 memory regions (reported by certain BIOSes).
 *     Alex Achenbach <xela@slit.de>, December 2002.
 *  Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/ioport.h>
#include <linux/string.h>
#include <linux/kexec.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/suspend.h>
#include <linux/pfn.h>

#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/e820.h>
#include <asm/proto.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/kdebug.h>
#include <asm/trampoline.h>

struct e820map e820;

/*
 * PFN of last memory page.
 */
unsigned long end_pfn;

/*
 * end_pfn only includes RAM, while max_pfn_mapped includes all e820 entries.
 * The direct mapping extends to max_pfn_mapped, so that we can directly access
 * apertures, ACPI and other tables without having to play with fixmaps.
 */
unsigned long max_pfn_mapped;

/*
 * Last pfn which the user wants to use.
 */
static unsigned long __initdata end_user_pfn = MAXMEM>>PAGE_SHIFT;

/*
 * Early reserved memory areas.
 */
#define MAX_EARLY_RES 20

struct early_res {
	unsigned long start, end;
	char name[16];
};
static struct early_res early_res[MAX_EARLY_RES] __initdata = {
	{ 0, PAGE_SIZE, "BIOS data page" },			/* BIOS data page */
#ifdef CONFIG_X86_TRAMPOLINE
	{ TRAMPOLINE_BASE, TRAMPOLINE_BASE + 2 * PAGE_SIZE, "TRAMPOLINE" },
#endif
	{}
};

void __init reserve_early(unsigned long start, unsigned long end, char *name)
{
	int i;
	struct early_res *r;
	for (i = 0; i < MAX_EARLY_RES && early_res[i].end; i++) {
		r = &early_res[i];
		if (end > r->start && start < r->end)
			panic("Overlapping early reservations %lx-%lx %s to %lx-%lx %s\n",
			      start, end - 1, name?name:"", r->start, r->end - 1, r->name);
	}
	if (i >= MAX_EARLY_RES)
		panic("Too many early reservations");
	r = &early_res[i];
	r->start = start;
	r->end = end;
	if (name)
		strncpy(r->name, name, sizeof(r->name) - 1);
}

void __init free_early(unsigned long start, unsigned long end)
{
	struct early_res *r;
	int i, j;

	for (i = 0; i < MAX_EARLY_RES && early_res[i].end; i++) {
		r = &early_res[i];
		if (start == r->start && end == r->end)
			break;
	}
	if (i >= MAX_EARLY_RES || !early_res[i].end)
		panic("free_early on not reserved area: %lx-%lx!", start, end);

	for (j = i + 1; j < MAX_EARLY_RES && early_res[j].end; j++)
		;

	memmove(&early_res[i], &early_res[i + 1],
	       (j - 1 - i) * sizeof(struct early_res));

	early_res[j - 1].end = 0;
}

void __init early_res_to_bootmem(unsigned long start, unsigned long end)
{
	int i;
	unsigned long final_start, final_end;
	for (i = 0; i < MAX_EARLY_RES && early_res[i].end; i++) {
		struct early_res *r = &early_res[i];
		final_start = max(start, r->start);
		final_end = min(end, r->end);
		if (final_start >= final_end)
			continue;
		printk(KERN_INFO "  early res: %d [%lx-%lx] %s\n", i,
			final_start, final_end - 1, r->name);
		reserve_bootmem_generic(final_start, final_end - final_start);
	}
}

/* Check for already reserved areas */
static inline int __init
bad_addr(unsigned long *addrp, unsigned long size, unsigned long align)
{
	int i;
	unsigned long addr = *addrp, last;
	int changed = 0;
again:
	last = addr + size;
	for (i = 0; i < MAX_EARLY_RES && early_res[i].end; i++) {
		struct early_res *r = &early_res[i];
		if (last >= r->start && addr < r->end) {
			*addrp = addr = round_up(r->end, align);
			changed = 1;
			goto again;
		}
	}
	return changed;
}

/* Check for already reserved areas */
static inline int __init
bad_addr_size(unsigned long *addrp, unsigned long *sizep, unsigned long align)
{
	int i;
	unsigned long addr = *addrp, last;
	unsigned long size = *sizep;
	int changed = 0;
again:
	last = addr + size;
	for (i = 0; i < MAX_EARLY_RES && early_res[i].end; i++) {
		struct early_res *r = &early_res[i];
		if (last > r->start && addr < r->start) {
			size = r->start - addr;
			changed = 1;
			goto again;
		}
		if (last > r->end && addr < r->end) {
			addr = round_up(r->end, align);
			size = last - addr;
			changed = 1;
			goto again;
		}
		if (last <= r->end && addr >= r->start) {
			(*sizep)++;
			return 0;
		}
	}
	if (changed) {
		*addrp = addr;
		*sizep = size;
	}
	return changed;
}
/*
 * This function checks if any part of the range <start,end> is mapped
 * with type.
 */
int
e820_any_mapped(unsigned long start, unsigned long end, unsigned type)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];

		if (type && ei->type != type)
			continue;
		if (ei->addr >= end || ei->addr + ei->size <= start)
			continue;
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(e820_any_mapped);

/*
 * This function checks if the entire range <start,end> is mapped with type.
 *
 * Note: this function only works correct if the e820 table is sorted and
 * not-overlapping, which is the case
 */
int __init e820_all_mapped(unsigned long start, unsigned long end,
			   unsigned type)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];

		if (type && ei->type != type)
			continue;
		/* is the region (part) in overlap with the current region ?*/
		if (ei->addr >= end || ei->addr + ei->size <= start)
			continue;

		/* if the region is at the beginning of <start,end> we move
		 * start to the end of the region since it's ok until there
		 */
		if (ei->addr <= start)
			start = ei->addr + ei->size;
		/*
		 * if start is now at or beyond end, we're done, full
		 * coverage
		 */
		if (start >= end)
			return 1;
	}
	return 0;
}

/*
 * Find a free area with specified alignment in a specific range.
 */
unsigned long __init find_e820_area(unsigned long start, unsigned long end,
				    unsigned long size, unsigned long align)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];
		unsigned long addr, last;
		unsigned long ei_last;

		if (ei->type != E820_RAM)
			continue;
		addr = round_up(ei->addr, align);
		ei_last = ei->addr + ei->size;
		if (addr < start)
			addr = round_up(start, align);
		if (addr >= ei_last)
			continue;
		while (bad_addr(&addr, size, align) && addr+size <= ei_last)
			;
		last = addr + size;
		if (last > ei_last)
			continue;
		if (last > end)
			continue;
		return addr;
	}
	return -1UL;
}

/*
 * Find next free range after *start
 */
unsigned long __init find_e820_area_size(unsigned long start,
					 unsigned long *sizep,
					 unsigned long align)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];
		unsigned long addr, last;
		unsigned long ei_last;

		if (ei->type != E820_RAM)
			continue;
		addr = round_up(ei->addr, align);
		ei_last = ei->addr + ei->size;
		if (addr < start)
			addr = round_up(start, align);
		if (addr >= ei_last)
			continue;
		*sizep = ei_last - addr;
		while (bad_addr_size(&addr, sizep, align) &&
			addr + *sizep <= ei_last)
			;
		last = addr + *sizep;
		if (last > ei_last)
			continue;
		return addr;
	}
	return -1UL;

}
/*
 * Find the highest page frame number we have available
 */
unsigned long __init e820_end_of_ram(void)
{
	unsigned long end_pfn;

	end_pfn = find_max_pfn_with_active_regions();

	if (end_pfn > max_pfn_mapped)
		max_pfn_mapped = end_pfn;
	if (max_pfn_mapped > MAXMEM>>PAGE_SHIFT)
		max_pfn_mapped = MAXMEM>>PAGE_SHIFT;
	if (end_pfn > end_user_pfn)
		end_pfn = end_user_pfn;
	if (end_pfn > max_pfn_mapped)
		end_pfn = max_pfn_mapped;

	printk(KERN_INFO "max_pfn_mapped = %lu\n", max_pfn_mapped);
	return end_pfn;
}

/*
 * Mark e820 reserved areas as busy for the resource manager.
 */
void __init e820_reserve_resources(void)
{
	int i;
	struct resource *res;

	res = alloc_bootmem_low(sizeof(struct resource) * e820.nr_map);
	for (i = 0; i < e820.nr_map; i++) {
		switch (e820.map[i].type) {
		case E820_RAM:	res->name = "System RAM"; break;
		case E820_ACPI:	res->name = "ACPI Tables"; break;
		case E820_NVS:	res->name = "ACPI Non-volatile Storage"; break;
		default:	res->name = "reserved";
		}
		res->start = e820.map[i].addr;
		res->end = res->start + e820.map[i].size - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;
		insert_resource(&iomem_resource, res);
		res++;
	}
}

/*
 * Find the ranges of physical addresses that do not correspond to
 * e820 RAM areas and mark the corresponding pages as nosave for software
 * suspend and suspend to RAM.
 *
 * This function requires the e820 map to be sorted and without any
 * overlapping entries and assumes the first e820 area to be RAM.
 */
void __init e820_mark_nosave_regions(void)
{
	int i;
	unsigned long paddr;

	paddr = round_down(e820.map[0].addr + e820.map[0].size, PAGE_SIZE);
	for (i = 1; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];

		if (paddr < ei->addr)
			register_nosave_region(PFN_DOWN(paddr),
						PFN_UP(ei->addr));

		paddr = round_down(ei->addr + ei->size, PAGE_SIZE);
		if (ei->type != E820_RAM)
			register_nosave_region(PFN_UP(ei->addr),
						PFN_DOWN(paddr));

		if (paddr >= (end_pfn << PAGE_SHIFT))
			break;
	}
}

/*
 * Finds an active region in the address range from start_pfn to end_pfn and
 * returns its range in ei_startpfn and ei_endpfn for the e820 entry.
 */
static int __init e820_find_active_region(const struct e820entry *ei,
					  unsigned long start_pfn,
					  unsigned long end_pfn,
					  unsigned long *ei_startpfn,
					  unsigned long *ei_endpfn)
{
	*ei_startpfn = round_up(ei->addr, PAGE_SIZE) >> PAGE_SHIFT;
	*ei_endpfn = round_down(ei->addr + ei->size, PAGE_SIZE) >> PAGE_SHIFT;

	/* Skip map entries smaller than a page */
	if (*ei_startpfn >= *ei_endpfn)
		return 0;

	/* Check if max_pfn_mapped should be updated */
	if (ei->type != E820_RAM && *ei_endpfn > max_pfn_mapped)
		max_pfn_mapped = *ei_endpfn;

	/* Skip if map is outside the node */
	if (ei->type != E820_RAM || *ei_endpfn <= start_pfn ||
				    *ei_startpfn >= end_pfn)
		return 0;

	/* Check for overlaps */
	if (*ei_startpfn < start_pfn)
		*ei_startpfn = start_pfn;
	if (*ei_endpfn > end_pfn)
		*ei_endpfn = end_pfn;

	/* Obey end_user_pfn to save on memmap */
	if (*ei_startpfn >= end_user_pfn)
		return 0;
	if (*ei_endpfn > end_user_pfn)
		*ei_endpfn = end_user_pfn;

	return 1;
}

/* Walk the e820 map and register active regions within a node */
void __init
e820_register_active_regions(int nid, unsigned long start_pfn,
							unsigned long end_pfn)
{
	unsigned long ei_startpfn;
	unsigned long ei_endpfn;
	int i;

	for (i = 0; i < e820.nr_map; i++)
		if (e820_find_active_region(&e820.map[i],
					    start_pfn, end_pfn,
					    &ei_startpfn, &ei_endpfn))
			add_active_range(nid, ei_startpfn, ei_endpfn);
}

/*
 * Add a memory region to the kernel e820 map.
 */
void __init add_memory_region(unsigned long start, unsigned long size, int type)
{
	int x = e820.nr_map;

	if (x == E820MAX) {
		printk(KERN_ERR "Ooops! Too many entries in the memory map!\n");
		return;
	}

	e820.map[x].addr = start;
	e820.map[x].size = size;
	e820.map[x].type = type;
	e820.nr_map++;
}

/*
 * Find the hole size (in bytes) in the memory range.
 * @start: starting address of the memory range to scan
 * @end: ending address of the memory range to scan
 */
unsigned long __init e820_hole_size(unsigned long start, unsigned long end)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long end_pfn = end >> PAGE_SHIFT;
	unsigned long ei_startpfn, ei_endpfn, ram = 0;
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		if (e820_find_active_region(&e820.map[i],
					    start_pfn, end_pfn,
					    &ei_startpfn, &ei_endpfn))
			ram += ei_endpfn - ei_startpfn;
	}
	return end - start - (ram << PAGE_SHIFT);
}

static void __init e820_print_map(char *who)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		printk(KERN_INFO " %s: %016Lx - %016Lx ", who,
		       (unsigned long long) e820.map[i].addr,
		       (unsigned long long)
		       (e820.map[i].addr + e820.map[i].size));
		switch (e820.map[i].type) {
		case E820_RAM:
			printk(KERN_CONT "(usable)\n");
			break;
		case E820_RESERVED:
			printk(KERN_CONT "(reserved)\n");
			break;
		case E820_ACPI:
			printk(KERN_CONT "(ACPI data)\n");
			break;
		case E820_NVS:
			printk(KERN_CONT "(ACPI NVS)\n");
			break;
		default:
			printk(KERN_CONT "type %u\n", e820.map[i].type);
			break;
		}
	}
}

/*
 * Sanitize the BIOS e820 map.
 *
 * Some e820 responses include overlapping entries. The following
 * replaces the original e820 map with a new one, removing overlaps.
 *
 */
static int __init sanitize_e820_map(struct e820entry *biosmap, char *pnr_map)
{
	struct change_member {
		struct e820entry *pbios; /* pointer to original bios entry */
		unsigned long long addr; /* address for this change point */
	};
	static struct change_member change_point_list[2*E820MAX] __initdata;
	static struct change_member *change_point[2*E820MAX] __initdata;
	static struct e820entry *overlap_list[E820MAX] __initdata;
	static struct e820entry new_bios[E820MAX] __initdata;
	struct change_member *change_tmp;
	unsigned long current_type, last_type;
	unsigned long long last_addr;
	int chgidx, still_changing;
	int overlap_entries;
	int new_bios_entry;
	int old_nr, new_nr, chg_nr;
	int i;

	/*
		Visually we're performing the following
		(1,2,3,4 = memory types)...

		Sample memory map (w/overlaps):
		   ____22__________________
		   ______________________4_
		   ____1111________________
		   _44_____________________
		   11111111________________
		   ____________________33__
		   ___________44___________
		   __________33333_________
		   ______________22________
		   ___________________2222_
		   _________111111111______
		   _____________________11_
		   _________________4______

		Sanitized equivalent (no overlap):
		   1_______________________
		   _44_____________________
		   ___1____________________
		   ____22__________________
		   ______11________________
		   _________1______________
		   __________3_____________
		   ___________44___________
		   _____________33_________
		   _______________2________
		   ________________1_______
		   _________________4______
		   ___________________2____
		   ____________________33__
		   ______________________4_
	*/

	/* if there's only one memory region, don't bother */
	if (*pnr_map < 2)
		return -1;

	old_nr = *pnr_map;

	/* bail out if we find any unreasonable addresses in bios map */
	for (i = 0; i < old_nr; i++)
		if (biosmap[i].addr + biosmap[i].size < biosmap[i].addr)
			return -1;

	/* create pointers for initial change-point information (for sorting) */
	for (i = 0; i < 2 * old_nr; i++)
		change_point[i] = &change_point_list[i];

	/* record all known change-points (starting and ending addresses),
	   omitting those that are for empty memory regions */
	chgidx = 0;
	for (i = 0; i < old_nr; i++)	{
		if (biosmap[i].size != 0) {
			change_point[chgidx]->addr = biosmap[i].addr;
			change_point[chgidx++]->pbios = &biosmap[i];
			change_point[chgidx]->addr = biosmap[i].addr +
				biosmap[i].size;
			change_point[chgidx++]->pbios = &biosmap[i];
		}
	}
	chg_nr = chgidx;

	/* sort change-point list by memory addresses (low -> high) */
	still_changing = 1;
	while (still_changing)	{
		still_changing = 0;
		for (i = 1; i < chg_nr; i++)  {
			unsigned long long curaddr, lastaddr;
			unsigned long long curpbaddr, lastpbaddr;

			curaddr = change_point[i]->addr;
			lastaddr = change_point[i - 1]->addr;
			curpbaddr = change_point[i]->pbios->addr;
			lastpbaddr = change_point[i - 1]->pbios->addr;

			/*
			 * swap entries, when:
			 *
			 * curaddr > lastaddr or
			 * curaddr == lastaddr and curaddr == curpbaddr and
			 * lastaddr != lastpbaddr
			 */
			if (curaddr < lastaddr ||
			    (curaddr == lastaddr && curaddr == curpbaddr &&
			     lastaddr != lastpbaddr)) {
				change_tmp = change_point[i];
				change_point[i] = change_point[i-1];
				change_point[i-1] = change_tmp;
				still_changing = 1;
			}
		}
	}

	/* create a new bios memory map, removing overlaps */
	overlap_entries = 0;	 /* number of entries in the overlap table */
	new_bios_entry = 0;	 /* index for creating new bios map entries */
	last_type = 0;		 /* start with undefined memory type */
	last_addr = 0;		 /* start with 0 as last starting address */

	/* loop through change-points, determining affect on the new bios map */
	for (chgidx = 0; chgidx < chg_nr; chgidx++) {
		/* keep track of all overlapping bios entries */
		if (change_point[chgidx]->addr ==
		    change_point[chgidx]->pbios->addr) {
			/*
			 * add map entry to overlap list (> 1 entry
			 * implies an overlap)
			 */
			overlap_list[overlap_entries++] =
				change_point[chgidx]->pbios;
		} else {
			/*
			 * remove entry from list (order independent,
			 * so swap with last)
			 */
			for (i = 0; i < overlap_entries; i++) {
				if (overlap_list[i] ==
				    change_point[chgidx]->pbios)
					overlap_list[i] =
						overlap_list[overlap_entries-1];
			}
			overlap_entries--;
		}
		/*
		 * if there are overlapping entries, decide which
		 * "type" to use (larger value takes precedence --
		 * 1=usable, 2,3,4,4+=unusable)
		 */
		current_type = 0;
		for (i = 0; i < overlap_entries; i++)
			if (overlap_list[i]->type > current_type)
				current_type = overlap_list[i]->type;
		/*
		 * continue building up new bios map based on this
		 * information
		 */
		if (current_type != last_type)	{
			if (last_type != 0)	 {
				new_bios[new_bios_entry].size =
					change_point[chgidx]->addr - last_addr;
				/*
				 * move forward only if the new size
				 * was non-zero
				 */
				if (new_bios[new_bios_entry].size != 0)
					/*
					 * no more space left for new
					 * bios entries ?
					 */
					if (++new_bios_entry >= E820MAX)
						break;
			}
			if (current_type != 0)	{
				new_bios[new_bios_entry].addr =
					change_point[chgidx]->addr;
				new_bios[new_bios_entry].type = current_type;
				last_addr = change_point[chgidx]->addr;
			}
			last_type = current_type;
		}
	}
	/* retain count for new bios entries */
	new_nr = new_bios_entry;

	/* copy new bios mapping into original location */
	memcpy(biosmap, new_bios, new_nr * sizeof(struct e820entry));
	*pnr_map = new_nr;

	return 0;
}

/*
 * Copy the BIOS e820 map into a safe place.
 *
 * Sanity-check it while we're at it..
 *
 * If we're lucky and live on a modern system, the setup code
 * will have given us a memory map that we can use to properly
 * set up memory.  If we aren't, we'll fake a memory map.
 */
static int __init copy_e820_map(struct e820entry *biosmap, int nr_map)
{
	/* Only one memory region (or negative)? Ignore it */
	if (nr_map < 2)
		return -1;

	do {
		u64 start = biosmap->addr;
		u64 size = biosmap->size;
		u64 end = start + size;
		u32 type = biosmap->type;

		/* Overflow in 64 bits? Ignore the memory map. */
		if (start > end)
			return -1;

		add_memory_region(start, size, type);
	} while (biosmap++, --nr_map);
	return 0;
}

static void early_panic(char *msg)
{
	early_printk(msg);
	panic(msg);
}

/* We're not void only for x86 32-bit compat */
char * __init machine_specific_memory_setup(void)
{
	char *who = "BIOS-e820";
	/*
	 * Try to copy the BIOS-supplied E820-map.
	 *
	 * Otherwise fake a memory map; one section from 0k->640k,
	 * the next section from 1mb->appropriate_mem_k
	 */
	sanitize_e820_map(boot_params.e820_map, &boot_params.e820_entries);
	if (copy_e820_map(boot_params.e820_map, boot_params.e820_entries) < 0)
		early_panic("Cannot find a valid memory map");
	printk(KERN_INFO "BIOS-provided physical RAM map:\n");
	e820_print_map(who);

	/* In case someone cares... */
	return who;
}

static int __init parse_memopt(char *p)
{
	if (!p)
		return -EINVAL;
	end_user_pfn = memparse(p, &p);
	end_user_pfn >>= PAGE_SHIFT;
	return 0;
}
early_param("mem", parse_memopt);

static int userdef __initdata;

static int __init parse_memmap_opt(char *p)
{
	char *oldp;
	unsigned long long start_at, mem_size;

	if (!strncmp(p, "exactmap", 8)) {
#ifdef CONFIG_CRASH_DUMP
		/*
		 * If we are doing a crash dump, we still need to know
		 * the real mem size before original memory map is
		 * reset.
		 */
		e820_register_active_regions(0, 0, -1UL);
		saved_max_pfn = e820_end_of_ram();
		remove_all_active_ranges();
#endif
		max_pfn_mapped = 0;
		e820.nr_map = 0;
		userdef = 1;
		return 0;
	}

	oldp = p;
	mem_size = memparse(p, &p);
	if (p == oldp)
		return -EINVAL;

	userdef = 1;
	if (*p == '@') {
		start_at = memparse(p+1, &p);
		add_memory_region(start_at, mem_size, E820_RAM);
	} else if (*p == '#') {
		start_at = memparse(p+1, &p);
		add_memory_region(start_at, mem_size, E820_ACPI);
	} else if (*p == '$') {
		start_at = memparse(p+1, &p);
		add_memory_region(start_at, mem_size, E820_RESERVED);
	} else {
		end_user_pfn = (mem_size >> PAGE_SHIFT);
	}
	return *p == '\0' ? 0 : -EINVAL;
}
early_param("memmap", parse_memmap_opt);

void __init finish_e820_parsing(void)
{
	if (userdef) {
		char nr = e820.nr_map;

		if (sanitize_e820_map(e820.map, &nr) < 0)
			early_panic("Invalid user supplied memory map");
		e820.nr_map = nr;

		printk(KERN_INFO "user-defined physical RAM map:\n");
		e820_print_map("user");
	}
}

void __init update_memory_range(u64 start, u64 size, unsigned old_type,
				unsigned new_type)
{
	int i;

	BUG_ON(old_type == new_type);

	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i];
		u64 final_start, final_end;
		if (ei->type != old_type)
			continue;
		/* totally covered? */
		if (ei->addr >= start && ei->size <= size) {
			ei->type = new_type;
			continue;
		}
		/* partially covered */
		final_start = max(start, ei->addr);
		final_end = min(start + size, ei->addr + ei->size);
		if (final_start >= final_end)
			continue;
		add_memory_region(final_start, final_end - final_start,
					 new_type);
	}
}

void __init update_e820(void)
{
	u8 nr_map;

	nr_map = e820.nr_map;
	if (sanitize_e820_map(e820.map, &nr_map))
		return;
	e820.nr_map = nr_map;
	printk(KERN_INFO "modified physical RAM map:\n");
	e820_print_map("modified");
}

unsigned long pci_mem_start = 0xaeedbabe;
EXPORT_SYMBOL(pci_mem_start);

/*
 * Search for the biggest gap in the low 32 bits of the e820
 * memory space.  We pass this space to PCI to assign MMIO resources
 * for hotplug or unconfigured devices in.
 * Hopefully the BIOS let enough space left.
 */
__init void e820_setup_gap(void)
{
	unsigned long gapstart, gapsize, round;
	unsigned long last;
	int i;
	int found = 0;

	last = 0x100000000ull;
	gapstart = 0x10000000;
	gapsize = 0x400000;
	i = e820.nr_map;
	while (--i >= 0) {
		unsigned long long start = e820.map[i].addr;
		unsigned long long end = start + e820.map[i].size;

		/*
		 * Since "last" is at most 4GB, we know we'll
		 * fit in 32 bits if this condition is true
		 */
		if (last > end) {
			unsigned long gap = last - end;

			if (gap > gapsize) {
				gapsize = gap;
				gapstart = end;
				found = 1;
			}
		}
		if (start < last)
			last = start;
	}

	if (!found) {
		gapstart = (end_pfn << PAGE_SHIFT) + 1024*1024;
		printk(KERN_ERR "PCI: Warning: Cannot find a gap in the 32bit "
		       "address range\n"
		       KERN_ERR "PCI: Unassigned devices with 32bit resource "
		       "registers may break!\n");
	}

	/*
	 * See how much we want to round up: start off with
	 * rounding to the next 1MB area.
	 */
	round = 0x100000;
	while ((gapsize >> 4) > round)
		round += round;
	/* Fun with two's complement */
	pci_mem_start = (gapstart + round) & -round;

	printk(KERN_INFO
	       "Allocating PCI resources starting at %lx (gap: %lx:%lx)\n",
	       pci_mem_start, gapstart, gapsize);
}

int __init arch_get_ram_range(int slot, u64 *addr, u64 *size)
{
	int i;

	if (slot < 0 || slot >= e820.nr_map)
		return -1;
	for (i = slot; i < e820.nr_map; i++) {
		if (e820.map[i].type != E820_RAM)
			continue;
		break;
	}
	if (i == e820.nr_map || e820.map[i].addr > (max_pfn << PAGE_SHIFT))
		return -1;
	*addr = e820.map[i].addr;
	*size = min_t(u64, e820.map[i].size + e820.map[i].addr,
		max_pfn << PAGE_SHIFT) - *addr;
	return i + 1;
}
