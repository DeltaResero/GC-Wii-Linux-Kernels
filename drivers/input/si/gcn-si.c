/*
 * drivers/input/gcn-si.c
 *
 * Nintendo GameCube Serial Interface driver
 * Copyright (C) 2004 The GameCube Linux Team
 * Copyright (C) 2004 Steven Looman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/* #define SI_DEBUG */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/ioport.h>
#include <linux/timer.h>
#include <linux/delay.h>

#include <asm/io.h>

#ifdef SI_DEBUG
#  define DPRINTK(fmt, args...) \
          printk(KERN_ERR "%s: " fmt, __FUNCTION__ , ## args)
#else
#  define DPRINTK(fmt, args...)
#endif

/*
 * Defining HACK_FORCE_KEYBOARD_PORT allows one to specify a port that
 * will be identified as a keyboard port in case the port gets incorrectly
 * identified.
 */
#define HACK_FORCE_KEYBOARD_PORT


#define DRV_MODULE_NAME  "gcn-si"
#define DRV_DESCRIPTION  "Nintendo GameCube Serial Interface driver"
#define DRV_AUTHOR       "Steven Looman <steven@krx.nl>"

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

#define PFX DRV_MODULE_NAME ": "
#define si_printk(level, format, arg...) \
        printk(level PFX format , ## arg)


/*
 * This keymap is for a datel adapter + normal US keyboard.
 */
#include "gcn-keymap.h"


#define REFRESH_TIME HZ/100

#define SICOUTBUF(x)	((void __iomem *)(0xcc006400 + (x)*12))
#define SICINBUFH(x)	((void __iomem *)(0xcc006404 + (x)*12))
#define SICINBUFL(x)	((void __iomem *)(0xcc006408 + (x)*12))

#define SIPOLL		((void __iomem *)0xcc006430)
#define SICOMCSR	((void __iomem *)0xcc006434)
#define SISR		((void __iomem *)0xcc006438)
#define SIEXILK		((void __iomem *)0xcc00643c)

#define ID_PAD		0x0900
#define ID_KEYBOARD	0x0820
#define ID_WIRELESS_BIT (1 << 15)
#define ID_WAVEBIRD_BIT (1 << 8)

#define PAD_START	(1 << 28)
#define PAD_Y		(1 << 27)
#define PAD_X		(1 << 26)
#define PAD_B		(1 << 25)
#define PAD_A		(1 << 24)
#define PAD_LT		(1 << 22)
#define PAD_RT		(1 << 21)
#define PAD_Z		(1 << 20)
#define PAD_UP		(1 << 19)
#define PAD_DOWN	(1 << 18)
#define PAD_RIGHT	(1 << 17)
#define PAD_LEFT	(1 << 16)


static struct resource gcn_si_resources = {
	DRV_MODULE_NAME,
	0xcc006400,
	0xcc006500,
	IORESOURCE_MEM | IORESOURCE_BUSY
};

typedef struct {
	unsigned char old[3];
} keyboard_status;

typedef enum {CTL_PAD,CTL_KEYBOARD,CTL_UNKNOWN} control_type;

struct {
	control_type id;
	int si_id;
	unsigned int raw[2];

#if 0
	unsigned char errStat:1;
	unsigned char errLatch:1;
#endif

	struct input_dev idev;
	struct timer_list timer;
	
	union {
		keyboard_status keyboard;
	};
	
	char name[32];
	/* char phys[32]; */
} port[4];


#ifdef HACK_FORCE_KEYBOARD_PORT

static int gcn_si_force_keyboard_port = -1;

#ifdef MODULE
module_param_named(force_keyboard_port, gcn_si_force_keyboard_port, int, 0644);
MODULE_PARM_DESC(force_keyboard_port, "port n becomes a keyboard port if"
		 " automatic identification fails");
#else
static int __init gcn_si_force_keyboard_port_setup(char *line)
{
        if (sscanf(line, "%d", &gcn_si_force_keyboard_port) != 1) {
		gcn_si_force_keyboard_port = -1;
	}
        return 1;
}
__setup("force_keyboard_port=", gcn_si_force_keyboard_port_setup);
#endif /* MODULE */

#endif /* HACK_FORCE_KEYBOARD_PORT */

/**
 *
 */
static void gcn_si_reset(void)
{
	int i;

	/* clear si registers */

	/* SICOUTBUF */
	for (i = 0; i < 4; ++i)
		writel(0, SICOUTBUF(i));

	/* SICINBUFH */
	for (i = 0; i < 4; ++i)
		writel(0, SICINBUFH(i));

	/* SICINBUFL */
	for (i = 0; i < 4; ++i)
		writel(0, SICINBUFL(i));

	writel(0, SIPOLL);
	writel(0, SICOMCSR);
	writel(0, SISR);

	writel(0, (void __iomem *)0xcc006480);
	writel(0, (void __iomem *)0xcc006484);
	writel(0, (void __iomem *)0xcc006488);
	writel(0, (void __iomem *)0xcc00648c);

	writel(0, (void __iomem *)0xcc006490);
	writel(0, (void __iomem *)0xcc006494);
	writel(0, (void __iomem *)0xcc006498);
	writel(0, (void __iomem *)0xcc00649c);

	writel(0, (void __iomem *)0xcc0064a0);
	writel(0, (void __iomem *)0xcc0064a4);
	writel(0, (void __iomem *)0xcc0064a8);
	writel(0, (void __iomem *)0xcc0064ac);
}

/**
 *
 */
static void gcn_si_wait_transfer_done(void)
{
	unsigned long transfer_done;

	do {
		transfer_done = readl(SICOMCSR) & (1 << 31);
	} while (!transfer_done);

	writel(readl(SICOMCSR) | (1 << 31), SICOMCSR);	/* ack IRQ */
}

/**
 *
 */
static unsigned long gcn_si_get_controller_id(int port)
{
	gcn_si_reset();

	writel(0, SIPOLL);
	writel(0, SICOUTBUF(port));
	writel(0x80000000, SISR);
	writel(0xd0010001 | port << 1, SICOMCSR);

	gcn_si_wait_transfer_done();

	return readl((void __iomem *)0xcc006480);
}

/**
 *
 */
static void gcn_si_set_polling(void)
{
	unsigned long pad_bits = 0;
	int i;

	for (i = 0; i < 4; ++i) {
		switch (port[i].id) {
		case CTL_PAD:
			writel(0x00400300, SICOUTBUF(i));
			break;
		case CTL_KEYBOARD:
			writel(0x00540000, SICOUTBUF(i));
			break;
		default:
			continue;
		}
		pad_bits |= 1 << (7 - i);
	}

	writel(0x00F70200 | pad_bits, SIPOLL);
	writel(0x80000000, SISR);
	writel(0xC0010801, SICOMCSR);

	gcn_si_wait_transfer_done();
}

/**
 *
 */
static void gcn_si_set_rumbling(int portno, int rumble)
{
	if (rumble) {
		writel(0x00400001, SICOUTBUF(portno));
		writel(0x80000000, SISR);
	} else {
		writel(0x00400000, SICOUTBUF(portno));
		writel(0x80000000, SISR);
	}
}

/**
 *
 */
static void gcn_si_timer(unsigned long portno)
{
	unsigned long raw[2];
	unsigned char key[3];
	unsigned char oldkey;
	int i;

	raw[0] = readl(SICINBUFH(portno));
	raw[1] = readl(SICINBUFL(portno));

	switch (port[portno].id) {
	case CTL_PAD:
		/* buttons */
		input_report_key(&port[portno].idev, BTN_A, raw[0] & PAD_A);
		input_report_key(&port[portno].idev, BTN_B, raw[0] & PAD_B);
		input_report_key(&port[portno].idev, BTN_X, raw[0] & PAD_X);
		input_report_key(&port[portno].idev, BTN_Y, raw[0] & PAD_Y);
		input_report_key(&port[portno].idev, BTN_Z, raw[0] & PAD_Z);
		input_report_key(&port[portno].idev, BTN_TL,
				 raw[0] & PAD_LT);
		input_report_key(&port[portno].idev, BTN_TR,
				 raw[0] & PAD_RT);
		input_report_key(&port[portno].idev, BTN_START,
				 raw[0] & PAD_START);

		/* axis */
		/* a stick */
		input_report_abs(&port[portno].idev, ABS_X,
				 raw[0] >> 8 & 0xFF);
		input_report_abs(&port[portno].idev, ABS_Y,
				 0xFF - (raw[0] >> 0 & 0xFF));

		/* b pad */
		if (raw[0] & PAD_RIGHT)
			input_report_abs(&port[portno].idev, ABS_HAT0X, 1);
		else if (raw[0] & PAD_LEFT)
			input_report_abs(&port[portno].idev, ABS_HAT0X, -1);
		else
			input_report_abs(&port[portno].idev, ABS_HAT0X, 0);

		if (raw[0] & PAD_UP)
			input_report_abs(&port[portno].idev, ABS_HAT0Y, 1);
		else if (raw[0] & PAD_DOWN)
			input_report_abs(&port[portno].idev, ABS_HAT0Y, -1);
		else
			input_report_abs(&port[portno].idev, ABS_HAT0Y, 0);

		/* c stick */
		input_report_abs(&port[portno].idev, ABS_RX,
				 raw[1] >> 24 & 0xFF);
		input_report_abs(&port[portno].idev, ABS_RY,
				 raw[1] >> 16 & 0xFF);

		/* triggers */
		input_report_abs(&port[portno].idev, ABS_BRAKE,
				 raw[1] >> 8 & 0xFF);
		input_report_abs(&port[portno].idev, ABS_GAS,
				 raw[1] >> 0 & 0xFF);

		break;
		
	case CTL_KEYBOARD:
		key[0] = (raw[0] >> 12) & 0xFF;
		key[1] = (raw[0] >> 4) & 0xFF;
		key[2] = (raw[0] << 4) & 0xFF;
		key[2] |= (raw[1] << 28) & 0xFF;

		/* check if anything was released */
		for (i = 0; i < 3; ++i) {
			oldkey = port[portno].keyboard.old[i];
			if (oldkey != key[0] &&
			    oldkey != key[1] && oldkey != key[2])
				input_report_key(&port[portno].idev,
						 gamecube_keymap[oldkey], 0);
		}

		/* report keys */
		for (i = 0; i < 3; ++i) {
			if (key[i])
				input_report_key(&port[portno].idev,
						 gamecube_keymap[key[i]], 1);
			port[portno].keyboard.old[i] = key[i];
		}
		break;

	default:
		break;
	}

	input_sync(&port[portno].idev);

	mod_timer(&port[portno].timer, jiffies + REFRESH_TIME);
}

/**
 *
 */
static int gcn_si_open(struct input_dev *idev)
{
	int portno = (int)idev->private;

	init_timer(&port[portno].timer);
	port[portno].timer.function = gcn_si_timer;
	port[portno].timer.data = (int)idev->private;
	port[portno].timer.expires = jiffies + REFRESH_TIME;
	add_timer(&port[portno].timer);

	return 0;
}

/**
 *
 */
static void gcn_si_close(struct input_dev *idev)
{
	int portno = (int)idev->private;

	del_timer(&port[portno].timer);
}

/**
 *
 */
static int gcn_si_event(struct input_dev *dev, unsigned int type,
			unsigned int code, int value)
{
	int portno = (int)dev->private;

	if (type == EV_FF) {
		if (code == FF_RUMBLE) {
			gcn_si_set_rumbling(portno, value);
		}
	}

	return value;
}

/**
 *
 */
static int __init gcn_si_init(void)
{
	int i;
	int j;
        si_printk(KERN_INFO, "%s\n", DRV_DESCRIPTION);

	if (request_resource(&iomem_resource, &gcn_si_resources) < 0) {
		printk(KERN_WARNING PFX "resource busy\n");
		return -EBUSY;
	}

	for (i = 0; i < 4; ++i) {
		memset(&port[i], 0, sizeof(port[i]));

		/* probe ports */
		port[i].si_id = gcn_si_get_controller_id(i) >> 16;

		/* convert si_id to id */
		if (port[i].si_id == ID_PAD) {
			port[i].id = CTL_PAD;
			strcpy(port[i].name,"Standard Pad");
		} else if (port[i].si_id & ID_WIRELESS_BIT) {
			port[i].id = CTL_PAD;
			strcpy(port[i].name,(port[i].si_id & ID_WAVEBIRD_BIT) ?
			       "Nintendo Wavebird" : "Wireless Pad");
		} else if (port[i].si_id == ID_KEYBOARD) {
			port[i].id = CTL_KEYBOARD;
			strcpy(port[i].name, "Keyboard");
		} else {
			port[i].id = CTL_UNKNOWN;
			if (port[i].si_id) {
				sprintf(port[i].name, "Unknown (%x)", 
					port[i].si_id);
#ifdef HACK_FORCE_KEYBOARD_PORT
				if (i+1 == gcn_si_force_keyboard_port) {
					si_printk(KERN_WARNING,
						  "port %d forced to"
						  " keyboard mode\n", i+1);
					port[i].si_id = ID_KEYBOARD;
					port[i].id = CTL_KEYBOARD;
					strcpy(port[i].name, "Keyboard"
					       " (forced)");
				}
#endif /* HACK_FORCE_KEYBOARD_PORT */
			} else {
				strcpy(port[i].name, "Not Present");
			}
		}
		
		init_input_dev(&port[i].idev);

		port[i].idev.open = gcn_si_open;
		port[i].idev.close = gcn_si_close;
		port[i].idev.private = (unsigned int *)i;
		port[i].idev.name = port[i].name;
		
		switch (port[i].id) {
		case CTL_PAD:
			set_bit(EV_KEY, port[i].idev.evbit);
			set_bit(EV_ABS, port[i].idev.evbit);
			set_bit(EV_FF, port[i].idev.evbit);

			set_bit(BTN_A, port[i].idev.keybit);
			set_bit(BTN_B, port[i].idev.keybit);
			set_bit(BTN_X, port[i].idev.keybit);
			set_bit(BTN_Y, port[i].idev.keybit);
			set_bit(BTN_Z, port[i].idev.keybit);
			set_bit(BTN_TL, port[i].idev.keybit);
			set_bit(BTN_TR, port[i].idev.keybit);
			set_bit(BTN_START, port[i].idev.keybit);

			/* a stick */
			set_bit(ABS_X, port[i].idev.absbit);
			port[i].idev.absmin[ABS_X] = 0;
			port[i].idev.absmax[ABS_X] = 255;
			port[i].idev.absfuzz[ABS_X] = 8;
			port[i].idev.absflat[ABS_X] = 8;

			set_bit(ABS_Y, port[i].idev.absbit);
			port[i].idev.absmin[ABS_Y] = 0;
			port[i].idev.absmax[ABS_Y] = 255;
			port[i].idev.absfuzz[ABS_Y] = 8;
			port[i].idev.absflat[ABS_Y] = 8;

			/* b pad */
			set_bit(ABS_HAT0X, port[i].idev.absbit);
			port[i].idev.absmin[ABS_HAT0X] = -1;
			port[i].idev.absmax[ABS_HAT0X] = 1;

			set_bit(ABS_HAT0Y, port[i].idev.absbit);
			port[i].idev.absmin[ABS_HAT0Y] = -1;
			port[i].idev.absmax[ABS_HAT0Y] = 1;

			/* c stick */
			set_bit(ABS_RX, port[i].idev.absbit);
			port[i].idev.absmin[ABS_RX] = 0;
			port[i].idev.absmax[ABS_RX] = 255;
			port[i].idev.absfuzz[ABS_RX] = 8;
			port[i].idev.absflat[ABS_RX] = 8;

			set_bit(ABS_RY, port[i].idev.absbit);
			port[i].idev.absmin[ABS_RY] = 0;
			port[i].idev.absmax[ABS_RY] = 255;
			port[i].idev.absfuzz[ABS_RY] = 8;
			port[i].idev.absflat[ABS_RY] = 8;

			/* triggers */
			set_bit(ABS_GAS, port[i].idev.absbit);
			port[i].idev.absmin[ABS_GAS] = -255;
			port[i].idev.absmax[ABS_GAS] = 255;
			port[i].idev.absfuzz[ABS_GAS] = 16;
			port[i].idev.absflat[ABS_GAS] = 16;

			set_bit(ABS_BRAKE, port[i].idev.absbit);
			port[i].idev.absmin[ABS_BRAKE] = -255;
			port[i].idev.absmax[ABS_BRAKE] = 255;
			port[i].idev.absfuzz[ABS_BRAKE] = 16;
			port[i].idev.absflat[ABS_BRAKE] = 16;

			/* rumbling */
			set_bit(FF_RUMBLE, port[i].idev.ffbit);
			port[i].idev.event = gcn_si_event;

			port[i].idev.ff_effects_max = 1;

			input_register_device(&port[i].idev);

			break;

		case CTL_KEYBOARD:
			set_bit(EV_KEY, port[i].idev.evbit);
			set_bit(EV_REP, port[i].idev.evbit);

			for (j = 0; j < 255; ++j)
				set_bit(gamecube_keymap[j],
					port[i].idev.keybit);

			input_register_device(&port[i].idev);

			break;
		default:
			/* this is here to avoid compiler warnings */
			break;
		}
		si_printk(KERN_INFO, "Port %d: %s\n", i+1, port[i].name);
	}

	gcn_si_set_polling();

	return 0;
}

/**
 *
 */
static void __exit gcn_si_exit(void)
{
	int i;

	si_printk(KERN_INFO, "exit\n");

	for (i = 0; i < 4; ++i) {
		if (port[i].id != CTL_UNKNOWN) {
			input_unregister_device(&port[i].idev);
		}
	}
	release_resource(&gcn_si_resources);
}

module_init(gcn_si_init);
module_exit(gcn_si_exit);
