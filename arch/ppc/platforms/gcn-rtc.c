/*
 * arch/ppc/platforms/gcn-rtc.c
 *
 * Nintendo GameCube RTC/SRAM functions
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2005 Albert Herranz
 *
 * Based on gamecube_time.c from Torben Nielsen.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/time.h>
#include <linux/exi.h>
#include <asm/machdep.h>

#define DRV_MODULE_NAME   "gcn-rtc"
#define DRV_DESCRIPTION   "Nintendo GameCube RTC/SRAM driver"
#define DRV_AUTHOR        "Albert Herranz"

static char rtc_driver_version[] = "1.4";

#define rtc_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)


#define RTC_EXI_ID	0xFFFF1698

#define RTC_EXI_CHANNEL	0
#define RTC_EXI_DEVICE	1
#define RTC_EXI_FREQ	3 /* 8MHz */

#define RTC_OFFSET	946684800L


struct gcn_sram {
	u16     csum1;
	u16     csum2;
	u32     ead0;
	u32     ead1;
	int     bias;
	s8      horz_display_offset;
	u8      ntd;
	u8      language;
	u8      flags;
	u8      reserved[44];
};

struct rtc_private {
	spinlock_t		lock;
	struct exi_device	*dev;

	struct gcn_sram         sram;
};

static struct rtc_private rtc_private;

/*
 * Loads the SRAM contents.
 * Context: user.
 */
static void sram_load(struct exi_device *dev)
{
	struct rtc_private *priv = exi_get_drvdata(dev);
	struct gcn_sram *sram = &priv->sram;
	u32 req;

	exi_dev_take(dev);

	/* select the SRAM device */
	exi_dev_select(dev);

	/* send the appropriate command */
	req = 0x20000100;
	exi_dev_write(dev, &req, sizeof(req));

	/* read the SRAM data */
	exi_dev_read(dev, sram, sizeof(*sram));

	/* deselect the SRAM device */
	exi_dev_deselect(dev);

	exi_dev_give(dev);

	return;
}

/*
 * Gets the hardware clock date and time.
 * Context: user.
 */
static unsigned long rtc_get_time(struct exi_device *dev)
{
	unsigned long a = 0;

	exi_dev_take(dev);

	/* select the SRAM device */
	exi_dev_select(dev);

	/* send the appropriate command */
	a = 0x20000000;
	exi_dev_write(dev, &a, sizeof(a));

	/* read the time and date value */
	exi_dev_read(dev, &a, sizeof(a));

	/* deselect the RTC device */
	exi_dev_deselect(dev);

	exi_dev_give(dev);

	return a;
}

/*
 * Sets the hardware clock date and time to @aval.
 * Context: user, interrupt (adjtimex).
 */
static int rtc_set_time(struct exi_device *dev, unsigned long aval)
{
	u32 req;
	int retval;

	/*
	 * We may get called from the timer interrupt. In that case,
	 * we could fail if the exi channel used to access the RTC
	 * is busy. If this happens, we just return an error. The timer
	 * interrupt code is prepared to deal with such case.
	 */

	retval = exi_dev_try_take(dev);
	if (!retval) {
		/* select the RTC device */
		exi_dev_select(dev);

		/* send the appropriate command */
		req = 0xa0000000;
		exi_dev_write(dev, &req, sizeof(req));

		/* set the new time and date value */
		exi_dev_write(dev, &aval, sizeof(aval));

		/* deselect the RTC device */
		exi_dev_deselect(dev);

		exi_dev_give(dev);
	}
	return retval;
}

/**
 * Platform specific function to return the current date and time.
 */
static unsigned long gcn_get_rtc_time(void)
{
	struct rtc_private *priv = &rtc_private;

	return rtc_get_time(priv->dev) + priv->sram.bias + RTC_OFFSET;
}

/**
 * Platform specific function to set the current date and time.
 *
 */
static int gcn_set_rtc_time(unsigned long nowtime)
{
	struct rtc_private *priv = &rtc_private;

	return rtc_set_time(priv->dev, nowtime - RTC_OFFSET - priv->sram.bias);
}

/**
 *
 */
static void rtc_remove(struct exi_device *dev)
{
	struct rtc_private *priv = exi_get_drvdata(dev);
	unsigned long flags;

	if (priv) {
		spin_lock_irqsave(&priv->lock, flags);
		ppc_md.set_rtc_time = NULL;
		ppc_md.get_rtc_time = NULL;
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	exi_device_put(dev);
}

/**
 *
 */
static int rtc_probe(struct exi_device *dev)
{
	struct rtc_private *priv = &rtc_private;
	unsigned long flags;
	int retval = -ENODEV;

	if (exi_device_get(dev)) {
		spin_lock_init(&priv->lock);

		exi_set_drvdata(dev, priv);
		priv->dev = dev;

		memset(&priv->sram, 0, sizeof(struct gcn_sram));
		sram_load(dev);

		spin_lock_irqsave(&priv->lock, flags);
		ppc_md.set_rtc_time = gcn_set_rtc_time;
		ppc_md.get_rtc_time = gcn_get_rtc_time;
		spin_unlock_irqrestore(&priv->lock, flags);
		
		retval = 0;
	}

	return retval;
}

static struct exi_device_id rtc_eid_table[] = {
        [0] = {
                .channel = RTC_EXI_CHANNEL,
                .device  = RTC_EXI_DEVICE,
                .id      = RTC_EXI_ID
        },
        { .id = 0 }
};

static struct exi_driver rtc_driver = {
	.name = "gcn-rtc",
	.eid_table = rtc_eid_table,
	.frequency = RTC_EXI_FREQ,
	.probe = rtc_probe,
	.remove = rtc_remove,
};

static int __init rtc_init_module(void)
{
	rtc_printk(KERN_INFO, "%s - version %s\n",
		   DRV_DESCRIPTION, rtc_driver_version);

	return exi_driver_register(&rtc_driver);
}

static void __exit rtc_exit_module(void)
{
	exi_driver_unregister(&rtc_driver);
}

module_init(rtc_init_module);
module_exit(rtc_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

