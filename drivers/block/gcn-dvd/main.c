/*
 * drivers/block/gcn-dvd/main.c
 *
 * Nintendo GameCube DVD driver 
 * Copyright (C) 2005 The GameCube Linux Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/major.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/cdrom.h>
#include <linux/wait.h>
#include <asm/setup.h>
#include <asm/bitops.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/cache.h>
#include <linux/interrupt.h>

#include <linux/fcntl.h>        /* O_ACCMODE */
#include <linux/hdreg.h>  /* HDIO_GETGEO */

#include <asm/io.h>

#include "request.h"

#define DEVICE_NAME "DVD"
#define DVD_MAJOR 60

#define LINUX_SECTOR_SIZE  512
#define LINUX_SECTOR_SHIFT 9
#define DVD_SECTOR_SIZE    2048
#define DVD_SECTOR_SHIFT   11
#define DVD_MAX_SECTORS    712880

#define DMA_ALIGNMENT_MASK 0x1F

#define DVD_REGISTER_INIT    ((void* __iomem)0xCC003024)

#define DVD_REGISTER_BLOCK_BASE   0xCC006000
#define DVD_REGISTER_BLOCK_LENGTH 0x40

#define DVD_GAMECODE_U32     0x80000000 /* Gamecode */
#define DVD_COMPANY_U16      0x80000004 /* Game company id */
#define DVD_DISK_ID_U8       0x80000006 /* Disk id */
#define DVD_DISK_VERSION_U8  0x80000007 /* Disk version */

#define DVD_IRQ			2

/* DI Status Register */
#define DI_DISR              ((void * __iomem)0xCC006000)
#define  DI_DISR_BRKINT      (1<<6)
#define  DI_DISR_BRKINTMASK  (1<<5)
#define  DI_DISR_TCINT       (1<<4)
#define  DI_DISR_TCINTMASK   (1<<3)
#define  DI_DISR_DEINT       (1<<2)
#define  DI_DISR_DEINTMASK   (1<<1)
#define  DI_DISR_BRK         (1<<0)

/* DI Cover Register */
#define DI_DICVR             ((void * __iomem)0xCC006004)
#define  DI_DICVR_CVRINT     (1<<2)
#define  DI_DICVR_CVRINTMASK (1<<1)
#define  DI_DICVR_CVR        (1<<0)

/* DI Command Buffer 0 */
#define DI_DICMDBUF0         ((void * __iomem)0xCC006008)
#define DI_DICMDBUF0_CMD     24
#define DI_DICMDBUF0_SUBCMD1 16
#define DI_DICMDBUF0_SUBCMD2 0

/* DI Command Buffer 1 */
#define DI_DICMDBUF1         ((void * __iomem)0xCC00600C)

/* DI Command Buffer 2 */ 
#define DI_DICMDBUF2         ((void * __iomem)0xCC006010)

/* DMA Memory Address Register */
#define DI_DIMAR             ((void * __iomem)0xCC006014)

/* DI DMA Transfer Length Register */
#define DI_DILENGTH          ((void * __iomem)0xCC006018)

/* DI Control Register */
#define DI_DICR              ((void * __iomem)0xCC00601C)
#define  DI_DICR_RW          (1<<2)
#define  DI_DICR_DMA         (1<<1)
#define  DI_DICR_TSTART      (1<<0)

/* DI Immediate Data Buffer */
#define DI_DIIMMBUF          ((void * __iomem)0xCC006020)

#define DI_CMD_REZERO        0x01
#define DI_CMD_INQUIRY       0x12
#define DI_CMD_READ          0xA8
#define DI_CMD_SEEK          0xAB
#define DI_CMD_READTOC       0x43
#define DI_CMD_STOP          0xE3
/* This is a fake command, don't send to the hardware */
#define DI_CMD_INITIALIZE    0xFF

#define IS_CMD_TYPE(cmd,type) (((cmd) >> DI_DICMDBUF0_CMD) == (type))

static struct gendisk *dvd_gendisk;
static struct request_queue *dvd_queue;
static spinlock_t dvd_queue_lock = SPIN_LOCK_UNLOCKED; 
static struct _taginterrupt_queue
{
	spinlock_t lock;
	int drive_initialized;
	struct list_head queue;
} interrupt_queue;

static struct _tagdvdinfo {
	spinlock_t lock;
	unsigned long refCount;
	unsigned int media_changed;
	unsigned long numDVDSectors;
	unsigned long numLinuxSectors;

	/* disc info */
	struct _tagdvdid
	{ 
		u32 gamecode;
		u16 company;
		u8 id;
		u8 version;
	} disc;
} dvd_info;

#define LOCK(flags)   spin_lock_irqsave(&dvd_info.lock,flags)
#define UNLOCK(flags) spin_unlock_irqrestore(&dvd_info.lock,flags)

/* Must be 0x20 in size = 32 bytes */
#pragma pack(1)

struct gc_dvd_drive_info
{
	u32 head;
	u32 middle;
	u32 last;
	u8 padding[20];
};

struct gc_dvd_disc_info
{
	u16 revision;
	u16 device_code;
	u32 release_date;
	u8  padding[24];
};

#pragma pack()

struct dma_buffer
{
	size_t size;
	void *ptr;
	void *alignedPtr;
	dma_addr_t handle;
};

#ifdef DEBUG
#define DPRINTK(fmt,args...) printk(KERN_INFO "%s (%u): " fmt,__FUNCTION__,__LINE__,## args)
#else
#define DPRINTK(fmt,args...)
#endif

static int alloc_dma_buffer(size_t size,struct dma_buffer *pRet)
{
	/* allocate space for the aligned memory */
	pRet->size = DMA_ALIGNMENT_MASK + size;
	if (!(pRet->ptr = kmalloc(pRet->size,GFP_KERNEL | GFP_DMA))) {
		DPRINTK("Cannot allocate DMA memory of size %i\n",pRet->size);
		return -ENOMEM;
	}
  
	/* align the pointer */
	if ((unsigned long)pRet->ptr & DMA_ALIGNMENT_MASK) {
		pRet->alignedPtr = (void*)(((unsigned long)pRet->ptr + DMA_ALIGNMENT_MASK) & ~DMA_ALIGNMENT_MASK);
		/* adjust the size if not aligned */
		pRet->size -= (pRet->alignedPtr - pRet->ptr);
	}
	else {
		pRet->alignedPtr = pRet->ptr;
	}
	/* get the dma mapping */
	pRet->handle = virt_to_phys(pRet->alignedPtr);
	return 0;
}

inline static void sync_dma_buffer(struct dma_buffer *pRet)
{
	invalidate_dcache_range((unsigned long)pRet->alignedPtr,
				(unsigned long)pRet->alignedPtr + pRet->size);
}

inline static void free_dma_buffer(struct dma_buffer *pRet)
{
	kfree(pRet->ptr);
}

/* hardware talk */
static void gc_dvd_enable_interrupts(int enable)
{
	unsigned long outval;
	/* enable main interrupts */
	if (enable) {
		outval = DI_DISR_BRKINT | DI_DISR_TCINT | DI_DISR_DEINT |
			DI_DISR_BRKINTMASK | DI_DISR_TCINTMASK | DI_DISR_DEINTMASK;
	}
	else {
		outval = DI_DISR_BRKINT | DI_DISR_TCINT | DI_DISR_DEINT;
	}
	
	writel(outval,DI_DISR);
	
	/* This enables cover interrupts */
	if (enable) {
		outval = DI_DICVR_CVRINT | DI_DICVR_CVRINTMASK;
	}
	else {
		outval = DI_DICVR_CVRINT;
	}
	
	writel(outval,DI_DICVR);
}

static void gc_dvd_execute_queue_command(struct gc_dvd_command *cmd)
{
	u32 val;
	/* if they're doing a read and the drive is not initialized */
	if (!interrupt_queue.drive_initialized && 
	    IS_CMD_TYPE(cmd->r_DI_DICMDBUF0,DI_CMD_READ)) {
		/* insert an initialize queue item BEFORE this one */
		static struct gc_dvd_command init_cmd = {
			.r_DI_DICMDBUF0 = DI_CMD_INITIALIZE << DI_DICMDBUF0_CMD,
			.completion_routine = NULL,
			.param = NULL,
		};
		
		/* insert before this item */
		list_add_tail(&init_cmd.list,&cmd->list);
		/* now execute the initialize routine */
		val = (readl(DVD_REGISTER_INIT) & ~4) | 1;
		writel(val,DVD_REGISTER_INIT);
		udelay(100);
		val |= (4 | 1);
		writel(val,DVD_REGISTER_INIT);
		udelay(100);
		/* it will return an interrupt when we're done, our 
		   queue item will pick it up */
	}
	else if (cmd) {
		writel(cmd->r_DI_DICMDBUF0,DI_DICMDBUF0);
		writel(cmd->r_DI_DICMDBUF1,DI_DICMDBUF1);
		writel(cmd->r_DI_DICMDBUF2,DI_DICMDBUF2);
		writel((unsigned long)cmd->r_DI_DIMAR,DI_DIMAR);
		writel(cmd->r_DI_DILENGTH,DI_DILENGTH);
		writel(cmd->r_DI_DICR,DI_DICR);
	}
}

static int gc_dvd_queue_command(struct gc_dvd_command *cmd)
{
	unsigned long flags;
	int execute_immediately;
  
	spin_lock_irqsave(&interrupt_queue.lock,flags);
  
	cmd->int_status = is_still_running;
	/* add to the tail of the list */
	execute_immediately = list_empty(&interrupt_queue.queue);

	list_add_tail(&cmd->list,&interrupt_queue.queue);
  
	if (execute_immediately) {
		gc_dvd_execute_queue_command(cmd);
	}
	/* release lock so interrupt handler can get it */
	spin_unlock_irqrestore(&interrupt_queue.lock,flags);
	return 0;
}

/* This function is called from an IRQ context, we just wake up the queue */
static void gc_dvd_queue_completion_wake_up(struct gc_dvd_command *cmd)
{
	wake_up_interruptible(((wait_queue_head_t*)cmd->param));
}

static int gc_dvd_execute_blocking_command(unsigned int di_cmd,unsigned int sz,struct dma_buffer *buf)
{
	struct gc_dvd_command cmd;
	wait_queue_head_t wait_queue;
    
	if ((sz > 0) && alloc_dma_buffer(sz,buf)) {
		return -ENOMEM;
	}
	
	init_waitqueue_head(&wait_queue);

	cmd.flags = 0;
	cmd.r_DI_DICMDBUF0 = di_cmd;
	cmd.r_DI_DICMDBUF1 = 0;
	cmd.r_DI_DICMDBUF2 = sz;
	cmd.r_DI_DIMAR     = (sz > 0) ? (void*)buf->handle : NULL;
	cmd.r_DI_DILENGTH  = sz;
	cmd.r_DI_DICR      = DI_DICR_TSTART | ((sz > 0) ? DI_DICR_DMA : 0);
	cmd.completion_routine = gc_dvd_queue_completion_wake_up;
	cmd.param = &wait_queue;

	gc_dvd_queue_command(&cmd);
	/* wait for it to finish */
	while (wait_event_interruptible(wait_queue,cmd.int_status != 
					is_still_running)) ;
	
	return cmd.int_status;
}

static inline void gc_dvd_stop_motor(void)
{
	gc_dvd_execute_blocking_command(DI_CMD_STOP << DI_DICMDBUF0_CMD,0,NULL);
}

static int gc_dvd_inquiry(void)
{
  
	/* get status on disk */
	struct dma_buffer buf;
	struct gc_dvd_drive_info *pdi;
	int is;
  
	is = gc_dvd_execute_blocking_command(DI_CMD_INQUIRY << DI_DICMDBUF0_CMD,
					     sizeof(struct gc_dvd_drive_info),
					     &buf);
	if (is == -ENOMEM) {
		return is;
	}
	else if (is != is_transfer_complete) {
		printk(KERN_ERR "Gamecube DVD: error in inquiry cmd\n");
		is = -ENODEV;
	}
	else {
		sync_dma_buffer(&buf);
		
		pdi = (struct gc_dvd_drive_info*)buf.alignedPtr;
		printk(KERN_INFO "Gamecube DVD: 0x%x, 0x%x,0x%x\n",pdi->head,pdi->middle,pdi->last);
		
		is = 0;
	}
	
	free_dma_buffer(&buf);
	return is;
}

static int gc_dvd_read_toc(void)
{
	struct dma_buffer buf;
	struct gc_dvd_disc_info *pdi;
	int i;

	i = gc_dvd_execute_blocking_command(DI_CMD_READ << DI_DICMDBUF0_CMD | 0x40,
					    sizeof(struct gc_dvd_disc_info),
					    &buf);
	if (i != is_transfer_complete) {
		dvd_info.numDVDSectors = 0;

		if (i != -ENOMEM) {
			free_dma_buffer(&buf);
		}
		else {
			i = -ENOMEDIUM;
		}
		
		printk(KERN_ERR "Gamecube DVD: error reading TOC - missing medium?\n");
	}
	else {
		sync_dma_buffer(&buf);
		
		pdi = (struct gc_dvd_disc_info*)buf.alignedPtr;
		printk(KERN_INFO "Gamecube DVD: revision: %u, device_code %u, release_date: %u\n",pdi->revision,pdi->device_code,pdi->release_date);
		
		dvd_info.numDVDSectors = DVD_MAX_SECTORS;
		/* reset media_changed flag */
		dvd_info.media_changed = 0;

		free_dma_buffer(&buf);

		i = 0;
	}
	/* inform the kernel of the size */
	dvd_info.numLinuxSectors = dvd_info.numDVDSectors << (DVD_SECTOR_SHIFT - LINUX_SECTOR_SHIFT);
	set_capacity(dvd_gendisk,dvd_info.numLinuxSectors);  
	return i;
}

/* Handlers */
static int gc_dvd_revalidate(struct gendisk *disk)
{
	gc_dvd_read_toc();
	return 0;
}

static int gc_dvd_open(struct inode *inode,struct file *filp)
{
	unsigned long flags;
	/* we are read only */
	if (filp->f_mode & FMODE_WRITE) {
		return -EROFS;
	}
	
	/* check the disc, only allow minor of 0 to be opened */
	if (iminor(inode)) {
		return -ENODEV;
	}
  
	/* update information about the disc */
	LOCK(flags);
	if (dvd_info.refCount > 0) {
		/* we only let one at a time */
		UNLOCK(flags);
		return -EBUSY;
	}
	else {
		check_disk_change(inode->i_bdev);
		/* revalidate should be called if necessary, check results here */
		if (dvd_info.numDVDSectors == 0) {
			UNLOCK(flags);
			return -ENOMEDIUM;
		}
	}
	dvd_info.refCount++;
	UNLOCK(flags);
	return 0;
}
static int gc_dvd_release(struct inode *inode,struct file *filp)
{
	unsigned long flags;

	gc_dvd_stop_motor();

	LOCK(flags);
	dvd_info.refCount--;
	/* force a media change so we re-read the toc and initialize the disc */
	dvd_info.media_changed = 1;
	UNLOCK(flags);
	return 0;
}

static int gc_dvd_ioctl(struct inode *inode,struct file *filp,
			unsigned int cmd,unsigned long arg)
{
	switch (cmd)
	{
	case CDROMMULTISESSION:
		/* struct cdrom_multisession */
		break;

	case CDROMSTART:
		break;

	case CDROMSTOP:
		break;

	case CDROMREADTOCHDR:
		/* struct cdrom_tochdr */
		break;

	case CDROMREADTOCENTRY:
		/* struct cdrom_tocentry */
		break;

	case CDROMREADMODE2:
	case CDROMREADMODE1:
	case CDROMREADRAW:
		/* struct cdrom_read (1-2048, 2-2336,RAW-2352) */
		break;

	case CDROM_GET_MCN:
		/* retrieve the universal product code */
		/* struct cdrom_mcn */
		break;

	case CDROMRESET:
		/* reset the drive */
		break;

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
	default:
		return -ENOTTY;
	}
	return -ENOTTY;
}

static int gc_dvd_media_changed(struct gendisk *disk)
{
	/* return 1 if the disc has changed */
	return (dvd_info.media_changed ? 1 : 0);
}

static void gc_dvd_read_request_callback(struct gc_dvd_command *cmd)
{
	unsigned long flags;
	struct request *req = (struct request*)cmd->param;
	struct request_queue *rqueue = req->q;
	enum gc_dvd_interrupt_status int_status = cmd->int_status;
  
	/* since this was performed via DMA, invalidate the cache */
	if (cmd->int_status == is_transfer_complete) {
		invalidate_dcache_range((unsigned int)req->buffer,(unsigned int)req->buffer + cmd->r_DI_DILENGTH);
	}
	/* free this item so another request can get it */
	gc_dvd_request_release_data(cmd);
	/* now end the request and send back to block layer */
	spin_lock_irqsave(rqueue->queue_lock,flags);
	if (!end_that_request_first(req,
				    (int_status == is_transfer_complete),
				    req->current_nr_sectors)) {
		add_disk_randomness(req->rq_disk);
		end_that_request_last(req);
	}
	/* start queue back up */
	blk_start_queue(rqueue);
	spin_unlock_irqrestore(rqueue->queue_lock,flags);
}

static void gc_dvd_do_request(request_queue_t *q)
{
	struct request *req;
	unsigned long start;
	unsigned long len;
	struct gc_dvd_command *cmd;
  
	while ((req = elv_next_request(q))) {
		/* check if they are reading beyond the limits */
		if ((req->sector + req->current_nr_sectors) > dvd_info.numLinuxSectors) {
			printk(KERN_ERR "Gamecube DVD: reading past end\n");
			end_request(req,0);
		}
		else if (rq_data_dir(req) == WRITE) {
			printk(KERN_ERR "Gamecube DVD: write attempted\n");
			end_request(req,0);
		}
		else if (dvd_info.media_changed) {
			DPRINTK("media changed in read routine, aborting\n");
			end_request(req,0);
		}
		else if (req->current_nr_sectors >= (1 << (DVD_SECTOR_SHIFT - LINUX_SECTOR_SHIFT))) {
			/* now schedule the read */
			if (gc_dvd_request_get_data(&cmd) || !cmd) {
				/* we're full, stop the queue */
				blk_stop_queue(q);
				return;
			}
			else {
				/* remove item from the queue */
				blkdev_dequeue_request(req);

				/* setup my structure */
				start = req->sector << LINUX_SECTOR_SHIFT;
				len   = req->current_nr_sectors << LINUX_SECTOR_SHIFT;
				
				cmd->flags = 0;
				cmd->r_DI_DICMDBUF0 = DI_CMD_READ << DI_DICMDBUF0_CMD;
				cmd->r_DI_DICMDBUF1 = start >> (DVD_SECTOR_SHIFT - LINUX_SECTOR_SHIFT);
				cmd->r_DI_DICMDBUF2 = len;
				cmd->r_DI_DIMAR     = (void*)virt_to_phys(req->buffer);
				cmd->r_DI_DILENGTH  = len;
				cmd->r_DI_DICR      = DI_DICR_TSTART | DI_DICR_DMA;
				cmd->completion_routine = gc_dvd_read_request_callback;
				cmd->param = req;
				gc_dvd_queue_command(cmd);
			}
		}
	}
}

static struct block_device_operations dvd_fops =
{
	.owner  = THIS_MODULE,
	.open   = gc_dvd_open,
	.release = gc_dvd_release,
	.revalidate_disk = gc_dvd_revalidate,
	.media_changed = gc_dvd_media_changed,
	.ioctl = gc_dvd_ioctl,
};

static irqreturn_t gc_dvd_irq_handler(int irq,void *dev_id,struct pt_regs *regs)
{
	unsigned int reason;
	unsigned long flags;
	struct gc_dvd_command *cur_item;
#define REASON_FLAG_COVER 0x80000000
	/* try the main status */
	if ((reason=readl(DI_DISR)) & (DI_DISR_BRKINT | DI_DISR_TCINT | DI_DISR_DEINT)) {
		/* acknowledge the interrupt */
		writel(reason | DI_DISR_BRKINT | DI_DISR_TCINT | DI_DISR_DEINT,DI_DISR);
	}
	else if ((reason=readl(DI_DICVR)) & (DI_DICVR_CVRINT))	{
		/* acknowlegde the interrupt */
		writel(reason | DI_DICVR_CVRINT,DI_DICVR);
		/* set media changed flag */
		if (!(reason & DI_DICVR_CVR)) {
			dvd_info.media_changed = 1;
		}
		/* set flag to be used later on */
		reason |= REASON_FLAG_COVER;
	}
	else {
		/* not for us, get out of here */
		return IRQ_NONE;
	}
	/* ok we have an interrupt, now process our queue */
	/* lock our structure */
	spin_lock_irqsave(&interrupt_queue.lock,flags);
	/* now look at our structure and call appropriate callback if necessary */
	if (!list_empty(&interrupt_queue.queue)) {
		/* first unlink the queue item */
		cur_item = (struct gc_dvd_command*)interrupt_queue.queue.next;
		list_del(&cur_item->list);
		/* do special checks on the command type to keep track of drive state */
		if (IS_CMD_TYPE(cur_item->r_DI_DICMDBUF0,DI_CMD_STOP)) {
			interrupt_queue.drive_initialized = 0;
		}
		else if (IS_CMD_TYPE(cur_item->r_DI_DICMDBUF0,DI_CMD_INITIALIZE)) {
			interrupt_queue.drive_initialized = 1;
		}
		/* now execute the next request if we have one */
		if (!list_empty(&interrupt_queue.queue)) {
			gc_dvd_execute_queue_command((struct gc_dvd_command*)
						     interrupt_queue.queue.next);
		}
		/* unlock the lock */
		spin_unlock_irqrestore(&interrupt_queue.lock,flags);
		/* determine the correct interrupt status */
		if (reason & REASON_FLAG_COVER) {
			if (reason & DI_DICVR_CVR) {
				cur_item->int_status = is_cover_opened;
			}
			else {
				cur_item->int_status = is_cover_closed;
			}
		}
		else if (reason & DI_DISR_TCINT) {
			cur_item->int_status = is_transfer_complete;
		}
		else if (reason & DI_DISR_DEINT) {
			cur_item->int_status = is_error;
		}
		else if (reason & DI_DISR_BRKINT) {
			cur_item->int_status = is_break;
		}
		/* call the callback */
		if (cur_item->completion_routine) {
			cur_item->completion_routine(cur_item);
		}
	}
	else {
		spin_unlock_irqrestore(&interrupt_queue.lock,flags);
		DPRINTK("Received interrupt but nothing was waiting for it\n");
	}
  
	return IRQ_HANDLED;
}

static int __init gc_dvd_init(void)
{
	int ret;

	printk(KERN_INFO "Gamecube DVD driver: init\n");

	/* initialize the refcount */
	memset(&dvd_info,0,sizeof(dvd_info));
	dvd_info.media_changed = 1;

	/* initialize the interrupt_queue */
	spin_lock_init(&interrupt_queue.lock);
	interrupt_queue.drive_initialized = 0;
	INIT_LIST_HEAD(&interrupt_queue.queue);

	spin_lock_init(&dvd_info.lock);
  
	/* initial the request queue */
	gc_dvd_request_init();
	/* first reserve our memory region to we can query hardware */
	if (check_mem_region(DVD_REGISTER_BLOCK_BASE,DVD_REGISTER_BLOCK_LENGTH) ||
	    !request_mem_region(DVD_REGISTER_BLOCK_BASE,DVD_REGISTER_BLOCK_LENGTH,"Gamecube DVD")) {
		printk(KERN_ERR "Couldn't reserve memory area for DVD\n");
		return -ENOMEM;
	}    
	
	if ((ret=request_irq(DVD_IRQ,gc_dvd_irq_handler,SA_INTERRUPT,"Gamecube DVD",0))) {
		printk(KERN_ERR "Unable to reserve DVD IRQ\n");
		goto delete_mem_region;
	}
	
	/* enable interrupts */
	gc_dvd_enable_interrupts(1);
	/* query the drive first */
	if ((ret=gc_dvd_inquiry())) {
		goto delete_irq;
	}
	/* now stop the dvd motor */
	gc_dvd_stop_motor();
    
	if ((ret=register_blkdev(DVD_MAJOR,DEVICE_NAME))) {
		goto delete_irq;
	}
      
	if (!(dvd_gendisk = alloc_disk(1))) {
		ret = -ENOMEM;
		goto unreg_blkdev;
	}

	if (!(dvd_queue = blk_init_queue(gc_dvd_do_request,&dvd_queue_lock))) {
		ret = -ENOMEM;
		goto delete_gendisk;
	}
	
	dvd_gendisk->major = DVD_MAJOR;
	dvd_gendisk->first_minor = 0;
	dvd_gendisk->fops = &dvd_fops;
	strcpy(dvd_gendisk->disk_name,"dvd");
	strcpy(dvd_gendisk->devfs_name,dvd_gendisk->disk_name);

	dvd_gendisk->queue = dvd_queue;

	/* ok now setup the desired parameters, like block size, hardsect size,
	   max hardware sectors to read at once, read ahead, etc */
	/* Hardware sector size */
	blk_queue_hardsect_size(dvd_queue,DVD_SECTOR_SIZE);
	/* Maximum sectors that can be read per request, hardware limit */
	blk_queue_max_phys_segments(dvd_queue,1);
	blk_queue_max_hw_segments(dvd_queue,1);
	/* Max size of coalesced segment */
	/* blk_queue_max_segment_size(dvd_queue,size); */
	/* Set the dma alignment */
	blk_queue_dma_alignment(dvd_queue,DMA_ALIGNMENT_MASK);

	set_disk_ro(dvd_gendisk,1);  
	add_disk(dvd_gendisk);

	return 0;

 delete_gendisk:
	del_gendisk(dvd_gendisk);
	put_disk(dvd_gendisk);
	dvd_gendisk = NULL;
 unreg_blkdev:
	unregister_blkdev(DVD_MAJOR,DEVICE_NAME);
 delete_irq:
	free_irq(DVD_IRQ,0);
 delete_mem_region:
	release_mem_region(DVD_REGISTER_BLOCK_BASE,DVD_REGISTER_BLOCK_LENGTH);
	return ret;
}

static void __exit gc_dvd_exit(void)
{
	printk(KERN_INFO "Gamecube DVD driver: exit\n");

	/* TODO send a break/interrupt to the device */
	gc_dvd_stop_motor();
	gc_dvd_enable_interrupts(0);

	free_irq(DVD_IRQ, 0);

	release_mem_region(DVD_REGISTER_BLOCK_BASE,DVD_REGISTER_BLOCK_LENGTH);
  
	blk_unregister_region(MKDEV(DVD_MAJOR,0),256);
	unregister_blkdev(DVD_MAJOR,DEVICE_NAME);

	if (dvd_gendisk) {
		del_gendisk(dvd_gendisk);
		put_disk(dvd_gendisk);
    
		dvd_gendisk = NULL;
	}

	if (dvd_queue) {
		blk_cleanup_queue(dvd_queue);
  
		dvd_queue = NULL;
	}
}

MODULE_AUTHOR("Scream|CT");
MODULE_DESCRIPTION("Gamecube DVD driver");
MODULE_LICENSE("GPL");

module_init(gc_dvd_init);
module_exit(gc_dvd_exit);
