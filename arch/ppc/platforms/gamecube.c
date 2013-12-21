/*
 * arch/ppc/platforms/gamecube.c
 *
 * Nintendo GameCube board-specific support
 * Copyright (C) 2004-2005 The GameCube Linux Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/config.h>
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

/*
 * include/asm-ppc/io.h assumes everyone else that is not APUS provides
 * these.  Since we don't have either PCI or ISA busses, these are only
 * here so things actually compile.
 */
unsigned long isa_io_base = 0;
unsigned long isa_mem_base = 0;
unsigned long pci_dram_offset = 0;


static unsigned long gamecube_find_end_of_memory(void)
{
	return GCN_MEM_SIZE;
}

static void gamecube_map_io(void)
{
	/* all RAM and more ??? */
	io_block_mapping(0xd0000000, 0, 0x02000000, _PAGE_IO);

	/* access to hardware registers */
	io_block_mapping(0xcc000000, 0x0c000000, 0x00100000, _PAGE_IO);
}

static void gamecube_restart(char *cmd)
{
	local_irq_disable();
	writeb(0x00, FLIPPER_RESET);
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

static void gamecube_calibrate_decr(void)
{
	int freq, divisor;
	freq = 162000000;
	divisor = 4;
	tb_ticks_per_jiffy = freq / HZ / divisor;
	tb_to_us = mulhwu_scale_factor(freq/divisor, 1000000);
}

static int gamecube_get_irq(struct pt_regs *regs)
{
	int irq;
	u32 irq_status;

	irq_status = readl(FLIPPER_ICR) & readl(FLIPPER_IMR);
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
	.typename	= " FLIPPER-PIC ",
	.enable		= flipper_unmask_irq,
	.disable	= flipper_mask_irq,
	.ack		= flipper_mask_and_ack_irq,
	.end		= flipper_end_irq,
};

static void gamecube_init_IRQ(void)
{
	int i;

	/* mask and ack all IRQs */
	writel(0x00000000, FLIPPER_IMR);
	writel(0xffffffff, FLIPPER_ICR);

	for (i = 0; i < FLIPPER_NR_IRQS; i++)
		irq_desc[i].handler = &flipper_pic;
}

static int gamecube_show_cpuinfo(struct seq_file *m)
{
	seq_printf(m, "vendor\t\t: IBM\n");
	seq_printf(m, "machine\t\t: Nintendo GameCube\n");
	seq_printf(m, "cpu MHz\t\t: 486\n");
	seq_printf(m, "clock\t\t: 486MHz\n");
	seq_printf(m, "cache size\t: 256 KB\n");
	seq_printf(m, "bus speed\t: 162 MHz\n");
	seq_printf(m, "mem bus speed\t: 200 MHz\n");
	seq_printf(m, "bus width\t: 64 bit\n");

	return 0;
}

static void gamecube_setup_arch(void)
{
#ifdef CONFIG_GAMECUBE_CONSOLE
#if (GCN_XFB_START <= 0x00fffe00) 
	#error Sorry, debug console needs the framebuffer at a higher address.
#endif
	writel(0x10000000 | (GCN_XFB_START>>5), GCN_VI_TFBL);
	writel(0x10000000 | ((GCN_XFB_START+2*640)>>5), GCN_VI_BFBL);
	gcn_con_init();
#endif
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

	ppc_md.setup_arch = gamecube_setup_arch;
	ppc_md.show_cpuinfo = gamecube_show_cpuinfo;

	ppc_md.init_IRQ = gamecube_init_IRQ;
	ppc_md.get_irq = gamecube_get_irq;

	ppc_md.restart = gamecube_restart;
	ppc_md.power_off = gamecube_power_off;
	ppc_md.halt = gamecube_halt;

	ppc_md.calibrate_decr = gamecube_calibrate_decr;

	ppc_md.find_end_of_memory = gamecube_find_end_of_memory;
	ppc_md.setup_io_mappings = gamecube_map_io;

#ifdef CONFIG_KEXEC
	ppc_md.machine_shutdown = gamecube_shutdown;
	ppc_md.machine_kexec_prepare = gamecube_kexec_prepare;
	ppc_md.machine_kexec = machine_kexec_simple;
#endif /* CONFIG_KEXEC */
}
