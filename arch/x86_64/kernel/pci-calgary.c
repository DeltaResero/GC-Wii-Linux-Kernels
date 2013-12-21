/*
 * Derived from arch/powerpc/kernel/iommu.c
 *
 * Copyright (C) IBM Corporation, 2006
 *
 * Author: Jon Mason <jdmason@us.ibm.com>
 * Author: Muli Ben-Yehuda <muli@il.ibm.com>

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <asm/proto.h>
#include <asm/calgary.h>
#include <asm/tce.h>
#include <asm/pci-direct.h>
#include <asm/system.h>
#include <asm/dma.h>

#define PCI_DEVICE_ID_IBM_CALGARY 0x02a1
#define PCI_VENDOR_DEVICE_ID_CALGARY \
	(PCI_VENDOR_ID_IBM | PCI_DEVICE_ID_IBM_CALGARY << 16)

/* we need these for register space address calculation */
#define START_ADDRESS           0xfe000000
#define CHASSIS_BASE            0
#define ONE_BASED_CHASSIS_NUM   1

/* register offsets inside the host bridge space */
#define PHB_CSR_OFFSET		0x0110
#define PHB_PLSSR_OFFSET	0x0120
#define PHB_CONFIG_RW_OFFSET	0x0160
#define PHB_IOBASE_BAR_LOW	0x0170
#define PHB_IOBASE_BAR_HIGH	0x0180
#define PHB_MEM_1_LOW		0x0190
#define PHB_MEM_1_HIGH		0x01A0
#define PHB_IO_ADDR_SIZE	0x01B0
#define PHB_MEM_1_SIZE		0x01C0
#define PHB_MEM_ST_OFFSET	0x01D0
#define PHB_AER_OFFSET		0x0200
#define PHB_CONFIG_0_HIGH	0x0220
#define PHB_CONFIG_0_LOW	0x0230
#define PHB_CONFIG_0_END	0x0240
#define PHB_MEM_2_LOW		0x02B0
#define PHB_MEM_2_HIGH		0x02C0
#define PHB_MEM_2_SIZE_HIGH	0x02D0
#define PHB_MEM_2_SIZE_LOW	0x02E0
#define PHB_DOSHOLE_OFFSET	0x08E0

/* PHB_CONFIG_RW */
#define PHB_TCE_ENABLE		0x20000000
#define PHB_SLOT_DISABLE	0x1C000000
#define PHB_DAC_DISABLE		0x01000000
#define PHB_MEM2_ENABLE		0x00400000
#define PHB_MCSR_ENABLE		0x00100000
/* TAR (Table Address Register) */
#define TAR_SW_BITS		0x0000ffffffff800fUL
#define TAR_VALID		0x0000000000000008UL
/* CSR (Channel/DMA Status Register) */
#define CSR_AGENT_MASK		0xffe0ffff

#define MAX_NUM_OF_PHBS		8 /* how many PHBs in total? */
#define MAX_NUM_CHASSIS		8 /* max number of chassis */
#define MAX_PHB_BUS_NUM		(MAX_NUM_OF_PHBS * MAX_NUM_CHASSIS * 2) /* max dev->bus->number */
#define PHBS_PER_CALGARY	4

/* register offsets in Calgary's internal register space */
static const unsigned long tar_offsets[] = {
	0x0580 /* TAR0 */,
	0x0588 /* TAR1 */,
	0x0590 /* TAR2 */,
	0x0598 /* TAR3 */
};

static const unsigned long split_queue_offsets[] = {
	0x4870 /* SPLIT QUEUE 0 */,
	0x5870 /* SPLIT QUEUE 1 */,
	0x6870 /* SPLIT QUEUE 2 */,
	0x7870 /* SPLIT QUEUE 3 */
};

static const unsigned long phb_offsets[] = {
	0x8000 /* PHB0 */,
	0x9000 /* PHB1 */,
	0xA000 /* PHB2 */,
	0xB000 /* PHB3 */
};

static char bus_to_phb[MAX_PHB_BUS_NUM];
void* tce_table_kva[MAX_PHB_BUS_NUM];
unsigned int specified_table_size = TCE_TABLE_SIZE_UNSPECIFIED;
static int translate_empty_slots __read_mostly = 0;
static int calgary_detected __read_mostly = 0;

/*
 * the bitmap of PHBs the user requested that we disable
 * translation on.
 */
static DECLARE_BITMAP(translation_disabled, MAX_PHB_BUS_NUM);

static void tce_cache_blast(struct iommu_table *tbl);

/* enable this to stress test the chip's TCE cache */
#ifdef CONFIG_IOMMU_DEBUG
static inline void tce_cache_blast_stress(struct iommu_table *tbl)
{
	tce_cache_blast(tbl);
}
#else
static inline void tce_cache_blast_stress(struct iommu_table *tbl)
{
}
#endif /* BLAST_TCE_CACHE_ON_UNMAP */

static inline unsigned int num_dma_pages(unsigned long dma, unsigned int dmalen)
{
	unsigned int npages;

	npages = PAGE_ALIGN(dma + dmalen) - (dma & PAGE_MASK);
	npages >>= PAGE_SHIFT;

	return npages;
}

static inline int translate_phb(struct pci_dev* dev)
{
	int disabled = test_bit(dev->bus->number, translation_disabled);
	return !disabled;
}

static void iommu_range_reserve(struct iommu_table *tbl,
        unsigned long start_addr, unsigned int npages)
{
	unsigned long index;
	unsigned long end;

	index = start_addr >> PAGE_SHIFT;

	/* bail out if we're asked to reserve a region we don't cover */
	if (index >= tbl->it_size)
		return;

	end = index + npages;
	if (end > tbl->it_size) /* don't go off the table */
		end = tbl->it_size;

	while (index < end) {
		if (test_bit(index, tbl->it_map))
			printk(KERN_ERR "Calgary: entry already allocated at "
			       "0x%lx tbl %p dma 0x%lx npages %u\n",
			       index, tbl, start_addr, npages);
		++index;
	}
	set_bit_string(tbl->it_map, start_addr >> PAGE_SHIFT, npages);
}

static unsigned long iommu_range_alloc(struct iommu_table *tbl,
	unsigned int npages)
{
	unsigned long offset;

	BUG_ON(npages == 0);

	offset = find_next_zero_string(tbl->it_map, tbl->it_hint,
				       tbl->it_size, npages);
	if (offset == ~0UL) {
		tce_cache_blast(tbl);
		offset = find_next_zero_string(tbl->it_map, 0,
					       tbl->it_size, npages);
		if (offset == ~0UL) {
			printk(KERN_WARNING "Calgary: IOMMU full.\n");
			if (panic_on_overflow)
				panic("Calgary: fix the allocator.\n");
			else
				return bad_dma_address;
		}
	}

	set_bit_string(tbl->it_map, offset, npages);
	tbl->it_hint = offset + npages;
	BUG_ON(tbl->it_hint > tbl->it_size);

	return offset;
}

static dma_addr_t iommu_alloc(struct iommu_table *tbl, void *vaddr,
	unsigned int npages, int direction)
{
	unsigned long entry, flags;
	dma_addr_t ret = bad_dma_address;

	spin_lock_irqsave(&tbl->it_lock, flags);

	entry = iommu_range_alloc(tbl, npages);

	if (unlikely(entry == bad_dma_address))
		goto error;

	/* set the return dma address */
	ret = (entry << PAGE_SHIFT) | ((unsigned long)vaddr & ~PAGE_MASK);

	/* put the TCEs in the HW table */
	tce_build(tbl, entry, npages, (unsigned long)vaddr & PAGE_MASK,
		  direction);

	spin_unlock_irqrestore(&tbl->it_lock, flags);

	return ret;

error:
	spin_unlock_irqrestore(&tbl->it_lock, flags);
	printk(KERN_WARNING "Calgary: failed to allocate %u pages in "
	       "iommu %p\n", npages, tbl);
	return bad_dma_address;
}

static void __iommu_free(struct iommu_table *tbl, dma_addr_t dma_addr,
	unsigned int npages)
{
	unsigned long entry;
	unsigned long i;

	entry = dma_addr >> PAGE_SHIFT;

	BUG_ON(entry + npages > tbl->it_size);

	tce_free(tbl, entry, npages);

	for (i = 0; i < npages; ++i) {
		if (!test_bit(entry + i, tbl->it_map))
			printk(KERN_ERR "Calgary: bit is off at 0x%lx "
			       "tbl %p dma 0x%Lx entry 0x%lx npages %u\n",
			       entry + i, tbl, dma_addr, entry, npages);
	}

	__clear_bit_string(tbl->it_map, entry, npages);

	tce_cache_blast_stress(tbl);
}

static void iommu_free(struct iommu_table *tbl, dma_addr_t dma_addr,
	unsigned int npages)
{
	unsigned long flags;

	spin_lock_irqsave(&tbl->it_lock, flags);

	__iommu_free(tbl, dma_addr, npages);

	spin_unlock_irqrestore(&tbl->it_lock, flags);
}

static void __calgary_unmap_sg(struct iommu_table *tbl,
	struct scatterlist *sglist, int nelems, int direction)
{
	while (nelems--) {
		unsigned int npages;
		dma_addr_t dma = sglist->dma_address;
		unsigned int dmalen = sglist->dma_length;

		if (dmalen == 0)
			break;

		npages = num_dma_pages(dma, dmalen);
		__iommu_free(tbl, dma, npages);
		sglist++;
	}
}

void calgary_unmap_sg(struct device *dev, struct scatterlist *sglist,
		      int nelems, int direction)
{
	unsigned long flags;
	struct iommu_table *tbl = to_pci_dev(dev)->bus->self->sysdata;

	if (!translate_phb(to_pci_dev(dev)))
		return;

	spin_lock_irqsave(&tbl->it_lock, flags);

	__calgary_unmap_sg(tbl, sglist, nelems, direction);

	spin_unlock_irqrestore(&tbl->it_lock, flags);
}

static int calgary_nontranslate_map_sg(struct device* dev,
	struct scatterlist *sg, int nelems, int direction)
{
	int i;

 	for (i = 0; i < nelems; i++ ) {
		struct scatterlist *s = &sg[i];
		BUG_ON(!s->page);
		s->dma_address = virt_to_bus(page_address(s->page) +s->offset);
		s->dma_length = s->length;
	}
	return nelems;
}

int calgary_map_sg(struct device *dev, struct scatterlist *sg,
	int nelems, int direction)
{
	struct iommu_table *tbl = to_pci_dev(dev)->bus->self->sysdata;
	unsigned long flags;
	unsigned long vaddr;
	unsigned int npages;
	unsigned long entry;
	int i;

	if (!translate_phb(to_pci_dev(dev)))
		return calgary_nontranslate_map_sg(dev, sg, nelems, direction);

	spin_lock_irqsave(&tbl->it_lock, flags);

	for (i = 0; i < nelems; i++ ) {
		struct scatterlist *s = &sg[i];
		BUG_ON(!s->page);

		vaddr = (unsigned long)page_address(s->page) + s->offset;
		npages = num_dma_pages(vaddr, s->length);

		entry = iommu_range_alloc(tbl, npages);
		if (entry == bad_dma_address) {
			/* makes sure unmap knows to stop */
			s->dma_length = 0;
			goto error;
		}

		s->dma_address = (entry << PAGE_SHIFT) | s->offset;

		/* insert into HW table */
		tce_build(tbl, entry, npages, vaddr & PAGE_MASK,
			  direction);

		s->dma_length = s->length;
	}

	spin_unlock_irqrestore(&tbl->it_lock, flags);

	return nelems;
error:
	__calgary_unmap_sg(tbl, sg, nelems, direction);
	for (i = 0; i < nelems; i++) {
		sg[i].dma_address = bad_dma_address;
		sg[i].dma_length = 0;
	}
	spin_unlock_irqrestore(&tbl->it_lock, flags);
	return 0;
}

dma_addr_t calgary_map_single(struct device *dev, void *vaddr,
	size_t size, int direction)
{
	dma_addr_t dma_handle = bad_dma_address;
	unsigned long uaddr;
	unsigned int npages;
	struct iommu_table *tbl = to_pci_dev(dev)->bus->self->sysdata;

	uaddr = (unsigned long)vaddr;
	npages = num_dma_pages(uaddr, size);

	if (translate_phb(to_pci_dev(dev)))
		dma_handle = iommu_alloc(tbl, vaddr, npages, direction);
	else
		dma_handle = virt_to_bus(vaddr);

	return dma_handle;
}

void calgary_unmap_single(struct device *dev, dma_addr_t dma_handle,
	size_t size, int direction)
{
	struct iommu_table *tbl = to_pci_dev(dev)->bus->self->sysdata;
	unsigned int npages;

	if (!translate_phb(to_pci_dev(dev)))
		return;

	npages = num_dma_pages(dma_handle, size);
	iommu_free(tbl, dma_handle, npages);
}

void* calgary_alloc_coherent(struct device *dev, size_t size,
	dma_addr_t *dma_handle, gfp_t flag)
{
	void *ret = NULL;
	dma_addr_t mapping;
	unsigned int npages, order;
	struct iommu_table *tbl;

	tbl = to_pci_dev(dev)->bus->self->sysdata;

	size = PAGE_ALIGN(size); /* size rounded up to full pages */
	npages = size >> PAGE_SHIFT;
	order = get_order(size);

	/* alloc enough pages (and possibly more) */
	ret = (void *)__get_free_pages(flag, order);
	if (!ret)
		goto error;
	memset(ret, 0, size);

	if (translate_phb(to_pci_dev(dev))) {
		/* set up tces to cover the allocated range */
		mapping = iommu_alloc(tbl, ret, npages, DMA_BIDIRECTIONAL);
		if (mapping == bad_dma_address)
			goto free;

		*dma_handle = mapping;
	} else /* non translated slot */
		*dma_handle = virt_to_bus(ret);

	return ret;

free:
	free_pages((unsigned long)ret, get_order(size));
	ret = NULL;
error:
	return ret;
}

static struct dma_mapping_ops calgary_dma_ops = {
	.alloc_coherent = calgary_alloc_coherent,
	.map_single = calgary_map_single,
	.unmap_single = calgary_unmap_single,
	.map_sg = calgary_map_sg,
	.unmap_sg = calgary_unmap_sg,
};

static inline int busno_to_phbid(unsigned char num)
{
	return bus_to_phb[num];
}

static inline unsigned long split_queue_offset(unsigned char num)
{
	size_t idx = busno_to_phbid(num);

	return split_queue_offsets[idx];
}

static inline unsigned long tar_offset(unsigned char num)
{
	size_t idx = busno_to_phbid(num);

	return tar_offsets[idx];
}

static inline unsigned long phb_offset(unsigned char num)
{
	size_t idx = busno_to_phbid(num);

	return phb_offsets[idx];
}

static inline void __iomem* calgary_reg(void __iomem *bar, unsigned long offset)
{
	unsigned long target = ((unsigned long)bar) | offset;
	return (void __iomem*)target;
}

static void tce_cache_blast(struct iommu_table *tbl)
{
	u64 val;
	u32 aer;
	int i = 0;
	void __iomem *bbar = tbl->bbar;
	void __iomem *target;

	/* disable arbitration on the bus */
	target = calgary_reg(bbar, phb_offset(tbl->it_busno) | PHB_AER_OFFSET);
	aer = readl(target);
	writel(0, target);

	/* read plssr to ensure it got there */
	target = calgary_reg(bbar, phb_offset(tbl->it_busno) | PHB_PLSSR_OFFSET);
	val = readl(target);

	/* poll split queues until all DMA activity is done */
	target = calgary_reg(bbar, split_queue_offset(tbl->it_busno));
	do {
		val = readq(target);
		i++;
	} while ((val & 0xff) != 0xff && i < 100);
	if (i == 100)
		printk(KERN_WARNING "Calgary: PCI bus not quiesced, "
		       "continuing anyway\n");

	/* invalidate TCE cache */
	target = calgary_reg(bbar, tar_offset(tbl->it_busno));
	writeq(tbl->tar_val, target);

	/* enable arbitration */
	target = calgary_reg(bbar, phb_offset(tbl->it_busno) | PHB_AER_OFFSET);
	writel(aer, target);
	(void)readl(target); /* flush */
}

static void __init calgary_reserve_mem_region(struct pci_dev *dev, u64 start,
	u64 limit)
{
	unsigned int numpages;

	limit = limit | 0xfffff;
	limit++;

	numpages = ((limit - start) >> PAGE_SHIFT);
	iommu_range_reserve(dev->sysdata, start, numpages);
}

static void __init calgary_reserve_peripheral_mem_1(struct pci_dev *dev)
{
	void __iomem *target;
	u64 low, high, sizelow;
	u64 start, limit;
	struct iommu_table *tbl = dev->sysdata;
	unsigned char busnum = dev->bus->number;
	void __iomem *bbar = tbl->bbar;

	/* peripheral MEM_1 region */
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_1_LOW);
	low = be32_to_cpu(readl(target));
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_1_HIGH);
	high = be32_to_cpu(readl(target));
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_1_SIZE);
	sizelow = be32_to_cpu(readl(target));

	start = (high << 32) | low;
	limit = sizelow;

	calgary_reserve_mem_region(dev, start, limit);
}

static void __init calgary_reserve_peripheral_mem_2(struct pci_dev *dev)
{
	void __iomem *target;
	u32 val32;
	u64 low, high, sizelow, sizehigh;
	u64 start, limit;
	struct iommu_table *tbl = dev->sysdata;
	unsigned char busnum = dev->bus->number;
	void __iomem *bbar = tbl->bbar;

	/* is it enabled? */
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_CONFIG_RW_OFFSET);
	val32 = be32_to_cpu(readl(target));
	if (!(val32 & PHB_MEM2_ENABLE))
		return;

	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_2_LOW);
	low = be32_to_cpu(readl(target));
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_2_HIGH);
	high = be32_to_cpu(readl(target));
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_2_SIZE_LOW);
	sizelow = be32_to_cpu(readl(target));
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_MEM_2_SIZE_HIGH);
	sizehigh = be32_to_cpu(readl(target));

	start = (high << 32) | low;
	limit = (sizehigh << 32) | sizelow;

	calgary_reserve_mem_region(dev, start, limit);
}

/*
 * some regions of the IO address space do not get translated, so we
 * must not give devices IO addresses in those regions. The regions
 * are the 640KB-1MB region and the two PCI peripheral memory holes.
 * Reserve all of them in the IOMMU bitmap to avoid giving them out
 * later.
 */
static void __init calgary_reserve_regions(struct pci_dev *dev)
{
	unsigned int npages;
	void __iomem *bbar;
	unsigned char busnum;
	u64 start;
	struct iommu_table *tbl = dev->sysdata;

	bbar = tbl->bbar;
	busnum = dev->bus->number;

	/* reserve bad_dma_address in case it's a legal address */
	iommu_range_reserve(tbl, bad_dma_address, 1);

	/* avoid the BIOS/VGA first 640KB-1MB region */
	start = (640 * 1024);
	npages = ((1024 - 640) * 1024) >> PAGE_SHIFT;
	iommu_range_reserve(tbl, start, npages);

	/* reserve the two PCI peripheral memory regions in IO space */
	calgary_reserve_peripheral_mem_1(dev);
	calgary_reserve_peripheral_mem_2(dev);
}

static int __init calgary_setup_tar(struct pci_dev *dev, void __iomem *bbar)
{
	u64 val64;
	u64 table_phys;
	void __iomem *target;
	int ret;
	struct iommu_table *tbl;

	/* build TCE tables for each PHB */
	ret = build_tce_table(dev, bbar);
	if (ret)
		return ret;

	calgary_reserve_regions(dev);

	/* set TARs for each PHB */
	target = calgary_reg(bbar, tar_offset(dev->bus->number));
	val64 = be64_to_cpu(readq(target));

	/* zero out all TAR bits under sw control */
	val64 &= ~TAR_SW_BITS;

	tbl = dev->sysdata;
	table_phys = (u64)__pa(tbl->it_base);
	val64 |= table_phys;

	BUG_ON(specified_table_size > TCE_TABLE_SIZE_8M);
	val64 |= (u64) specified_table_size;

	tbl->tar_val = cpu_to_be64(val64);
	writeq(tbl->tar_val, target);
	readq(target); /* flush */

	return 0;
}

static void __init calgary_free_tar(struct pci_dev *dev)
{
	u64 val64;
	struct iommu_table *tbl = dev->sysdata;
	void __iomem *target;

	target = calgary_reg(tbl->bbar, tar_offset(dev->bus->number));
	val64 = be64_to_cpu(readq(target));
	val64 &= ~TAR_SW_BITS;
	writeq(cpu_to_be64(val64), target);
	readq(target); /* flush */

	kfree(tbl);
	dev->sysdata = NULL;
}

static void calgary_watchdog(unsigned long data)
{
	struct pci_dev *dev = (struct pci_dev *)data;
	struct iommu_table *tbl = dev->sysdata;
	void __iomem *bbar = tbl->bbar;
	u32 val32;
	void __iomem *target;

	target = calgary_reg(bbar, phb_offset(tbl->it_busno) | PHB_CSR_OFFSET);
	val32 = be32_to_cpu(readl(target));

	/* If no error, the agent ID in the CSR is not valid */
	if (val32 & CSR_AGENT_MASK) {
		printk(KERN_EMERG "calgary_watchdog: DMA error on bus %d, "
				  "CSR = %#x\n", dev->bus->number, val32);
		writel(0, target);

		/* Disable bus that caused the error */
		target = calgary_reg(bbar, phb_offset(tbl->it_busno) |
					   PHB_CONFIG_RW_OFFSET);
		val32 = be32_to_cpu(readl(target));
		val32 |= PHB_SLOT_DISABLE;
		writel(cpu_to_be32(val32), target);
		readl(target); /* flush */
	} else {
		/* Reset the timer */
		mod_timer(&tbl->watchdog_timer, jiffies + 2 * HZ);
	}
}

static void __init calgary_enable_translation(struct pci_dev *dev)
{
	u32 val32;
	unsigned char busnum;
	void __iomem *target;
	void __iomem *bbar;
	struct iommu_table *tbl;

	busnum = dev->bus->number;
	tbl = dev->sysdata;
	bbar = tbl->bbar;

	/* enable TCE in PHB Config Register */
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_CONFIG_RW_OFFSET);
	val32 = be32_to_cpu(readl(target));
	val32 |= PHB_TCE_ENABLE | PHB_DAC_DISABLE | PHB_MCSR_ENABLE;

	printk(KERN_INFO "Calgary: enabling translation on PHB %d\n", busnum);
	printk(KERN_INFO "Calgary: errant DMAs will now be prevented on this "
	       "bus.\n");

	writel(cpu_to_be32(val32), target);
	readl(target); /* flush */

	init_timer(&tbl->watchdog_timer);
	tbl->watchdog_timer.function = &calgary_watchdog;
	tbl->watchdog_timer.data = (unsigned long)dev;
	mod_timer(&tbl->watchdog_timer, jiffies);
}

static void __init calgary_disable_translation(struct pci_dev *dev)
{
	u32 val32;
	unsigned char busnum;
	void __iomem *target;
	void __iomem *bbar;
	struct iommu_table *tbl;

	busnum = dev->bus->number;
	tbl = dev->sysdata;
	bbar = tbl->bbar;

	/* disable TCE in PHB Config Register */
	target = calgary_reg(bbar, phb_offset(busnum) | PHB_CONFIG_RW_OFFSET);
	val32 = be32_to_cpu(readl(target));
	val32 &= ~(PHB_TCE_ENABLE | PHB_DAC_DISABLE | PHB_MCSR_ENABLE);

	printk(KERN_INFO "Calgary: disabling translation on PHB %d!\n", busnum);
	writel(cpu_to_be32(val32), target);
	readl(target); /* flush */

	del_timer_sync(&tbl->watchdog_timer);
}

static inline unsigned int __init locate_register_space(struct pci_dev *dev)
{
	int rionodeid;
	u32 address;

	/*
	 * Each Calgary has four busses. The first four busses (first Calgary)
	 * have RIO node ID 2, then the next four (second Calgary) have RIO
	 * node ID 3, the next four (third Calgary) have node ID 2 again, etc.
	 * We use a gross hack - relying on the dev->bus->number ordering,
	 * modulo 14 - to decide which Calgary a given bus is on. Busses 0, 1,
	 * 2 and 4 are on the first Calgary (id 2), 6, 8, a and c are on the
	 * second (id 3), and then it repeats modulo 14.
 	 */
	rionodeid = (dev->bus->number % 14 > 4) ? 3 : 2;
	/*
	 * register space address calculation as follows:
	 * FE0MB-8MB*OneBasedChassisNumber+1MB*(RioNodeId-ChassisBase)
	 * ChassisBase is always zero for x366/x260/x460
	 * RioNodeId is 2 for first Calgary, 3 for second Calgary
	 */
	address = START_ADDRESS	-
		(0x800000 * (ONE_BASED_CHASSIS_NUM + dev->bus->number / 14)) +
		(0x100000) * (rionodeid - CHASSIS_BASE);
	return address;
}

static int __init calgary_init_one_nontraslated(struct pci_dev *dev)
{
	dev->sysdata = NULL;
	dev->bus->self = dev;

	return 0;
}

static int __init calgary_init_one(struct pci_dev *dev)
{
	u32 address;
	void __iomem *bbar;
	int ret;

	address = locate_register_space(dev);
	/* map entire 1MB of Calgary config space */
	bbar = ioremap_nocache(address, 1024 * 1024);
	if (!bbar) {
		ret = -ENODATA;
		goto done;
	}

	ret = calgary_setup_tar(dev, bbar);
	if (ret)
		goto iounmap;

	dev->bus->self = dev;
	calgary_enable_translation(dev);

	return 0;

iounmap:
	iounmap(bbar);
done:
	return ret;
}

static int __init calgary_init(void)
{
	int i, ret = -ENODEV;
	struct pci_dev *dev = NULL;

	for (i = 0; i < MAX_PHB_BUS_NUM; i++) {
		dev = pci_get_device(PCI_VENDOR_ID_IBM,
				     PCI_DEVICE_ID_IBM_CALGARY,
				     dev);
		if (!dev)
			break;
		if (!translate_phb(dev)) {
			calgary_init_one_nontraslated(dev);
			continue;
		}
		if (!tce_table_kva[dev->bus->number] && !translate_empty_slots) {
			pci_dev_put(dev);
			continue;
		}
		ret = calgary_init_one(dev);
		if (ret)
			goto error;
	}

	return ret;

error:
	for (i--; i >= 0; i--) {
		dev = pci_find_device_reverse(PCI_VENDOR_ID_IBM,
					      PCI_DEVICE_ID_IBM_CALGARY,
					      dev);
		if (!translate_phb(dev)) {
			pci_dev_put(dev);
			continue;
		}
		if (!tce_table_kva[dev->bus->number] && !translate_empty_slots)
			continue;
		calgary_disable_translation(dev);
		calgary_free_tar(dev);
		pci_dev_put(dev);
	}

	return ret;
}

static inline int __init determine_tce_table_size(u64 ram)
{
	int ret;

	if (specified_table_size != TCE_TABLE_SIZE_UNSPECIFIED)
		return specified_table_size;

	/*
	 * Table sizes are from 0 to 7 (TCE_TABLE_SIZE_64K to
	 * TCE_TABLE_SIZE_8M). Table size 0 has 8K entries and each
	 * larger table size has twice as many entries, so shift the
	 * max ram address by 13 to divide by 8K and then look at the
	 * order of the result to choose between 0-7.
	 */
	ret = get_order(ram >> 13);
	if (ret > TCE_TABLE_SIZE_8M)
		ret = TCE_TABLE_SIZE_8M;

	return ret;
}

void __init detect_calgary(void)
{
	u32 val;
	int bus;
	void *tbl;
	int calgary_found = 0;
	int phb = -1;

	/*
	 * if the user specified iommu=off or iommu=soft or we found
	 * another HW IOMMU already, bail out.
	 */
	if (swiotlb || no_iommu || iommu_detected)
		return;

	specified_table_size = determine_tce_table_size(end_pfn * PAGE_SIZE);

	for (bus = 0; bus < MAX_PHB_BUS_NUM; bus++) {
		int dev;

		tce_table_kva[bus] = NULL;
		bus_to_phb[bus] = -1;

		if (read_pci_config(bus, 0, 0, 0) != PCI_VENDOR_DEVICE_ID_CALGARY)
			continue;

		/*
		 * There are 4 PHBs per Calgary chip.  Set phb to which phb (0-3)
		 * it is connected to releative to the clagary chip.
		 */
		phb = (phb + 1) % PHBS_PER_CALGARY;

		if (test_bit(bus, translation_disabled)) {
			printk(KERN_INFO "Calgary: translation is disabled for "
			       "PHB 0x%x\n", bus);
			/* skip this phb, don't allocate a tbl for it */
			continue;
		}
		/*
		 * Scan the slots of the PCI bus to see if there is a device present.
		 * The parent bus will be the zero-ith device, so start at 1.
		 */
		for (dev = 1; dev < 8; dev++) {
			val = read_pci_config(bus, dev, 0, 0);
			if (val != 0xffffffff || translate_empty_slots) {
				tbl = alloc_tce_table();
				if (!tbl)
					goto cleanup;
				tce_table_kva[bus] = tbl;
				bus_to_phb[bus] = phb;
				calgary_found = 1;
				break;
			}
		}
	}

	if (calgary_found) {
		iommu_detected = 1;
		calgary_detected = 1;
		printk(KERN_INFO "PCI-DMA: Calgary IOMMU detected. "
		       "TCE table spec is %d.\n", specified_table_size);
	}
	return;

cleanup:
	for (--bus; bus >= 0; --bus)
		if (tce_table_kva[bus])
			free_tce_table(tce_table_kva[bus]);
}

int __init calgary_iommu_init(void)
{
	int ret;

	if (no_iommu || swiotlb)
		return -ENODEV;

	if (!calgary_detected)
		return -ENODEV;

	/* ok, we're trying to use Calgary - let's roll */
	printk(KERN_INFO "PCI-DMA: Using Calgary IOMMU\n");

	ret = calgary_init();
	if (ret) {
		printk(KERN_ERR "PCI-DMA: Calgary init failed %d, "
		       "falling back to no_iommu\n", ret);
		if (end_pfn > MAX_DMA32_PFN)
			printk(KERN_ERR "WARNING more than 4GB of memory, "
					"32bit PCI may malfunction.\n");
		return ret;
	}

	force_iommu = 1;
	dma_ops = &calgary_dma_ops;

	return 0;
}

static int __init calgary_parse_options(char *p)
{
	unsigned int bridge;
	size_t len;
	char* endp;

	while (*p) {
		if (!strncmp(p, "64k", 3))
			specified_table_size = TCE_TABLE_SIZE_64K;
		else if (!strncmp(p, "128k", 4))
			specified_table_size = TCE_TABLE_SIZE_128K;
		else if (!strncmp(p, "256k", 4))
			specified_table_size = TCE_TABLE_SIZE_256K;
		else if (!strncmp(p, "512k", 4))
			specified_table_size = TCE_TABLE_SIZE_512K;
		else if (!strncmp(p, "1M", 2))
			specified_table_size = TCE_TABLE_SIZE_1M;
		else if (!strncmp(p, "2M", 2))
			specified_table_size = TCE_TABLE_SIZE_2M;
		else if (!strncmp(p, "4M", 2))
			specified_table_size = TCE_TABLE_SIZE_4M;
		else if (!strncmp(p, "8M", 2))
			specified_table_size = TCE_TABLE_SIZE_8M;

		len = strlen("translate_empty_slots");
		if (!strncmp(p, "translate_empty_slots", len))
			translate_empty_slots = 1;

		len = strlen("disable");
		if (!strncmp(p, "disable", len)) {
			p += len;
			if (*p == '=')
				++p;
			if (*p == '\0')
				break;
			bridge = simple_strtol(p, &endp, 0);
			if (p == endp)
				break;

			if (bridge < MAX_PHB_BUS_NUM) {
				printk(KERN_INFO "Calgary: disabling "
				       "translation for PHB 0x%x\n", bridge);
				set_bit(bridge, translation_disabled);
			}
		}

		p = strpbrk(p, ",");
		if (!p)
			break;

		p++; /* skip ',' */
	}
	return 1;
}
__setup("calgary=", calgary_parse_options);
