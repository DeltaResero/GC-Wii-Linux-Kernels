/*
 * drivers/block/gcn-memcard.c
 *
 * Nintendo GameCube Memory Card block driver
 * Copyright (C) 2004 The GameCube Linux Team
 *
 * Based on work from Torben Nielsen.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#define DEVICE_NAME "memcard"

#include <linux/major.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */

#include <linux/exi.h>

#include <asm/setup.h>
#include <asm/bitops.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>

#define MEMCARD_MAX_UNITS 2

static int major_num = 0;
module_param(major_num, int, 0);

#define TRUE                  (1)
#define FALSE                 (0)

static int curr_card_sector[MEMCARD_MAX_UNITS] = { -1, -1 };
static int card_sector_mask[MEMCARD_MAX_UNITS] = { 0, 0 };
static int current_device = -1;
static spinlock_t memcard_lock = SPIN_LOCK_UNLOCKED;



static struct block_device_operations memcard_fops;
static struct gendisk *memcard_gendisk[MEMCARD_MAX_UNITS];

static unsigned char *card_sector_buffer[MEMCARD_MAX_UNITS];

#define MEMCARD_READ				1
#define MEMCARD_WRITE				0
#define ALLOW_WRITE
#define CARD_SECTOR_SIZE 0x2000

/*#define CARD_DBG	printk*/
#define CARD_DBG(format, arg...); { }

/* MemCard commands originally by Costis */


#define CARD_FILENAME     32
#define CARD_MAXFILES    127
#define CARD_MAXICONS      8
#define CARD_READSIZE    512
#define CARD_SECTORSIZE 8192
#define CARD_SLOTA         0
#define CARD_SLOTB         1
#define CARD_SYSAREA       5
#define CARD_WRITESIZE   128


#define EXI_CONTROL_TYPE_READ        0
#define EXI_CONTROL_TYPE_WRITE       1
#define EXI_STATUS0	*(unsigned long*)0xCC006800

#define EXI_DMABUF0	*(unsigned long*)0xCC006804
#define EXI_DMALEN0	*(unsigned long*)0xCC006808
#define EXI_DMACNT0	*(unsigned long*)0xCC00680C

#define EXI_CONTROL_DMA              2
#define EXI_CONTROL_ENABLE           1

static int exi_probe(unsigned long channel)
{
	if (*(unsigned long *) (&EXI_STATUS0 + (channel * 5)) & 0x1000)
		return 1;
	else
		return 0;
}

static unsigned long exi_retrieve_id(unsigned long channel,
				     unsigned long device)
{
	unsigned long tID;

	if (exi_probe(channel)) {

		/* Select the specified EXI channel and device. */
		exi_select(channel, device, 0);

		/* Send the EXI ID command (0x0000) */
		tID = 0;
		exi_write(channel, (unsigned char *) &tID, 2);
		/* Read the actual ID data (4 bytes) */
		exi_read(channel, (unsigned char *) &tID, 4);
		/* Deselect the selected EXI device. */
		exi_deselect(channel);

		return tID;
	} else {
		return 0;
	}
}


/*
  Determines if a memcard is present in the given slot.
*/

static int card_is_present(unsigned long channel)
{

	unsigned long id;

	id = exi_retrieve_id(channel, 0);

	if (id & 0xffff0000 || id & 3)
		return 0;

	return id;
}


/**
   Channel must be from 0 to 2.
   Buffer must be aligned to a 32-bit offset.
   Size must be a multiple of 32 bytes.
   Type must be either EXI_CONTROL_TYPE_READ or EXI_CONTROL_TYPE_WRITE. 
*/
static void exi_dma(unsigned long channel, unsigned char *abuffer,
		    unsigned long size, unsigned long type)
{
	/* EXI DMA Operation */
	*(unsigned long *) (&EXI_DMABUF0 + (channel * 5)) =
	    (unsigned long) abuffer & 0x3FFFFE0;
	*(unsigned long *) (&EXI_DMALEN0 + (channel * 5)) =
	    size & 0x3FFFFE0;
	*(unsigned long *) (&EXI_DMACNT0 + (channel * 5)) =
	    EXI_CONTROL_ENABLE | EXI_CONTROL_DMA | (type << 2);

	/* Wait until the EXI DMA operation has been completed. */
	while (*(volatile unsigned long *) (&EXI_DMACNT0 + channel * 5) &
	       EXI_CONTROL_ENABLE);

	return;
}


static unsigned char card_read_status(unsigned long channel)
{
	unsigned char cbuf[4];

	/* Select the specified EXI channel and device. */
	exi_select(channel, 0, 4);

	/* Send the EXI ID command (0x83xx) */
	cbuf[0] = 0x83;		/* Command Byte */
	cbuf[1] = 0x00;
	exi_write(channel, cbuf, 2);
	/* Read the actual ID data (2 bytes) */
	exi_read(channel, cbuf, 1);

	/* Deselect the selected EXI device. */
	exi_deselect(channel);

	return cbuf[0];
}

static void card_read_array(unsigned long channel, unsigned char *abuf,
			    unsigned long address, unsigned long size)
{
	int i;
	unsigned char cbuf[4];
	unsigned char *bbuf = abuf;

	for (i = 0; i < size / CARD_READSIZE; i++) {

		/* Check if the card is ready */
		while (!(card_read_status(channel) & 1));

		/* Select the specified EXI channel and device. */
		exi_select(channel, 0, 4);

		/* Send the EXI Sector Read command (0x52xxxxxx) */
		cbuf[0] = 0x52;	/* Command Byte */
		cbuf[1] = (address >> 17) & 0x3F;
		cbuf[2] = (address >> 9) & 0xFF;
		cbuf[3] = (address >> 7) & 3;
		exi_write(channel, cbuf, 4);

		cbuf[0] = address & 0x7F;
		exi_write(channel, cbuf, 1);

		cbuf[0] = 0;
		cbuf[1] = 0;
		cbuf[2] = 0;
		cbuf[3] = 0;
		exi_write(channel, cbuf, 4);

		exi_dma(channel, bbuf, CARD_READSIZE,
			EXI_CONTROL_TYPE_READ);

		/* Deselect the selected EXI device. */
		exi_deselect(channel);

		address += CARD_READSIZE;
		bbuf += CARD_READSIZE;
	}
}

static void card_sector_erase(unsigned long channel, unsigned long sector)
{
	unsigned char cbuf[3];

	/* Check if the card is ready */
	while (!(card_read_status(channel) & 1));

	/* Select the specified EXI channel and device. */
	exi_select(channel, 0, 4);

	/* Send the EXI Sector Erase command (0xF1xxxx) */
	cbuf[0] = 0xF1;		// Command Byte
	cbuf[1] = (sector >> 17) & 0x7F;
	cbuf[2] = (sector >> 9) & 0xFF;
	exi_write(channel, cbuf, 3);

	/* Deselect the selected EXI device. */
	exi_deselect(channel);

	/* Wait till the erase is finished */
	while (card_read_status(channel) & 0x8000);
}

static void card_sector_program(unsigned long channel, unsigned char *abuf,
				unsigned long address, unsigned long size)
{
	int i;
	unsigned char cbuf[4];
	unsigned char *bbuf = abuf;

	for (i = 0; i < size / CARD_WRITESIZE; i++) {

		/* Check if the card is ready */
		while (!(card_read_status(channel) & 1));

		/* Select the specified EXI channel and device. */
		exi_select(channel, 0, 4);

		/* Send the EXI Sector Program command (0xF2xxxxxx) */
		cbuf[0] = 0xF2;	/* Command Byte */
		cbuf[1] = (address >> 17) & 0x3F;
		cbuf[2] = (address >> 9) & 0xFF;
		cbuf[3] = (address >> 7) & 3;
		exi_write(channel, cbuf, 4);

		cbuf[0] = address & 0x7F;
		exi_write(channel, cbuf, 1);

		exi_dma(channel, bbuf, CARD_WRITESIZE,
			EXI_CONTROL_TYPE_WRITE);

		/* Deselect the selected EXI device. */
		exi_deselect(channel);

		/* Wait till the write is finished */
		while (card_read_status(channel) & 0x8000);

		address += CARD_WRITESIZE;
		bbuf += CARD_WRITESIZE;
	}
}

/**
   Block device handling
 */

static int memcard_buffersize(int channel)
{
	return card_is_present(channel) << 17;
}

static void card_flush(int channel)
{
	int i, mask;
	unsigned long start = curr_card_sector[channel] * CARD_SECTOR_SIZE;
	CARD_DBG("card_flush(%d)\n", channel);
	if (curr_card_sector[channel] == -1)
		return;
	CARD_DBG("Flush channel = %d mask = %x\n", channel,
		 card_sector_mask[channel]);
	for (i = 0, mask = ~card_sector_mask[channel]; i < 16; i++) {
		if (mask & 1) {
			CARD_DBG("card_flush: read block %d\n", i);
		}
	}
	CARD_DBG("card_flush: writing out ch = %d, start = %08lx\n",
		 channel, start);
	flush_dcache_range((unsigned long) card_sector_buffer[channel],
			   (unsigned long) card_sector_buffer[channel] +
			   CARD_SECTOR_SIZE);

	card_sector_erase(channel, start);
	card_sector_program(channel, card_sector_buffer[channel], start,
			    CARD_SECTOR_SIZE);
	card_sector_mask[channel] = 0;
	curr_card_sector[channel] = -1;
}

static void do_memcard_request(request_queue_t * q)
{
	struct request *req;
	blk_stop_queue(q);
	spin_lock(&memcard_lock);

	while ((req = elv_next_request(q)) != NULL) {
		unsigned long start = req->sector << 9;
		unsigned long len = req->current_nr_sectors << 9;
		int channel = req->rq_disk->first_minor;

		if (start + len > memcard_buffersize(channel)) {
			printk(KERN_ERR DEVICE_NAME
			       ": bad access: block=%lu, count=%u\n",
			       (unsigned long) req->sector,
			       req->current_nr_sectors);
			end_request(req, 0);
			continue;
		}

		if (rq_data_dir(req) == READ) {
			CARD_DBG
			    ("do_memcard_request: READ(%s,%lu,%lu), channel = %d\n",
			     req->rq_disk->disk_name,
			     (unsigned long) req->sector,
			     (unsigned long) req->current_nr_sectors,
			     channel);

			card_flush(channel);
			flush_dcache_range((unsigned long) req->buffer,
					   (unsigned long) req->buffer +
					   len);
			card_read_array(channel, req->buffer, start, len);
			invalidate_dcache_range((unsigned long) req->
						buffer,
						(unsigned long) req->
						buffer + len);

		} else {
			int i;
			CARD_DBG
			    ("do_memcard_request: WRITE channel = %d\n",
			     channel);
			for (i = 0; i < req->current_nr_sectors; i++) {
				int new_card_sector =
				    (req->sector + i) >> 4;
				int card_sector_idx =
				    (req->sector + i) & 0xf;
				if (new_card_sector !=
				    curr_card_sector[channel]) {
					card_flush(channel);
					curr_card_sector[channel] =
					    new_card_sector;
				}
				card_sector_mask[channel] |=
				    1 << card_sector_idx;
				CARD_DBG("update block %d, sector %d\n",
					 card_sector_idx, new_card_sector);
				memcpy(card_sector_buffer[channel] +
				       (card_sector_idx << 9),
				       req->buffer + (i << 9), 1 << 9);
				if (card_sector_idx == 0xF)
					card_flush(channel);
			}
		}


		end_request(req, 1);
	}

	spin_unlock(&memcard_lock);
	blk_start_queue(q);

}


static int memcard_open(struct inode *inode, struct file *filp)
{
	CARD_DBG("MEMCARD Open device\n");

	int device;
	int rc = -ENOMEM;

	device = iminor(inode);

	if (current_device != -1 && current_device != device) {
		rc = -EBUSY;
		goto err_out;
	}

	if (memcard_buffersize(device) == 0) {
		rc = -ENODEV;
		goto err_out;
	}

	return 0;

      err_out:
	CARD_DBG("MEMCARD Open device Error %d\n", rc);

	return rc;

}

static int memcard_ioctl(struct inode *inode, struct file *file,
			 unsigned int cmd, unsigned long arg)
{
	CARD_DBG("MEMCARD IOCTL\n");

	if (cmd == HDIO_GETGEO) {
		long size;
		struct hd_geometry geo;
		/*
		 * get geometry: we have to fake one...  trim the size to a
		 * multiple of 1024 (0.5M): tell we have 32 sectors, 32 heads,
		 * whatever cylinders.
		 */
		size = memcard_buffersize(iminor(inode));
		geo.heads = 32;
		geo.sectors = 32;
		geo.start = 0;
		geo.cylinders = size / (geo.heads * geo.sectors);

		if (copy_to_user((void *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	}

	return -EINVAL;
}

static int memcard_release(struct inode *inode, struct file *filp)
{
	CARD_DBG("MEMCARD Close device\n");
	card_flush(iminor(inode));
	if (current_device == -1)
		return 0;

	return 0;
}


static int memcard_revalidate(struct gendisk *disk)
{
	CARD_DBG("MEMCARD Revalidate\n");
	set_capacity(disk, memcard_buffersize(disk->first_minor) >> 9);
	return 0;
}

static struct block_device_operations memcard_fops = {
	.owner = THIS_MODULE,
	.open = memcard_open,
	.release = memcard_release,
	.revalidate_disk = memcard_revalidate,
	.ioctl = memcard_ioctl,
};


static struct request_queue *memcard_queue[MEMCARD_MAX_UNITS];

int __init memcard_init(void)
{
	int ret;
	int i;

	CARD_DBG("MemCard Block Device Driver Init\n");

	ret = -EBUSY;
	major_num = register_blkdev(major_num, DEVICE_NAME);
	if (major_num <= 0)
		goto err;

	ret = -ENOMEM;
	for (i = 0; i < MEMCARD_MAX_UNITS; i++) {
		memcard_gendisk[i] = alloc_disk(1);
		if (!memcard_gendisk[i])
			goto out_disk;
	}

	for (i = 0; i < MEMCARD_MAX_UNITS; i++) {
		memcard_queue[i] =
		    blk_init_queue(do_memcard_request, &memcard_lock);
		if (!memcard_queue[i])
			goto out_queue;
	}

	for (i = 0; i < MEMCARD_MAX_UNITS; i++) {
		memcard_gendisk[i]->major = major_num;
		memcard_gendisk[i]->first_minor = i;
		memcard_gendisk[i]->fops = &memcard_fops;
		sprintf(memcard_gendisk[i]->disk_name, "memcard%d", i);
		strcpy(memcard_gendisk[i]->devfs_name,
		       memcard_gendisk[i]->disk_name);

		memcard_gendisk[i]->queue = memcard_queue[i];
		set_capacity(memcard_gendisk[i],
			     memcard_buffersize(i) >> 9);

		add_disk(memcard_gendisk[i]);
	}

	spin_lock_init(&memcard_lock);
	
	for (i = 0; i < MEMCARD_MAX_UNITS; i++) {
	  card_sector_buffer[i] = kmalloc(CARD_SECTOR_SIZE, GFP_KERNEL);
	}

	return 0;

      out_queue:
	for (i = 0; i < MEMCARD_MAX_UNITS; i++)
		put_disk(memcard_gendisk[i]);
      out_disk:
	unregister_blkdev(major_num, DEVICE_NAME);
      err:
	return ret;
}


void __exit memcard_cleanup(void)
{
	int i;
	blk_unregister_region(MKDEV(major_num, 0), 256);
	for (i = 0; i < MEMCARD_MAX_UNITS; i++) {
		del_gendisk(memcard_gendisk[i]);
		put_disk(memcard_gendisk[i]);
	}

	if (unregister_blkdev(major_num, DEVICE_NAME) != 0)
		printk(KERN_ERR DEVICE_NAME
		       ": unregister of device failed\n");

	for (i = 0; i < MEMCARD_MAX_UNITS; i++) {
		blk_cleanup_queue(memcard_queue[i]);
		kfree(card_sector_buffer[i]);
	}


	CARD_DBG("Removed gc_memcard\n");
	return;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Torben Nielsen");
module_init(memcard_init);
module_exit(memcard_cleanup);
