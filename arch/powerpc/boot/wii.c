/*
 * arch/powerpc/boot/wii.c
 *
 * Nintendo Wii platform
 * Copyright (C) 2004-2009 The GameCube Linux Team
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

/*
 * We enter with the MMU enabled and some legacy memory mappings active.
 *
 * We leave the MMU enabled, but we switch to an identity mapped memory
 * scheme as expected by the start code.
 *
 */
asm ("\n\
.text\n\
.globl _zimage_start\n\
_zimage_start:\n\
\n\
	mfmsr	9\n\
	andi.	0, 9, (1<<4)|(1<<5) /* MSR_DR|MSR_IR */\n\
	bcl	20,31,1f\n\
1:\n\
	mflr	8\n\
	clrlwi	8, 8, 3		/* convert to a real address */\n\
	addi	8, 8, _mmu_off - 1b\n\
	andc	9, 9, 0\n\
	mtspr	0x01a, 8	/* SRR0 */\n\
	mtspr	0x01b, 9	/* SRR1 */\n\
	sync\n\
	rfi\n\
_mmu_off:\n\
	/* MMU disabled */\n\
\n\
	/* Setup BATs */\n\
	isync\n\
	li      8, 0\n\
	mtspr	0x210, 8	/* IBAT0U */\n\
	mtspr	0x211, 8	/* IBAT0L */\n\
	mtspr	0x212, 8	/* IBAT1U */\n\
	mtspr	0x213, 8	/* IBAT1L */\n\
	mtspr	0x214, 8	/* IBAT2U */\n\
	mtspr	0x215, 8	/* IBAT2L */\n\
	mtspr	0x216, 8	/* IBAT3U */\n\
	mtspr	0x217, 8	/* IBAT3L */\n\
	mtspr	0x218, 8	/* DBAT0U */\n\
	mtspr	0x219, 8	/* DBAT0L */\n\
	mtspr	0x21a, 8	/* DBAT1U */\n\
	mtspr	0x21b, 8	/* DBAT1L */\n\
	mtspr	0x21c, 8	/* DBAT2U */\n\
	mtspr	0x21d, 8	/* DBAT2L */\n\
	mtspr	0x21e, 8	/* DBAT3U */\n\
	mtspr	0x21f, 8	/* DBAT3L */\n\
\n\
	mtspr	0x230, 8	/* IBAT4U */\n\
	mtspr	0x231, 8	/* IBAT4L */\n\
	mtspr	0x232, 8	/* IBAT5U */\n\
	mtspr	0x233, 8	/* IBAT5L */\n\
	mtspr	0x234, 8	/* IBAT6U */\n\
	mtspr	0x235, 8	/* IBAT6L */\n\
	mtspr	0x236, 8	/* IBAT7U */\n\
	mtspr	0x237, 8	/* IBAT7L */\n\
	mtspr	0x238, 8	/* DBAT4U */\n\
	mtspr	0x239, 8	/* DBAT4L */\n\
	mtspr	0x23a, 8	/* DBAT5U */\n\
	mtspr	0x23b, 8	/* DBAT5L */\n\
	mtspr	0x23c, 8	/* DBAT6U */\n\
	mtspr	0x23d, 8	/* DBAT6L */\n\
	mtspr	0x23e, 8	/* DBAT7U */\n\
	mtspr	0x23f, 8	/* DBAT7L */\n\
\n\
	isync\n\
	li	8, 0x01ff	/* first 16MiB */\n\
	li	9, 0x0002	/* rw */\n\
	mtspr	0x210, 8	/* IBAT0U */\n\
	mtspr	0x211, 9	/* IBAT0L */\n\
	mtspr	0x218, 8	/* DBAT0U */\n\
	mtspr	0x219, 9	/* DBAT0L */\n\
\n\
	lis	8, 0xcc00	/* I/O mem */\n\
	ori	8,8, 0x3ff	/* 32MiB */\n\
	lis	9, 0x0c00\n\
	ori	9, 9, 0x002a	/* uncached, guarded, rw */\n\
	mtspr	0x21a, 8	/* DBAT1U */\n\
	mtspr	0x21b, 9	/* DBAT1L */\n\
\n\
	lis	8, 0x0100	/* next 8MiB */\n\
	ori	8, 8, 0x00ff	/* 8MiB */\n\
	lis	9, 0x0100	/* next 8MiB */\n\
	ori	9, 9, 0x0002	/* rw */\n\
	mtspr	0x214, 8	/* IBAT2U */\n\
	mtspr	0x215, 9	/* IBAT2L */\n\
	mtspr	0x21c, 8	/* DBAT2U */\n\
	mtspr	0x21d, 9	/* DBAT2L */\n\
\n\
	sync\n\
	isync\n\
\n\
	li	3, 0\n\
	li	4, 0\n\
	li	5, 0\n\
\n\
	bcl	20,31,1f\n\
1:\n\
	mflr    8\n\
	addi    8, 8, _mmu_on - 1b\n\
	mfmsr	9\n\
	ori	9, 9, (1<<4)|(1<<5) /* MSR_DR|MSR_IR */\n\
	mtspr	0x01a, 8	/* SRR0 */\n\
	mtspr	0x01b, 9	/* SRR1 */\n\
	sync\n\
	rfi\n\
_mmu_on:\n\
	b _zimage_start_lib\n\
");

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
 */
void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	u32 heapsize = 24*1024*1024 - (u32)_end;

	simple_alloc_init(_end, heapsize, 32, 64);
	fdt_init(_dtb_start);

	if (!ug_grab_io_base() && ug_is_adapter_present())
		console_ops.write = ug_console_write;

	save_lowmem_stub();
}

