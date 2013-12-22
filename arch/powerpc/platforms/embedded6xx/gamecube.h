/*
 * arch/powerpc/platforms/embedded6xx/gamecube.h
 *
 * Nintendo GameCube board-specific definitions
 * Copyright (C) 2004-2008 The GameCube Linux Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __MACH_GAMECUBE_H
#define __MACH_GAMECUBE_H

#define GCN_IO1_PHYS_BASE	0x0c000000
#ifdef CONFIG_GAMECUBE_WII
#define GCN_IO2_PHYS_BASE	0x0d000000
#else
#define GCN_IO2_PHYS_BASE	0x0c000000
#endif
#define GCN_IO1_BASE	(0xc0000000 | GCN_IO1_PHYS_BASE)
#define GCN_IO2_BASE	(0xc0000000 | GCN_IO2_PHYS_BASE)

/*
 * Each interrupt has a corresponding bit in both
 * the Interrupt Cause (ICR) and Interrupt Mask (IMR) registers.
 *
 * Enabling/disabling an interrupt line involves asserting/clearing
 * the corresponding bit in IMR. ACK'ing a request simply involves
 * asserting the corresponding bit in ICR.
 */
#ifdef CONFIG_GAMECUBE_WII
#define FLIPPER_NR_IRQS		(15)
#else
#define FLIPPER_NR_IRQS		(14)
#endif
#define FLIPPER_ICR		((void __iomem *)(GCN_IO1_BASE+0x3000))
#define FLIPPER_IMR		((void __iomem *)(GCN_IO1_BASE+0x3004))

/*
 * Anything written here automagically puts us through reset.
 */
#define FLIPPER_RESET		((void __iomem *)(GCN_IO1_BASE+0x3024))

/*
 * This is the current memory layout for the GameCube Linux port.
 *
 *   +------------------------------+ 
 *   | framebuffer  640x576x2 bytes | GCN_XFB_END
 *   .                              .
 *   .                              .
 *   | framebuffer  640x576x2 bytes | Second buffer
 *   .                              .
 *   .                              .
 *   +------------------------------+ GCN_XFB_START
 *   | GX FIFO reserved 256k        | GCN_GX_FIFO_END
 *   .                              . 
 *   +------------------------------+ GCN_GX_FIFO_START
 *   | kexec reserved  4x4096 bytes | GCN_KXC_END
 *   .                              .
 *   +------------------------------+ GCN_KXC_START
 *   | memory       remaining bytes | GCN_MEM_END
 *   .                              .
 *   .                              .
 *   .                              .
 *   +- - - - - - - - - - - - - - - + 
 *   | Dolphin OS       12544 bytes |
 *   | globals, pre-kernel          |
 *   |                              |
 *   |                              |
 *   +------------------------------+ GCN_MEM_START
 *
 */

/*
 * XXX
 * It seems not a good idea to hot change the memory map by simply
 * changing a video register.
 * Be conservative here, and assume we're using (or will use) the bigger
 * of the two framebuffer sizes supported.
 */
//#define GCN_VIDEO_REG   (*((volatile u16*)0xCC002002))
//#define GCN_VIDEO_LINES (((GCN_VIDEO_REG >> 8) & 3) ? 576 : 480)
#define GCN_VIDEO_LINES 576

/*
 * Total amount of RAM found in the system
 */
#define GCN_RAM_SIZE            (24*1024*1024) /* 24 MB */

/*
 * Size of reserved memory for the video subsystem
 */
#ifdef CONFIG_FB_GAMECUBE
  #define GCN_XFB_SIZE		(2*640*GCN_VIDEO_LINES*2) /* framebuffer */
#else
  #define GCN_XFB_SIZE		(0)
#endif

#ifdef CONFIG_FB_GAMECUBE_GX
  #define GCN_GX_FIFO_SIZE	(256*1024)
#else
  #define GCN_GX_FIFO_SIZE	(0)
#endif

/*
 * Size of reserved memory for kexec compatibility with some homebrew DOLs
 */
#ifdef CONFIG_KEXEC
  #define GCN_KXC_SIZE          (4*4096) /* PAGE_ALIGN(GCN_PRESERVE_SIZE) */
#else
  #define GCN_KXC_SIZE          (0)
#endif

/*
 * Amount of useable memory
 */
#define GCN_MEM_SIZE            (GCN_MEM_END+1)

/*
 * Start and end of several regions
 */
#define GCN_XFB_END             (GCN_RAM_SIZE-1)
#define GCN_XFB_START           (GCN_XFB_END-GCN_XFB_SIZE+1)
#define GCN_GX_FIFO_END         (GCN_XFB_START-1)
#define GCN_GX_FIFO_START       (GCN_GX_FIFO_END-GCN_GX_FIFO_SIZE+1)
#define GCN_KXC_END             (GCN_GX_FIFO_START-1)
#define GCN_KXC_START           (GCN_KXC_END-GCN_KXC_SIZE+1)
#define GCN_MEM_END             (GCN_KXC_START-1)
#define GCN_MEM_START           (0x00000000)

/*
 * Some memory regions will be preserved across kexec reboots, if enabled.
 */
#define GCN_PRESERVE_START      (0x00000000)
#define GCN_PRESERVE_END        (0x000030ff)
#define GCN_PRESERVE_FROM       (GCN_PRESERVE_START)
#define GCN_PRESERVE_TO         (GCN_KXC_START)
#define GCN_PRESERVE_SIZE       (GCN_PRESERVE_END+1)

/*
 * These registers control where the visible framebuffer is located.
 */
#define GCN_VI_TFBL		((void __iomem *)(GCN_IO1_BASE+0x201c))
#define GCN_VI_BFBL		((void __iomem *)(GCN_IO1_BASE+0x2024))


/* arch/ppc/platforms/gcn-time.c */
extern long gcn_time_init(void);
extern unsigned long gcn_get_rtc_time(void);
extern int gcn_set_rtc_time(unsigned long nowtime);

/* arch/ppc/platforms/gcn-con.c */
extern void gcn_con_init(void);

#endif /* !__MACH_GAMECUBE_H */
