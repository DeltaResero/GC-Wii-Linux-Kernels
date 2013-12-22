/*
 * arch/ppc/platforms/gamecube.c
 *
 * Nintendo GameCube board-specific support
 * Copyright (C) 2004-2008 The GameCube Linux Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/initrd.h>
#include <linux/seq_file.h>

#include <asm/io.h>
#include <asm/time.h>
#include <asm/bitops.h>
#include <asm/bootinfo.h>
#include <asm/machdep.h>
#include <asm/pgtable.h>

#include "gamecube.h"

#if 0
/*
 * include/asm-ppc/io.h assumes everyone else that is not APUS provides
 * these.  Since we don't have either PCI or ISA busses, these are only
 * here so things actually compile.
 */
unsigned long isa_io_base = 0;
unsigned long isa_mem_base = 0;
unsigned long pci_dram_offset = 0;
#endif

/*
 * These are used in setup_arch. *
 */
#define CSR_REG			((void __iomem *)(GCN_IO1_BASE+0x500A))
#define  DSP_CSR_RES		(1<<0)
#define  DSP_CSR_PIINT		(1<<1)
#define  DSP_CSR_HALT		(1<<2)
#define  DSP_CSR_AIDINT		(1<<3)
#define  DSP_CSR_AIDINTMASK	(1<<4)
#define  DSP_CSR_ARINT		(1<<5)
#define  DSP_CSR_ARINTMASK	(1<<6)
#define  DSP_CSR_DSPINT		(1<<7)
#define  DSP_CSR_DSPINTMASK	(1<<8)
#define  DSP_CSR_DSPDMA		(1<<9)
#define  DSP_CSR_RESETXXX	(1<<11)

#define AUDIO_DMA_LENGTH	((void __iomem *)(GCN_IO1_BASE+0x5036))
#define  AI_DCL_PLAY		(1<<15)

static unsigned long __init gamecube_find_end_of_memory(void)
{
	return GCN_MEM_SIZE;
}

static void __init gamecube_map_io(void)
{
#ifdef CONFIG_GAMECUBE_CONSOLE
	io_block_mapping(0xd0000000, 0, 0x02000000, _PAGE_IO);
#endif

	/* access to hardware registers */
	io_block_mapping(GCN_IO1_BASE, GCN_IO1_PHYS_BASE, 0x00100000, _PAGE_IO);
#if GCN_IO1_BASE != GCN_IO2_BASE
	io_block_mapping(GCN_IO2_BASE, GCN_IO2_PHYS_BASE, 0x00100000, _PAGE_IO);
#endif
}

static void __init gamecube_calibrate_decr(void)
{
	int freq, divisor;
	freq = 162000000;
	divisor = 4;
	tb_ticks_per_jiffy = freq / HZ / divisor;
	tb_to_us = mulhwu_scale_factor(freq/divisor, 1000000);
}

static void __init gamecube_setup_arch(void)
{
#ifdef CONFIG_GAMECUBE_CONSOLE
	gcn_con_init();
#endif

	/* On my North American Launch cube booted
	 * via PSO, I get a flooding of ARAM interrupts and audio MADNESS 
	 * when I first boot.  By clearing the AI interrupts and stopping 
	 * audio, it goes away and I can boot normally.
	 */

	/* ack and clear the interrupts for the AI line */
	out_be16(CSR_REG,
		 DSP_CSR_PIINT|DSP_CSR_AIDINT|DSP_CSR_ARINT|DSP_CSR_DSPINT);
	/* stop any audio */
	out_be16(AUDIO_DMA_LENGTH,
		 in_be16(AUDIO_DMA_LENGTH) & ~AI_DCL_PLAY);
}

static void gamecube_restart(char *cmd)
{
	local_irq_disable();
	out_8(FLIPPER_RESET, 0x00);
}

static void gamecube_power_off(void)
{
	local_irq_disable();
	for (;;); /* spin until power button pressed */
}

static void gamecube_halt(void)
{
	gamecube_restart(NULL);
}

static int gamecube_get_irq(void)
{
	int irq;
	u32 irq_status;

	irq_status = in_be32(FLIPPER_ICR) & in_be32(FLIPPER_IMR);
	if (irq_status == 0)
		return -1;	/* no more IRQs pending */

        __asm __volatile ("cntlzw %0,%1": "=r"(irq) : "r"(irq_status));

	return (31 - irq);
}

static void flipper_mask_and_ack_irq(unsigned int irq)
{
	clear_bit(irq, FLIPPER_IMR);
	set_bit(irq, FLIPPER_ICR);
}

static void flipper_mask_irq(unsigned int irq)
{
	clear_bit(irq, FLIPPER_IMR);
}

static void flipper_unmask_irq(unsigned int irq)
{
	set_bit(irq, FLIPPER_IMR);
}

static void flipper_end_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS))
	    && irq_desc[irq].action)
		flipper_unmask_irq(irq);
}

static struct hw_interrupt_type flipper_pic = {
	.typename	= "flipper-pic",
	.enable		= flipper_unmask_irq,
	.disable	= flipper_mask_irq,
	.ack		= flipper_mask_and_ack_irq,
	.end		= flipper_end_irq,
};

static void __init gamecube_init_IRQ(void)
{
	int i;

	/* mask and ack all IRQs */
	out_be32(FLIPPER_IMR, 0x00000000);
	out_be32(FLIPPER_ICR, 0xffffffff);

	for (i = 0; i < FLIPPER_NR_IRQS; i++)
		irq_desc[i].chip = &flipper_pic;
}

static int gamecube_show_cpuinfo(struct seq_file *m)
{
	seq_printf(m, "vendor\t\t: IBM\n");
#ifdef CONFIG_GAMECUBE_WII
	seq_printf(m, "machine\t\t: Nintendo Wii\n");
#else
	seq_printf(m, "machine\t\t: Nintendo GameCube\n");
#endif

	return 0;
}

#ifdef CONFIG_KEXEC
static void gamecube_shutdown(void)
{
	/* currently not used */
}

static int gamecube_kexec_prepare(struct kimage *image)
{
	return 0;
}
#endif /* CONFIG_KEXEC */

void __init
platform_init(unsigned long r3, unsigned long r4, unsigned long r5,
	      unsigned long r6, unsigned long r7)
{
	parse_bootinfo(find_bootinfo());

#ifdef CONFIG_BLK_DEV_INITRD
	if ( r4 )
	{
		initrd_start = r4 + KERNELBASE;
		initrd_end = r5 + KERNELBASE;
	}
#endif

	ppc_md.find_end_of_memory = gamecube_find_end_of_memory;
	ppc_md.setup_io_mappings = gamecube_map_io;
	ppc_md.calibrate_decr = gamecube_calibrate_decr;
	ppc_md.setup_arch = gamecube_setup_arch;

	ppc_md.show_cpuinfo = gamecube_show_cpuinfo;

	ppc_md.get_irq = gamecube_get_irq;
	ppc_md.init_IRQ = gamecube_init_IRQ;

	ppc_md.restart = gamecube_restart;
	ppc_md.power_off = gamecube_power_off;
	ppc_md.halt = gamecube_halt;

#ifdef CONFIG_KEXEC
	ppc_md.machine_shutdown = gamecube_shutdown;
	ppc_md.machine_kexec_prepare = gamecube_kexec_prepare;
	ppc_md.machine_kexec = machine_kexec_simple;
#endif /* CONFIG_KEXEC */
}
