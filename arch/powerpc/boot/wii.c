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
		goto err_out;
	}

	if (getprop(devp, "reg", reg, sizeof(reg)) != sizeof(reg)) {
		printf("unable to find %s property\n", "reg");
		goto err_out;
	}
	src = (void *)reg[0];
	size = reg[1];
	if (getprop(devp, "save-area", &v, sizeof(v)) != sizeof(v)) {
		printf("unable to find %s property\n", "save-area");
		goto err_out;
	}
	dst = (void *)v;

	printf("lowmem-stub: relocating from %08lX to %08lX (%u bytes)\n",
		(unsigned long)src, (unsigned long)dst, size);
	memcpy(dst, src, size);
	flush_cache(dst, size);

	return 0;
err_out:
	return -1;
}

/*
 *
 *
 */

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
	if (pa < 0x10000000 || pa > 0x14000000)
		return -EINVAL;
	return 0;
}

static void platform_fixups(void)
{
	struct mipc_infohdr **hdrp, *hdr;
	u32 reg[4];
	u32 mem2_boundary, top;
	void *devp;

	/*
	 * The mini header pointer is specified in the second "reg" entry
	 * of the starlet-mini-ipc node.
	 */
	devp = find_node_by_compatible(NULL, "twiizers,starlet-mini-ipc");
	if (!devp) {
		printf("unable to find %s node\n", "twiizers,starlet-mini-ipc");
		goto err_out;
	}
	if (getprop(devp, "reg", &reg, sizeof(reg)) != sizeof(reg)) {
		printf("unable to find %s property\n", "reg");
		goto err_out;
	}
	hdrp = (struct mipc_infohdr **)reg[2];
	if (mipc_check_address((u32)hdrp)) {
		printf("mini: invalid hdrp %08X\n", (u32)hdrp);
		goto err_out;
	}

	hdr = *hdrp;
	if (mipc_check_address((u32)hdr)) {
		printf("mini: invalid hdr %08X\n", (u32)hdr);
		goto err_out;
	}
	if (memcmp(hdr->magic, "IPC", 3)) {
		printf("mini: invalid magic, asuming ios\n");
		goto err_out;
	}
	mem2_boundary = hdr->mem2_boundary;
	if (mipc_check_address(mem2_boundary)) {
		printf("mini: invalid mem2_boundary %08X\n", mem2_boundary);
		goto err_out;
	}

	top = mem2_boundary;
	printf("top of mem @ %08X (%s)\n", top, "current");

	/* fixup local memory for EHCI controller */
	devp = NULL;
	while ((devp = find_node_by_compatible(devp,
					       "nintendo,hollywood-usb-ehci"))) {
		if (getprop(devp, "reg", &reg, sizeof(reg)) == sizeof(reg)) {
			top -= reg[3];
			printf("ehci %08X -> %08X\n", reg[2], top);
			reg[2] = top;
			setprop(devp, "reg", &reg, sizeof(reg));
		}
	}

	/* fixup local memory for OHCI controllers */
	devp = NULL;
	while ((devp = find_node_by_compatible(devp,
					       "nintendo,hollywood-usb-ohci"))) {
		if (getprop(devp, "reg", &reg, sizeof(reg)) == sizeof(reg)) {
			top -= reg[3];
			printf("ohci %08X -> %08X\n", reg[2], top);
			reg[2] = top;
			setprop(devp, "reg", &reg, sizeof(reg));
		}
	}

	/* fixup available memory */
	dt_fixup_memory(0, top);

	printf("top of mem @ %08X (%s)\n", top, "final");

	return;

err_out:
	return;
}

/*
 *
 */
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

