/*
 * arch/powerpc/platforms/embedded6xx/wii-dma.c
 *
 * DMA functions for the Nintendo Wii video game console.
 * Copyright (C) 2010 The GameCube Linux Team
 * Copyright (C) 2010 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/*
 * The Nintendo Wii video game console is a NOT_COHERENT_CACHE
 * platform that is unable to safely perform non-32 bit uncached writes
 * to RAM because the byte enables are not connected to the bus.
 * Thus, in this platform, "coherent" DMA buffers cannot be directly used
 * by the kernel code unless it guarantees that all write accesses
 * to said buffers are done in 32 bit chunks.
 *
 * In addition, some of the devices in the "Hollywood" chipset have a
 * similar restriction regarding DMA transfers: those with non-32bit
 * aligned lengths only work when performed to/from the second contiguous
 * region of memory (known as MEM2).
 *
 * To solve these issues a specific set of dma mapping operations is made
 * available for devices requiring it. When enabled, the kernel will make
 * sure that DMA buffers sitting in MEM1 get bounced to/from DMA buffers
 * allocated from MEM2.
 *
 * Bouncing is performed with the help of the swiotlb support.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/kernel.h>
#include <linux/bootmem.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/lmb.h>
#include <asm/wii.h>

#include "mm/mmu_decl.h"

/*
 * The mem2_dma "device".
 *
 * This device "owns" a pool of coherent MEM2 memory that can be shared among
 * several devices requiring MEM2 DMA buffers, instead of dedicating specific
 * pools for each device.
 *
 * A device can use the shared coherent MEM2 memory pool by calling
 * wii_set_mem2_dma_constraints().
 *
 */

struct mem2_dma {
	struct platform_device *pdev;

	dma_addr_t dma_base;
	void *base;
	size_t size;
};

static struct mem2_dma mem2_dma_instance;

static inline struct mem2_dma *mem2_dma_get_instance(void)
{
	return &mem2_dma_instance;
}

static int __init mem2_dma_init(dma_addr_t dma_base, size_t size)
{
	struct mem2_dma *mem2_dma = mem2_dma_get_instance();
	const int flags = DMA_MEMORY_MAP | DMA_MEMORY_EXCLUSIVE;
	struct device *dev;
	int error = 0;

	mem2_dma->pdev = platform_device_register_simple("mem2_dma",
							 0, NULL, 0);
	if (IS_ERR(mem2_dma->pdev)) {
		error = PTR_ERR(mem2_dma->pdev);
		pr_err("error %d registering platform device\n", error);
		goto err_pdev_register;
	}
	dev = &mem2_dma->pdev->dev;

	if (!dma_declare_coherent_memory(dev, dma_base, dma_base,
					 size, flags)) {
		dev_err(dev, "error declaring coherent memory %zu@%Lx\n",
			size, (unsigned long long)dma_base);
		error = -EBUSY;
		goto err_declare_coherent;
	}
	mem2_dma->dma_base = dma_base;
	mem2_dma->size = size;
	dev_info(dev, "using %zu KiB at 0x%Lx\n", size / 1024,
		 (unsigned long long)dma_base);
	goto out;

err_declare_coherent:
	platform_device_unregister(mem2_dma->pdev);
err_pdev_register:
	mem2_dma->pdev = NULL;
out:
	return error;
}

static __initdata dma_addr_t dma_base, dma_size;

static int __init mem2_setup_parse(char *str)
{
	char *p, *q = str;

	dma_size = memparse(q, &p);
	if (p == q) {
		pr_err("dma_size expected\n");
		return -EINVAL;
	}
	if (*p != '@') {
		pr_err("missing @ separator between dma_size and dma_base\n");
		return -EINVAL;
	}

	q = p + 1;
	dma_base = memparse(q, &p);
	if (p == q) {
		pr_err("dma_base expected\n");
		return -EINVAL;
	}

	return 0;
}
__setup("mem2_dma=", mem2_setup_parse);

static int __init mem2_dma_setup(void)
{
	int error;

	BUG_ON(dma_base < lmb_end_of_DRAM());
	BUG_ON(dma_size == 0);

	error = mem2_dma_init(dma_base, dma_size);
	if (error)
		pr_err("error %d during setup\n", error);
	return error;
}
arch_initcall(mem2_dma_setup);

/**
 * wii_mem2_dma_dev() - returns the device "owning" the shared MEM2 DMA region
 *
 * Use this function to retrieve the device for which the shared pool of
 * coherent MEM2 memory has been registered.
 */
static struct device *wii_mem2_dma_dev(void)
{
	struct mem2_dma *mem2_dma = mem2_dma_get_instance();
	BUG_ON(!mem2_dma->pdev);
	return &mem2_dma->pdev->dev;
}

/**
 * wii_set_mem2_dma_constraints() - forces device to use MEM2 DMA buffers only
 * @dev:	device for which DMA constraints are defined
 *
 * Instructs device @dev to always use MEM2 DMA buffers for DMA transfers.
 */
int wii_set_mem2_dma_constraints(struct device *dev)
{
	struct dev_archdata *sd;

	sd = &dev->archdata;
	sd->max_direct_dma_addr = 0;
	sd->min_direct_dma_addr = wii_hole_start + wii_hole_size;

	set_dma_ops(dev, &wii_mem2_dma_ops);
	return 0;
}
EXPORT_SYMBOL_GPL(wii_set_mem2_dma_constraints);

/**
 * wii_clear_mem2_dma_constraints() - clears device MEM2 DMA constraints
 * @dev:	device for which DMA constraints are cleared
 *
 * Instructs device @dev to stop using MEM2 DMA buffers for DMA transfers.
 * Must be called to undo wii_set_mem2_dma_constraints().
 */
void wii_clear_mem2_dma_constraints(struct device *dev)
{
	struct dev_archdata *sd;

	sd = &dev->archdata;
	sd->max_direct_dma_addr = 0;
	sd->min_direct_dma_addr = 0;

	set_dma_ops(dev, &dma_direct_ops);
}
EXPORT_SYMBOL_GPL(wii_clear_mem2_dma_constraints);

/*
 * swiotlb-based DMA ops for MEM2-only devices on the Wii.
 *
 */

/*
 * Allocate the SWIOTLB from MEM2.
 */
void * __init swiotlb_alloc_boot(size_t size, unsigned long nslabs)
{
	return __alloc_bootmem_low(size, PAGE_SIZE,
				   wii_hole_start + wii_hole_size);
}

/*
 * Bounce: copy the swiotlb buffer back to the original dma location
 * This is a platform specific version replacing the generic __weak version.
 */
void swiotlb_bounce(phys_addr_t phys, char *dma_buf, size_t size,
		    enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(phys);

	if (dir == DMA_TO_DEVICE) {
		memcpy(dma_buf, vaddr, size);
		__dma_sync(dma_buf, size, dir);
	} else {
		__dma_sync(dma_buf, size, dir);
		memcpy(vaddr, dma_buf, size);
	}
}

static dma_addr_t
mem2_virt_to_bus(struct device *dev, void *address)
{
	return phys_to_dma(dev, virt_to_phys(address));
}

static int
mem2_dma_mapping_error(struct device *dev, dma_addr_t dma_handle)
{
	return !dma_handle;
}

static int
mem2_dma_supported(struct device *dev, u64 mask)
{
	return mem2_virt_to_bus(dev, swiotlb_tbl_start - 1 +
				(swiotlb_tbl_nslabs << IO_TLB_SHIFT)) <= mask;
}

/*
 * Determines if a given DMA region specified by @dma_handle
 * requires bouncing.
 *
 * Bouncing is required if the DMA region falls within MEM1.
 */
static int mem2_needs_dmabounce(dma_addr_t dma_handle)
{
	return dma_handle < wii_hole_start;
}

/*
 * Use the dma_direct_ops hooks for allocating and freeing coherent memory
 * from the MEM2 DMA region.
 */

static void *mem2_alloc_coherent(struct device *dev, size_t size,
				 dma_addr_t *dma_handle, gfp_t gfp)
{
	void *vaddr;

	vaddr = dma_direct_ops.alloc_coherent(wii_mem2_dma_dev(), size,
					      dma_handle, gfp);
	if (vaddr && mem2_needs_dmabounce(*dma_handle)) {
		dma_direct_ops.free_coherent(wii_mem2_dma_dev(), size, vaddr,
					     *dma_handle);
		dev_err(dev, "failed to allocate MEM2 coherent memory\n");
		vaddr = NULL;
	}
	return vaddr;
}

static void mem2_free_coherent(struct device *dev, size_t size,
			       void *vaddr, dma_addr_t dma_handle)
{
	dma_direct_ops.free_coherent(wii_mem2_dma_dev(), size, vaddr,
				     dma_handle);
}

/*
 * Maps (part of) a page so it can be safely accessed by a device.
 *
 * Calls the corresponding dma_direct_ops hook if the page region falls
 * within MEM2.
 * Otherwise, a bounce buffer allocated from MEM2 coherent memory is used.
 */
static dma_addr_t
mem2_map_page(struct device *dev, struct page *page, unsigned long offset,
	      size_t size, enum dma_data_direction dir,
	      struct dma_attrs *attrs)
{
	phys_addr_t phys = page_to_phys(page) + offset;
	dma_addr_t dma_handle = phys_to_dma(dev, phys);
	dma_addr_t swiotlb_start_dma;
	void *map;

	BUG_ON(dir == DMA_NONE);

	if (dma_capable(dev, dma_handle, size) && !swiotlb_force) {
		return dma_direct_ops.map_page(dev, page, offset, size,
					       dir, attrs);
	}

	swiotlb_start_dma = mem2_virt_to_bus(dev, swiotlb_tbl_start);
	map = swiotlb_tbl_map_single(dev, phys, swiotlb_start_dma, size, dir);
	if (!map) {
		swiotlb_full(dev, size, dir, 1);
		return 0;
	}

	dma_handle = mem2_virt_to_bus(dev, map);
	BUG_ON(!dma_capable(dev, dma_handle, size));

	return dma_handle;
}

/*
 * Unmaps (part of) a page previously mapped.
 *
 * Calls the corresponding dma_direct_ops hook if the DMA region associated
 * to the dma handle @dma_handle wasn't bounced.
 * Otherwise, the associated bounce buffer is de-bounced.
 */
static void
mem2_unmap_page(struct device *dev, dma_addr_t dma_handle, size_t size,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	swiotlb_unmap_page(dev, dma_handle, size, dir, attrs);
}

/*
 * Unmaps a scatter/gather list by unmapping each entry.
 */
static void
mem2_unmap_sg(struct device *dev, struct scatterlist *sgl, int nents,
	      enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		mem2_unmap_page(dev, sg->dma_address, sg->length, dir, attrs);
}

/*
 * Maps a scatter/gather list by mapping each entry.
 */
static int
mem2_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
	    enum dma_data_direction dir, struct dma_attrs *attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		sg->dma_length = sg->length;
		sg->dma_address = mem2_map_page(dev, sg_page(sg), sg->offset,
						sg->length, dir, attrs);
		if (mem2_dma_mapping_error(dev, sg->dma_address)) {
			mem2_unmap_sg(dev, sgl, i, dir, attrs);
			nents = 0;
			sgl[nents].dma_length = 0;
			pr_debug("%s: mem2_map_page error\n", __func__);
			break;
		}
	}
	return nents;
}

/*
 * The sync functions synchronize streaming mode DMA translations
 * making physical memory consistent before/after a DMA transfer.
 *
 * They call the corresponding dma_direct_ops hook if the DMA region
 * associated to the dma handle @dma_handle wasn't bounced.
 * Otherwise, original DMA buffers and their matching bounce buffers are put
 * in sync.
 */

static int
mem2_sync_range(struct device *dev, dma_addr_t dma_handle,
		unsigned long offset, size_t size, int dir, int target)
{
	phys_addr_t paddr = dma_to_phys(dev, dma_handle) + offset;
	void *vaddr = phys_to_virt(paddr);

	BUG_ON(dir == DMA_NONE);

	if (is_swiotlb_buffer(paddr)) {
		swiotlb_tbl_sync_single(dev, vaddr, size, dir, target);
		return 1;
	}
	return 0;
}

static void
mem2_sync_range_for_cpu(struct device *dev, dma_addr_t dma_handle,
			unsigned long offset, size_t size,
			enum dma_data_direction dir)
{
	int done = mem2_sync_range(dev, dma_handle, offset, size, dir,
				   SYNC_FOR_CPU);
	if (!done) {
		dma_direct_ops.sync_single_range_for_cpu(dev, dma_handle,
							 offset, size, dir);
	}
}

static void
mem2_sync_range_for_device(struct device *dev, dma_addr_t dma_handle,
			   unsigned long offset, size_t size,
			   enum dma_data_direction dir)
{
	int done = mem2_sync_range(dev, dma_handle, offset, size, dir,
				   SYNC_FOR_DEVICE);
	if (!done) {
		dma_direct_ops.sync_single_range_for_device(dev, dma_handle,
							    offset, size, dir);
	}
}

static void
mem2_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl, int nents,
		     enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		mem2_sync_range_for_cpu(dev, sg_dma_address(sg), sg->offset,
					sg_dma_len(sg), dir);
	}
}

static void
mem2_sync_sg_for_device(struct device *dev, struct scatterlist *sgl, int nents,
			enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		mem2_sync_range_for_device(dev, sg_dma_address(sg), sg->offset,
					   sg_dma_len(sg), dir);
	}
}

/*
 * Set of DMA operations for devices requiring MEM2 DMA buffers.
 */
struct dma_map_ops wii_mem2_dma_ops = {
	.alloc_coherent = mem2_alloc_coherent,
	.free_coherent = mem2_free_coherent,
	.map_sg = mem2_map_sg,
	.unmap_sg = mem2_unmap_sg,
	.dma_supported = mem2_dma_supported,
	.map_page = mem2_map_page,
	.unmap_page = mem2_unmap_page,
	.sync_single_range_for_cpu = mem2_sync_range_for_cpu,
	.sync_single_range_for_device = mem2_sync_range_for_device,
	.sync_sg_for_cpu = mem2_sync_sg_for_cpu,
	.sync_sg_for_device = mem2_sync_sg_for_device,
	.mapping_error = mem2_dma_mapping_error,
};
