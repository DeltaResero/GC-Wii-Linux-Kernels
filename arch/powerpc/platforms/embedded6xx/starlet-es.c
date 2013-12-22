/*
 * arch/powerpc/platforms/embedded6xx/starlet-es.c
 *
 * Nintendo Wii starlet ES routines
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define DEBUG

#include <linux/kernel.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <asm/starlet.h>

#define DRV_MODULE_NAME		"starlet-es"
#define DRV_DESCRIPTION		"Nintendo Wii starlet ES driver"
#define DRV_AUTHOR		"Albert Herranz"

static const char starlet_es_driver_version[] = "0.2i";

#define DBG(fmt, arg...)	pr_debug(fmt, ##arg)

#define drv_printk(level, format, arg...) \
	 printk(level DRV_MODULE_NAME ": " format , ## arg)

struct starlet_es_device {
	int fd;

	struct device *dev;
};

struct starlet_es_ticket_limit {
	u32 tag;
	u32 value;
} __attribute__((packed));

struct starlet_es_ticket_view {
	u32 view;
	u64 ticketid;
	u32 devicetype;
	u64 title;
	u16 access_mask;
	u8 reserved[0x3c];
	u8 cidx_mask[0x40];
	u16 padding;
	struct starlet_es_ticket_limit limits[8];
} __attribute__((packed));

#if 0
struct starlet_es_ticket {
	char issuer[0x40];
	u8 fill[63]; /* TODO: not really fill */
	u8 title_key[16];
	u8 fill2;
	u64 ticketid;
	u32 devicetype;
	u64 title;
	u16 access_mask;
	u8 reserved[0x3c];
	u8 cidx_mask[0x40];
	u16 padding;
	struct starlet_es_ticket_limit limits[8];
} __attribute__((packed));
#endif

/*
 * /dev/es
 *
 */

#define ES_IOCTLV_LAUNCHTITLE		0x08
#define ES_IOCTLV_GETTITLECOUNT		0x0e
#define ES_IOCTLV_GETTITLES		0x0f
#define ES_IOCTLV_GETTICKETVIEWCOUNT	0x12
#define ES_IOCTLV_GETTICKETVIEWS	0x13

static const char dev_es[] = "/dev/es";

/*
 * Handy small buffer routines.
 * We use a small static aligned buffer to avoid allocations for short-lived
 * operations involving 1 to 4 byte data transfers to/from IOS.
 *
 */

static u32 es_small_buf[L1_CACHE_BYTES / sizeof(u32)]
		    __attribute__ ((aligned(STARLET_IPC_DMA_ALIGN + 1)));
static const size_t es_small_buf_size = sizeof(es_small_buf_size);
static DEFINE_MUTEX(es_small_buf_lock);

static u32 *es_small_buf_get(void)
{
	u32 *buf;

	if (!mutex_trylock(&es_small_buf_lock))
		buf = starlet_kzalloc(es_small_buf_size, GFP_KERNEL);
	else {
		memset(es_small_buf, 0, es_small_buf_size);
		buf = es_small_buf;
	}

	return buf;
}

static void es_small_buf_put(u32 *buf)
{
	if (buf == es_small_buf)
		mutex_unlock(&es_small_buf_lock);
	else
		starlet_kfree(buf);
}

#if 0
static void es_small_buf_dump(void)
{
	int i;
	size_t nelems = sizeof(es_small_buf) / sizeof(u32);

	drv_printk(KERN_INFO, "es_small_buf[%d]= {\n", nelems);
	for (i = 0; i < nelems; i++)
		drv_printk(KERN_INFO, "%08x, ", es_small_buf[i]);
	drv_printk(KERN_INFO, "\n}\n");

}
#endif

/*
 *
 *
 */

static struct starlet_es_device *starlet_es_device_instance;

/**
 *
 */
struct starlet_es_device *starlet_es_get_device(void)
{
	if (!starlet_es_device_instance)
		drv_printk(KERN_ERR, "uninitialized device instance!\n");
	return starlet_es_device_instance;
}
EXPORT_SYMBOL_GPL(starlet_es_get_device);

/**
 *
 */
int starlet_es_get_title_count(unsigned long *count)
{
	struct starlet_es_device *es_dev = starlet_es_get_device();
	struct scatterlist io[1];
	u32 *count_buf;
	int error;

	if (!es_dev)
		return -ENODEV;

	count_buf = es_small_buf_get();
	if (!count_buf)
		return -ENOMEM;

	*count_buf = 0;
	sg_init_one(io, count_buf, sizeof(*count_buf));

	error = starlet_ioctlv(es_dev->fd, ES_IOCTLV_GETTITLECOUNT,
				   0, NULL, 1, io);
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	else
		*count = *count_buf;

	es_small_buf_put(count_buf);

	return error;
}

/**
 *
 */
int starlet_es_get_titles(u64 *titles, unsigned long count)
{
	struct starlet_es_device *es_dev = starlet_es_get_device();
	struct scatterlist in[1], io[1];
	u32 *count_buf;
	int error;

	if (!es_dev)
		return -ENODEV;

	count_buf = es_small_buf_get();
	if (!count_buf)
		return -ENOMEM;

	*count_buf = count;
	sg_init_one(in, count_buf, sizeof(*count_buf));
	sg_init_one(io, titles, sizeof(*titles)*count);

	error = starlet_ioctlv(es_dev->fd, ES_IOCTLV_GETTITLES,
				   1, in, 1, io);
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	es_small_buf_put(count_buf);

	return error;
}

/**
 *
 */
int starlet_es_get_ticket_view_count(u64 title, unsigned long *count)
{
	struct starlet_es_device *es_dev = starlet_es_get_device();
	struct scatterlist in[1], io[1];
	u64 *title_buf;
	u32 *count_buf;
	int error;

	if (!es_dev)
		return -ENODEV;

	title_buf = starlet_kzalloc(sizeof(*title_buf), GFP_KERNEL);
	if (!title_buf)
		return -ENOMEM;

	count_buf = es_small_buf_get();
	if (!count_buf) {
		starlet_kfree(title_buf);
		return -ENOMEM;
	}

	*title_buf = title;
	sg_init_one(in, title_buf, sizeof(*title_buf));
	sg_init_one(io, count_buf, sizeof(*count_buf));

	error = starlet_ioctlv(es_dev->fd, ES_IOCTLV_GETTICKETVIEWCOUNT,
				   1, in, 1, io);
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	else
		*count = *count_buf;

	starlet_kfree(title_buf);
	es_small_buf_put(count_buf);

	return error;
}

/**
 *
 */
int starlet_es_get_ticket_views(u64 title,
				struct starlet_es_ticket_view *views,
				unsigned long count)
{
	struct starlet_es_device *es_dev = starlet_es_get_device();
	struct scatterlist in[2], io[1];
	u32 *count_buf;
	u64 *title_buf;
	int error;

	if (!es_dev)
		return -ENODEV;

	title_buf = starlet_kzalloc(sizeof(*title_buf), GFP_KERNEL);
	if (!title_buf)
		return -ENOMEM;

	count_buf = es_small_buf_get();
	if (!count_buf) {
		starlet_kfree(title_buf);
		return -ENOMEM;
	}

	*title_buf = title;
	*count_buf = count;
	sg_init_table(in, 2);
	sg_set_buf(&in[0], title_buf, sizeof(*title_buf));
	sg_set_buf(&in[1], count_buf, sizeof(*count_buf));

	sg_init_one(io, views, sizeof(*views)*count);

	error = starlet_ioctlv(es_dev->fd, ES_IOCTLV_GETTICKETVIEWS,
				   2, in, 1, io);
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	es_small_buf_put(count_buf);
	starlet_kfree(title_buf);

	return error;
}

/**
 *
 */
int starlet_es_launch_title_view(u64 title, struct starlet_es_ticket_view *view)
{
	struct starlet_es_device *es_dev = starlet_es_get_device();
	struct scatterlist in[2];
	u64 *title_buf;
	int error;

	if (!es_dev)
		return -ENODEV;

	title_buf = starlet_kzalloc(sizeof(*title_buf), GFP_KERNEL);
	if (!title_buf)
		return -ENOMEM;

	*title_buf = title;
	sg_init_table(in, 2);
	sg_set_buf(&in[0], title_buf, sizeof(*title_buf));
	sg_set_buf(&in[1], view, sizeof(*view));

	error = starlet_ioctlv_and_reboot(es_dev->fd,
					      ES_IOCTLV_LAUNCHTITLE,
					      2, in, 0, NULL);
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	starlet_kfree(title_buf);

	return error;
}



/*
 * Setup routines.
 *
 */

#define STARLET_ES_IOS_MIN 30
#define STARLET_ES_IOS_MAX 36

static int starlet_es_find_newest_title(struct starlet_es_device *es_dev,
					u64 *title,
					u64 title_min, u64 title_max)
{
	u64 *titles;
	u64 candidate;
	unsigned long count;
	int found, i;
	int error;

	error = starlet_es_get_title_count(&count);
	if (error)
		return error;

	titles = starlet_kzalloc(sizeof(*titles)*count, GFP_KERNEL);
	if (!titles) {
		DBG("%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	error =	starlet_es_get_titles(titles, count);
	if (error) {
		starlet_kfree(titles);
		return error;
	}

	found = 0;
	candidate = title_min;
	for (i = 0; i < count; i++) {
		if (titles[i] > candidate && titles[i] <= title_max) {
			candidate = titles[i];
			found = 1;
		}
	}

	starlet_kfree(titles);

	if (!found)
		return 0;

	*title = candidate;

	return 1;
}

static int starlet_es_launch_title(struct starlet_es_device *es_dev, u64 title)
{
	struct starlet_es_ticket_view *views;
	unsigned long count;
	int error;

	error = starlet_es_get_ticket_view_count(title, &count);
	if (error)
		return error;

	views = starlet_kzalloc(sizeof(*views)*count, GFP_KERNEL);
	if (!views) {
		DBG("%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	error =	starlet_es_get_ticket_views(title, views, count);
	if (error) {
		starlet_kfree(views);
		return error;
	}

	drv_printk(KERN_INFO, "launching IOS%u\n", (u32)(title & 0xffffffff));
	error = starlet_es_launch_title_view(title, views); /* first view */

	starlet_kfree(views);

	return error;
}

static int starlet_es_load_preferred(struct starlet_es_device *es_dev,
					 u64 ios_min, u64 ios_max)
{
	u64 title;
	int error;

	error = starlet_es_find_newest_title(es_dev, &title, ios_min, ios_max);
	if (!error)
		return -EINVAL;
	if (error > 0)
		error = starlet_es_launch_title(es_dev, title);

	return error;
}

static int starlet_nwc24_stop_scheduler(void)
{
	void *obuf;
	const size_t osize = 0x20;
	int fd;
	int error = 0;

	obuf = es_small_buf_get();
	if (!obuf)
		return -ENOMEM;

	fd = starlet_open("/dev/net/kd/request", 0);
	if (fd >= 0) {
		error = starlet_ioctl(fd, 1, NULL, 0, obuf, osize);
		starlet_close(fd);
	}

	es_small_buf_put(obuf);

	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}

static int starlet_es_init(struct starlet_es_device *es_dev)
{
	u64 ios_min, ios_max;
	int error;

	error = starlet_open(dev_es, 0);
	if (error >= 0) {
		starlet_es_device_instance = es_dev;
		es_dev->fd = error;

		ios_min = 0x100000000ULL | STARLET_ES_IOS_MIN;
		ios_max = 0x100000000ULL | STARLET_ES_IOS_MAX;

		error = starlet_es_load_preferred(es_dev, ios_min, ios_max);
		if (error) {
			drv_printk(KERN_WARNING, "unable to load preferred"
				   " IOS version (min %llx, max %llx)\n",
				   ios_min, ios_max);
		}
	}

	/*
	 * Try to disable the Nintendo Wifi Connect 24 scheduler.
	 * And do this even if we failed to load our preferred IOS.
	 *
	 * When the scheduler kicks in, starlet IPC calls from Broadway fail.
	 */
	starlet_nwc24_stop_scheduler();

	return error;
}

static void starlet_es_exit(struct starlet_es_device *es_dev)
{
	starlet_es_device_instance = NULL;
	starlet_close(es_dev->fd);
	es_dev->fd = -1;
}


/*
 * Driver model helper routines.
 *
 */

static int starlet_es_do_probe(struct device *dev)
{
	struct starlet_es_device *es_dev;
	int retval;

	es_dev = kzalloc(sizeof(*es_dev), GFP_KERNEL);
	if (!es_dev) {
		drv_printk(KERN_ERR, "failed to allocate es_dev\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, es_dev);
	es_dev->dev = dev;

	retval = starlet_es_init(es_dev);
	if (retval) {
		dev_set_drvdata(dev, NULL);
		kfree(es_dev);
	}
	return retval;
}

static int starlet_es_do_remove(struct device *dev)
{
	struct starlet_es_device *es_dev = dev_get_drvdata(dev);

	if (es_dev) {
		starlet_es_exit(es_dev);
		dev_set_drvdata(dev, NULL);
		kfree(es_dev);
		return 0;
	}
	return -ENODEV;
}

/*
 * OF platform driver hooks.
 *
 */

static int starlet_es_of_probe(struct of_device *odev,
			       const struct of_device_id *dev_id)
{
	return starlet_es_do_probe(&odev->dev);
}

static int starlet_es_of_remove(struct of_device *odev)
{
	return starlet_es_do_remove(&odev->dev);
}

static struct of_device_id starlet_es_of_match[] = {
	{ .compatible = "nintendo,starlet-es" },
	{ },
};

MODULE_DEVICE_TABLE(of, starlet_es_of_match);

static struct of_platform_driver starlet_es_of_driver = {
	.owner = THIS_MODULE,
	.name = DRV_MODULE_NAME,
	.match_table = starlet_es_of_match,
	.probe = starlet_es_of_probe,
	.remove = starlet_es_of_remove,
};

/*
 * Kernel module interface hooks.
 *
 */

static int __init starlet_es_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   starlet_es_driver_version);

	return of_register_platform_driver(&starlet_es_of_driver);
}

static void __exit starlet_es_exit_module(void)
{
	of_unregister_platform_driver(&starlet_es_of_driver);
}

module_init(starlet_es_init_module);
module_exit(starlet_es_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

