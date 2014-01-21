/*
 * arch/powerpc/boot/wii.c
 *
 * Nintendo Wii bootwrapper support
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <stddef.h>
#include "stdio.h"
#include "types.h"
#include "io.h"
#include "ops.h"

#include "ugecon.h"

BSS_STACK(8192);

#define HW_REG(x)		((void *)(x))

#define EXI_CTRL		HW_REG(0x0d800070)
#define EXI_CTRL_ENABLE		(1<<0)
#define MEM2_TOP		(0x10000000 + 64*1024*1024)
#define FIRMWARE_DEFAULT_SIZE	(12*1024*1024)

#define MEM2_DMA_DEFAULT_SIZE	(512*1024)

static int save_lowmem_stub(void)
{
	void *src, *dst;
	size_t size;
	u32 reg[2];
	u32 v;
	void *devp;

	devp = finddevice("/lowmem-stub");
	if (devp == NULL) {
		printf("lowmem-stub: none\n");
		goto out;
	}

	if (getprop(devp, "reg", reg, sizeof(reg)) != sizeof(reg)) {
		printf("unable to find %s property\n", "reg");
		goto out;
	}
	src = (void *)reg[0];
	size = reg[1];
	if (getprop(devp, "save-area", &v, sizeof(v)) != sizeof(v)) {
		printf("unable to find %s property\n", "save-area");
		goto out;
	}
	dst = (void *)v;

	printf("lowmem-stub: relocating from %08lX to %08lX (%u bytes)\n",
		(unsigned long)src, (unsigned long)dst, size);
	memcpy(dst, src, size);
	flush_cache(dst, size);

	return 0;
out:
	return -1;
}


struct mipc_infohdr {
	char magic[3];
	u8 version;
	u32 mem2_boundary;
	u32 ipc_in;
	size_t ipc_in_size;
	u32 ipc_out;
	size_t ipc_out_size;
};

static int mipc_check_address(u32 pa)
{
	/* only MEM2 addresses */
	if (pa < 0x10000000 || pa > 0x14000000)
		return -EINVAL;
	return 0;
}

static struct mipc_infohdr *mipc_get_infohdr(void)
{
	struct mipc_infohdr **hdrp, *hdr;

	/* 'mini' header pointer is the last word of MEM2 memory */
	hdrp = (struct mipc_infohdr **)0x13fffffc;
	if (mipc_check_address((u32)hdrp)) {
		printf("mini: invalid hdrp %08X\n", (u32)hdrp);
		hdr = NULL;
		goto out;
	}

	hdr = *hdrp;
	if (mipc_check_address((u32)hdr)) {
		printf("mini: invalid hdr %08X\n", (u32)hdr);
		hdr = NULL;
		goto out;
	}
	if (memcmp(hdr->magic, "IPC", 3)) {
		printf("mini: invalid magic\n");
		hdr = NULL;
		goto out;
	}

out:
	return hdr;
}

static int mipc_get_mem2_boundary(u32 *mem2_boundary)
{
	struct mipc_infohdr *hdr;
	int error;

	hdr = mipc_get_infohdr();
	if (!hdr) {
		error = -1;
		goto out;
	}

	if (mipc_check_address(hdr->mem2_boundary)) {
		printf("mini: invalid mem2_boundary %08X\n",
		       hdr->mem2_boundary);
		error = -EINVAL;
		goto out;
	}
	*mem2_boundary = hdr->mem2_boundary;
	error = 0;
out:
	return error;

}

static char tmp_cmdline[COMMAND_LINE_SIZE];

static void mem2_fixups(u32 *top, u32 *reg)
{
	/* ' mem2_dma=' + nnnnnnn + 'K@0x' + aaaaaaaa */
	const int max_param_len = 10 + 7 + 4 + 8;
	void *chosen;
	u32 dma_base, dma_size;
	char *p;

	chosen = finddevice("/chosen");
	if (!chosen)
		fatal("Can't find chosen node\n");

	/* the MEM2 DMA region must fit within MEM2 */
	dma_size = MEM2_DMA_DEFAULT_SIZE;
	if (dma_size > reg[3])
		dma_size = reg[3];
	/* reserve the region from the top of MEM2 */
	*top -= dma_size;
	dma_base = *top;
	printf("mem2_dma: %uk@0x%08x\n", dma_size >> 10, dma_base);

	/*
	 * Finally, add the MEM2 DMA region location information to the
	 * kernel command line. The wii-dma driver will pick this info up.
	 */
	getprop(chosen, "bootargs", tmp_cmdline, COMMAND_LINE_SIZE-1);
	p = strchr(tmp_cmdline, 0);
	if (p - tmp_cmdline + max_param_len >= COMMAND_LINE_SIZE)
		fatal("No space left for mem2_dma param\n");

	sprintf(p, " mem2_dma=%uk@0x%08x", dma_size >> 10, dma_base);
	setprop_str(chosen, "bootargs", tmp_cmdline);
}

static void platform_fixups(void)
{
	void *mem;
	u32 reg[4];
	u32 mem2_boundary;
	int len;
	int error;

	mem = finddevice("/memory");
	if (!mem)
		fatal("Can't find memory node\n");

	/* two ranges of (address, size) words */
	len = getprop(mem, "reg", reg, sizeof(reg));
	if (len != sizeof(reg)) {
		/* nothing to do */
		goto out;
	}

	/* retrieve MEM2 boundary from 'mini' */
	error = mipc_get_mem2_boundary(&mem2_boundary);
	if (error) {
		/* if that fails use a sane value */
		mem2_boundary = MEM2_TOP - FIRMWARE_DEFAULT_SIZE;
	}

	mem2_fixups(&mem2_boundary, reg);

	if (mem2_boundary > reg[2] && mem2_boundary < reg[2] + reg[3]) {
		reg[3] = mem2_boundary - reg[2];
		printf("top of MEM2 @ %08X\n", reg[2] + reg[3]);
		/*
		 * Find again the memory node as it may have changed its
		 * position after adding some non-existing properties.
		 */
		mem = finddevice("/memory");
		setprop(mem, "reg", reg, sizeof(reg));
	}

	/* fixup local memory for EHCI controller */
	mem = NULL;
	while ((mem = find_node_by_compatible(mem,
					       "nintendo,hollywood-usb-ehci"))) {
		if (getprop(mem, "reg", &reg, sizeof(reg)) == sizeof(reg)) {
			mem2_boundary -= reg[3];
			printf("ehci %08X -> %08X\n", reg[2], mem2_boundary);
			reg[2] = mem2_boundary;
			setprop(mem, "reg", &reg, sizeof(reg));
		}
	}

	/* fixup local memory for OHCI controllers */
	mem = NULL;
	while ((mem = find_node_by_compatible(mem,
					       "nintendo,hollywood-usb-ohci"))) {
		if (getprop(mem, "reg", &reg, sizeof(reg)) == sizeof(reg)) {
			mem2_boundary -= reg[3];
			printf("ohci %08X -> %08X\n", reg[2], mem2_boundary);
			reg[2] = mem2_boundary;
			setprop(mem, "reg", &reg, sizeof(reg));
		}
	}

	/* fixup available memory */
	dt_fixup_memory(0, mem2_boundary);
	printf("top of mem @ %08X (%s)\n", mem2_boundary, "final");

out:
	return;
}

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	u32 heapsize = 24*1024*1024 - (u32)_end;

	simple_alloc_init(_end, heapsize, 32, 64);
	fdt_init(_dtb_start);

	/*
	 * 'mini' boots the Broadway processor with EXI disabled.
	 * We need it enabled before probing for the USB Gecko.
	 */
	out_be32(EXI_CTRL, in_be32(EXI_CTRL) | EXI_CTRL_ENABLE);

	if (ug_probe())
		console_ops.write = ug_console_write;

	platform_ops.fixups = platform_fixups;
	save_lowmem_stub();
}

