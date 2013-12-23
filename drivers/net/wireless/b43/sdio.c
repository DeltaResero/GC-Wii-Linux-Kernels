/*
 * drivers/net/wireless/b43/sdio.c
 *
 * Broadcom B43 wireless driver
 *
 * SDIO over Sonics Silicon Backplane bus glue for b43.
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/ssb/ssb.h>

#include "sdio.h"


#define HNBU_CHIPID		0x01	/* vendor & device id */

#define B43_SDIO_BLOCK_SIZE	64	/* rx fifo max size in bytes */

static int b43_sdio_probe(struct sdio_func *func,
			  const struct sdio_device_id *id)
{
	struct ssb_bus *ssb;
	struct b43_sdio_dev_wrapper *wrapper;
	struct sdio_func_tuple *tuple;
	u16 vendor = 0, device = 0;
	unsigned int quirks = 0;
	int error = -ENOMEM;

	/* Look for the card chip identifier. */
	tuple = func->tuples;
	while (tuple) {
		switch (tuple->code) {
		case 0x80:
			switch (tuple->data[0]) {
			case HNBU_CHIPID:
				if (tuple->size != 5)
					break;
				vendor = tuple->data[1] | (tuple->data[2]<<8);
				device = tuple->data[3] | (tuple->data[4]<<8);
				dev_info(&func->dev, "Chip ID %04x:%04x\n",
					 vendor, device);
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
		tuple = tuple->next;
	}

	/*
	 * FIXME, maybe convert to a lookup table with quirks.
	 */
	switch (vendor) {
	case 0x14e4:
		switch (device) {
		case 0x4318:
			quirks |= SSB_QUIRK_SDIO_READ_AFTER_WRITE32;
			goto chip_accepted;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	/*
	 * Bail out for untested chips for now.
	 */
	error = -ENODEV;
	goto err_out;

chip_accepted:
	sdio_claim_host(func);
	error = sdio_set_block_size(func, B43_SDIO_BLOCK_SIZE);
	if (error) {
		dev_err(&func->dev, "failed to set block size to %u bytes,"
			" error %d\n", B43_SDIO_BLOCK_SIZE, error);
		goto err_release_host;
	}
	error = sdio_enable_func(func);
	if (error) {
		dev_err(&func->dev, "failed to enable func, error %d\n", error);
		goto err_release_host;
	}
	sdio_release_host(func);

	ssb = kzalloc(sizeof(*ssb) + sizeof(*wrapper), GFP_KERNEL);
	if (!ssb) {
		dev_err(&func->dev, "failed to allocate ssb bus\n");
		goto err_disable_func;
	}
	error = ssb_bus_sdiobus_register(ssb, func, quirks);
	if (error) {
		dev_err(&func->dev, "failed to register ssb sdio bus,"
			" error %d\n", error);
		goto err_free_ssb;
	}
	wrapper = (struct b43_sdio_dev_wrapper *)(ssb + 1);
	wrapper->ssb = ssb;
	sdio_set_drvdata(func, wrapper);

	goto out;

err_free_ssb:
	kfree(ssb);
err_disable_func:
	sdio_disable_func(func);
err_release_host:
	sdio_release_host(func);
err_out:
	if (error)
		pr_devel("error %d\n", error);
out:
	return error;
}

static void b43_sdio_remove(struct sdio_func *func)
{
	struct b43_sdio_dev_wrapper *wrapper = sdio_get_drvdata(func);
	struct ssb_bus *ssb;

	if (!wrapper)
		goto out;

	ssb = wrapper->ssb;
	ssb_bus_unregister(ssb);
	wrapper->ssb = NULL;
	sdio_disable_func(func);
	kfree(ssb);
	sdio_set_drvdata(func, NULL);

out:
	return;
}

static const struct sdio_device_id b43_sdio_ids[] = {
	/* Nintendo Wii WLAN daughter card */
	{ SDIO_DEVICE(0x02d0, 0x044b) },
	{},
};

static struct sdio_driver b43_sdio_driver = {
	.name		= "b43-sdio",
	.id_table	= b43_sdio_ids,
	.probe		= b43_sdio_probe,
	.remove		= b43_sdio_remove,
};

int b43_sdio_init(void)
{
	int error;

	error = sdio_register_driver(&b43_sdio_driver);

	return error;
}

void b43_sdio_exit(void)
{
	sdio_unregister_driver(&b43_sdio_driver);
}

