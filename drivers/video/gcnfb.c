/*
 * drivers/video/gcnfb.c
 *
 * Nintendo GameCube "Flipper" chipset frame buffer driver
 * Copyright (C) 2004-2006 The GameCube Linux Team
 * Copyright (C) 2004 Michael Steil <mist@c64.org>
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2006 Albert Herranz
 *
 * Based on vesafb (c) 1998 Gerd Knorr <kraxel@goldbach.in-berlin.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <asm/io.h>
#include <platforms/gamecube.h>
#include "gcngx.h"

#define DRV_MODULE_NAME   "gcnfb"
#define DRV_DESCRIPTION   "Nintendo GameCube framebuffer driver"
#define DRV_AUTHOR        "Michael Steil <mist@c64.org>, " \
                          "Todd Jeffreys <todd@voidpointer.org>, " \
			  "Albert Herranz"

/*
 *
 * Hardware.
 */

#define VI_IRQ	8

#define VI_BASE			0xcc002000
#define VI_SIZE			0x100

#define VI_IO_BASE		((void __iomem *)VI_BASE)

#define VI_DCR			0x02
#define VI_HTR0			0x04
#define VI_TFBL			0x1c
#define VI_TFBR			0x20
#define VI_BFBL			0x24
#define VI_BFBR			0x28
#define VI_DPV			0x2c

#define VI_DI0			0x30
#define VI_DI1			0x34
#define VI_DI2			0x38
#define VI_DI3			0x3C
#define VI_DI_INT		(1 << 31)
#define VI_DI_ENB		(1 << 28)
#define VI_DI_VCT_SHIFT	16
#define VI_DI_VCT_MASK		0x03FF0000
#define VI_DI_HCT_SHIFT	0
#define VI_DI_HCT_MASK		0x000003FF

#define VI_VISEL		0x6e
#define VI_VISEL_PROGRESSIVE	(1 << 0)

struct vi_ctl {
	spinlock_t lock;

	void __iomem *io_base;

	int in_vtrace;
	wait_queue_head_t vtrace_waitq;

	int visible_page;
	unsigned long page_address[2];
	unsigned long flip_pending;

	struct fb_info *info;
};


/*
 * Video mode handling
 */

struct vi_video_mode {
	char *name;
	const u32 *regs;
	int width;
	int height;
	int lines;
};

static const u32 vi_Mode640X480NtscYUV16[32] = {
	0x0F060001, 0x476901AD, 0x02EA5140, 0x00030018,
	0x00020019, 0x410C410C, 0x40ED40ED, 0x00435A4E,
	0x00000000, 0x00435A4E, 0x00000000, 0x00000000,
	0x110701AE, 0x10010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

static const u32 vi_Mode640x480NtscProgressiveYUV16[32] = {
	0x1e0c0005, 0x476901ad, 0x02ea5140, 0x00060030,
	0x00060030, 0x81d881d8, 0x81d881d8, 0x10000000,
	0x00000000, 0x00000000, 0x00000000, 0x037702b6,
	0x90010001, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x28280100, 0x1ae771f0,
	0x0db4a574, 0x00c1188e, 0xc4c0cbe2, 0xfcecdecf,
	0x13130f08, 0x00080c0f, 0x00ff0000, 0x00010001,
	0x02800000, 0x000000ff, 0x00ff00ff, 0x00ff00ff,
};

static const u32 vi_Mode640X576Pal50YUV16[32] = {
	0x11F50101, 0x4B6A01B0, 0x02F85640, 0x00010023,
	0x00000024, 0x4D2B4D6D, 0x4D8A4D4C, 0x0066D480,
	0x00000000, 0x0066D980, 0x00000000, 0x00C901F3,
	0x913901B1, 0x90010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

static const u32 vi_Mode640X480Pal60YUV16[32] = {
	0x0F060001, 0x476901AD, 0x02EA5140, 0x00030018,
	0x00020019, 0x410C410C, 0x40ED40ED, 0x0066D480,
	0x00000000, 0x0066D980, 0x00000000, 0x00C9010F,
	0x910701AE, 0x90010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

static struct vi_video_mode vi_video_modes[] = {
#define VI_VM_NTSC                0
	[VI_VM_NTSC] = {
		.name = "NTSC/PAL60 480i",
		.regs = vi_Mode640X480NtscYUV16,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
#define VI_VM_NTSC_PROGRESSIVE    (VI_VM_NTSC+1)
	[VI_VM_NTSC_PROGRESSIVE] = {
		.name = "NTSC 480p",
		.regs = vi_Mode640x480NtscProgressiveYUV16,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
#define VI_VM_PAL50               (VI_VM_NTSC_PROGRESSIVE+1)
	[VI_VM_PAL50] = {
		.name = "PAL50 576i",
		.regs = vi_Mode640X576Pal50YUV16,
		.width = 640,
		.height = 576,
		.lines = 625,
	},
#define VI_VM_PAL60               (VI_VM_PAL50+1)
	[VI_VM_PAL60] = {
		/* this seems to be actually the same as NTSC 480i */
		.name = "PAL60 480i",
		.regs = vi_Mode640X480Pal60YUV16,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
};


static struct fb_fix_screeninfo gcnfb_fix __initdata = {
	.id = DRV_MODULE_NAME,
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,	/* lies, lies, lies, ... */
	.accel = FB_ACCEL_NONE,
};

static struct fb_var_screeninfo gcnfb_var __initdata = {
	.bits_per_pixel = 16,
	.activate = FB_ACTIVATE_NOW,
	.height = -1,
	.width = -1,
	.right_margin = 32,
	.upper_margin = 16,
	.lower_margin = 4,
	.vsync_len = 4,
	.vmode = FB_VMODE_INTERLACED,
};

/*
 * setup parameters
 */
static struct vi_video_mode *vi_current_video_mode = NULL;
static int ypan = 1;		/* 0..nothing, 1..ypan */

/* legacy stuff, XXX really needed? */
static u32 pseudo_palette[17];


/* some glue to the gx side */
static inline void gcngx_dispatch_vtrace(struct vi_ctl *ctl)
{
#ifdef CONFIG_FB_GAMECUBE_GX
	gcngx_vtrace(ctl);
#endif
}


/*
 *
 * Color space handling.
 */

/*
 * RGB to YCbYCr conversion support bits.
 * We are using here the ITU.BT-601 Y'CbCr standard.
 *
 * References:
 * - "Colour Space Conversions" by Adrian Ford and Alan Roberts, 1998
 *   (google for coloureq.pdf)
 *
 */

#define RGB2YUV_SHIFT   16
#define RGB2YUV_LUMA    16
#define RGB2YUV_CHROMA 128

#define Yr ((int)( 0.299*(1<<RGB2YUV_SHIFT)))
#define Yg ((int)( 0.587*(1<<RGB2YUV_SHIFT)))
#define Yb ((int)( 0.114*(1<<RGB2YUV_SHIFT)))

#define Ur ((int)(-0.169*(1<<RGB2YUV_SHIFT)))
#define Ug ((int)(-0.331*(1<<RGB2YUV_SHIFT)))
#define Ub ((int)( 0.500*(1<<RGB2YUV_SHIFT)))

#define Vr ((int)( 0.500*(1<<RGB2YUV_SHIFT)))	/* same as Ub */
#define Vg ((int)(-0.419*(1<<RGB2YUV_SHIFT)))
#define Vb ((int)(-0.081*(1<<RGB2YUV_SHIFT)))

#define clamp(x, y, z) ((z < x) ? x : ((z > y) ? y : z))

/*
 * Converts two 16bpp rgb pixels into a dual yuy2 pixel.
 */
static inline uint32_t rgbrgb16toycbycr(uint16_t rgb1, uint16_t rgb2)
{
	register int Y1, Cb, Y2, Cr;
	register int r1, g1, b1;
	register int r2, g2, b2;
	register int r, g, b;

	/* fast path, thanks to bohdy */
	if (!(rgb1 | rgb2)) {
		return 0x00800080;	/* black, black */
	}

	/* RGB565 */
	r1 = ((rgb1 >> 11) & 0x1f);
	g1 = ((rgb1 >> 5) & 0x3f);
	b1 = ((rgb1 >> 0) & 0x1f);

	/* fast (approximated) scaling to 8 bits, thanks to Masken */
	r1 = (r1 << 3) | (r1 >> 2);
	g1 = (g1 << 2) | (g1 >> 4);
	b1 = (b1 << 3) | (b1 >> 2);

	Y1 = clamp(16, 235, ((Yr * r1 + Yg * g1 + Yb * b1) >> RGB2YUV_SHIFT)
		   + RGB2YUV_LUMA);
	if (rgb1 == rgb2) {
		/* this is just another fast path */
		Y2 = Y1;
		r = r1;
		g = g1;
		b = b1;
	} else {
		/* same as we did for r1 before */
		r2 = ((rgb2 >> 11) & 0x1f);
		g2 = ((rgb2 >> 5) & 0x3f);
		b2 = ((rgb2 >> 0) & 0x1f);
		r2 = (r2 << 3) | (r2 >> 2);
		g2 = (g2 << 2) | (g2 >> 4);
		b2 = (b2 << 3) | (b2 >> 2);

		Y2 = clamp(16, 235,
			   ((Yr * r2 + Yg * g2 + Yb * b2) >> RGB2YUV_SHIFT)
			   + RGB2YUV_LUMA);

		r = (r1 + r2) / 2;
		g = (g1 + g2) / 2;
		b = (b1 + b2) / 2;
	}

	Cb = clamp(16, 240, ((Ur * r + Ug * g + Ub * b) >> RGB2YUV_SHIFT)
		   + RGB2YUV_CHROMA);
	Cr = clamp(16, 240, ((Vr * r + Vg * g + Vb * b) >> RGB2YUV_SHIFT)
		   + RGB2YUV_CHROMA);

	return (((uint8_t) Y1) << 24) | (((uint8_t) Cb) << 16) |
	    (((uint8_t) Y2) << 8) | (((uint8_t) Cr) << 0);
}

/*
 *
 * Video hardware support.
 */

/*
 * Get video mode reported by hardware.
 * 0=NTSC, 1=PAL, 2=MPAL, 3=debug
 */
static inline int vi_get_mode(struct vi_ctl *ctl)
{
	return (readw(ctl->io_base + VI_DCR) >> 8) & 3;
}

/*
 * Check if the current video mode is NTSC.
 */
static inline int vi_is_mode_ntsc(struct vi_ctl *ctl)
{
	return vi_get_mode(ctl) == 0;
}

/*
 * Check if the passed video mode is a progressive one.
 */
static inline int vi_is_mode_progressive(__u32 vmode)
{
	return (vmode & FB_VMODE_MASK) == FB_VMODE_NONINTERLACED;
}

/*
 * Check if the display supports progressive modes.
 */
static inline int vi_can_do_progressive(struct vi_ctl *ctl)
{
	return readw(ctl->io_base + VI_VISEL) & VI_VISEL_PROGRESSIVE;
}

/*
 * Try to guess a suitable video mode if none is currently selected.
 */
static void vi_mode_guess(struct vi_ctl *ctl)
{
	void __iomem *io_base = ctl->io_base;
	u16 mode;

	if (vi_current_video_mode == NULL) {
		/* auto detection */
		if (readl(io_base + VI_HTR0) == 0x4B6A01B0) {
			/* PAL50 */
			vi_current_video_mode = vi_video_modes + VI_VM_PAL50;
		} else {
			/* NTSC/PAL60 */
			mode = vi_get_mode(ctl);
			switch (mode) {
			case 0:	/* NTSC */
				/* check if we can support progressive */
				vi_current_video_mode =
				    vi_video_modes +
				    (vi_can_do_progressive(ctl) ?
				     VI_VM_NTSC_PROGRESSIVE : VI_VM_NTSC);
				break;
				/* XXX this code is never reached */
			case 1:	/* PAL60 */
				vi_current_video_mode =
				    vi_video_modes + VI_VM_PAL60;
				break;
			default:	/* MPAL or DEBUG, we don't support */
				break;
			}
		}
	}

	/* if we get here something wrong happened */
	if (vi_current_video_mode == NULL) {
		printk(KERN_DEBUG "HEY! SOMETHING WEIRD HERE!\n");
		vi_current_video_mode = vi_video_modes + VI_VM_NTSC;
	}
}

/*
 * Set the address from where the video encoder will display data on screen.
 */
void vi_set_framebuffer(struct vi_ctl *ctl, u32 addr)
{
	struct fb_info *info = ctl->info;
	void __iomem *io_base = ctl->io_base;

	/* set top field */
	writel(0x10000000 | (addr >> 5), io_base + VI_TFBL);

	/* set bottom field */
	if (!vi_is_mode_progressive(info->var.vmode)) {
		addr += info->fix.line_length;
	}
	writel(0x10000000 | (addr >> 5), io_base + VI_BFBL);
}

/*
 * Swap the visible and back pages.
 */
static inline void vi_flip_page(struct vi_ctl *ctl)
{
	ctl->visible_page ^= 1;
	vi_set_framebuffer(ctl, ctl->page_address[ctl->visible_page]);

	ctl->flip_pending = 0;
}

/*
 * Enable video related interrupts.
 */
static void vi_enable_interrupts(struct vi_ctl *ctl, int enable)
{
	void __iomem *io_base = ctl->io_base;
	u16 vtrap, htrap;

	if (enable) {
		/*
		 * The vertical retrace happens while the beam moves from
		 * the last drawn dot in the last line to the first dot in
		 * the first line.
		 */

		/* XXX should we incorporate this in the video mode struct ? */
		vtrap = vi_current_video_mode->lines;
		htrap = vi_is_mode_ntsc(ctl) ? 430 : 433;

		/* non-progressive needs interlacing */
		if (!(vi_is_mode_progressive(ctl->info->var.vmode)
		    && vi_can_do_progressive(ctl))) {
			vtrap /= 2;
		}

		/* first dot, first line */
		writel(VI_DI_INT | VI_DI_ENB |
		       (1 << VI_DI_VCT_SHIFT) | (1 << VI_DI_HCT_SHIFT),
		       io_base + VI_DI0);
		/* last dot, last line */
		writel(VI_DI_INT | VI_DI_ENB |
		       (vtrap << VI_DI_VCT_SHIFT) | (htrap << VI_DI_HCT_SHIFT),
		       io_base + VI_DI1);
	} else {
		writel(0, io_base + VI_DI0);
		writel(0, io_base + VI_DI1);
	}
	/* these two are currently not used */
	writel(0, io_base + VI_DI2);
	writel(0, io_base + VI_DI3);
}

/*
 * Take care of vertical retrace events.
 */
static void vi_dispatch_vtrace(struct vi_ctl *ctl)
{
	unsigned long flags;

	spin_lock_irqsave(&ctl->lock, flags);
	if (ctl->flip_pending)
		vi_flip_page(ctl);
	spin_unlock_irqrestore(&ctl->lock, flags);

	wake_up_interruptible(&ctl->vtrace_waitq);
}


/*
 * Handler for video related interrupts.
 */
static irqreturn_t vi_irq_handler(int irq, void *dev, struct pt_regs *regs)
{
	struct fb_info *info =
	    platform_get_drvdata((struct platform_device *)dev);
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;
	u32 val;

	/* DI0 and DI1 are used to account for the vertical retrace */
	val = readl(io_base + VI_DI0);
	if (val & VI_DI_INT) {
		ctl->in_vtrace = 0;
		gcngx_dispatch_vtrace(ctl); /* backwards compatibility */

		writel(val & ~VI_DI_INT, io_base + VI_DI0);
		return IRQ_HANDLED;
	}
	val = readl(io_base + VI_DI1);
	if (val & VI_DI_INT) {
		ctl->in_vtrace = 1;
		vi_dispatch_vtrace(ctl);
		gcngx_dispatch_vtrace(ctl); /* backwards compatibility */

		writel(val & ~VI_DI_INT, io_base + VI_DI1);
		return IRQ_HANDLED;
	}

	/* currently unused, just in case */
	val = readl(io_base + VI_DI2);
	if (val & VI_DI_INT) {
		writel(val & ~VI_DI_INT, io_base + VI_DI2);
		return IRQ_HANDLED;
	}
	val = readl(io_base + VI_DI3);
	if (val & VI_DI_INT) {
		writel(val & ~VI_DI_INT, io_base + VI_DI3);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

/*
 *
 * Linux framebuffer support routines.
 */

/*
 * This is just a quick, dirty and cheap way of getting right colors on the
 * linux framebuffer console.
 */
unsigned int gcnfb_writel(unsigned int rgbrgb, void *address)
{
	uint16_t *rgb = (uint16_t *) & rgbrgb;
	return fb_writel_real(rgbrgb16toycbycr(rgb[0], rgb[1]), address);
}

/*
 * Restore the video hardware to sane defaults.
 */
int gcnfb_restorefb(struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;
	int i;
	unsigned long flags;

/*
	printk(KERN_INFO "Setting mode %s\n", vi_current_video_mode->name);
*/
	/* set page 0 as the visible page and cancel pending flips */
	spin_lock_irqsave(&ctl->lock, flags);
	ctl->visible_page = 1;
	vi_flip_page(ctl);
	spin_unlock_irqrestore(&ctl->lock, flags);

	/* initialize video registers */
	for (i = 0; i < 7; i++) {
		writel(vi_current_video_mode->regs[i],
		       io_base + i * sizeof(__u32));
	}
	writel(vi_current_video_mode->regs[VI_TFBR / sizeof(__u32)],
	       io_base + VI_TFBR);
	writel(vi_current_video_mode->regs[VI_BFBR / sizeof(__u32)],
	       io_base + VI_BFBR);
	writel(vi_current_video_mode->regs[VI_DPV / sizeof(__u32)],
	       io_base + VI_DPV);
	for (i = 16; i < 32; i++) {
		writel(vi_current_video_mode->regs[i],
		       io_base + i * sizeof(__u32));
	}

	/* enable the video retrace handling */
	vi_enable_interrupts(ctl, 1);

	return 0;
}

EXPORT_SYMBOL(gcnfb_restorefb);

/*
 * XXX I wonder if we really need this.
 */
static int gcnfb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *info)
{
	/*
	 *  Set a single color register. The values supplied are
	 *  already rounded down to the hardware's capabilities
	 *  (according to the entries in the `var' structure). Return
	 *  != 0 for invalid regno.
	 */

	if (regno >= info->cmap.len)
		return 1;

	switch (info->var.bits_per_pixel) {
	case 8:
		break;
	case 15:
	case 16:
		if (info->var.red.offset == 10) {
			/* XXX, not used currently */
			/* 1:5:5:5 */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800) >> 1) |
			    ((green & 0xf800) >> 6) | ((blue & 0xf800) >> 11);
		} else {
			/* 0:5:6:5 */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800)) |
			    ((green & 0xfc00) >> 5) | ((blue & 0xf800) >> 11);
		}
		break;
	case 24:
	case 32:
		/* XXX, not used currently */
		red >>= 8;
		green >>= 8;
		blue >>= 8;
		((u32 *) (info->pseudo_palette))[regno] =
		    (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset);
		break;
	}
	return 0;
}

/*
 * Pan the display by altering the framebuffer address in hardware.
 */
static int gcnfb_pan_display(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	unsigned long flags;
	int offset;

	offset = (var->yoffset * info->fix.line_length) +
	    var->xoffset * (var->bits_per_pixel / 8);
	vi_set_framebuffer(ctl, info->fix.smem_start + offset);

	spin_lock_irqsave(&ctl->lock, flags);
	ctl->visible_page = (offset) ? 1 : 0;
	spin_unlock_irqrestore(&ctl->lock, flags);

	return 0;
}

/*
 * Miscellaneous stuff end up here.
 */
static int gcnfb_ioctl(struct fb_info *info,
		       unsigned int cmd, unsigned long arg)
{
	struct vi_ctl *ctl = info->par;
	void __user *argp;
	unsigned long flags;
	int page;

	switch (cmd) {
	case FBIOWAITRETRACE:
		interruptible_sleep_on(&ctl->vtrace_waitq);
		return (signal_pending(current) ? -EINTR : 0);
	case FBIOFLIPHACK:
		/*
		 * If arg == NULL then
		 *   Try to flip the video page as soon as possible.
		 *   Returns the current visible video page number.
		 */
		if (!arg) {
			spin_lock_irqsave(&ctl->lock, flags);
			if (ctl->in_vtrace)
				vi_flip_page(ctl);
			else
				ctl->flip_pending = 1;
			spin_unlock_irqrestore(&ctl->lock, flags);
			return ctl->visible_page;
		}

		/*
		 * If arg != NULL then
		 *   Wait until the video page number pointed by arg
		 *   is not visible.
		 *   Returns the current visible video page number.
		 */
		argp = (void __user *)arg;
		if (copy_from_user(&page, argp, sizeof(int)))
			return -EFAULT;

		if (page != 0 && page != 1)
			return -EINVAL;

		spin_lock_irqsave(&ctl->lock, flags);
		ctl->flip_pending = 0;
		if (ctl->visible_page == page) {
			if (ctl->in_vtrace) {
				vi_flip_page(ctl);
			} else {
				ctl->flip_pending = 1;
				spin_unlock_irqrestore(&ctl->lock, flags);
				interruptible_sleep_on(&ctl->vtrace_waitq);
				return (signal_pending(current) ? 
					-EINTR : ctl->visible_page);
			}
		}
		spin_unlock_irqrestore(&ctl->lock, flags);
		return ctl->visible_page;
	}
#ifdef CONFIG_FB_GAMECUBE_GX
	/* see if the GX module will handle it */
	return gcngx_ioctl(info, cmd, arg);
#else
	return -EINVAL;
#endif
}

/*
 * Set the video mode according to info->var.
 */
static int gcnfb_set_par(struct fb_info *info)
{
	/* just load sane default here */
	gcnfb_restorefb(info);
	return 0;
}

/*
 * Check var and eventually tweak it to something supported.
 * Do not modify par here.
 */
static int gcnfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;

	/* check bpp */
	if (var->bits_per_pixel != 16 ||	/* check bpp */
	    var->xres_virtual != vi_current_video_mode->width ||
	    var->xres != vi_current_video_mode->width ||
	    /* XXX isobel, do not break old sdl */
	    var->yres_virtual > 2 * vi_current_video_mode->height ||
	    var->yres > vi_current_video_mode->height ||
	    (vi_is_mode_progressive(var->vmode) && !vi_can_do_progressive(ctl))) {	/* trying to set progressive? */
		return -EINVAL;
	}
	return 0;
}

/* linux framebuffer operations */
struct fb_ops gcnfb_ops = {
	.owner = THIS_MODULE,
	.fb_setcolreg = gcnfb_setcolreg,
	.fb_pan_display = gcnfb_pan_display,
	.fb_ioctl = gcnfb_ioctl,
#ifdef CONFIG_FB_GAMECUBE_GX
	.fb_mmap = gcngx_mmap,
#endif
	.fb_check_var = gcnfb_check_var,
	.fb_set_par = gcnfb_set_par,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

/*
 *
 */
static int gcnfb_probe(struct platform_device *dev)
{
	struct fb_info *info;
	struct vi_ctl *ctl;

	int video_cmap_len;
	int err = -EINVAL;

	info = framebuffer_alloc(sizeof(struct vi_ctl), &dev->dev);
	if (!info)
		goto err_framebuffer_alloc;

	info->fbops = &gcnfb_ops;
	info->var = gcnfb_var;
	info->fix = gcnfb_fix;
	ctl = info->par;
	ctl->info = info;

	/* first thing needed */
	ctl->io_base = VI_IO_BASE;

	vi_mode_guess(ctl);

	info->var.xres = vi_current_video_mode->width;
	info->var.yres = vi_current_video_mode->height;

	/* enable non-interlaced if it supports progressive */
	if (vi_can_do_progressive(ctl))
		info->var.vmode = FB_VMODE_NONINTERLACED;

	/* horizontal line in bytes */
	info->fix.line_length = info->var.xres * (info->var.bits_per_pixel / 8);

	/*
	 * Location and size of the external framebuffer.
	 */
	info->fix.smem_start = GCN_XFB_START;
	info->fix.smem_len = GCN_XFB_SIZE;

	if (!request_mem_region(info->fix.smem_start, info->fix.smem_len,
				DRV_MODULE_NAME)) {
		printk(KERN_WARNING
		       "gcnfb: abort, cannot reserve video memory at %p\n",
		       (void *)info->fix.smem_start);
		/* We cannot make this fatal. Sometimes this comes from magic
		   spaces our resource handlers simply don't know about */
	}

	info->screen_base = ioremap(info->fix.smem_start, info->fix.smem_len);
	if (!info->screen_base) {
		printk(KERN_ERR
		       "gcnfb: abort, cannot ioremap video memory"
		       " at %p (%dk)\n",
		       (void *)info->fix.smem_start, info->fix.smem_len / 1024);
		err = -EIO;
		goto err_ioremap;
	}

	spin_lock_init(&ctl->lock);
	init_waitqueue_head(&ctl->vtrace_waitq);

	ctl->visible_page = 0;
	ctl->page_address[0] = info->fix.smem_start;
	ctl->page_address[1] =
	    info->fix.smem_start + info->var.yres * info->fix.line_length;

	ctl->flip_pending = 0;

	printk(KERN_INFO
	       "gcnfb: framebuffer at 0x%p, mapped to 0x%p, size %dk\n",
	       (void *)info->fix.smem_start, info->screen_base,
	       info->fix.smem_len / 1024);
	printk(KERN_INFO
	       "gcnfb: mode is %dx%dx%d, linelength=%d, pages=%d\n",
	       info->var.xres, info->var.yres,
	       info->var.bits_per_pixel,
	       info->fix.line_length,
	       info->fix.smem_len / (info->fix.line_length * info->var.yres));

	info->var.xres_virtual = info->var.xres;
	info->var.yres_virtual = info->fix.smem_len / info->fix.line_length;

	if (ypan && info->var.yres_virtual > info->var.yres) {
		printk(KERN_INFO "gcnfb: scrolling: pan,  yres_virtual=%d\n",
		       info->var.yres_virtual);
	} else {
		printk(KERN_INFO "gcnfb: scrolling: redraw, yres_virtual=%d\n",
		       info->var.yres_virtual);
		info->var.yres_virtual = info->var.yres;
		ypan = 0;
	}

	info->fix.ypanstep = ypan ? 1 : 0;
	info->fix.ywrapstep = 0;
	if (!ypan)
		info->fbops->fb_pan_display = NULL;

	/* FIXME! Please, use here *real* values */
	/* some dummy values for timing to make fbset happy */
	info->var.pixclock = 10000000 / info->var.xres * 1000 / info->var.yres;
	info->var.left_margin = (info->var.xres / 8) & 0xf8;
	info->var.hsync_len = (info->var.xres / 8) & 0xf8;

	if (info->var.bits_per_pixel == 15) {
		info->var.red.offset = 11;
		info->var.red.length = 5;
		info->var.green.offset = 6;
		info->var.green.length = 5;
		info->var.blue.offset = 1;
		info->var.blue.length = 5;
		info->var.transp.offset = 15;
		info->var.transp.length = 1;
		video_cmap_len = 16;
	} else if (info->var.bits_per_pixel == 16) {
		info->var.red.offset = 11;
		info->var.red.length = 5;
		info->var.green.offset = 5;
		info->var.green.length = 6;
		info->var.blue.offset = 0;
		info->var.blue.length = 5;
		info->var.transp.offset = 0;
		info->var.transp.length = 0;
		video_cmap_len = 16;
	} else {
		info->var.red.length = 6;
		info->var.green.length = 6;
		info->var.blue.length = 6;
		video_cmap_len = 256;
	}

	info->pseudo_palette = pseudo_palette;
	if (fb_alloc_cmap(&info->cmap, video_cmap_len, 0)) {
		err = -ENOMEM;
		goto err_alloc_cmap;
	}

	info->flags = FBINFO_FLAG_DEFAULT | (ypan) ? FBINFO_HWACCEL_YPAN : 0;

	platform_set_drvdata(dev, info);

	if (request_irq(VI_IRQ, vi_irq_handler, SA_INTERRUPT, "gcn-vi", dev)) {
		printk(KERN_ERR "unable to register IRQ %u\n", VI_IRQ);
		goto err_request_irq;
	}

	/* now register us */
	if (register_framebuffer(info) < 0) {
		err = -EINVAL;
		goto err_register_framebuffer;
	}

	/* setup the framebuffer address */
	gcnfb_restorefb(info);

#ifdef CONFIG_FB_GAMECUBE_GX
	err = gcngx_init(info);
	if (err)
		goto err_gcngx_init;
#endif

	printk(KERN_INFO "fb%d: %s frame buffer device\n",
	       info->node, info->fix.id);

	return 0;

#ifdef CONFIG_FB_GAMECUBE_GX
err_gcngx_init:
	unregister_framebuffer(info);
#endif
err_register_framebuffer:
	free_irq(VI_IRQ, 0);
err_request_irq:
	fb_dealloc_cmap(&info->cmap);
err_alloc_cmap:
	iounmap(info->screen_base);
err_ioremap:
	release_mem_region(info->fix.smem_start, info->fix.smem_len);

	platform_set_drvdata(dev, NULL);
	framebuffer_release(info);
err_framebuffer_alloc:
	return err;
}

/**
 *
 */
static int gcnfb_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);

	if (!info)
		return -ENODEV;

#ifdef CONFIG_FB_GAMECUBE_GX
	gcngx_exit(info);
#endif
	free_irq(VI_IRQ, dev);
	unregister_framebuffer(info);
	fb_dealloc_cmap(&info->cmap);
	iounmap(info->screen_base);
	release_mem_region(info->fix.smem_start, info->fix.smem_len);

	platform_set_drvdata(dev, NULL);
	framebuffer_release(info);
	return 0;
}

static struct platform_driver gcnfb_driver = {
	.probe = gcnfb_probe,
	.remove = gcnfb_remove,
	.driver = {
		   .name = DRV_MODULE_NAME,
		   },
};

static struct platform_device gcnfb_device = {
	.name = DRV_MODULE_NAME,
};

#ifndef MODULE

/**
 *
 */
static int __devinit gcnfb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return 0;

	printk("gcnfb: options = %s\n", options);

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;

		if (!strcmp(this_opt, "redraw"))
			ypan = 0;
		else if (!strcmp(this_opt, "ypan"))
			ypan = 1;
		else if (!strcmp(this_opt, "ywrap"))
			ypan = 2;
		else if (!strncmp(this_opt, "tv=", 3)) {
			if (!strncmp(this_opt + 3, "PAL", 3))
				vi_current_video_mode =
				    vi_video_modes + VI_VM_PAL50;
			else if (!strncmp(this_opt + 3, "NTSC", 4))
				vi_current_video_mode =
				    vi_video_modes + VI_VM_NTSC;
		}
	}
	return 0;
}

#endif				/* MODULE */

/*
 *
 */
static int __init gcnfb_init_module(void)
{
	int ret = 0;
#ifndef MODULE
	char *option = NULL;

	if (fb_get_options(DRV_MODULE_NAME, &option)) {
		/* for backwards compatibility */
		if (fb_get_options("gamecubefb", &option))
			return -ENODEV;
	}
	gcnfb_setup(option);
#endif

	ret = platform_driver_register(&gcnfb_driver);
	if (!ret) {
		ret = platform_device_register(&gcnfb_device);
		if (ret)
			platform_driver_unregister(&gcnfb_driver);
	}
	return ret;
}

/*
 *
 */
static void __exit gcnfb_exit_module(void)
{
	platform_device_unregister(&gcnfb_device);
	platform_driver_unregister(&gcnfb_driver);
}

module_init(gcnfb_init_module);
module_exit(gcnfb_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

