/*
 * arch/ppc/platforms/gcn-dvdcover.c
 *
 * Nintendo GameCube DVD cover driver
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004 Stefan Esser
 * Copyright (C) 2004,2005 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/io.h>

#define DVD_IRQ		     2

/* DI Status Register */
#define DI_DISR              ((void __iomem *)0xcc006000)
#define  DI_DISR_BRKINT      (1<<6)
#define  DI_DISR_BRKINTMASK  (1<<5)
#define  DI_DISR_TCINT       (1<<4)
#define  DI_DISR_TCINTMASK   (1<<3)
#define  DI_DISR_DEINT       (1<<2)
#define  DI_DISR_DEINTMASK   (1<<1)
#define  DI_DISR_BRK         (1<<0)

/* DI Cover Register */
#define DI_DICVR             ((void __iomem *)0xcc006004)
#define  DI_DICVR_CVRINT     (1<<2)
#define  DI_DICVR_CVRINTMASK (1<<1)
#define  DI_DICVR_CVR        (1<<0)

/* DI Command Buffer 0 */
#define DI_DICMDBUF0         ((void __iomem *)0xcc006008)

/* DI Control Register */
#define DI_DICR              ((void __iomem *)0xcc00601c)
#define  DI_DICR_RW          (1<<2)
#define  DI_DICR_DMA         (1<<1)
#define  DI_DICR_TSTART      (1<<0)

#define DI_CMD_STOP          (0xE3)


#define DRV_MODULE_NAME      "gcn-dvdcover"
#define DRV_DESCRIPTION      "Nintendo GameCube DVD cover driver"
#define DRV_AUTHOR           "Stefan Esser <se@nopiracy.de>"

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

#define PFX DRV_MODULE_NAME ": "
#define di_printk(level, format, arg...) \
	printk(level PFX format , ## arg)

static int gcn_dvdcover_init(void)
{
	unsigned long outval;

	/* clear pending DI interrupts and mask new ones */
	/* this prevents an annoying bug while we lack a complete DVD driver */
	outval = DI_DISR_BRKINT | DI_DISR_TCINT | DI_DISR_DEINT;
	outval &= ~(DI_DISR_BRKINTMASK | DI_DISR_TCINTMASK | DI_DISR_DEINTMASK);
	writel(outval, DI_DISR);

	/* stop DVD motor */
	writel(DI_CMD_STOP << 24, DI_DICMDBUF0);
	writel(DI_DICR_TSTART, DI_DICR);

	return 0;
}

/**
 *
 */
static void gcn_dvdcover_exit(void)
{
	
}

module_init(gcn_dvdcover_init);
module_exit(gcn_dvdcover_exit);

