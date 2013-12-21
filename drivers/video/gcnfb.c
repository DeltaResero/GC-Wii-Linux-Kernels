/*
 * drivers/video/gcnfb.c
 *
 * Nintendo GameCube "Flipper" chipset frame buffer driver
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004 Michael Steil <mist@c64.org>
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
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
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <platforms/gamecube.h>
#include "gcngx.h"

#define DRV_MODULE_NAME   "gcnfb"
#define DRV_DESCRIPTION   "Nintendo GameCube frame buffer driver"
#define DRV_AUTHOR        "Michael Steil <mist@c64.org>, " \
                          "Todd Jeffreys <todd@voidpointer.org>"

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

/*
 * Video mode handling
 */

#define VI_FMT_IS_NTSC(a) (((((u16*)vi_regs)[1] >> 8) & 3) == 0)
#define VI_FMT_IS_PAL(a)  (((((u16*)vi_regs)[1] >> 8) & 3) == 1)

struct vi_video_mode {
	char *name;
	const u32 *regs;
	int width;
	int height;
	int lines;
};

static const u32 VIDEO_Mode640X480NtscYUV16[32] = {
	0x0F060001, 0x476901AD, 0x02EA5140, 0x00030018,
	0x00020019, 0x410C410C, 0x40ED40ED, 0x00435A4E,
	0x00000000, 0x00435A4E, 0x00000000, 0x00000000,
	0x110701AE, 0x10010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

static const u32 VIDEO_Mode640x480NtscProgressiveYUV16[32] = {
	0x1e0c0005, 0x476901ad, 0x02ea5140, 0x00060030,
	0x00060030, 0x81d881d8, 0x81d881d8, 0x10000000,
	0x00000000, 0x00000000, 0x00000000, 0x037702b6,
	0x90010001, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x28280100, 0x1ae771f0,
	0x0db4a574, 0x00c1188e, 0xc4c0cbe2, 0xfcecdecf,
	0x13130f08, 0x00080c0f, 0x00ff0000, 0x00010001,
	0x02800000, 0x000000ff, 0x00ff00ff, 0x00ff00ff,
};

static const u32 VIDEO_Mode640X576Pal50YUV16[32] = {
	0x11F50101, 0x4B6A01B0, 0x02F85640, 0x00010023,
	0x00000024, 0x4D2B4D6D, 0x4D8A4D4C, 0x0066D480,
	0x00000000, 0x0066D980, 0x00000000, 0x00C901F3,
	0x913901B1, 0x90010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

static const u32 VIDEO_Mode640X480Pal60YUV16[32] = {
	0x0F060001, 0x476901AD, 0x02EA5140, 0x00030018,
	0x00020019, 0x410C410C, 0x40ED40ED, 0x0066D480,
	0x00000000, 0x0066D980, 0x00000000, 0x00C9010F,
	0x910701AE, 0x90010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

static struct vi_video_mode gcnfb_video_modes[] = {
	{
		.name = "NTSC/PAL60 480i",
		.regs = VIDEO_Mode640X480NtscYUV16,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
	{
		.name = "NTSC 480p",
		.regs = VIDEO_Mode640x480NtscProgressiveYUV16,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
	{
		.name = "PAL50 576i",
		.regs = VIDEO_Mode640X576Pal50YUV16,
		.width = 640,
		.height = 576,
		.lines = 625,
	},
	{
		/* this seems to be actually the same as NTSC 480i */
		.name = "PAL60 480i",
		.regs = VIDEO_Mode640X480Pal60YUV16,
		.width = 640,
		.height = 480,
		.lines = 525,
	},
};

#define GCNFB_VM_NTSC                0
#define GCNFB_VM_NTSC_PROGRESSIVE    1
#define GCNFB_VM_PAL50               2
#define GCNFB_VM_PAL60               3

static struct vi_video_mode *gcnfb_current_video_mode = NULL;


#define VI_IRQ	8

#define VI_DI0			((void __iomem *)0xcc002030)
#define VI_DI1			((void __iomem *)0xcc002034)
#define VI_DI2			((void __iomem *)0xcc002038)
#define VI_DI3			((void __iomem *)0xcc00203C)

#define VI_DI_INT		(1 << 31)
#define VI_DI_ENB		(1 << 28)
#define VI_DI_VCT_SHIFT	16
#define VI_DI_VCT_MASK		0x03FF0000
#define VI_DI_HCT_SHIFT	0
#define VI_DI_HCT_MASK		0x000003FF

#define VI_VISEL		((void __iomem *)0xcc00206e)
#define VI_VISEL_PROGRESSIVE	(1 << 0)

static volatile u32 *vi_regs = (volatile u32 __iomem *)0xcc002000;


static u32 pseudo_palette[17];
static int ypan = 0;		/* 0..nothing, 1..ypan, 2..ywrap */

static struct fb_info gcnfb_info = {
	.var = {
		.activate = FB_ACTIVATE_NOW,
		.height = -1,
		.width = -1,
		.right_margin = 32,
		.upper_margin = 16,
		.lower_margin = 4,
		.vsync_len = 4,
		.vmode = FB_VMODE_INTERLACED,
		},
	.fix = {
		.id = "GameCube",
		.type = FB_TYPE_PACKED_PIXELS,
		.accel = FB_ACCEL_NONE,
		}
};
static DECLARE_WAIT_QUEUE_HEAD(vtrace_wait_queue);


int gcnfb_restorefb(struct fb_info *info);


static inline int gcnfb_can_do_progressive(void)
{
	return readw(VI_VISEL) & VI_VISEL_PROGRESSIVE;
}

static inline int gcnfb_is_progressive(__u32 vmode)
{
	return (vmode & FB_VMODE_MASK) == FB_VMODE_NONINTERLACED;
}


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
 *
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

/**
 *
 */
unsigned int gcnfb_writel(unsigned int rgbrgb, void *address)
{
	uint16_t *rgb = (uint16_t *) & rgbrgb;
	return fb_writel_real(rgbrgb16toycbycr(rgb[0], rgb[1]), address);
}

/**
 *
 */
static int gcnfb_pan_display(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	return 0;
}

/**
 *
 */
static irqreturn_t gcnfb_vi_irq_handler(int irq, void *dev_id,
				       struct pt_regs *regs)
{
	u32 val;

	if ((val = readl(VI_DI0)) & VI_DI_INT) {
		gcngx_vtrace();
		wake_up_interruptible(&vtrace_wait_queue);
		writel(val & ~VI_DI_INT, VI_DI0);
		return IRQ_HANDLED;
	}
	if ((val = readl(VI_DI1)) & VI_DI_INT) {
		gcngx_vtrace();
		wake_up_interruptible(&vtrace_wait_queue);
		writel(val & ~VI_DI_INT, VI_DI1);
		return IRQ_HANDLED;
	}
	if ((val = readl(VI_DI2)) & VI_DI_INT) {
		gcngx_vtrace();
		wake_up_interruptible(&vtrace_wait_queue);
		writel(val & ~VI_DI_INT, VI_DI2);
		return IRQ_HANDLED;
	}
	if ((val = readl(VI_DI3)) & VI_DI_INT) {
		gcngx_vtrace();
		wake_up_interruptible(&vtrace_wait_queue);
		writel(val & ~VI_DI_INT, VI_DI3);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

/**
 *
 */
static u32 gcnfb_uvirt_to_phys(u32 virt)
{
	pgd_t *dir;
	pmd_t *pmd;
	pte_t *pte;
	u32 ret = 0;
	struct mm_struct *mm = get_task_mm(current);
	u32 offset = virt & (PAGE_SIZE - 1);
	virt &= PAGE_MASK;

	if (!mm) {
		return 0;
	}
	down_read(&mm->mmap_sem);
	/* convert to kernel address */
	if ((dir = pgd_offset(mm, virt)) && pgd_present(*dir)) {
		if ((pmd = pmd_offset(dir, virt)) && pmd_present(*pmd)) {
			pte = pte_offset_kernel(pmd, virt);
			if (pte && pte_present(*pte)) {
				ret =
				    (u32) page_address(pte_page(*pte)) + offset;
				/* ok now we have the kern addr, map to phys */
				ret = virt_to_phys((void *)ret);
			}
		}
	}

	up_read(&mm->mmap_sem);
	mmput(mm);
	return ret;
}

/**
 *
 */
static int gcnfb_ioctl(struct inode *inode, struct file *file,
		       unsigned int cmd, unsigned long arg,
		       struct fb_info *info)
{
	u32 phys;
	void __user *argp;

	if (cmd == FBIOWAITRETRACE) {
		interruptible_sleep_on(&vtrace_wait_queue);
		return (signal_pending(current) ? -EINTR : 0);
	} else if (cmd == FBIOVIRTTOPHYS) {
		argp = (void __user *)arg;
		if (copy_from_user(&phys, argp, sizeof(void *)))
			return -EFAULT;

		phys = gcnfb_uvirt_to_phys(phys);

		if (copy_to_user(argp, &phys, sizeof(void *)))
			return -EFAULT;
		return 0;
	}
	/* see if the GX module will handle it */
	return gcngx_ioctl(inode, file, cmd, arg, info);
}

/**
 *
 */
static int gcnfb_set_par(struct fb_info *info)
{
	/* update the video registers now */
	gcnfb_restorefb(info);
	return 0;
}

/**
 *
 */
static int gcnfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	/* check bpp */
	if (var->bits_per_pixel != 16 || /* check bpp */
	    var->xres_virtual != gcnfb_current_video_mode->width ||
	    var->xres != gcnfb_current_video_mode->width ||
	    /* XXX isobel, do not break old sdl */
	    var->yres_virtual > gcnfb_current_video_mode->height ||
	    var->yres > gcnfb_current_video_mode->height ||
	    (gcnfb_is_progressive(var->vmode) && !gcnfb_can_do_progressive())) {	/* trying to set progressive? */
		return -EINVAL;
	}
	return 0;
}

/**
 *
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

/**
 *
 */
void gcnfb_enable_interrupts(int enable)
{
	u16 vtrap, htrap;

	if (enable) {
		/* XXX should we incorporate this in the video mode struct ? */
		vtrap = gcnfb_current_video_mode->lines / 2;
		htrap = VI_FMT_IS_NTSC() ? 430 : 433;

		/* progressive interrupts at 526 */
		if (gcnfb_is_progressive(gcnfb_info.var.vmode)
		    && gcnfb_can_do_progressive()) {
			vtrap *= 2;
		}

		/* interrupt on line 1 */
		writel(VI_DI_INT | VI_DI_ENB |
		       (1 << VI_DI_VCT_SHIFT) | (1 << VI_DI_HCT_SHIFT),
		       VI_DI0);

		writel(VI_DI_INT | VI_DI_ENB |
		       (vtrap << VI_DI_VCT_SHIFT) | (htrap << VI_DI_HCT_SHIFT),
		       VI_DI1);
	} else {
		writel(0, VI_DI0);
		writel(0, VI_DI1);
	}
	writel(0, VI_DI2);
	writel(0, VI_DI3);
}

/**
 *
 */
void gcnfb_set_framebuffer(u32 addr)
{
	/* set top field */
	vi_regs[7] = 0x10000000 | (addr >> 5);

	/* set bottom field */
	if (!gcnfb_is_progressive(gcnfb_info.var.vmode)) {
		addr += gcnfb_info.fix.line_length;
	}
	vi_regs[9] = 0x10000000 | (addr >> 5);
}

/**
 *
 */
int gcnfb_restorefb(struct fb_info *info)
{
	int i;

/*
	printk(KERN_INFO "Setting mode %s\n", gcnfb_current_video_mode->name);
*/
	gcnfb_set_framebuffer(info->fix.smem_start);

	/* initialize video registers */
	for (i = 0; i < 7; i++) {
		vi_regs[i] = gcnfb_current_video_mode->regs[i];
	}
	vi_regs[8] = gcnfb_current_video_mode->regs[8];
	vi_regs[10] = gcnfb_current_video_mode->regs[10];
	vi_regs[11] = gcnfb_current_video_mode->regs[11];
	for (i = 16; i < 32; i++) {
		vi_regs[i] = gcnfb_current_video_mode->regs[i];
	}
	gcnfb_enable_interrupts(1);
	return 0;
}

EXPORT_SYMBOL(gcnfb_restorefb);

struct fb_ops gcnfb_ops = {
	.owner = THIS_MODULE,
	.fb_setcolreg = gcnfb_setcolreg,
	.fb_pan_display = gcnfb_pan_display,
	.fb_ioctl = gcnfb_ioctl,
	.fb_mmap = gcngx_mmap,
	.fb_check_var = gcnfb_check_var,
	.fb_set_par = gcnfb_set_par,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_cursor = soft_cursor,
};

/**
 *
 */
void gcnfb_video_mode_select(void)
{
	u16 mode;
	
	if (gcnfb_current_video_mode == NULL) {
		/* auto detection */
		if (vi_regs[1] == 0x4B6A01B0) {
			/* PAL50 */
			gcnfb_current_video_mode = 
				gcnfb_video_modes + GCNFB_VM_PAL50;
		} else {
			/* NTSC/PAL60 */
			mode = (((u16*)vi_regs)[1] >> 8) & 3;
			switch (mode)
			{
			case 0:	/* NTSC */
				/* check if we can support progressive */
				gcnfb_current_video_mode = 
					gcnfb_video_modes + 
					(gcnfb_can_do_progressive() ? 
					 GCNFB_VM_NTSC_PROGRESSIVE : 
					 GCNFB_VM_NTSC);
				break;
			/* XXX this code is never reached */
			case 1:	/* PAL60 */
				gcnfb_current_video_mode = 
					gcnfb_video_modes + GCNFB_VM_PAL60;
				break;
			default: /* MPAL or DEBUG, we don't support */
				break;
			}
		}
	}
	
	/* if we get here something wrong happened */
	if (gcnfb_current_video_mode == NULL) {
		printk(KERN_DEBUG "HEY! SOMETHING WEIRD HERE!\n");
		gcnfb_current_video_mode = gcnfb_video_modes + GCNFB_VM_NTSC;
	}
}

/**
 *
 */
int __init gcnfb_setup(char *options)
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
				gcnfb_current_video_mode = 
					gcnfb_video_modes + GCNFB_VM_PAL50;
			else if (!strncmp(this_opt + 3, "NTSC", 4))
				gcnfb_current_video_mode = 
					gcnfb_video_modes + GCNFB_VM_NTSC;
		}
	}
	return 0;
}

/**
 *
 */
static int __init gcnfb_init(void)
{
	int video_cmap_len;
	int err = -EINVAL;
	char *option = NULL;

#if 0
	int i;
	printk(KERN_INFO "vi_regs[] = {\n");
	for (i = 0; i < 32; i += 4) {
		printk(KERN_INFO "0x%08X, 0x%08X, 0x%08X, 0x%08X,\n",
		       vi_regs[i],
		       vi_regs[i + 1],
		       vi_regs[i + 2], vi_regs[i + 3]);
	}
	printk(KERN_INFO "}\n");
#endif

	if (fb_get_options("gcnfb", &option)) {
		if (fb_get_options("gamecubefb", &option))
			return -ENODEV;
	}
	gcnfb_setup(option);

	gcnfb_video_mode_select();

	gcnfb_info.var.bits_per_pixel = 16;
	gcnfb_info.var.xres = gcnfb_current_video_mode->width;
	gcnfb_info.var.yres = gcnfb_current_video_mode->height;
	/* enable non-interlaced if it supports progressive */
	if (gcnfb_can_do_progressive()) {
		gcnfb_info.var.vmode = FB_VMODE_NONINTERLACED;
	}

	gcnfb_info.fix.line_length =
	    gcnfb_info.var.xres * (gcnfb_info.var.bits_per_pixel / 8);
	/* add space for double-buffering */
	gcnfb_info.fix.smem_len =
	    2 * gcnfb_info.fix.line_length * gcnfb_info.var.yres;
	/* place XFB at end of RAM */
	gcnfb_info.fix.smem_start = GCN_XFB_START;

	gcnfb_info.fix.visual = (gcnfb_info.var.bits_per_pixel == 8) ?
	    FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR;

	if (!request_mem_region
	    (gcnfb_info.fix.smem_start, gcnfb_info.fix.smem_len,
	     "Framebuffer")) {
		printk(KERN_WARNING
		       "gcnfb: abort, cannot reserve video memory at %p\n",
		       (void *)gcnfb_info.fix.smem_start);
		/* We cannot make this fatal. Sometimes this comes from magic
		   spaces our resource handlers simply don't know about */
	}

	gcnfb_info.screen_base =
	    ioremap(gcnfb_info.fix.smem_start, gcnfb_info.fix.smem_len);
	if (!gcnfb_info.screen_base) {
		printk(KERN_ERR
		       "gcnfb: abort, cannot ioremap video memory"
		       " at %p (%dk)\n",
		       (void *)gcnfb_info.fix.smem_start,
		       gcnfb_info.fix.smem_len / 1024);
		err = -EIO;
		goto err_ioremap;
	}

	printk(KERN_INFO
	       "gcnfb: framebuffer at 0x%p, mapped to 0x%p, size %dk\n",
	       (void *)gcnfb_info.fix.smem_start, gcnfb_info.screen_base,
	       gcnfb_info.fix.smem_len / 1024);
	printk(KERN_INFO
	       "gcnfb: mode is %dx%dx%d, linelength=%d, pages=%d\n",
	       gcnfb_info.var.xres, gcnfb_info.var.yres,
	       gcnfb_info.var.bits_per_pixel, gcnfb_info.fix.line_length,
	       0 /*screen_info.pages */ );

	gcnfb_info.var.xres_virtual = gcnfb_info.var.xres;
	gcnfb_info.var.yres_virtual = gcnfb_info.var.yres;
	ypan = 0;

	/* FIXME! Please, use here *real* values */
	/* some dummy values for timing to make fbset happy */
	gcnfb_info.var.pixclock =
	    10000000 / gcnfb_info.var.xres * 1000 / gcnfb_info.var.yres;
	gcnfb_info.var.left_margin = (gcnfb_info.var.xres / 8) & 0xf8;
	gcnfb_info.var.hsync_len = (gcnfb_info.var.xres / 8) & 0xf8;

	if (gcnfb_info.var.bits_per_pixel == 15) {
		gcnfb_info.var.red.offset = 11;
		gcnfb_info.var.red.length = 5;
		gcnfb_info.var.green.offset = 6;
		gcnfb_info.var.green.length = 5;
		gcnfb_info.var.blue.offset = 1;
		gcnfb_info.var.blue.length = 5;
		gcnfb_info.var.transp.offset = 15;
		gcnfb_info.var.transp.length = 1;
		video_cmap_len = 16;
	} else if (gcnfb_info.var.bits_per_pixel == 16) {
		gcnfb_info.var.red.offset = 11;
		gcnfb_info.var.red.length = 5;
		gcnfb_info.var.green.offset = 5;
		gcnfb_info.var.green.length = 6;
		gcnfb_info.var.blue.offset = 0;
		gcnfb_info.var.blue.length = 5;
		gcnfb_info.var.transp.offset = 0;
		gcnfb_info.var.transp.length = 0;
		video_cmap_len = 16;
	} else {
		gcnfb_info.var.red.length = 6;
		gcnfb_info.var.green.length = 6;
		gcnfb_info.var.blue.length = 6;
		video_cmap_len = 256;
	}

	gcnfb_info.fix.ypanstep = ypan ? 1 : 0;
	gcnfb_info.fix.ywrapstep = (ypan > 1) ? 1 : 0;

	gcnfb_info.fbops = &gcnfb_ops;
	gcnfb_info.pseudo_palette = pseudo_palette;
	gcnfb_info.flags = FBINFO_FLAG_DEFAULT;

	if (fb_alloc_cmap(&gcnfb_info.cmap, video_cmap_len, 0)) {
		err = -ENOMEM;
		goto err_alloc_cmap;
	}

	if (request_irq
	    (VI_IRQ, gcnfb_vi_irq_handler, SA_INTERRUPT, "VI Line", 0)) {
		printk(KERN_ERR "Unable to register IRQ %u\n", VI_IRQ);
		goto err_request_irq;
	}

	/* now register us */
	if (register_framebuffer(&gcnfb_info) < 0) {
		err = -EINVAL;
		goto err_register_framebuffer;
	}

	/* setup the framebuffer address */
	gcnfb_restorefb(&gcnfb_info);

	if ((err = gcngx_init(&gcnfb_info))) {
		goto err_gcngx_init;
	}

	printk(KERN_INFO "fb%d: %s frame buffer device\n",
	       gcnfb_info.node, gcnfb_info.fix.id);

	return 0;

err_gcngx_init:
	unregister_framebuffer(&gcnfb_info);
err_register_framebuffer:
	free_irq(VI_IRQ, 0);
err_request_irq:
	fb_dealloc_cmap(&gcnfb_info.cmap);
err_alloc_cmap:
	iounmap(gcnfb_info.screen_base);
err_ioremap:
	release_mem_region(gcnfb_info.fix.smem_start, gcnfb_info.fix.smem_len);
	return err;
}

/**
 *
 */
static void __exit gcnfb_exit(void)
{
	gcngx_exit(&gcnfb_info);
	free_irq(VI_IRQ, 0);
	unregister_framebuffer(&gcnfb_info);
	fb_dealloc_cmap(&gcnfb_info.cmap);
	iounmap(gcnfb_info.screen_base);
	release_mem_region(gcnfb_info.fix.smem_start, gcnfb_info.fix.smem_len);
}

module_init(gcnfb_init);
module_exit(gcnfb_exit);
