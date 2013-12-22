/*
 * drivers/input/gcn-si.c
 *
 * Nintendo GameCube Serial Interface driver
 * Copyright (C) 2004 The GameCube Linux Team
 * Copyright (C) 2004 Steven Looman
 * Copyright (C) 2005 Albert Herranz
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


#define SI_MAX_PORTS	4

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
	.start = 0xcc006400,
	.end = 0xcc006500,
	.name = DRV_MODULE_NAME,
	.flags = IORESOURCE_MEM | IORESOURCE_BUSY
};

typedef struct {
	unsigned char old[3];
} keyboard_status;

typedef enum {CTL_PAD,CTL_KEYBOARD,CTL_UNKNOWN} control_type;

struct si_device {
	control_type id;
	int si_id;
	unsigned int raw[2];

	struct input_dev *idev;
	struct timer_list timer;
	
	union {
		keyboard_status keyboard;
	};
	
	char name[32];
};

static struct si_device port[SI_MAX_PORTS];


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
	for (i = 0; i < SI_MAX_PORTS; ++i)
		writel(0, SICOUTBUF(i));

	/* SICINBUFH */
	for (i = 0; i < SI_MAX_PORTS; ++i)
		writel(0, SICINBUFH(i));

	/* SICINBUFL */
	for (i = 0; i < SI_MAX_PORTS; ++i)
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

	for (i = 0; i < SI_MAX_PORTS; ++i) {
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
	struct si_device *sdev = &port[portno];
	unsigned long raw[2];
	unsigned char key[3];
	unsigned char oldkey;
	int i;

	raw[0] = readl(SICINBUFH(portno));
	raw[1] = readl(SICINBUFL(portno));

	switch (sdev->id) {
	case CTL_PAD:
		/* buttons */
		input_report_key(sdev->idev, BTN_A, raw[0] & PAD_A);
		input_report_key(sdev->idev, BTN_B, raw[0] & PAD_B);
		input_report_key(sdev->idev, BTN_X, raw[0] & PAD_X);
		input_report_key(sdev->idev, BTN_Y, raw[0] & PAD_Y);
		input_report_key(sdev->idev, BTN_Z, raw[0] & PAD_Z);
		input_report_key(sdev->idev, BTN_TL,
				 raw[0] & PAD_LT);
		input_report_key(sdev->idev, BTN_TR,
				 raw[0] & PAD_RT);
		input_report_key(sdev->idev, BTN_START,
				 raw[0] & PAD_START);
		input_report_key(sdev->idev, BTN_0, raw[0] & PAD_UP);
		input_report_key(sdev->idev, BTN_1, raw[0] & PAD_RIGHT);
		input_report_key(sdev->idev, BTN_2, raw[0] & PAD_DOWN);
		input_report_key(sdev->idev, BTN_3, raw[0] & PAD_LEFT);

		/* axis */
		/* a stick */
		input_report_abs(sdev->idev, ABS_X,
				 raw[0] >> 8 & 0xFF);
		input_report_abs(sdev->idev, ABS_Y,
				 0xFF - (raw[0] >> 0 & 0xFF));

		/* b pad */
		if (raw[0] & PAD_RIGHT)
			input_report_abs(sdev->idev, ABS_HAT0X, 1);
		else if (raw[0] & PAD_LEFT)
			input_report_abs(sdev->idev, ABS_HAT0X, -1);
		else
			input_report_abs(sdev->idev, ABS_HAT0X, 0);

		if (raw[0] & PAD_DOWN)
			input_report_abs(sdev->idev, ABS_HAT0Y, 1);
		else if (raw[0] & PAD_UP)
			input_report_abs(sdev->idev, ABS_HAT0Y, -1);
		else
			input_report_abs(sdev->idev, ABS_HAT0Y, 0);

		/* c stick */
		input_report_abs(sdev->idev, ABS_RX,
				 raw[1] >> 24 & 0xFF);
		input_report_abs(sdev->idev, ABS_RY,
				 raw[1] >> 16 & 0xFF);

		/* triggers */
		input_report_abs(sdev->idev, ABS_BRAKE,
				 raw[1] >> 8 & 0xFF);
		input_report_abs(sdev->idev, ABS_GAS,
				 raw[1] >> 0 & 0xFF);

		break;
		
	case CTL_KEYBOARD:
		key[0] = (raw[0] >> 12) & 0xFF;
		key[1] = (raw[0] >> 4) & 0xFF;
		key[2] = (raw[0] << 4) & 0xFF;
		key[2] |= (raw[1] << 28) & 0xFF;

		/* check if anything was released */
		for (i = 0; i < 3; ++i) {
			oldkey = sdev->keyboard.old[i];
			if (oldkey != key[0] &&
			    oldkey != key[1] && oldkey != key[2])
				input_report_key(sdev->idev,
						 gamecube_keymap[oldkey], 0);
		}

		/* report keys */
		for (i = 0; i < 3; ++i) {
			if (key[i])
				input_report_key(sdev->idev,
						 gamecube_keymap[key[i]], 1);
			sdev->keyboard.old[i] = key[i];
		}
		break;

	default:
		break;
	}

	input_sync(sdev->idev);

	mod_timer(&sdev->timer, jiffies + REFRESH_TIME);
}

/**
 *
 */
static int gcn_si_open(struct input_dev *idev)
{
	int portno = (int)idev->private;
	struct si_device *sdev = &port[portno];

	init_timer(&sdev->timer);
	sdev->timer.function = gcn_si_timer;
	sdev->timer.data = (int)idev->private;
	sdev->timer.expires = jiffies + REFRESH_TIME;
	add_timer(&sdev->timer);

	return 0;
}

/**
 *
 */
static void gcn_si_close(struct input_dev *idev)
{
	int portno = (int)idev->private;
	struct si_device *sdev = &port[portno];

	del_timer(&sdev->timer);
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
static int si_setup_pad(struct input_dev *idev)
{
	struct ff_device *ff;
	int error;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_ABS, idev->evbit);

	set_bit(BTN_A, idev->keybit);
	set_bit(BTN_B, idev->keybit);
	set_bit(BTN_X, idev->keybit);
	set_bit(BTN_Y, idev->keybit);
	set_bit(BTN_Z, idev->keybit);
	set_bit(BTN_TL, idev->keybit);
	set_bit(BTN_TR, idev->keybit);
	set_bit(BTN_START, idev->keybit);
	set_bit(BTN_0, idev->keybit);
	set_bit(BTN_1, idev->keybit);
	set_bit(BTN_2, idev->keybit);
	set_bit(BTN_3, idev->keybit);

	/* a stick */
	set_bit(ABS_X, idev->absbit);
	idev->absmin[ABS_X] = 0;
	idev->absmax[ABS_X] = 255;
	idev->absfuzz[ABS_X] = 8;
	idev->absflat[ABS_X] = 8;

	set_bit(ABS_Y, idev->absbit);
	idev->absmin[ABS_Y] = 0;
	idev->absmax[ABS_Y] = 255;
	idev->absfuzz[ABS_Y] = 8;
	idev->absflat[ABS_Y] = 8;

	/* b pad */
	set_bit(ABS_HAT0X, idev->absbit);
	idev->absmin[ABS_HAT0X] = -1;
	idev->absmax[ABS_HAT0X] = 1;

	set_bit(ABS_HAT0Y, idev->absbit);
	idev->absmin[ABS_HAT0Y] = -1;
	idev->absmax[ABS_HAT0Y] = 1;

	/* c stick */
	set_bit(ABS_RX, idev->absbit);
	idev->absmin[ABS_RX] = 0;
	idev->absmax[ABS_RX] = 255;
	idev->absfuzz[ABS_RX] = 8;
	idev->absflat[ABS_RX] = 8;

	set_bit(ABS_RY, idev->absbit);
	idev->absmin[ABS_RY] = 0;
	idev->absmax[ABS_RY] = 255;
	idev->absfuzz[ABS_RY] = 8;
	idev->absflat[ABS_RY] = 8;

	/* triggers */
	set_bit(ABS_GAS, idev->absbit);
	idev->absmin[ABS_GAS] = -255;
	idev->absmax[ABS_GAS] = 255;
	idev->absfuzz[ABS_GAS] = 16;
	idev->absflat[ABS_GAS] = 16;

	set_bit(ABS_BRAKE, idev->absbit);
	idev->absmin[ABS_BRAKE] = -255;
	idev->absmax[ABS_BRAKE] = 255;
	idev->absfuzz[ABS_BRAKE] = 16;
	idev->absflat[ABS_BRAKE] = 16;

	/* rumbling */
	set_bit(EV_FF, idev->evbit);
	set_bit(FF_RUMBLE, idev->ffbit);
	error = input_ff_create(idev, 1);
	if (error)
		return error;
	ff = idev->ff;
	idev->event = gcn_si_event;
	return 0;
}

/**
 *
 */
static void si_setup_keyboard(struct input_dev *idev)
{
	int i;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_REP, idev->evbit);

	for (i = 0; i < 255; ++i)
		set_bit(gamecube_keymap[i], idev->keybit);
}

/**
 *
 */
static int si_setup_device(struct si_device *sdev, int idx)
{
	struct input_dev *idev;
	int result = 0;

	memset(sdev, 0, sizeof(*sdev));	

	/* probe port */
	sdev->si_id = gcn_si_get_controller_id(idx) >> 16;

	/* convert si_id to id */
	if (sdev->si_id == ID_PAD) {
		sdev->id = CTL_PAD;
		strcpy(sdev->name, "standard pad");
	} else if (sdev->si_id & ID_WIRELESS_BIT) {
		sdev->id = CTL_PAD;
		strcpy(sdev->name,(sdev->si_id & ID_WAVEBIRD_BIT) ?
		       "Nintendo Wavebird" : "wireless pad");
	} else if (sdev->si_id == ID_KEYBOARD) {
		sdev->id = CTL_KEYBOARD;
		strcpy(sdev->name, "keyboard");
	} else {
		sdev->id = CTL_UNKNOWN;
		if (sdev->si_id) {
			sprintf(sdev->name, "unknown (%x)", 
				sdev->si_id);
#ifdef HACK_FORCE_KEYBOARD_PORT
			if (idx+1 == gcn_si_force_keyboard_port) {
				si_printk(KERN_WARNING,
					  "port %d forced to keyboard mode\n",
					  idx+1);
				sdev->si_id = ID_KEYBOARD;
				sdev->id = CTL_KEYBOARD;
				strcpy(sdev->name, "keyboard (forced)");
			}
#endif /* HACK_FORCE_KEYBOARD_PORT */
		} else {
			strcpy(sdev->name, "Not Present");
		}
	}

	if (sdev->id == CTL_UNKNOWN) {
		result = -ENODEV;
		goto done;
	}

	idev = input_allocate_device();		
	if (!idev) {
		si_printk(KERN_ERR, "not enough memory for input device\n");
		result = -ENOMEM;
		goto done;
	}

	idev->open = gcn_si_open;
	idev->close = gcn_si_close;
	idev->private = (unsigned int *)idx;
	idev->name = sdev->name;
		
	switch (sdev->id) {
	case CTL_PAD:
		result = si_setup_pad(idev);
		break;
	case CTL_KEYBOARD:
		si_setup_keyboard(idev);
		break;
	default:
		/* this is here to avoid compiler warnings */
		break;
	}

	if (result) {
		input_free_device(idev);
		goto done;
	}

	sdev->idev = idev;

done:
	return result;
}

/**
 *
 */
static int __init gcn_si_init(void)
{
	struct si_device *sdev;
	int idx;
	int result;

        si_printk(KERN_INFO, "%s\n", DRV_DESCRIPTION);

	if (request_resource(&iomem_resource, &gcn_si_resources) < 0) {
		printk(KERN_WARNING PFX "resource busy\n");
		return -EBUSY;
	}

	for (idx = 0; idx < SI_MAX_PORTS; ++idx) {
		sdev = &port[idx];

		result = si_setup_device(sdev, idx);
		if (!result)
			input_register_device(sdev->idev);

		si_printk(KERN_INFO, "Port %d: %s\n", idx+1, sdev->name);
	}

	gcn_si_set_polling();

	return 0;
}

/**
 *
 */
static void __exit gcn_si_exit(void)
{
	struct si_device *sdev;
	int idx;

	si_printk(KERN_INFO, "exit\n");

	for (idx = 0; idx < SI_MAX_PORTS; ++idx) {
		sdev = &port[idx];
		if (sdev->idev)
			input_unregister_device(sdev->idev);
	}

	release_resource(&gcn_si_resources);
}

module_init(gcn_si_init);
module_exit(gcn_si_exit);


