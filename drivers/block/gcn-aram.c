/*
 * drivers/block/gcn-aram.c
 *
 * Nintendo GameCube Auxiliary RAM block driver
 * Copyright (C) 2004-2005 The GameCube Linux Team
 * Copyright (C) 2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005 Albert Herranz
 *
 * Based on previous work by Franz Lehner.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/major.h>
#include <linux/platform_device.h>
#include <linux/blkdev.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <asm/io.h>


#define DRV_MODULE_NAME "gcn-aram"
#define DRV_DESCRIPTION "Nintendo GameCube Auxiliary RAM block driver"
#define DRV_AUTHOR      "Todd Jeffreys <todd@voidpointer.org>, " \
			"Albert Herranz"

static char aram_driver_version[] = "2.0";

#define aram_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

#ifdef ARAM_DEBUG
#  define DBG(fmt, args...) \
          printk(KERN_ERR "%s: " fmt, __FUNCTION__ , ## args)
#else
#  define DBG(fmt, args...)
#endif

/*
 * Hardware.
 */
#define ARAM_IRQ		6

#define ARAM_DMA_ALIGN		0x1f	/* 32 bytes */

#define DSP_BASE		0xcc005000
#define DSP_SIZE		0x200

#define DSP_IO_BASE		((void __iomem *)DSP_BASE)

#define DSP_CSR			0x00a
#define  DSP_CSR_RES		(1<<0)
#define  DSP_CSR_PIINT		(1<<1)
#define  DSP_CSR_HALT		(1<<2)
#define  DSP_CSR_AIDINT		(1<<3)
#define  DSP_CSR_AIDINTMASK	(1<<4)
#define  DSP_CSR_ARINT		(1<<5)
#define  DSP_CSR_ARINTMASK	(1<<6)
#define  DSP_CSR_DSPINT		(1<<7)
#define  DSP_CSR_DSPINTMASK	(1<<8)
#define  DSP_CSR_DSPDMA		(1<<9)
#define  DSP_CSR_RESETXXX	(1<<11)

#define AR_SIZE			0x012

#define AR_MODE			0x016
#define   AR_MODE_ACCELERATOR	(1 << 0)

#define AR_REFRESH		0x01a

#define AR_DMA_MMADDR		0x020

#define AR_DMA_ARADDR		0x024

#define AR_DMA_CNT_H		0x028
#define   AR_READ		(1 << 31)
#define   AR_WRITE		0

#define AR_DMA_CNT_L		0x02a

#define AR_DMA_CNT		AR_DMA_CNT_H

/*
 * Driver settings
 */
#define ARAM_NAME		"aram"
#define ARAM_MAJOR		Z2RAM_MAJOR

#define ARAM_SECTOR_SIZE	PAGE_SIZE

#define ARAM_SOUNDMEMORYOFFSET	0
#define ARAM_BUFFERSIZE		(16*1024*1024 - ARAM_SOUNDMEMORYOFFSET)



struct aram_device {
	spinlock_t			lock;

	int				irq;
	void __iomem			*io_base;
	spinlock_t			io_lock;

	struct block_device_operations	fops;
	struct gendisk			*disk;
	struct request_queue		*queue;

	struct request			*req;
	dma_addr_t			dma_addr;
	size_t				dma_len;

	int				ref_count;

	struct platform_device		pdev;	/* must be last member */
};


/* get the aram device given the platform device of an aram device */
#define to_aram_device(n) container_of(n,struct aram_device,pdev)

/*
 * Converts a request direction into a DMA data direction.
 */
static inline enum dma_data_direction rq_dir_to_dma_dir(struct request *req)
{
	if (rq_data_dir(req) == READ) {
		return DMA_FROM_DEVICE;
	} else {
		return DMA_TO_DEVICE;
	}
}

/*
 * Converts a request direction into an ARAM data direction.
 */
static inline int rq_dir_to_aram_dir(struct request *req)
{
	if (rq_data_dir(req) == READ) {
		return AR_READ;
	} else {
		return AR_WRITE;
	}
}


/*
 * Starts an ARAM DMA transfer.
 */
static void aram_start_dma_transfer(struct aram_device *adev,
				    unsigned long aram_addr)
{
	void __iomem *io_base = adev->io_base;
	dma_addr_t dma_addr = adev->dma_addr;
	size_t dma_len = adev->dma_len;

	/* DMA transfers require proper alignment */
	BUG_ON((dma_addr & ARAM_DMA_ALIGN) != 0 ||
	       (dma_len & ARAM_DMA_ALIGN) != 0);

	writel(dma_addr, io_base + AR_DMA_MMADDR);
	writel(aram_addr, io_base + AR_DMA_ARADDR);

	/* writing the low-word kicks off the DMA */
	writel(rq_dir_to_aram_dir(adev->req) | dma_len, io_base + AR_DMA_CNT);
}

/*
 * Handles ARAM interrupts.
 */
static irqreturn_t aram_irq_handler(int irq, void *dev0, struct pt_regs *regs)
{
	struct aram_device *adev = dev0;
	struct request *req;
	u16 __iomem *csr_reg = adev->io_base + DSP_CSR;
	u16 csr;
	unsigned long flags;

	spin_lock_irqsave(&adev->io_lock, flags);

	csr = readw(csr_reg);

	/*
	 * Do nothing if the interrupt is not targetted for us.
	 * (We share this interrupt with the sound driver).
	 */
	if (!(csr & DSP_CSR_ARINT)) {
		spin_unlock_irqrestore(&adev->io_lock, flags);
		return IRQ_NONE;
	}

	/* strictly ack the ARAM interrupt, and nothing more */
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT);
	writew(csr, csr_reg);

	/* pick up current request being serviced */
	req = adev->req;
	adev->req = NULL;

	spin_unlock_irqrestore(&adev->io_lock, flags);
		
	if (req) {
		if (!end_that_request_first(req, 1, req->current_nr_sectors)) {
			add_disk_randomness(req->rq_disk);
			end_that_request_last(req, 1);
		}
		dma_unmap_single(&adev->pdev.dev, adev->dma_addr, adev->dma_len,
				 rq_dir_to_dma_dir(req));
		spin_lock(&adev->lock);
		blk_start_queue(adev->queue);
		spin_unlock(&adev->lock);
	} else {
		aram_printk(KERN_ERR, "ignoring interrupt, no request\n");
	}

	return IRQ_HANDLED;
}

/*
 * Performs block layer requests.
 */
static void aram_do_request(request_queue_t *q)
{
	struct aram_device *adev = q->queuedata;
	struct request *req;
	unsigned long aram_addr;
	size_t len;
	unsigned long flags;

	req = elv_next_request(q);
	while(req) {
		spin_lock_irqsave(&adev->io_lock, flags);

		/* we can schedule just a single request each time */
		if (adev->req) {
			spin_unlock_irqrestore(&adev->io_lock, flags);
			blk_stop_queue(q);
			break;
		}

		blkdev_dequeue_request(req);

		/* ignore requests that we can't handle */
		if (!blk_fs_request(req)) {
			spin_unlock_irqrestore(&adev->io_lock, flags);
			continue;
		}

		/* store the request being handled */
		adev->req = req;
		blk_stop_queue(q);

		spin_unlock_irqrestore(&adev->io_lock, flags);

		/* calculate the ARAM address and length */
		aram_addr = req->sector << 9;
		len = req->current_nr_sectors << 9;

		/* give up if the request goes out of bounds */
		if (aram_addr + len > ARAM_BUFFERSIZE) {
			aram_printk(KERN_ERR, "bad access: block=%lu,"
				    " size=%u\n", (unsigned long)req->sector,
				    len);
			/* XXX correct? the request is already dequeued */
			end_request(req, 0);
			continue;
		}

		BUG_ON(req->nr_phys_segments != 1);

		/* perform DMA mappings */
		adev->dma_len = len;
		adev->dma_addr = dma_map_single(&adev->pdev.dev, req->buffer,
						len, rq_dir_to_dma_dir(req));

		/* start the DMA transfer */
		aram_start_dma_transfer(adev,
					ARAM_SOUNDMEMORYOFFSET + aram_addr);
		break;
	}
}

/*
 * Opens the ARAM device.
 */
static int aram_open(struct inode *inode, struct file *filp)
{
	struct aram_device *adev = inode->i_bdev->bd_disk->private_data;
	unsigned long flags;
	int retval = 0;

	spin_lock_irqsave(&adev->lock, flags);

	/* only allow a minor of 0 to be opened */
	if (iminor(inode)) {
		retval =  -ENODEV;
		goto out;
	}

	/* honor exclusive open mode */
	if (adev->ref_count == -1 ||
	    (adev->ref_count && (filp->f_flags & O_EXCL))) {
		retval = -EBUSY;
		goto out;
	}

	if ((filp->f_flags & O_EXCL))
		adev->ref_count = -1;
	else
		adev->ref_count++;

out:
	spin_unlock_irqrestore(&adev->lock, flags);
	return retval;
}

/*
 * Closes the ARAM device.
 */
static int aram_release(struct inode *inode, struct file *filp)
{
	struct aram_device *adev = inode->i_bdev->bd_disk->private_data;
	unsigned long flags;

	spin_lock_irqsave(&adev->lock, flags);
	if (adev->ref_count > 0)
		adev->ref_count--;
	else
		adev->ref_count = 0;
	spin_unlock_irqrestore(&adev->lock, flags);
	
	return 0;
}

/*
 * Minimal ioctl for the ARAM device.
 */
static int aram_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg)
{
	struct hd_geometry geo;
	
	switch (cmd) {
	case BLKRAGET:
	case BLKFRAGET:
	case BLKROGET:
	case BLKBSZGET:
	case BLKSSZGET:
	case BLKSECTGET:
	case BLKGETSIZE:
	case BLKGETSIZE64:
	case BLKFLSBUF:
		return ioctl_by_bdev(inode->i_bdev,cmd,arg);
	case HDIO_GETGEO:
		/* fake the entries */
		geo.heads = 16;
		geo.sectors = 32;
		geo.start = 0;
		geo.cylinders = ARAM_BUFFERSIZE / (geo.heads * geo.sectors);
		if (copy_to_user((void __user*)arg,&geo,sizeof(geo)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}


static struct block_device_operations aram_fops = {
	.owner = THIS_MODULE,
	.open = aram_open,
	.release = aram_release,
	.ioctl = aram_ioctl,
};


/*
 *
 */
static int aram_init_blk_dev(struct aram_device *adev)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int retval;

	adev->ref_count = 0;

	retval = register_blkdev(ARAM_MAJOR, ARAM_NAME);
	if (retval)
		goto err_register_blkdev;
	
	retval = -ENOMEM;
	spin_lock_init(&adev->lock);
	spin_lock_init(&adev->io_lock);
	queue = blk_init_queue(aram_do_request, &adev->lock);
	if (!queue)
		goto err_blk_init_queue;

	blk_queue_hardsect_size(queue, ARAM_SECTOR_SIZE);
	blk_queue_dma_alignment(queue, ARAM_DMA_ALIGN);
	blk_queue_max_phys_segments(queue, 1);
	blk_queue_max_hw_segments(queue, 1);
	queue->queuedata = adev;
	adev->queue = queue;

	disk = alloc_disk(1);
	if (!disk)
		goto err_alloc_disk;

	disk->major = ARAM_MAJOR;
	disk->first_minor = 0;
	disk->fops = &aram_fops;
	strcpy(disk->disk_name, ARAM_NAME);
	strcpy(disk->devfs_name, disk->disk_name);
	disk->queue = adev->queue;
	set_capacity(disk, ARAM_BUFFERSIZE >> 9);
	disk->private_data = adev;
	adev->disk = disk;

	add_disk(adev->disk);

	retval = 0;
	goto out;

err_alloc_disk:
	blk_cleanup_queue(adev->queue);
err_blk_init_queue:
	unregister_blkdev(ARAM_MAJOR, ARAM_NAME);
err_register_blkdev:
out:
	return retval;
}

/*
 *
 */
static void aram_exit_blk_dev(struct aram_device *adev)
{
	if (adev->disk) {
		del_gendisk(adev->disk);
		put_disk(adev->disk);
	}
	if (adev->queue)
		blk_cleanup_queue(adev->queue);
	unregister_blkdev(ARAM_MAJOR, ARAM_NAME);
}

/*
 *
 */
static void aram_quiesce(struct aram_device *adev)
{
	u16 __iomem *csr_reg = adev->io_base + DSP_CSR;
	u16 csr;
	unsigned long flags;

	/*
	 * Disable ARAM interrupts, but do not accidentally ack non-ARAM ones.
	 */
	spin_lock_irqsave(&adev->io_lock, flags);
	csr = readw(csr_reg);
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT | DSP_CSR_ARINTMASK);
	writew(csr, csr_reg);
	spin_unlock_irqrestore(&adev->io_lock, flags);

	/* wait until pending transfers are finished */
	while(readw(csr_reg) & DSP_CSR_DSPDMA)
		cpu_relax();
}

/*
 *
 */
static int aram_init_irq(struct aram_device *adev)
{
	u16 __iomem *csr_reg = adev->io_base + DSP_CSR;
	u16 csr;
	unsigned long flags;
	int retval;

	/* request interrupt */
	retval = request_irq(adev->irq, aram_irq_handler,
			     SA_INTERRUPT | SA_SHIRQ,
			     DRV_MODULE_NAME, adev);
	if (retval) {
		aram_printk(KERN_ERR, "request of irq%d failed\n", adev->irq);
		goto out;
	}

	/*
	 * Enable ARAM interrupts, and route them to the processor.
	 * As in the other cases, preserve the AI and DSP interrupts.
	 */
	spin_lock_irqsave(&adev->io_lock, flags);
	csr = readw(csr_reg);
	csr |= (DSP_CSR_ARINT | DSP_CSR_ARINTMASK | DSP_CSR_PIINT);
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT);
	writew(csr, csr_reg);
	spin_unlock_irqrestore(&adev->io_lock, flags);

out:
	return retval;
}

/*
 *
 */
static void aram_exit_irq(struct aram_device *adev)
{
	aram_quiesce(adev);

	free_irq(adev->irq, adev);
}

/*
 *
 */
static int aram_init(struct aram_device *adev,
		     struct resource *mem, int irq)
{
	int retval;

	memset(adev, 0, sizeof(*adev) - sizeof(adev->pdev));

	adev->io_base = (void __iomem *)mem->start;
	adev->irq = irq;

	retval = aram_init_blk_dev(adev);
	if (!retval) {
		retval = aram_init_irq(adev);
		if (retval) {
			aram_exit_blk_dev(adev);
		}
	}
	return retval;
}

/*
 *
 */
static void aram_exit(struct aram_device *adev)
{
	aram_exit_blk_dev(adev);
	aram_exit_irq(adev);
}

/*
 * Needed for platform devices.
 */
static void aram_dev_release(struct device *dev)
{
}


static struct resource aram_resources[] = {
	[0] = {
		.start = DSP_BASE,
		.end = DSP_BASE + DSP_SIZE -1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = ARAM_IRQ,
		.end = ARAM_IRQ,
		.flags = IORESOURCE_IRQ,
	},
};

static struct aram_device aram_device = {
	.pdev = {
	       	.name = ARAM_NAME,
	        .id = 0,
	        .num_resources = ARRAY_SIZE(aram_resources),
	        .resource = aram_resources,
		.dev = {
			.release = aram_dev_release,
		},
	},
};


/*
 *
 */
static int aram_probe(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct aram_device *adev = to_aram_device(pdev);
	struct resource *mem;
	int irq;
	int retval;

	retval = -ENODEV;
	irq = platform_get_irq(pdev, 0);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (mem) {
		retval = aram_init(adev, mem, irq);
	}
	return retval;
}

/*
 *
 */
static int aram_remove(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct aram_device *adev = to_aram_device(pdev);

	aram_exit(adev);

	return 0;
}

/*
 *
 */
static void aram_shutdown(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct aram_device *adev = to_aram_device(pdev);

	aram_quiesce(adev);
}


static struct device_driver aram_driver = {
       	.name = ARAM_NAME,
	.bus = &platform_bus_type,
	.probe = aram_probe,
	.remove = aram_remove,
	.shutdown = aram_shutdown,
};


/*
 *
 */
static int __init aram_init_module(void)
{
	int retval = 0;

	aram_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		    aram_driver_version);

	retval = driver_register(&aram_driver);
	if (!retval) {
		retval = platform_device_register(&aram_device.pdev);
	}

	return retval;
}

/*
 *
 */
static void __exit aram_exit_module(void)
{
	platform_device_unregister(&aram_device.pdev);
	driver_unregister(&aram_driver);
}

module_init(aram_init_module);
module_exit(aram_exit_module);


MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

