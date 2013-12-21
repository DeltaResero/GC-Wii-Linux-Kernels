/*
 * drivers/block/gcn-sd.c
 *
 * Nintendo GameCube SD/MMC memory card driver
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2004,2005 by Rob Reilink (rob@reilink.net)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/*
 *
 *   THE AUTHOR OF THIS SOFTWARE AND HARDWARE CAN NOT BE HELD LIABLE
 *   FOR ANY POSSIBLE DAMAGE CAUSED TO THE GAMECUBE, THE SD- OR MMC
 *   CARD OR ANY OTHER HARDWARE USED. YOU USE THIS INFORMATION AND
 *   SOFTWARE AT YOUR OWN RISK
 *
 *		How to connect the SD/MMC card to the GC
 *
 *   GC P5 (mem card B)           SD/MMC
 *    2  gnd                    3,6 vss1,vss2
 *    4  3v3                    4   vdd
 *    5  do                     2   DataIn
 *    7  di                     7   DataOut 
 *    9  /cs                    1   /CS
 *    11 clk                    5   clk
 *    1  connected to 12 (card detect)
 *
 *   According to 'good electronics practise' it is advised to solder a 100nF
 *   capacitor between vdd and vss near the SD/MMC connector
 *
 *   Please note the layout of the pads on the sd card:
 *     __________________
 *    /                  |
 *   /   1 2 3 4 5 6 7 8 |
 *   | 9                 |
 *   |                   |
 *   |                   |
 *   |                   |
 *   |                   |
 *   |                   |
 *   |                   |
 *   |                   |
 *   |                   |
 *   |___________________|
 *
 *   Pin 8 and 9 do not exist on the MMC card; that explains the 'weird' numbering
 *
 *  To connect the card to the gamecube you can either solder wires to the bottom
 *  side of the mainboard (they can be led to the outside through a hole in the metal
 *  shielding on the right of the cube, but be sure the wires do not get damaged by
 *  the sharp edge) or scrap a GC memory card. For the last option, ne sure that the
 *  original electronics are disconnected!
 *
 *  If you choose to connect it by soldering, do not use the memory card slot anymore!
 *  
 *  For the connection to the card, it seems you can use an old floppy connector, but
 *  there are also special SD connectors available. The SD connector accepts both SD 
 *  and MMC cards; the MMC connector only accepts MMC cards (they are thinner). It is
 *  therefore better to buy an SD connector.
 *
 *  Note that this software has only been tested with an SD card.
 *  The MMC-adaptor commercially available has not been tested, but might work as well
 */

/* Software hints
 *
 * # This driver is in alpha stage:
 *     Only reading supported, not writing
 *     Card size is reported as about 100MB, it is not read from the card
 *     Cards are NOT hot-pluggable
 *     The driver cannot be compiled as module due to dependence of the
 *         EXI driver which is still 'work in progress'
 *
 * # add the following line to /drivers/block/Makefile:
 *    obj-$(CONFIG_GAMECUBE_SD)	+= gcn-sd.o
 * 
 * # add the following lines to /drivers/block/Kconfig:
 *    config GAMECUBE_SD
 *      tristate "Nintendo GameCube SD and MMC memory card (EXPERIMENTAL)"
 *      depends on GAMECUBE && EXI && EXPERIMENTAL
 *      help
 *        This enables support for SD and MMC memory cards
 *
 * # to support fat, it might be nescessary to select cp437=y under Filesystems->NLS
 *
 * # This is the author's first real kernel driver, so please tell me
 *   how it 'should have been done' ;)
 */

/*
 *  TODO: clean up this
 */
#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_ALERT */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/exi.h>
#include <asm/uaccess.h>
#include <linux/blkdev.h>

#ifndef EXI_LITE
#error Sorry, this driver needs currently the gcn-exi-lite framework.
#endif

#define DEVICE_NAME            "gcn-sd"
#define DRIVER_AUTHOR          "Rob Reilink <rob@reilink.net>"
#define DRIVER_DESC            "Gamecube SD-card driver"
#define DRIVER_MODULE_NAME     "gcn-sd"
#define PFX DRIVER_MODULE_NAME ": "

#define GCN_SD_MAJOR 0

#define SUCCESS 0
#define FAIL -1

#define GCN_SD_SPEED 4

static int Major;
static struct request_queue *queue;

static struct gcn_sd_device_struct {
	unsigned long size;
	spinlock_t lock;
	struct gendisk *gd;
} gcn_sd_device;



/*
 *  Funtions start here
 *
 */

/*
 *  EXI/SD functions
 *
 */
static char gcn_sd_cmd_short_nofinish(unsigned char cmd) {
	unsigned char dat;
	int tries;
	exi_select(1,0,GCN_SD_SPEED);
	dat=cmd|0x40;
	exi_write(1,&dat,1);
	dat=255;
	for (tries=0;(tries < 30) && (dat == 0xff);tries++)
		exi_read(1,&dat,1);

	return dat;
}

static char gcn_sd_cmd_nofinish(unsigned char cmd, unsigned long data) {
	unsigned char snd[6],ret;
	int tries;
	exi_select(1,0,GCN_SD_SPEED);
	snd[0]=cmd|0x40;
	snd[1]=data >> 24;
	snd[2]=(data >> 16) & 0xff;
	snd[3]=(data >> 8) & 0xff;
	snd[4]=data & 0xff;
	snd[5]=0x95;

	exi_write(1,&snd,6);
	
	//wait loop
	ret=0xff;
	for(tries=0;(tries<100) && (ret==0xff);tries++)
		exi_read(1,&ret,1);
	return ret;
}

static void gcn_sd_finish(void) {
	unsigned char zero=0;
	exi_deselect(1);
	/*last dummy write. This may look weird, accessing the device
	  after it has been deselected, but the SD manual states that a SD
	  card needs 8 extra clock cycles *after* deselection, and this is
	  how to generate those
	*/  
	exi_write(1, &zero, 1);

}

inline static char gcn_sd_cmd(unsigned char cmd) {
	char i=gcn_sd_cmd_nofinish(cmd,0);
	gcn_sd_finish();
	return i;
} 

inline static char gcn_sd_cmd_short(unsigned char cmd) {
	char i=gcn_sd_cmd_short_nofinish(cmd);
	gcn_sd_finish();
	return i;
}

/* returns: found?SUCCESS:FAIL */
static int gcn_sd_init(void) {
	int i,ret; long c;
	int tries=0;

	//Send 80 clocks while not selected
	exi_select(1,1,GCN_SD_SPEED);  //SLOOW TODO:fast, select=-1 ofzo (niets selecten)
	exi_deselect(1);
	for(i=0;i<20;i++) {
	 	c=0xffffffff;
		exi_write(1,&c,4);
	}

	
	//send CMD0 to init
	gcn_sd_cmd(0);
	gcn_sd_cmd_short(0);
	for(tries=0;tries<100;tries++) {
		if ((ret=gcn_sd_cmd_short(58))==0) return SUCCESS;
		printk(KERN_ALERT PFX "cmd 58 returned %d\n",ret);
		if ((ret=gcn_sd_cmd_short(1))==0) return SUCCESS;
		printk(KERN_ALERT PFX "cmd 1 returned %d\n",ret);
	}
	return FAIL;
}

#if 0
static int gcn_sd_read_csd(void) {
	unsigned char c;
	unsigned char data[16];
	int tries;
	c=gcn_sd_cmd_short_nofinish(9);
	if (c!=0) {
		gcn_sd_finish();
		printk(KERN_INFO PFX "could not read csd(%d)\n",c);
		return FAIL;
	}
	for (tries=0;(tries<100000) & (c!=0xfe);tries++)
		exi_read(1,&c,1);

	if (c!=0xfe) {
		gcn_sd_finish();
		printk(KERN_ERR PFX "error: no start token after csd read request\n");
		return FAIL;		
	}
	exi_read(1,data,16);
	gcn_sd_finish();
	int i;
	printk(KERN_INFO PFX "CSD data:");
	for (i=0;i<16;i++) printk ("%x ",data[i]);
	printk("\n");

	return SUCCESS;
}
#endif

static int gcn_sd_block_read(unsigned long sector,void * data) {
	unsigned char c;
	long dummy;
	int tries;
	c=gcn_sd_cmd_nofinish(17,(sector) << 9);
	if (c!=0)	{
		gcn_sd_finish();
		printk(KERN_ERR PFX "error: read command returned %d while reading sector %lu\n" ,c,sector);
		return FAIL;
	}

	//Wait for start of data token 0xfe TODO put this in a function
	for (tries=0;(tries<100000) & (c!=0xfe);tries++)
		exi_read(1,&c,1);

	if (c!=0xfe) {
		gcn_sd_finish();
		printk(KERN_ERR PFX "error: no start token after read request sector=%lu\n" ,sector);
		return FAIL;
	}

	exi_read(1,data,512);
	exi_read(1,&dummy,3); //response,CRC, dummy. todo: use it?
	gcn_sd_finish();

	return SUCCESS;
	
}

/*
 * Block driver functions
 *
 */ 

static void gcn_sd_request(request_queue_t *q) {
	struct request *req;
	int sector,tries,success;
	while ((req=elv_next_request(q)) != NULL) {
		if (!blk_fs_request(req)) {
			end_request(req,0);
			continue;
		}
		if (rq_data_dir(req)) {  //write
			printk(KERN_ALERT "WRITE not implemented yet\n");
			
/*			gcn_sd_read_csd();
			gcn_sd_init();
			if (gcn_sd_init()==SUCCESS) {
				printk(KERN_ALERT PFX "resetting succeeded\n");
			} else {
				printk(KERN_ALERT PFX "resetting failed\n");
			}
			end_request(req,1);*/
	
		} else {	
			//read
   		//printk(KERN_ALERT "READ start=%lu num=%u\n", (unsigned long)req->sector,req->current_nr_sectors);
			success=1;
			tries=0;
			//process one sector at a time
			for (sector=0;sector < req->current_nr_sectors;sector++) {
				if (gcn_sd_block_read(req->sector+sector,req->buffer+(sector<<9))==SUCCESS) {
					tries=0;
				} else {
					tries++;
					sector--;
					if (tries>10) {
						success=0;
						break;
					}						
				}
			}
			end_request(req,success); //TODO 0
		}
	}
}


static int gcn_sd_open (struct inode *inode, struct file *file) {
//	printk(KERN_INFO "gcn-sd device driver: open\n");
	return SUCCESS;
}

static int gcn_sd_release(struct inode *inode, struct file *file) {
//	printk(KERN_INFO "gcn-sd device driver: release\n");
	return SUCCESS;
}


struct block_device_operations gcn_sd_bdops = {
  //TODO: ioctl?
	open: gcn_sd_open,
	release: gcn_sd_release
};

static int __init init_gcn_sd(void)
{

	if (gcn_sd_init()!=SUCCESS) {
		printk(KERN_WARNING PFX "No SD-card found; driver not registered\n");
		return 0; //TODO: what should we return?
	}
	printk(KERN_INFO PFX "SD-card found\n");
//	gcn_sd_read_csd();
	//Init device struct
	gcn_sd_device.size=200000*512;
	spin_lock_init(&gcn_sd_device.lock);

	//Registed device, get major number
 	Major = register_blkdev(GCN_SD_MAJOR, DEVICE_NAME);

	if (Major <= 0) {
		printk(KERN_ALERT PFX "Registering the block device failed with %d\n", Major);
		return Major;
	}

	printk(KERN_INFO PFX "got major %d\n",Major);

	//Init gendisk
	gcn_sd_device.gd=alloc_disk(16);
	if (!gcn_sd_device.gd) {
		printk(KERN_ALERT PFX "Could not allocate gendisk\n");
		//TODO:unreg
		return -EFAULT;

	}

	gcn_sd_device.gd->major=Major;
	gcn_sd_device.gd->first_minor=0;
	gcn_sd_device.gd->fops=&gcn_sd_bdops;
	gcn_sd_device.gd->private_data=&gcn_sd_device;
	strcpy(gcn_sd_device.gd->disk_name,"gcnsd0");
	set_capacity(gcn_sd_device.gd, 200000); //TODO

	//init queue
	queue = blk_init_queue(gcn_sd_request, &gcn_sd_device.lock);
	if (queue == NULL) {
		printk(KERN_ALERT PFX "Could not init queue\n");
		//TODO: unreg
		return -EFAULT;
	}

	blk_queue_hardsect_size(queue, 512);
	gcn_sd_device.gd->queue = queue;	

	//add the disk
	add_disk(gcn_sd_device.gd);

	return 0;
}
		    
static void __exit cleanup_gcn_sd(void)
{
	printk(KERN_INFO PFX "unloading\n");

	int ret = unregister_blkdev(Major, DRIVER_MODULE_NAME);
	if (ret < 0)
		printk(KERN_ALERT "Error in unregister_blkdev: %d\n", ret);
}


MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

module_init(init_gcn_sd);
module_exit(cleanup_gcn_sd);
