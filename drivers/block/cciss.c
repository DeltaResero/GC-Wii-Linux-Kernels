/*
 *    Disk Array driver for HP Smart Array controllers.
 *    (C) Copyright 2000, 2007 Hewlett-Packard Development Company, L.P.
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; version 2 of the License.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *    General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *    02111-1307, USA.
 *
 *    Questions/Comments/Bugfixes to iss_storagedev@hp.com
 *
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/blkpg.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/hdreg.h>
#include <linux/spinlock.h>
#include <linux/compat.h>
#include <linux/blktrace_api.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/dma-mapping.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/completion.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <scsi/scsi_ioctl.h>
#include <linux/cdrom.h>
#include <linux/scatterlist.h>

#define CCISS_DRIVER_VERSION(maj,min,submin) ((maj<<16)|(min<<8)|(submin))
#define DRIVER_NAME "HP CISS Driver (v 3.6.20)"
#define DRIVER_VERSION CCISS_DRIVER_VERSION(3, 6, 20)

/* Embedded module documentation macros - see modules.h */
MODULE_AUTHOR("Hewlett-Packard Company");
MODULE_DESCRIPTION("Driver for HP Smart Array Controllers");
MODULE_SUPPORTED_DEVICE("HP SA5i SA5i+ SA532 SA5300 SA5312 SA641 SA642 SA6400"
			" SA6i P600 P800 P400 P400i E200 E200i E500 P700m"
			" Smart Array G2 Series SAS/SATA Controllers");
MODULE_VERSION("3.6.20");
MODULE_LICENSE("GPL");

#include "cciss_cmd.h"
#include "cciss.h"
#include <linux/cciss_ioctl.h>

/* define the PCI info for the cards we can control */
static const struct pci_device_id cciss_pci_device_id[] = {
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISS,  0x0E11, 0x4070},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSB, 0x0E11, 0x4080},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSB, 0x0E11, 0x4082},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSB, 0x0E11, 0x4083},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSC, 0x0E11, 0x4091},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSC, 0x0E11, 0x409A},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSC, 0x0E11, 0x409B},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSC, 0x0E11, 0x409C},
	{PCI_VENDOR_ID_COMPAQ, PCI_DEVICE_ID_COMPAQ_CISSC, 0x0E11, 0x409D},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSA,     0x103C, 0x3225},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSC,     0x103C, 0x3223},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSC,     0x103C, 0x3234},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSC,     0x103C, 0x3235},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSD,     0x103C, 0x3211},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSD,     0x103C, 0x3212},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSD,     0x103C, 0x3213},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSD,     0x103C, 0x3214},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSD,     0x103C, 0x3215},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSC,     0x103C, 0x3237},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSC,     0x103C, 0x323D},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x3241},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x3243},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x3245},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x3247},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x3249},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x324A},
	{PCI_VENDOR_ID_HP,     PCI_DEVICE_ID_HP_CISSE,     0x103C, 0x324B},
	{PCI_VENDOR_ID_HP,     PCI_ANY_ID,	PCI_ANY_ID, PCI_ANY_ID,
		PCI_CLASS_STORAGE_RAID << 8, 0xffff << 8, 0},
	{0,}
};

MODULE_DEVICE_TABLE(pci, cciss_pci_device_id);

/*  board_id = Subsystem Device ID & Vendor ID
 *  product = Marketing Name for the board
 *  access = Address of the struct of function pointers
 */
static struct board_type products[] = {
	{0x40700E11, "Smart Array 5300", &SA5_access},
	{0x40800E11, "Smart Array 5i", &SA5B_access},
	{0x40820E11, "Smart Array 532", &SA5B_access},
	{0x40830E11, "Smart Array 5312", &SA5B_access},
	{0x409A0E11, "Smart Array 641", &SA5_access},
	{0x409B0E11, "Smart Array 642", &SA5_access},
	{0x409C0E11, "Smart Array 6400", &SA5_access},
	{0x409D0E11, "Smart Array 6400 EM", &SA5_access},
	{0x40910E11, "Smart Array 6i", &SA5_access},
	{0x3225103C, "Smart Array P600", &SA5_access},
	{0x3223103C, "Smart Array P800", &SA5_access},
	{0x3234103C, "Smart Array P400", &SA5_access},
	{0x3235103C, "Smart Array P400i", &SA5_access},
	{0x3211103C, "Smart Array E200i", &SA5_access},
	{0x3212103C, "Smart Array E200", &SA5_access},
	{0x3213103C, "Smart Array E200i", &SA5_access},
	{0x3214103C, "Smart Array E200i", &SA5_access},
	{0x3215103C, "Smart Array E200i", &SA5_access},
	{0x3237103C, "Smart Array E500", &SA5_access},
	{0x323D103C, "Smart Array P700m", &SA5_access},
	{0x3241103C, "Smart Array P212", &SA5_access},
	{0x3243103C, "Smart Array P410", &SA5_access},
	{0x3245103C, "Smart Array P410i", &SA5_access},
	{0x3247103C, "Smart Array P411", &SA5_access},
	{0x3249103C, "Smart Array P812", &SA5_access},
	{0x324A103C, "Smart Array P712m", &SA5_access},
	{0x324B103C, "Smart Array P711m", &SA5_access},
	{0xFFFF103C, "Unknown Smart Array", &SA5_access},
};

/* How long to wait (in milliseconds) for board to go into simple mode */
#define MAX_CONFIG_WAIT 30000
#define MAX_IOCTL_CONFIG_WAIT 1000

/*define how many times we will try a command because of bus resets */
#define MAX_CMD_RETRIES 3

#define MAX_CTLR	32

/* Originally cciss driver only supports 8 major numbers */
#define MAX_CTLR_ORIG 	8

static ctlr_info_t *hba[MAX_CTLR];

static void do_cciss_request(struct request_queue *q);
static irqreturn_t do_cciss_intr(int irq, void *dev_id);
static int cciss_open(struct inode *inode, struct file *filep);
static int cciss_release(struct inode *inode, struct file *filep);
static int cciss_ioctl(struct inode *inode, struct file *filep,
		       unsigned int cmd, unsigned long arg);
static int cciss_getgeo(struct block_device *bdev, struct hd_geometry *geo);

static int cciss_revalidate(struct gendisk *disk);
static int rebuild_lun_table(ctlr_info_t *h, int first_time);
static int deregister_disk(struct gendisk *disk, drive_info_struct *drv,
			   int clear_all);

static void cciss_read_capacity(int ctlr, int logvol, int withirq,
			sector_t *total_size, unsigned int *block_size);
static void cciss_read_capacity_16(int ctlr, int logvol, int withirq,
			sector_t *total_size, unsigned int *block_size);
static void cciss_geometry_inquiry(int ctlr, int logvol,
			int withirq, sector_t total_size,
			unsigned int block_size, InquiryData_struct *inq_buff,
				   drive_info_struct *drv);
static void __devinit cciss_interrupt_mode(ctlr_info_t *, struct pci_dev *,
					   __u32);
static void start_io(ctlr_info_t *h);
static int sendcmd(__u8 cmd, int ctlr, void *buff, size_t size,
		   unsigned int use_unit_num, unsigned int log_unit,
		   __u8 page_code, unsigned char *scsi3addr, int cmd_type);
static int sendcmd_withirq(__u8 cmd, int ctlr, void *buff, size_t size,
			   unsigned int use_unit_num, unsigned int log_unit,
			   __u8 page_code, int cmd_type);

static void fail_all_cmds(unsigned long ctlr);

#ifdef CONFIG_PROC_FS
static void cciss_procinit(int i);
#else
static void cciss_procinit(int i)
{
}
#endif				/* CONFIG_PROC_FS */

#ifdef CONFIG_COMPAT
static long cciss_compat_ioctl(struct file *f, unsigned cmd, unsigned long arg);
#endif

static struct block_device_operations cciss_fops = {
	.owner = THIS_MODULE,
	.open = cciss_open,
	.release = cciss_release,
	.ioctl = cciss_ioctl,
	.getgeo = cciss_getgeo,
#ifdef CONFIG_COMPAT
	.compat_ioctl = cciss_compat_ioctl,
#endif
	.revalidate_disk = cciss_revalidate,
};

/*
 * Enqueuing and dequeuing functions for cmdlists.
 */
static inline void addQ(CommandList_struct **Qptr, CommandList_struct *c)
{
	if (*Qptr == NULL) {
		*Qptr = c;
		c->next = c->prev = c;
	} else {
		c->prev = (*Qptr)->prev;
		c->next = (*Qptr);
		(*Qptr)->prev->next = c;
		(*Qptr)->prev = c;
	}
}

static inline CommandList_struct *removeQ(CommandList_struct **Qptr,
					  CommandList_struct *c)
{
	if (c && c->next != c) {
		if (*Qptr == c)
			*Qptr = c->next;
		c->prev->next = c->next;
		c->next->prev = c->prev;
	} else {
		*Qptr = NULL;
	}
	return c;
}

#include "cciss_scsi.c"		/* For SCSI tape support */

#define RAID_UNKNOWN 6

#ifdef CONFIG_PROC_FS

/*
 * Report information about this controller.
 */
#define ENG_GIG 1000000000
#define ENG_GIG_FACTOR (ENG_GIG/512)
#define ENGAGE_SCSI	"engage scsi"
static const char *raid_label[] = { "0", "4", "1(1+0)", "5", "5+1", "ADG",
	"UNKNOWN"
};

static struct proc_dir_entry *proc_cciss;

static void cciss_seq_show_header(struct seq_file *seq)
{
	ctlr_info_t *h = seq->private;

	seq_printf(seq, "%s: HP %s Controller\n"
		"Board ID: 0x%08lx\n"
		"Firmware Version: %c%c%c%c\n"
		"IRQ: %d\n"
		"Logical drives: %d\n"
		"Current Q depth: %d\n"
		"Current # commands on controller: %d\n"
		"Max Q depth since init: %d\n"
		"Max # commands on controller since init: %d\n"
		"Max SG entries since init: %d\n",
		h->devname,
		h->product_name,
		(unsigned long)h->board_id,
		h->firm_ver[0], h->firm_ver[1], h->firm_ver[2],
		h->firm_ver[3], (unsigned int)h->intr[SIMPLE_MODE_INT],
		h->num_luns,
		h->Qdepth, h->commands_outstanding,
		h->maxQsinceinit, h->max_outstanding, h->maxSG);

#ifdef CONFIG_CISS_SCSI_TAPE
	cciss_seq_tape_report(seq, h->ctlr);
#endif /* CONFIG_CISS_SCSI_TAPE */
}

static void *cciss_seq_start(struct seq_file *seq, loff_t *pos)
{
	ctlr_info_t *h = seq->private;
	unsigned ctlr = h->ctlr;
	unsigned long flags;

	/* prevent displaying bogus info during configuration
	 * or deconfiguration of a logical volume
	 */
	spin_lock_irqsave(CCISS_LOCK(ctlr), flags);
	if (h->busy_configuring) {
		spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);
		return ERR_PTR(-EBUSY);
	}
	h->busy_configuring = 1;
	spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);

	if (*pos == 0)
		cciss_seq_show_header(seq);

	return pos;
}

static int cciss_seq_show(struct seq_file *seq, void *v)
{
	sector_t vol_sz, vol_sz_frac;
	ctlr_info_t *h = seq->private;
	unsigned ctlr = h->ctlr;
	loff_t *pos = v;
	drive_info_struct *drv = &h->drv[*pos];

	if (*pos > h->highest_lun)
		return 0;

	if (drv->heads == 0)
		return 0;

	vol_sz = drv->nr_blocks;
	vol_sz_frac = sector_div(vol_sz, ENG_GIG_FACTOR);
	vol_sz_frac *= 100;
	sector_div(vol_sz_frac, ENG_GIG_FACTOR);

	if (drv->raid_level > 5)
		drv->raid_level = RAID_UNKNOWN;
	seq_printf(seq, "cciss/c%dd%d:"
			"\t%4u.%02uGB\tRAID %s\n",
			ctlr, (int) *pos, (int)vol_sz, (int)vol_sz_frac,
			raid_label[drv->raid_level]);
	return 0;
}

static void *cciss_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	ctlr_info_t *h = seq->private;

	if (*pos > h->highest_lun)
		return NULL;
	*pos += 1;

	return pos;
}

static void cciss_seq_stop(struct seq_file *seq, void *v)
{
	ctlr_info_t *h = seq->private;

	/* Only reset h->busy_configuring if we succeeded in setting
	 * it during cciss_seq_start. */
	if (v == ERR_PTR(-EBUSY))
		return;

	h->busy_configuring = 0;
}

static struct seq_operations cciss_seq_ops = {
	.start = cciss_seq_start,
	.show  = cciss_seq_show,
	.next  = cciss_seq_next,
	.stop  = cciss_seq_stop,
};

static int cciss_seq_open(struct inode *inode, struct file *file)
{
	int ret = seq_open(file, &cciss_seq_ops);
	struct seq_file *seq = file->private_data;

	if (!ret)
		seq->private = PDE(inode)->data;

	return ret;
}

static ssize_t
cciss_proc_write(struct file *file, const char __user *buf,
		 size_t length, loff_t *ppos)
{
	int err;
	char *buffer;

#ifndef CONFIG_CISS_SCSI_TAPE
	return -EINVAL;
#endif

	if (!buf || length > PAGE_SIZE - 1)
		return -EINVAL;

	buffer = (char *)__get_free_page(GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	err = -EFAULT;
	if (copy_from_user(buffer, buf, length))
		goto out;
	buffer[length] = '\0';

#ifdef CONFIG_CISS_SCSI_TAPE
	if (strncmp(ENGAGE_SCSI, buffer, sizeof ENGAGE_SCSI - 1) == 0) {
		struct seq_file *seq = file->private_data;
		ctlr_info_t *h = seq->private;
		int rc;

		rc = cciss_engage_scsi(h->ctlr);
		if (rc != 0)
			err = -rc;
		else
			err = length;
	} else
#endif /* CONFIG_CISS_SCSI_TAPE */
		err = -EINVAL;
	/* might be nice to have "disengage" too, but it's not
	   safely possible. (only 1 module use count, lock issues.) */

out:
	free_page((unsigned long)buffer);
	return err;
}

static struct file_operations cciss_proc_fops = {
	.owner	 = THIS_MODULE,
	.open    = cciss_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release,
	.write	 = cciss_proc_write,
};

static void __devinit cciss_procinit(int i)
{
	struct proc_dir_entry *pde;

	if (proc_cciss == NULL)
		proc_cciss = proc_mkdir("driver/cciss", NULL);
	if (!proc_cciss)
		return;
	pde = proc_create_data(hba[i]->devname, S_IWUSR | S_IRUSR | S_IRGRP |
					S_IROTH, proc_cciss,
					&cciss_proc_fops, hba[i]);
}
#endif				/* CONFIG_PROC_FS */

/*
 * For operations that cannot sleep, a command block is allocated at init,
 * and managed by cmd_alloc() and cmd_free() using a simple bitmap to track
 * which ones are free or in use.  For operations that can wait for kmalloc
 * to possible sleep, this routine can be called with get_from_pool set to 0.
 * cmd_free() MUST be called with a got_from_pool set to 0 if cmd_alloc was.
 */
static CommandList_struct *cmd_alloc(ctlr_info_t *h, int get_from_pool)
{
	CommandList_struct *c;
	int i;
	u64bit temp64;
	dma_addr_t cmd_dma_handle, err_dma_handle;

	if (!get_from_pool) {
		c = (CommandList_struct *) pci_alloc_consistent(h->pdev,
			sizeof(CommandList_struct), &cmd_dma_handle);
		if (c == NULL)
			return NULL;
		memset(c, 0, sizeof(CommandList_struct));

		c->cmdindex = -1;

		c->err_info = (ErrorInfo_struct *)
		    pci_alloc_consistent(h->pdev, sizeof(ErrorInfo_struct),
			    &err_dma_handle);

		if (c->err_info == NULL) {
			pci_free_consistent(h->pdev,
				sizeof(CommandList_struct), c, cmd_dma_handle);
			return NULL;
		}
		memset(c->err_info, 0, sizeof(ErrorInfo_struct));
	} else {		/* get it out of the controllers pool */

		do {
			i = find_first_zero_bit(h->cmd_pool_bits, h->nr_cmds);
			if (i == h->nr_cmds)
				return NULL;
		} while (test_and_set_bit
			 (i & (BITS_PER_LONG - 1),
			  h->cmd_pool_bits + (i / BITS_PER_LONG)) != 0);
#ifdef CCISS_DEBUG
		printk(KERN_DEBUG "cciss: using command buffer %d\n", i);
#endif
		c = h->cmd_pool + i;
		memset(c, 0, sizeof(CommandList_struct));
		cmd_dma_handle = h->cmd_pool_dhandle
		    + i * sizeof(CommandList_struct);
		c->err_info = h->errinfo_pool + i;
		memset(c->err_info, 0, sizeof(ErrorInfo_struct));
		err_dma_handle = h->errinfo_pool_dhandle
		    + i * sizeof(ErrorInfo_struct);
		h->nr_allocs++;

		c->cmdindex = i;
	}

	c->busaddr = (__u32) cmd_dma_handle;
	temp64.val = (__u64) err_dma_handle;
	c->ErrDesc.Addr.lower = temp64.val32.lower;
	c->ErrDesc.Addr.upper = temp64.val32.upper;
	c->ErrDesc.Len = sizeof(ErrorInfo_struct);

	c->ctlr = h->ctlr;
	return c;
}

/*
 * Frees a command block that was previously allocated with cmd_alloc().
 */
static void cmd_free(ctlr_info_t *h, CommandList_struct *c, int got_from_pool)
{
	int i;
	u64bit temp64;

	if (!got_from_pool) {
		temp64.val32.lower = c->ErrDesc.Addr.lower;
		temp64.val32.upper = c->ErrDesc.Addr.upper;
		pci_free_consistent(h->pdev, sizeof(ErrorInfo_struct),
				    c->err_info, (dma_addr_t) temp64.val);
		pci_free_consistent(h->pdev, sizeof(CommandList_struct),
				    c, (dma_addr_t) c->busaddr);
	} else {
		i = c - h->cmd_pool;
		clear_bit(i & (BITS_PER_LONG - 1),
			  h->cmd_pool_bits + (i / BITS_PER_LONG));
		h->nr_frees++;
	}
}

static inline ctlr_info_t *get_host(struct gendisk *disk)
{
	return disk->queue->queuedata;
}

static inline drive_info_struct *get_drv(struct gendisk *disk)
{
	return disk->private_data;
}

/*
 * Open.  Make sure the device is really there.
 */
static int cciss_open(struct inode *inode, struct file *filep)
{
	ctlr_info_t *host = get_host(inode->i_bdev->bd_disk);
	drive_info_struct *drv = get_drv(inode->i_bdev->bd_disk);

#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "cciss_open %s\n", inode->i_bdev->bd_disk->disk_name);
#endif				/* CCISS_DEBUG */

	if (host->busy_initializing || drv->busy_configuring)
		return -EBUSY;
	/*
	 * Root is allowed to open raw volume zero even if it's not configured
	 * so array config can still work. Root is also allowed to open any
	 * volume that has a LUN ID, so it can issue IOCTL to reread the
	 * disk information.  I don't think I really like this
	 * but I'm already using way to many device nodes to claim another one
	 * for "raw controller".
	 */
	if (drv->heads == 0) {
		if (iminor(inode) != 0) {	/* not node 0? */
			/* if not node 0 make sure it is a partition = 0 */
			if (iminor(inode) & 0x0f) {
				return -ENXIO;
				/* if it is, make sure we have a LUN ID */
			} else if (drv->LunID == 0) {
				return -ENXIO;
			}
		}
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
	}
	drv->usage_count++;
	host->usage_count++;
	return 0;
}

/*
 * Close.  Sync first.
 */
static int cciss_release(struct inode *inode, struct file *filep)
{
	ctlr_info_t *host = get_host(inode->i_bdev->bd_disk);
	drive_info_struct *drv = get_drv(inode->i_bdev->bd_disk);

#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "cciss_release %s\n",
	       inode->i_bdev->bd_disk->disk_name);
#endif				/* CCISS_DEBUG */

	drv->usage_count--;
	host->usage_count--;
	return 0;
}

#ifdef CONFIG_COMPAT

static int do_ioctl(struct file *f, unsigned cmd, unsigned long arg)
{
	int ret;
	lock_kernel();
	ret = cciss_ioctl(f->f_path.dentry->d_inode, f, cmd, arg);
	unlock_kernel();
	return ret;
}

static int cciss_ioctl32_passthru(struct file *f, unsigned cmd,
				  unsigned long arg);
static int cciss_ioctl32_big_passthru(struct file *f, unsigned cmd,
				      unsigned long arg);

static long cciss_compat_ioctl(struct file *f, unsigned cmd, unsigned long arg)
{
	switch (cmd) {
	case CCISS_GETPCIINFO:
	case CCISS_GETINTINFO:
	case CCISS_SETINTINFO:
	case CCISS_GETNODENAME:
	case CCISS_SETNODENAME:
	case CCISS_GETHEARTBEAT:
	case CCISS_GETBUSTYPES:
	case CCISS_GETFIRMVER:
	case CCISS_GETDRIVVER:
	case CCISS_REVALIDVOLS:
	case CCISS_DEREGDISK:
	case CCISS_REGNEWDISK:
	case CCISS_REGNEWD:
	case CCISS_RESCANDISK:
	case CCISS_GETLUNINFO:
		return do_ioctl(f, cmd, arg);

	case CCISS_PASSTHRU32:
		return cciss_ioctl32_passthru(f, cmd, arg);
	case CCISS_BIG_PASSTHRU32:
		return cciss_ioctl32_big_passthru(f, cmd, arg);

	default:
		return -ENOIOCTLCMD;
	}
}

static int cciss_ioctl32_passthru(struct file *f, unsigned cmd,
				  unsigned long arg)
{
	IOCTL32_Command_struct __user *arg32 =
	    (IOCTL32_Command_struct __user *) arg;
	IOCTL_Command_struct arg64;
	IOCTL_Command_struct __user *p = compat_alloc_user_space(sizeof(arg64));
	int err;
	u32 cp;

	err = 0;
	err |=
	    copy_from_user(&arg64.LUN_info, &arg32->LUN_info,
			   sizeof(arg64.LUN_info));
	err |=
	    copy_from_user(&arg64.Request, &arg32->Request,
			   sizeof(arg64.Request));
	err |=
	    copy_from_user(&arg64.error_info, &arg32->error_info,
			   sizeof(arg64.error_info));
	err |= get_user(arg64.buf_size, &arg32->buf_size);
	err |= get_user(cp, &arg32->buf);
	arg64.buf = compat_ptr(cp);
	err |= copy_to_user(p, &arg64, sizeof(arg64));

	if (err)
		return -EFAULT;

	err = do_ioctl(f, CCISS_PASSTHRU, (unsigned long)p);
	if (err)
		return err;
	err |=
	    copy_in_user(&arg32->error_info, &p->error_info,
			 sizeof(arg32->error_info));
	if (err)
		return -EFAULT;
	return err;
}

static int cciss_ioctl32_big_passthru(struct file *file, unsigned cmd,
				      unsigned long arg)
{
	BIG_IOCTL32_Command_struct __user *arg32 =
	    (BIG_IOCTL32_Command_struct __user *) arg;
	BIG_IOCTL_Command_struct arg64;
	BIG_IOCTL_Command_struct __user *p =
	    compat_alloc_user_space(sizeof(arg64));
	int err;
	u32 cp;

	err = 0;
	err |=
	    copy_from_user(&arg64.LUN_info, &arg32->LUN_info,
			   sizeof(arg64.LUN_info));
	err |=
	    copy_from_user(&arg64.Request, &arg32->Request,
			   sizeof(arg64.Request));
	err |=
	    copy_from_user(&arg64.error_info, &arg32->error_info,
			   sizeof(arg64.error_info));
	err |= get_user(arg64.buf_size, &arg32->buf_size);
	err |= get_user(arg64.malloc_size, &arg32->malloc_size);
	err |= get_user(cp, &arg32->buf);
	arg64.buf = compat_ptr(cp);
	err |= copy_to_user(p, &arg64, sizeof(arg64));

	if (err)
		return -EFAULT;

	err = do_ioctl(file, CCISS_BIG_PASSTHRU, (unsigned long)p);
	if (err)
		return err;
	err |=
	    copy_in_user(&arg32->error_info, &p->error_info,
			 sizeof(arg32->error_info));
	if (err)
		return -EFAULT;
	return err;
}
#endif

static int cciss_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	drive_info_struct *drv = get_drv(bdev->bd_disk);

	if (!drv->cylinders)
		return -ENXIO;

	geo->heads = drv->heads;
	geo->sectors = drv->sectors;
	geo->cylinders = drv->cylinders;
	return 0;
}

/*
 * ioctl
 */
static int cciss_ioctl(struct inode *inode, struct file *filep,
		       unsigned int cmd, unsigned long arg)
{
	struct block_device *bdev = inode->i_bdev;
	struct gendisk *disk = bdev->bd_disk;
	ctlr_info_t *host = get_host(disk);
	drive_info_struct *drv = get_drv(disk);
	int ctlr = host->ctlr;
	void __user *argp = (void __user *)arg;

#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "cciss_ioctl: Called with cmd=%x %lx\n", cmd, arg);
#endif				/* CCISS_DEBUG */

	switch (cmd) {
	case CCISS_GETPCIINFO:
		{
			cciss_pci_info_struct pciinfo;

			if (!arg)
				return -EINVAL;
			pciinfo.domain = pci_domain_nr(host->pdev->bus);
			pciinfo.bus = host->pdev->bus->number;
			pciinfo.dev_fn = host->pdev->devfn;
			pciinfo.board_id = host->board_id;
			if (copy_to_user
			    (argp, &pciinfo, sizeof(cciss_pci_info_struct)))
				return -EFAULT;
			return 0;
		}
	case CCISS_GETINTINFO:
		{
			cciss_coalint_struct intinfo;
			if (!arg)
				return -EINVAL;
			intinfo.delay =
			    readl(&host->cfgtable->HostWrite.CoalIntDelay);
			intinfo.count =
			    readl(&host->cfgtable->HostWrite.CoalIntCount);
			if (copy_to_user
			    (argp, &intinfo, sizeof(cciss_coalint_struct)))
				return -EFAULT;
			return 0;
		}
	case CCISS_SETINTINFO:
		{
			cciss_coalint_struct intinfo;
			unsigned long flags;
			int i;

			if (!arg)
				return -EINVAL;
			if (!capable(CAP_SYS_ADMIN))
				return -EPERM;
			if (copy_from_user
			    (&intinfo, argp, sizeof(cciss_coalint_struct)))
				return -EFAULT;
			if ((intinfo.delay == 0) && (intinfo.count == 0))
			{
//                      printk("cciss_ioctl: delay and count cannot be 0\n");
				return -EINVAL;
			}
			spin_lock_irqsave(CCISS_LOCK(ctlr), flags);
			/* Update the field, and then ring the doorbell */
			writel(intinfo.delay,
			       &(host->cfgtable->HostWrite.CoalIntDelay));
			writel(intinfo.count,
			       &(host->cfgtable->HostWrite.CoalIntCount));
			writel(CFGTBL_ChangeReq, host->vaddr + SA5_DOORBELL);

			for (i = 0; i < MAX_IOCTL_CONFIG_WAIT; i++) {
				if (!(readl(host->vaddr + SA5_DOORBELL)
				      & CFGTBL_ChangeReq))
					break;
				/* delay and try again */
				udelay(1000);
			}
			spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);
			if (i >= MAX_IOCTL_CONFIG_WAIT)
				return -EAGAIN;
			return 0;
		}
	case CCISS_GETNODENAME:
		{
			NodeName_type NodeName;
			int i;

			if (!arg)
				return -EINVAL;
			for (i = 0; i < 16; i++)
				NodeName[i] =
				    readb(&host->cfgtable->ServerName[i]);
			if (copy_to_user(argp, NodeName, sizeof(NodeName_type)))
				return -EFAULT;
			return 0;
		}
	case CCISS_SETNODENAME:
		{
			NodeName_type NodeName;
			unsigned long flags;
			int i;

			if (!arg)
				return -EINVAL;
			if (!capable(CAP_SYS_ADMIN))
				return -EPERM;

			if (copy_from_user
			    (NodeName, argp, sizeof(NodeName_type)))
				return -EFAULT;

			spin_lock_irqsave(CCISS_LOCK(ctlr), flags);

			/* Update the field, and then ring the doorbell */
			for (i = 0; i < 16; i++)
				writeb(NodeName[i],
				       &host->cfgtable->ServerName[i]);

			writel(CFGTBL_ChangeReq, host->vaddr + SA5_DOORBELL);

			for (i = 0; i < MAX_IOCTL_CONFIG_WAIT; i++) {
				if (!(readl(host->vaddr + SA5_DOORBELL)
				      & CFGTBL_ChangeReq))
					break;
				/* delay and try again */
				udelay(1000);
			}
			spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);
			if (i >= MAX_IOCTL_CONFIG_WAIT)
				return -EAGAIN;
			return 0;
		}

	case CCISS_GETHEARTBEAT:
		{
			Heartbeat_type heartbeat;

			if (!arg)
				return -EINVAL;
			heartbeat = readl(&host->cfgtable->HeartBeat);
			if (copy_to_user
			    (argp, &heartbeat, sizeof(Heartbeat_type)))
				return -EFAULT;
			return 0;
		}
	case CCISS_GETBUSTYPES:
		{
			BusTypes_type BusTypes;

			if (!arg)
				return -EINVAL;
			BusTypes = readl(&host->cfgtable->BusTypes);
			if (copy_to_user
			    (argp, &BusTypes, sizeof(BusTypes_type)))
				return -EFAULT;
			return 0;
		}
	case CCISS_GETFIRMVER:
		{
			FirmwareVer_type firmware;

			if (!arg)
				return -EINVAL;
			memcpy(firmware, host->firm_ver, 4);

			if (copy_to_user
			    (argp, firmware, sizeof(FirmwareVer_type)))
				return -EFAULT;
			return 0;
		}
	case CCISS_GETDRIVVER:
		{
			DriverVer_type DriverVer = DRIVER_VERSION;

			if (!arg)
				return -EINVAL;

			if (copy_to_user
			    (argp, &DriverVer, sizeof(DriverVer_type)))
				return -EFAULT;
			return 0;
		}

	case CCISS_DEREGDISK:
	case CCISS_REGNEWD:
	case CCISS_REVALIDVOLS:
		return rebuild_lun_table(host, 0);

	case CCISS_GETLUNINFO:{
			LogvolInfo_struct luninfo;

			luninfo.LunID = drv->LunID;
			luninfo.num_opens = drv->usage_count;
			luninfo.num_parts = 0;
			if (copy_to_user(argp, &luninfo,
					 sizeof(LogvolInfo_struct)))
				return -EFAULT;
			return 0;
		}
	case CCISS_PASSTHRU:
		{
			IOCTL_Command_struct iocommand;
			CommandList_struct *c;
			char *buff = NULL;
			u64bit temp64;
			unsigned long flags;
			DECLARE_COMPLETION_ONSTACK(wait);

			if (!arg)
				return -EINVAL;

			if (!capable(CAP_SYS_RAWIO))
				return -EPERM;

			if (copy_from_user
			    (&iocommand, argp, sizeof(IOCTL_Command_struct)))
				return -EFAULT;
			if ((iocommand.buf_size < 1) &&
			    (iocommand.Request.Type.Direction != XFER_NONE)) {
				return -EINVAL;
			}
#if 0				/* 'buf_size' member is 16-bits, and always smaller than kmalloc limit */
			/* Check kmalloc limits */
			if (iocommand.buf_size > 128000)
				return -EINVAL;
#endif
			if (iocommand.buf_size > 0) {
				buff = kmalloc(iocommand.buf_size, GFP_KERNEL);
				if (buff == NULL)
					return -EFAULT;
			}
			if (iocommand.Request.Type.Direction == XFER_WRITE) {
				/* Copy the data into the buffer we created */
				if (copy_from_user
				    (buff, iocommand.buf, iocommand.buf_size)) {
					kfree(buff);
					return -EFAULT;
				}
			} else {
				memset(buff, 0, iocommand.buf_size);
			}
			if ((c = cmd_alloc(host, 0)) == NULL) {
				kfree(buff);
				return -ENOMEM;
			}
			// Fill in the command type
			c->cmd_type = CMD_IOCTL_PEND;
			// Fill in Command Header
			c->Header.ReplyQueue = 0;	// unused in simple mode
			if (iocommand.buf_size > 0)	// buffer to fill
			{
				c->Header.SGList = 1;
				c->Header.SGTotal = 1;
			} else	// no buffers to fill
			{
				c->Header.SGList = 0;
				c->Header.SGTotal = 0;
			}
			c->Header.LUN = iocommand.LUN_info;
			c->Header.Tag.lower = c->busaddr;	// use the kernel address the cmd block for tag

			// Fill in Request block
			c->Request = iocommand.Request;

			// Fill in the scatter gather information
			if (iocommand.buf_size > 0) {
				temp64.val = pci_map_single(host->pdev, buff,
					iocommand.buf_size,
					PCI_DMA_BIDIRECTIONAL);
				c->SG[0].Addr.lower = temp64.val32.lower;
				c->SG[0].Addr.upper = temp64.val32.upper;
				c->SG[0].Len = iocommand.buf_size;
				c->SG[0].Ext = 0;	// we are not chaining
			}
			c->waiting = &wait;

			/* Put the request on the tail of the request queue */
			spin_lock_irqsave(CCISS_LOCK(ctlr), flags);
			addQ(&host->reqQ, c);
			host->Qdepth++;
			start_io(host);
			spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);

			wait_for_completion(&wait);

			/* unlock the buffers from DMA */
			temp64.val32.lower = c->SG[0].Addr.lower;
			temp64.val32.upper = c->SG[0].Addr.upper;
			pci_unmap_single(host->pdev, (dma_addr_t) temp64.val,
					 iocommand.buf_size,
					 PCI_DMA_BIDIRECTIONAL);

			/* Copy the error information out */
			iocommand.error_info = *(c->err_info);
			if (copy_to_user
			    (argp, &iocommand, sizeof(IOCTL_Command_struct))) {
				kfree(buff);
				cmd_free(host, c, 0);
				return -EFAULT;
			}

			if (iocommand.Request.Type.Direction == XFER_READ) {
				/* Copy the data out of the buffer we created */
				if (copy_to_user
				    (iocommand.buf, buff, iocommand.buf_size)) {
					kfree(buff);
					cmd_free(host, c, 0);
					return -EFAULT;
				}
			}
			kfree(buff);
			cmd_free(host, c, 0);
			return 0;
		}
	case CCISS_BIG_PASSTHRU:{
			BIG_IOCTL_Command_struct *ioc;
			CommandList_struct *c;
			unsigned char **buff = NULL;
			int *buff_size = NULL;
			u64bit temp64;
			unsigned long flags;
			BYTE sg_used = 0;
			int status = 0;
			int i;
			DECLARE_COMPLETION_ONSTACK(wait);
			__u32 left;
			__u32 sz;
			BYTE __user *data_ptr;

			if (!arg)
				return -EINVAL;
			if (!capable(CAP_SYS_RAWIO))
				return -EPERM;
			ioc = (BIG_IOCTL_Command_struct *)
			    kmalloc(sizeof(*ioc), GFP_KERNEL);
			if (!ioc) {
				status = -ENOMEM;
				goto cleanup1;
			}
			if (copy_from_user(ioc, argp, sizeof(*ioc))) {
				status = -EFAULT;
				goto cleanup1;
			}
			if ((ioc->buf_size < 1) &&
			    (ioc->Request.Type.Direction != XFER_NONE)) {
				status = -EINVAL;
				goto cleanup1;
			}
			/* Check kmalloc limits  using all SGs */
			if (ioc->malloc_size > MAX_KMALLOC_SIZE) {
				status = -EINVAL;
				goto cleanup1;
			}
			if (ioc->buf_size > ioc->malloc_size * MAXSGENTRIES) {
				status = -EINVAL;
				goto cleanup1;
			}
			buff =
			    kzalloc(MAXSGENTRIES * sizeof(char *), GFP_KERNEL);
			if (!buff) {
				status = -ENOMEM;
				goto cleanup1;
			}
			buff_size = kmalloc(MAXSGENTRIES * sizeof(int),
						   GFP_KERNEL);
			if (!buff_size) {
				status = -ENOMEM;
				goto cleanup1;
			}
			left = ioc->buf_size;
			data_ptr = ioc->buf;
			while (left) {
				sz = (left >
				      ioc->malloc_size) ? ioc->
				    malloc_size : left;
				buff_size[sg_used] = sz;
				buff[sg_used] = kmalloc(sz, GFP_KERNEL);
				if (buff[sg_used] == NULL) {
					status = -ENOMEM;
					goto cleanup1;
				}
				if (ioc->Request.Type.Direction == XFER_WRITE) {
					if (copy_from_user
					    (buff[sg_used], data_ptr, sz)) {
						status = -EFAULT;
						goto cleanup1;
					}
				} else {
					memset(buff[sg_used], 0, sz);
				}
				left -= sz;
				data_ptr += sz;
				sg_used++;
			}
			if ((c = cmd_alloc(host, 0)) == NULL) {
				status = -ENOMEM;
				goto cleanup1;
			}
			c->cmd_type = CMD_IOCTL_PEND;
			c->Header.ReplyQueue = 0;

			if (ioc->buf_size > 0) {
				c->Header.SGList = sg_used;
				c->Header.SGTotal = sg_used;
			} else {
				c->Header.SGList = 0;
				c->Header.SGTotal = 0;
			}
			c->Header.LUN = ioc->LUN_info;
			c->Header.Tag.lower = c->busaddr;

			c->Request = ioc->Request;
			if (ioc->buf_size > 0) {
				int i;
				for (i = 0; i < sg_used; i++) {
					temp64.val =
					    pci_map_single(host->pdev, buff[i],
						    buff_size[i],
						    PCI_DMA_BIDIRECTIONAL);
					c->SG[i].Addr.lower =
					    temp64.val32.lower;
					c->SG[i].Addr.upper =
					    temp64.val32.upper;
					c->SG[i].Len = buff_size[i];
					c->SG[i].Ext = 0;	/* we are not chaining */
				}
			}
			c->waiting = &wait;
			/* Put the request on the tail of the request queue */
			spin_lock_irqsave(CCISS_LOCK(ctlr), flags);
			addQ(&host->reqQ, c);
			host->Qdepth++;
			start_io(host);
			spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);
			wait_for_completion(&wait);
			/* unlock the buffers from DMA */
			for (i = 0; i < sg_used; i++) {
				temp64.val32.lower = c->SG[i].Addr.lower;
				temp64.val32.upper = c->SG[i].Addr.upper;
				pci_unmap_single(host->pdev,
					(dma_addr_t) temp64.val, buff_size[i],
					PCI_DMA_BIDIRECTIONAL);
			}
			/* Copy the error information out */
			ioc->error_info = *(c->err_info);
			if (copy_to_user(argp, ioc, sizeof(*ioc))) {
				cmd_free(host, c, 0);
				status = -EFAULT;
				goto cleanup1;
			}
			if (ioc->Request.Type.Direction == XFER_READ) {
				/* Copy the data out of the buffer we created */
				BYTE __user *ptr = ioc->buf;
				for (i = 0; i < sg_used; i++) {
					if (copy_to_user
					    (ptr, buff[i], buff_size[i])) {
						cmd_free(host, c, 0);
						status = -EFAULT;
						goto cleanup1;
					}
					ptr += buff_size[i];
				}
			}
			cmd_free(host, c, 0);
			status = 0;
		      cleanup1:
			if (buff) {
				for (i = 0; i < sg_used; i++)
					kfree(buff[i]);
				kfree(buff);
			}
			kfree(buff_size);
			kfree(ioc);
			return status;
		}

	/* scsi_cmd_ioctl handles these, below, though some are not */
	/* very meaningful for cciss.  SG_IO is the main one people want. */

	case SG_GET_VERSION_NUM:
	case SG_SET_TIMEOUT:
	case SG_GET_TIMEOUT:
	case SG_GET_RESERVED_SIZE:
	case SG_SET_RESERVED_SIZE:
	case SG_EMULATED_HOST:
	case SG_IO:
	case SCSI_IOCTL_SEND_COMMAND:
		return scsi_cmd_ioctl(filep, disk->queue, disk, cmd, argp);

	/* scsi_cmd_ioctl would normally handle these, below, but */
	/* they aren't a good fit for cciss, as CD-ROMs are */
	/* not supported, and we don't have any bus/target/lun */
	/* which we present to the kernel. */

	case CDROM_SEND_PACKET:
	case CDROMCLOSETRAY:
	case CDROMEJECT:
	case SCSI_IOCTL_GET_IDLUN:
	case SCSI_IOCTL_GET_BUS_NUMBER:
	default:
		return -ENOTTY;
	}
}

static void cciss_check_queues(ctlr_info_t *h)
{
	int start_queue = h->next_to_run;
	int i;

	/* check to see if we have maxed out the number of commands that can
	 * be placed on the queue.  If so then exit.  We do this check here
	 * in case the interrupt we serviced was from an ioctl and did not
	 * free any new commands.
	 */
	if ((find_first_zero_bit(h->cmd_pool_bits, h->nr_cmds)) == h->nr_cmds)
		return;

	/* We have room on the queue for more commands.  Now we need to queue
	 * them up.  We will also keep track of the next queue to run so
	 * that every queue gets a chance to be started first.
	 */
	for (i = 0; i < h->highest_lun + 1; i++) {
		int curr_queue = (start_queue + i) % (h->highest_lun + 1);
		/* make sure the disk has been added and the drive is real
		 * because this can be called from the middle of init_one.
		 */
		if (!(h->drv[curr_queue].queue) || !(h->drv[curr_queue].heads))
			continue;
		blk_start_queue(h->gendisk[curr_queue]->queue);

		/* check to see if we have maxed out the number of commands
		 * that can be placed on the queue.
		 */
		if ((find_first_zero_bit(h->cmd_pool_bits, h->nr_cmds)) == h->nr_cmds) {
			if (curr_queue == start_queue) {
				h->next_to_run =
				    (start_queue + 1) % (h->highest_lun + 1);
				break;
			} else {
				h->next_to_run = curr_queue;
				break;
			}
		}
	}
}

static void cciss_softirq_done(struct request *rq)
{
	CommandList_struct *cmd = rq->completion_data;
	ctlr_info_t *h = hba[cmd->ctlr];
	unsigned long flags;
	u64bit temp64;
	int i, ddir;

	if (cmd->Request.Type.Direction == XFER_READ)
		ddir = PCI_DMA_FROMDEVICE;
	else
		ddir = PCI_DMA_TODEVICE;

	/* command did not need to be retried */
	/* unmap the DMA mapping for all the scatter gather elements */
	for (i = 0; i < cmd->Header.SGList; i++) {
		temp64.val32.lower = cmd->SG[i].Addr.lower;
		temp64.val32.upper = cmd->SG[i].Addr.upper;
		pci_unmap_page(h->pdev, temp64.val, cmd->SG[i].Len, ddir);
	}

#ifdef CCISS_DEBUG
	printk("Done with %p\n", rq);
#endif				/* CCISS_DEBUG */

	if (blk_end_request(rq, (rq->errors == 0) ? 0 : -EIO, blk_rq_bytes(rq)))
		BUG();

	spin_lock_irqsave(&h->lock, flags);
	cmd_free(h, cmd, 1);
	cciss_check_queues(h);
	spin_unlock_irqrestore(&h->lock, flags);
}

/* This function gets the serial number of a logical drive via
 * inquiry page 0x83.  Serial no. is 16 bytes.  If the serial
 * number cannot be had, for whatever reason, 16 bytes of 0xff
 * are returned instead.
 */
static void cciss_get_serial_no(int ctlr, int logvol, int withirq,
				unsigned char *serial_no, int buflen)
{
#define PAGE_83_INQ_BYTES 64
	int rc;
	unsigned char *buf;

	if (buflen > 16)
		buflen = 16;
	memset(serial_no, 0xff, buflen);
	buf = kzalloc(PAGE_83_INQ_BYTES, GFP_KERNEL);
	if (!buf)
		return;
	memset(serial_no, 0, buflen);
	if (withirq)
		rc = sendcmd_withirq(CISS_INQUIRY, ctlr, buf,
			PAGE_83_INQ_BYTES, 1, logvol, 0x83, TYPE_CMD);
	else
		rc = sendcmd(CISS_INQUIRY, ctlr, buf,
			PAGE_83_INQ_BYTES, 1, logvol, 0x83, NULL, TYPE_CMD);
	if (rc == IO_OK)
		memcpy(serial_no, &buf[8], buflen);
	kfree(buf);
	return;
}

static void cciss_add_disk(ctlr_info_t *h, struct gendisk *disk,
				int drv_index)
{
	disk->queue = blk_init_queue(do_cciss_request, &h->lock);
	sprintf(disk->disk_name, "cciss/c%dd%d", h->ctlr, drv_index);
	disk->major = h->major;
	disk->first_minor = drv_index << NWD_SHIFT;
	disk->fops = &cciss_fops;
	disk->private_data = &h->drv[drv_index];
	disk->driverfs_dev = &h->pdev->dev;

	/* Set up queue information */
	blk_queue_bounce_limit(disk->queue, h->pdev->dma_mask);

	/* This is a hardware imposed limit. */
	blk_queue_max_hw_segments(disk->queue, MAXSGENTRIES);

	/* This is a limit in the driver and could be eliminated. */
	blk_queue_max_phys_segments(disk->queue, MAXSGENTRIES);

	blk_queue_max_sectors(disk->queue, h->cciss_max_sectors);

	blk_queue_softirq_done(disk->queue, cciss_softirq_done);

	disk->queue->queuedata = h;

	blk_queue_hardsect_size(disk->queue,
				h->drv[drv_index].block_size);

	/* Make sure all queue data is written out before */
	/* setting h->drv[drv_index].queue, as setting this */
	/* allows the interrupt handler to start the queue */
	wmb();
	h->drv[drv_index].queue = disk->queue;
	add_disk(disk);
}

/* This function will check the usage_count of the drive to be updated/added.
 * If the usage_count is zero and it is a heretofore unknown drive, or,
 * the drive's capacity, geometry, or serial number has changed,
 * then the drive information will be updated and the disk will be
 * re-registered with the kernel.  If these conditions don't hold,
 * then it will be left alone for the next reboot.  The exception to this
 * is disk 0 which will always be left registered with the kernel since it
 * is also the controller node.  Any changes to disk 0 will show up on
 * the next reboot.
 */
static void cciss_update_drive_info(int ctlr, int drv_index, int first_time)
{
	ctlr_info_t *h = hba[ctlr];
	struct gendisk *disk;
	InquiryData_struct *inq_buff = NULL;
	unsigned int block_size;
	sector_t total_size;
	unsigned long flags = 0;
	int ret = 0;
	drive_info_struct *drvinfo;
	int was_only_controller_node;

	/* Get information about the disk and modify the driver structure */
	inq_buff = kmalloc(sizeof(InquiryData_struct), GFP_KERNEL);
	drvinfo = kmalloc(sizeof(*drvinfo), GFP_KERNEL);
	if (inq_buff == NULL || drvinfo == NULL)
		goto mem_msg;

	/* See if we're trying to update the "controller node"
	 * this will happen the when the first logical drive gets
	 * created by ACU.
	 */
	was_only_controller_node = (drv_index == 0 &&
				h->drv[0].raid_level == -1);

	/* testing to see if 16-byte CDBs are already being used */
	if (h->cciss_read == CCISS_READ_16) {
		cciss_read_capacity_16(h->ctlr, drv_index, 1,
			&total_size, &block_size);

	} else {
		cciss_read_capacity(ctlr, drv_index, 1,
				    &total_size, &block_size);

		/* if read_capacity returns all F's this volume is >2TB */
		/* in size so we switch to 16-byte CDB's for all */
		/* read/write ops */
		if (total_size == 0xFFFFFFFFULL) {
			cciss_read_capacity_16(ctlr, drv_index, 1,
			&total_size, &block_size);
			h->cciss_read = CCISS_READ_16;
			h->cciss_write = CCISS_WRITE_16;
		} else {
			h->cciss_read = CCISS_READ_10;
			h->cciss_write = CCISS_WRITE_10;
		}
	}

	cciss_geometry_inquiry(ctlr, drv_index, 1, total_size, block_size,
			       inq_buff, drvinfo);
	drvinfo->block_size = block_size;
	drvinfo->nr_blocks = total_size + 1;

	cciss_get_serial_no(ctlr, drv_index, 1, drvinfo->serial_no,
			sizeof(drvinfo->serial_no));

	/* Is it the same disk we already know, and nothing's changed? */
	if (h->drv[drv_index].raid_level != -1 &&
		((memcmp(drvinfo->serial_no,
				h->drv[drv_index].serial_no, 16) == 0) &&
		drvinfo->block_size == h->drv[drv_index].block_size &&
		drvinfo->nr_blocks == h->drv[drv_index].nr_blocks &&
		drvinfo->heads == h->drv[drv_index].heads &&
		drvinfo->sectors == h->drv[drv_index].sectors &&
		drvinfo->cylinders == h->drv[drv_index].cylinders))
			/* The disk is unchanged, nothing to update */
			goto freeret;

	/* If we get here it's not the same disk, or something's changed,
	 * so we need to * deregister it, and re-register it, if it's not
	 * in use.
	 * If the disk already exists then deregister it before proceeding
	 * (unless it's the first disk (for the controller node).
	 */
	if (h->drv[drv_index].raid_level != -1 && drv_index != 0) {
		printk(KERN_WARNING "disk %d has changed.\n", drv_index);
		spin_lock_irqsave(CCISS_LOCK(h->ctlr), flags);
		h->drv[drv_index].busy_configuring = 1;
		spin_unlock_irqrestore(CCISS_LOCK(h->ctlr), flags);

		/* deregister_disk sets h->drv[drv_index].queue = NULL
		 * which keeps the interrupt handler from starting
		 * the queue.
		 */
		ret = deregister_disk(h->gendisk[drv_index],
				      &h->drv[drv_index], 0);
		h->drv[drv_index].busy_configuring = 0;
	}

	/* If the disk is in use return */
	if (ret)
		goto freeret;

	/* Save the new information from cciss_geometry_inquiry
	 * and serial number inquiry.
	 */
	h->drv[drv_index].block_size = drvinfo->block_size;
	h->drv[drv_index].nr_blocks = drvinfo->nr_blocks;
	h->drv[drv_index].heads = drvinfo->heads;
	h->drv[drv_index].sectors = drvinfo->sectors;
	h->drv[drv_index].cylinders = drvinfo->cylinders;
	h->drv[drv_index].raid_level = drvinfo->raid_level;
	memcpy(h->drv[drv_index].serial_no, drvinfo->serial_no, 16);

	++h->num_luns;
	disk = h->gendisk[drv_index];
	set_capacity(disk, h->drv[drv_index].nr_blocks);

	/* If it's not disk 0 (drv_index != 0)
	 * or if it was disk 0, but there was previously
	 * no actual corresponding configured logical drive
	 * (raid_leve == -1) then we want to update the
	 * logical drive's information.
	 */
	if (drv_index || first_time)
		cciss_add_disk(h, disk, drv_index);

freeret:
	kfree(inq_buff);
	kfree(drvinfo);
	return;
mem_msg:
	printk(KERN_ERR "cciss: out of memory\n");
	goto freeret;
}

/* This function will find the first index of the controllers drive array
 * that has a -1 for the raid_level and will return that index.  This is
 * where new drives will be added.  If the index to be returned is greater
 * than the highest_lun index for the controller then highest_lun is set
 * to this new index.  If there are no available indexes then -1 is returned.
 * "controller_node" is used to know if this is a real logical drive, or just
 * the controller node, which determines if this counts towards highest_lun.
 */
static int cciss_find_free_drive_index(int ctlr, int controller_node)
{
	int i;

	for (i = 0; i < CISS_MAX_LUN; i++) {
		if (hba[ctlr]->drv[i].raid_level == -1) {
			if (i > hba[ctlr]->highest_lun)
				if (!controller_node)
					hba[ctlr]->highest_lun = i;
			return i;
		}
	}
	return -1;
}

/* cciss_add_gendisk finds a free hba[]->drv structure
 * and allocates a gendisk if needed, and sets the lunid
 * in the drvinfo structure.   It returns the index into
 * the ->drv[] array, or -1 if none are free.
 * is_controller_node indicates whether highest_lun should
 * count this disk, or if it's only being added to provide
 * a means to talk to the controller in case no logical
 * drives have yet been configured.
 */
static int cciss_add_gendisk(ctlr_info_t *h, __u32 lunid, int controller_node)
{
	int drv_index;

	drv_index = cciss_find_free_drive_index(h->ctlr, controller_node);
	if (drv_index == -1)
		return -1;
	/*Check if the gendisk needs to be allocated */
	if (!h->gendisk[drv_index]) {
		h->gendisk[drv_index] =
			alloc_disk(1 << NWD_SHIFT);
		if (!h->gendisk[drv_index]) {
			printk(KERN_ERR "cciss%d: could not "
				"allocate a new disk %d\n",
				h->ctlr, drv_index);
			return -1;
		}
	}
	h->drv[drv_index].LunID = lunid;

	/* Don't need to mark this busy because nobody */
	/* else knows about this disk yet to contend */
	/* for access to it. */
	h->drv[drv_index].busy_configuring = 0;
	wmb();
	return drv_index;
}

/* This is for the special case of a controller which
 * has no logical drives.  In this case, we still need
 * to register a disk so the controller can be accessed
 * by the Array Config Utility.
 */
static void cciss_add_controller_node(ctlr_info_t *h)
{
	struct gendisk *disk;
	int drv_index;

	if (h->gendisk[0] != NULL) /* already did this? Then bail. */
		return;

	drv_index = cciss_add_gendisk(h, 0, 1);
	if (drv_index == -1) {
		printk(KERN_WARNING "cciss%d: could not "
			"add disk 0.\n", h->ctlr);
		return;
	}
	h->drv[drv_index].block_size = 512;
	h->drv[drv_index].nr_blocks = 0;
	h->drv[drv_index].heads = 0;
	h->drv[drv_index].sectors = 0;
	h->drv[drv_index].cylinders = 0;
	h->drv[drv_index].raid_level = -1;
	memset(h->drv[drv_index].serial_no, 0, 16);
	disk = h->gendisk[drv_index];
	cciss_add_disk(h, disk, drv_index);
}

/* This function will add and remove logical drives from the Logical
 * drive array of the controller and maintain persistency of ordering
 * so that mount points are preserved until the next reboot.  This allows
 * for the removal of logical drives in the middle of the drive array
 * without a re-ordering of those drives.
 * INPUT
 * h		= The controller to perform the operations on
 */
static int rebuild_lun_table(ctlr_info_t *h, int first_time)
{
	int ctlr = h->ctlr;
	int num_luns;
	ReportLunData_struct *ld_buff = NULL;
	int return_code;
	int listlength = 0;
	int i;
	int drv_found;
	int drv_index = 0;
	__u32 lunid = 0;
	unsigned long flags;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	/* Set busy_configuring flag for this operation */
	spin_lock_irqsave(CCISS_LOCK(h->ctlr), flags);
	if (h->busy_configuring) {
		spin_unlock_irqrestore(CCISS_LOCK(h->ctlr), flags);
		return -EBUSY;
	}
	h->busy_configuring = 1;
	spin_unlock_irqrestore(CCISS_LOCK(h->ctlr), flags);

	ld_buff = kzalloc(sizeof(ReportLunData_struct), GFP_KERNEL);
	if (ld_buff == NULL)
		goto mem_msg;

	return_code = sendcmd_withirq(CISS_REPORT_LOG, ctlr, ld_buff,
				      sizeof(ReportLunData_struct), 0,
				      0, 0, TYPE_CMD);

	if (return_code == IO_OK)
		listlength = be32_to_cpu(*(__be32 *) ld_buff->LUNListLength);
	else {	/* reading number of logical volumes failed */
		printk(KERN_WARNING "cciss: report logical volume"
		       " command failed\n");
		listlength = 0;
		goto freeret;
	}

	num_luns = listlength / 8;	/* 8 bytes per entry */
	if (num_luns > CISS_MAX_LUN) {
		num_luns = CISS_MAX_LUN;
		printk(KERN_WARNING "cciss: more luns configured"
		       " on controller than can be handled by"
		       " this driver.\n");
	}

	if (num_luns == 0)
		cciss_add_controller_node(h);

	/* Compare controller drive array to driver's drive array
	 * to see if any drives are missing on the controller due
	 * to action of Array Config Utility (user deletes drive)
	 * and deregister logical drives which have disappeared.
	 */
	for (i = 0; i <= h->highest_lun; i++) {
		int j;
		drv_found = 0;

		/* skip holes in the array from already deleted drives */
		if (h->drv[i].raid_level == -1)
			continue;

		for (j = 0; j < num_luns; j++) {
			memcpy(&lunid, &ld_buff->LUN[j][0], 4);
			lunid = le32_to_cpu(lunid);
			if (h->drv[i].LunID == lunid) {
				drv_found = 1;
				break;
			}
		}
		if (!drv_found) {
			/* Deregister it from the OS, it's gone. */
			spin_lock_irqsave(CCISS_LOCK(h->ctlr), flags);
			h->drv[i].busy_configuring = 1;
			spin_unlock_irqrestore(CCISS_LOCK(h->ctlr), flags);
			return_code = deregister_disk(h->gendisk[i],
				&h->drv[i], 1);
			h->drv[i].busy_configuring = 0;
		}
	}

	/* Compare controller drive array to driver's drive array.
	 * Check for updates in the drive information and any new drives
	 * on the controller due to ACU adding logical drives, or changing
	 * a logical drive's size, etc.  Reregister any new/changed drives
	 */
	for (i = 0; i < num_luns; i++) {
		int j;

		drv_found = 0;

		memcpy(&lunid, &ld_buff->LUN[i][0], 4);
		lunid = le32_to_cpu(lunid);

		/* Find if the LUN is already in the drive array
		 * of the driver.  If so then update its info
		 * if not in use.  If it does not exist then find
		 * the first free index and add it.
		 */
		for (j = 0; j <= h->highest_lun; j++) {
			if (h->drv[j].raid_level != -1 &&
				h->drv[j].LunID == lunid) {
				drv_index = j;
				drv_found = 1;
				break;
			}
		}

		/* check if the drive was found already in the array */
		if (!drv_found) {
			drv_index = cciss_add_gendisk(h, lunid, 0);
			if (drv_index == -1)
				goto freeret;
		}
		cciss_update_drive_info(ctlr, drv_index, first_time);
	}		/* end for */

freeret:
	kfree(ld_buff);
	h->busy_configuring = 0;
	/* We return -1 here to tell the ACU that we have registered/updated
	 * all of the drives that we can and to keep it from calling us
	 * additional times.
	 */
	return -1;
mem_msg:
	printk(KERN_ERR "cciss: out of memory\n");
	h->busy_configuring = 0;
	goto freeret;
}

/* This function will deregister the disk and it's queue from the
 * kernel.  It must be called with the controller lock held and the
 * drv structures busy_configuring flag set.  It's parameters are:
 *
 * disk = This is the disk to be deregistered
 * drv  = This is the drive_info_struct associated with the disk to be
 *        deregistered.  It contains information about the disk used
 *        by the driver.
 * clear_all = This flag determines whether or not the disk information
 *             is going to be completely cleared out and the highest_lun
 *             reset.  Sometimes we want to clear out information about
 *             the disk in preparation for re-adding it.  In this case
 *             the highest_lun should be left unchanged and the LunID
 *             should not be cleared.
*/
static int deregister_disk(struct gendisk *disk, drive_info_struct *drv,
			   int clear_all)
{
	int i;
	ctlr_info_t *h = get_host(disk);

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	/* make sure logical volume is NOT is use */
	if (clear_all || (h->gendisk[0] == disk)) {
		if (drv->usage_count > 1)
			return -EBUSY;
	} else if (drv->usage_count > 0)
		return -EBUSY;

	/* invalidate the devices and deregister the disk.  If it is disk
	 * zero do not deregister it but just zero out it's values.  This
	 * allows us to delete disk zero but keep the controller registered.
	 */
	if (h->gendisk[0] != disk) {
		struct request_queue *q = disk->queue;
		if (disk->flags & GENHD_FL_UP)
			del_gendisk(disk);
		if (q) {
			blk_cleanup_queue(q);
			/* Set drv->queue to NULL so that we do not try
			 * to call blk_start_queue on this queue in the
			 * interrupt handler
			 */
			drv->queue = NULL;
		}
		/* If clear_all is set then we are deleting the logical
		 * drive, not just refreshing its info.  For drives
		 * other than disk 0 we will call put_disk.  We do not
		 * do this for disk 0 as we need it to be able to
		 * configure the controller.
		 */
		if (clear_all){
			/* This isn't pretty, but we need to find the
			 * disk in our array and NULL our the pointer.
			 * This is so that we will call alloc_disk if
			 * this index is used again later.
			 */
			for (i=0; i < CISS_MAX_LUN; i++){
				if (h->gendisk[i] == disk) {
					h->gendisk[i] = NULL;
					break;
				}
			}
			put_disk(disk);
		}
	} else {
		set_capacity(disk, 0);
	}

	--h->num_luns;
	/* zero out the disk size info */
	drv->nr_blocks = 0;
	drv->block_size = 0;
	drv->heads = 0;
	drv->sectors = 0;
	drv->cylinders = 0;
	drv->raid_level = -1;	/* This can be used as a flag variable to
				 * indicate that this element of the drive
				 * array is free.
				 */

	if (clear_all) {
		/* check to see if it was the last disk */
		if (drv == h->drv + h->highest_lun) {
			/* if so, find the new hightest lun */
			int i, newhighest = -1;
			for (i = 0; i <= h->highest_lun; i++) {
				/* if the disk has size > 0, it is available */
				if (h->drv[i].heads)
					newhighest = i;
			}
			h->highest_lun = newhighest;
		}

		drv->LunID = 0;
	}
	return 0;
}

static int fill_cmd(CommandList_struct *c, __u8 cmd, int ctlr, void *buff, size_t size, unsigned int use_unit_num,	/* 0: address the controller,
															   1: address logical volume log_unit,
															   2: periph device address is scsi3addr */
		    unsigned int log_unit, __u8 page_code,
		    unsigned char *scsi3addr, int cmd_type)
{
	ctlr_info_t *h = hba[ctlr];
	u64bit buff_dma_handle;
	int status = IO_OK;

	c->cmd_type = CMD_IOCTL_PEND;
	c->Header.ReplyQueue = 0;
	if (buff != NULL) {
		c->Header.SGList = 1;
		c->Header.SGTotal = 1;
	} else {
		c->Header.SGList = 0;
		c->Header.SGTotal = 0;
	}
	c->Header.Tag.lower = c->busaddr;

	c->Request.Type.Type = cmd_type;
	if (cmd_type == TYPE_CMD) {
		switch (cmd) {
		case CISS_INQUIRY:
			/* If the logical unit number is 0 then, this is going
			   to controller so It's a physical command
			   mode = 0 target = 0.  So we have nothing to write.
			   otherwise, if use_unit_num == 1,
			   mode = 1(volume set addressing) target = LUNID
			   otherwise, if use_unit_num == 2,
			   mode = 0(periph dev addr) target = scsi3addr */
			if (use_unit_num == 1) {
				c->Header.LUN.LogDev.VolId =
				    h->drv[log_unit].LunID;
				c->Header.LUN.LogDev.Mode = 1;
			} else if (use_unit_num == 2) {
				memcpy(c->Header.LUN.LunAddrBytes, scsi3addr,
				       8);
				c->Header.LUN.LogDev.Mode = 0;
			}
			/* are we trying to read a vital product page */
			if (page_code != 0) {
				c->Request.CDB[1] = 0x01;
				c->Request.CDB[2] = page_code;
			}
			c->Request.CDBLen = 6;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_READ;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = CISS_INQUIRY;
			c->Request.CDB[4] = size & 0xFF;
			break;
		case CISS_REPORT_LOG:
		case CISS_REPORT_PHYS:
			/* Talking to controller so It's a physical command
			   mode = 00 target = 0.  Nothing to write.
			 */
			c->Request.CDBLen = 12;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_READ;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = cmd;
			c->Request.CDB[6] = (size >> 24) & 0xFF;	//MSB
			c->Request.CDB[7] = (size >> 16) & 0xFF;
			c->Request.CDB[8] = (size >> 8) & 0xFF;
			c->Request.CDB[9] = size & 0xFF;
			break;

		case CCISS_READ_CAPACITY:
			c->Header.LUN.LogDev.VolId = h->drv[log_unit].LunID;
			c->Header.LUN.LogDev.Mode = 1;
			c->Request.CDBLen = 10;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_READ;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = cmd;
			break;
		case CCISS_READ_CAPACITY_16:
			c->Header.LUN.LogDev.VolId = h->drv[log_unit].LunID;
			c->Header.LUN.LogDev.Mode = 1;
			c->Request.CDBLen = 16;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_READ;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = cmd;
			c->Request.CDB[1] = 0x10;
			c->Request.CDB[10] = (size >> 24) & 0xFF;
			c->Request.CDB[11] = (size >> 16) & 0xFF;
			c->Request.CDB[12] = (size >> 8) & 0xFF;
			c->Request.CDB[13] = size & 0xFF;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = cmd;
			break;
		case CCISS_CACHE_FLUSH:
			c->Request.CDBLen = 12;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_WRITE;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = BMIC_WRITE;
			c->Request.CDB[6] = BMIC_CACHE_FLUSH;
			break;
		default:
			printk(KERN_WARNING
			       "cciss%d:  Unknown Command 0x%c\n", ctlr, cmd);
			return IO_ERROR;
		}
	} else if (cmd_type == TYPE_MSG) {
		switch (cmd) {
		case 0:	/* ABORT message */
			c->Request.CDBLen = 12;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_WRITE;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = cmd;	/* abort */
			c->Request.CDB[1] = 0;	/* abort a command */
			/* buff contains the tag of the command to abort */
			memcpy(&c->Request.CDB[4], buff, 8);
			break;
		case 1:	/* RESET message */
			c->Request.CDBLen = 12;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_WRITE;
			c->Request.Timeout = 0;
			memset(&c->Request.CDB[0], 0, sizeof(c->Request.CDB));
			c->Request.CDB[0] = cmd;	/* reset */
			c->Request.CDB[1] = 0x04;	/* reset a LUN */
			break;
		case 3:	/* No-Op message */
			c->Request.CDBLen = 1;
			c->Request.Type.Attribute = ATTR_SIMPLE;
			c->Request.Type.Direction = XFER_WRITE;
			c->Request.Timeout = 0;
			c->Request.CDB[0] = cmd;
			break;
		default:
			printk(KERN_WARNING
			       "cciss%d: unknown message type %d\n", ctlr, cmd);
			return IO_ERROR;
		}
	} else {
		printk(KERN_WARNING
		       "cciss%d: unknown command type %d\n", ctlr, cmd_type);
		return IO_ERROR;
	}
	/* Fill in the scatter gather information */
	if (size > 0) {
		buff_dma_handle.val = (__u64) pci_map_single(h->pdev,
							     buff, size,
							     PCI_DMA_BIDIRECTIONAL);
		c->SG[0].Addr.lower = buff_dma_handle.val32.lower;
		c->SG[0].Addr.upper = buff_dma_handle.val32.upper;
		c->SG[0].Len = size;
		c->SG[0].Ext = 0;	/* we are not chaining */
	}
	return status;
}

static int sendcmd_withirq(__u8 cmd,
			   int ctlr,
			   void *buff,
			   size_t size,
			   unsigned int use_unit_num,
			   unsigned int log_unit, __u8 page_code, int cmd_type)
{
	ctlr_info_t *h = hba[ctlr];
	CommandList_struct *c;
	u64bit buff_dma_handle;
	unsigned long flags;
	int return_status;
	DECLARE_COMPLETION_ONSTACK(wait);

	if ((c = cmd_alloc(h, 0)) == NULL)
		return -ENOMEM;
	return_status = fill_cmd(c, cmd, ctlr, buff, size, use_unit_num,
				 log_unit, page_code, NULL, cmd_type);
	if (return_status != IO_OK) {
		cmd_free(h, c, 0);
		return return_status;
	}
      resend_cmd2:
	c->waiting = &wait;

	/* Put the request on the tail of the queue and send it */
	spin_lock_irqsave(CCISS_LOCK(ctlr), flags);
	addQ(&h->reqQ, c);
	h->Qdepth++;
	start_io(h);
	spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);

	wait_for_completion(&wait);

	if (c->err_info->CommandStatus != 0) {	/* an error has occurred */
		switch (c->err_info->CommandStatus) {
		case CMD_TARGET_STATUS:
			printk(KERN_WARNING "cciss: cmd %p has "
			       " completed with errors\n", c);
			if (c->err_info->ScsiStatus) {
				printk(KERN_WARNING "cciss: cmd %p "
				       "has SCSI Status = %x\n",
				       c, c->err_info->ScsiStatus);
			}

			break;
		case CMD_DATA_UNDERRUN:
		case CMD_DATA_OVERRUN:
			/* expected for inquire and report lun commands */
			break;
		case CMD_INVALID:
			printk(KERN_WARNING "cciss: Cmd %p is "
			       "reported invalid\n", c);
			return_status = IO_ERROR;
			break;
		case CMD_PROTOCOL_ERR:
			printk(KERN_WARNING "cciss: cmd %p has "
			       "protocol error \n", c);
			return_status = IO_ERROR;
			break;
		case CMD_HARDWARE_ERR:
			printk(KERN_WARNING "cciss: cmd %p had "
			       " hardware error\n", c);
			return_status = IO_ERROR;
			break;
		case CMD_CONNECTION_LOST:
			printk(KERN_WARNING "cciss: cmd %p had "
			       "connection lost\n", c);
			return_status = IO_ERROR;
			break;
		case CMD_ABORTED:
			printk(KERN_WARNING "cciss: cmd %p was "
			       "aborted\n", c);
			return_status = IO_ERROR;
			break;
		case CMD_ABORT_FAILED:
			printk(KERN_WARNING "cciss: cmd %p reports "
			       "abort failed\n", c);
			return_status = IO_ERROR;
			break;
		case CMD_UNSOLICITED_ABORT:
			printk(KERN_WARNING
			       "cciss%d: unsolicited abort %p\n", ctlr, c);
			if (c->retry_count < MAX_CMD_RETRIES) {
				printk(KERN_WARNING
				       "cciss%d: retrying %p\n", ctlr, c);
				c->retry_count++;
				/* erase the old error information */
				memset(c->err_info, 0,
				       sizeof(ErrorInfo_struct));
				return_status = IO_OK;
				INIT_COMPLETION(wait);
				goto resend_cmd2;
			}
			return_status = IO_ERROR;
			break;
		default:
			printk(KERN_WARNING "cciss: cmd %p returned "
			       "unknown status %x\n", c,
			       c->err_info->CommandStatus);
			return_status = IO_ERROR;
		}
	}
	/* unlock the buffers from DMA */
	buff_dma_handle.val32.lower = c->SG[0].Addr.lower;
	buff_dma_handle.val32.upper = c->SG[0].Addr.upper;
	pci_unmap_single(h->pdev, (dma_addr_t) buff_dma_handle.val,
			 c->SG[0].Len, PCI_DMA_BIDIRECTIONAL);
	cmd_free(h, c, 0);
	return return_status;
}

static void cciss_geometry_inquiry(int ctlr, int logvol,
				   int withirq, sector_t total_size,
				   unsigned int block_size,
				   InquiryData_struct *inq_buff,
				   drive_info_struct *drv)
{
	int return_code;
	unsigned long t;

	memset(inq_buff, 0, sizeof(InquiryData_struct));
	if (withirq)
		return_code = sendcmd_withirq(CISS_INQUIRY, ctlr,
					      inq_buff, sizeof(*inq_buff), 1,
					      logvol, 0xC1, TYPE_CMD);
	else
		return_code = sendcmd(CISS_INQUIRY, ctlr, inq_buff,
				      sizeof(*inq_buff), 1, logvol, 0xC1, NULL,
				      TYPE_CMD);
	if (return_code == IO_OK) {
		if (inq_buff->data_byte[8] == 0xFF) {
			printk(KERN_WARNING
			       "cciss: reading geometry failed, volume "
			       "does not support reading geometry\n");
			drv->heads = 255;
			drv->sectors = 32;	// Sectors per track
			drv->cylinders = total_size + 1;
			drv->raid_level = RAID_UNKNOWN;
		} else {
			drv->heads = inq_buff->data_byte[6];
			drv->sectors = inq_buff->data_byte[7];
			drv->cylinders = (inq_buff->data_byte[4] & 0xff) << 8;
			drv->cylinders += inq_buff->data_byte[5];
			drv->raid_level = inq_buff->data_byte[8];
		}
		drv->block_size = block_size;
		drv->nr_blocks = total_size + 1;
		t = drv->heads * drv->sectors;
		if (t > 1) {
			sector_t real_size = total_size + 1;
			unsigned long rem = sector_div(real_size, t);
			if (rem)
				real_size++;
			drv->cylinders = real_size;
		}
	} else {		/* Get geometry failed */
		printk(KERN_WARNING "cciss: reading geometry failed\n");
	}
	printk(KERN_INFO "      heads=%d, sectors=%d, cylinders=%d\n\n",
	       drv->heads, drv->sectors, drv->cylinders);
}

static void
cciss_read_capacity(int ctlr, int logvol, int withirq, sector_t *total_size,
		    unsigned int *block_size)
{
	ReadCapdata_struct *buf;
	int return_code;

	buf = kzalloc(sizeof(ReadCapdata_struct), GFP_KERNEL);
	if (!buf) {
		printk(KERN_WARNING "cciss: out of memory\n");
		return;
	}

	if (withirq)
		return_code = sendcmd_withirq(CCISS_READ_CAPACITY,
				ctlr, buf, sizeof(ReadCapdata_struct),
					1, logvol, 0, TYPE_CMD);
	else
		return_code = sendcmd(CCISS_READ_CAPACITY,
				ctlr, buf, sizeof(ReadCapdata_struct),
					1, logvol, 0, NULL, TYPE_CMD);
	if (return_code == IO_OK) {
		*total_size = be32_to_cpu(*(__be32 *) buf->total_size);
		*block_size = be32_to_cpu(*(__be32 *) buf->block_size);
	} else {		/* read capacity command failed */
		printk(KERN_WARNING "cciss: read capacity failed\n");
		*total_size = 0;
		*block_size = BLOCK_SIZE;
	}
	if (*total_size != 0)
		printk(KERN_INFO "      blocks= %llu block_size= %d\n",
		(unsigned long long)*total_size+1, *block_size);
	kfree(buf);
}

static void
cciss_read_capacity_16(int ctlr, int logvol, int withirq, sector_t *total_size, 				unsigned int *block_size)
{
	ReadCapdata_struct_16 *buf;
	int return_code;

	buf = kzalloc(sizeof(ReadCapdata_struct_16), GFP_KERNEL);
	if (!buf) {
		printk(KERN_WARNING "cciss: out of memory\n");
		return;
	}

	if (withirq) {
		return_code = sendcmd_withirq(CCISS_READ_CAPACITY_16,
			ctlr, buf, sizeof(ReadCapdata_struct_16),
				1, logvol, 0, TYPE_CMD);
	}
	else {
		return_code = sendcmd(CCISS_READ_CAPACITY_16,
			ctlr, buf, sizeof(ReadCapdata_struct_16),
				1, logvol, 0, NULL, TYPE_CMD);
	}
	if (return_code == IO_OK) {
		*total_size = be64_to_cpu(*(__be64 *) buf->total_size);
		*block_size = be32_to_cpu(*(__be32 *) buf->block_size);
	} else {		/* read capacity command failed */
		printk(KERN_WARNING "cciss: read capacity failed\n");
		*total_size = 0;
		*block_size = BLOCK_SIZE;
	}
	printk(KERN_INFO "      blocks= %llu block_size= %d\n",
	       (unsigned long long)*total_size+1, *block_size);
	kfree(buf);
}

static int cciss_revalidate(struct gendisk *disk)
{
	ctlr_info_t *h = get_host(disk);
	drive_info_struct *drv = get_drv(disk);
	int logvol;
	int FOUND = 0;
	unsigned int block_size;
	sector_t total_size;
	InquiryData_struct *inq_buff = NULL;

	for (logvol = 0; logvol < CISS_MAX_LUN; logvol++) {
		if (h->drv[logvol].LunID == drv->LunID) {
			FOUND = 1;
			break;
		}
	}

	if (!FOUND)
		return 1;

	inq_buff = kmalloc(sizeof(InquiryData_struct), GFP_KERNEL);
	if (inq_buff == NULL) {
		printk(KERN_WARNING "cciss: out of memory\n");
		return 1;
	}
	if (h->cciss_read == CCISS_READ_10) {
		cciss_read_capacity(h->ctlr, logvol, 1,
					&total_size, &block_size);
	} else {
		cciss_read_capacity_16(h->ctlr, logvol, 1,
					&total_size, &block_size);
	}
	cciss_geometry_inquiry(h->ctlr, logvol, 1, total_size, block_size,
			       inq_buff, drv);

	blk_queue_hardsect_size(drv->queue, drv->block_size);
	set_capacity(disk, drv->nr_blocks);

	kfree(inq_buff);
	return 0;
}

/*
 *   Wait polling for a command to complete.
 *   The memory mapped FIFO is polled for the completion.
 *   Used only at init time, interrupts from the HBA are disabled.
 */
static unsigned long pollcomplete(int ctlr)
{
	unsigned long done;
	int i;

	/* Wait (up to 20 seconds) for a command to complete */

	for (i = 20 * HZ; i > 0; i--) {
		done = hba[ctlr]->access.command_completed(hba[ctlr]);
		if (done == FIFO_EMPTY)
			schedule_timeout_uninterruptible(1);
		else
			return done;
	}
	/* Invalid address to tell caller we ran out of time */
	return 1;
}

static int add_sendcmd_reject(__u8 cmd, int ctlr, unsigned long complete)
{
	/* We get in here if sendcmd() is polling for completions
	   and gets some command back that it wasn't expecting --
	   something other than that which it just sent down.
	   Ordinarily, that shouldn't happen, but it can happen when
	   the scsi tape stuff gets into error handling mode, and
	   starts using sendcmd() to try to abort commands and
	   reset tape drives.  In that case, sendcmd may pick up
	   completions of commands that were sent to logical drives
	   through the block i/o system, or cciss ioctls completing, etc.
	   In that case, we need to save those completions for later
	   processing by the interrupt handler.
	 */

#ifdef CONFIG_CISS_SCSI_TAPE
	struct sendcmd_reject_list *srl = &hba[ctlr]->scsi_rejects;

	/* If it's not the scsi tape stuff doing error handling, (abort */
	/* or reset) then we don't expect anything weird. */
	if (cmd != CCISS_RESET_MSG && cmd != CCISS_ABORT_MSG) {
#endif
		printk(KERN_WARNING "cciss cciss%d: SendCmd "
		       "Invalid command list address returned! (%lx)\n",
		       ctlr, complete);
		/* not much we can do. */
#ifdef CONFIG_CISS_SCSI_TAPE
		return 1;
	}

	/* We've sent down an abort or reset, but something else
	   has completed */
	if (srl->ncompletions >= (hba[ctlr]->nr_cmds + 2)) {
		/* Uh oh.  No room to save it for later... */
		printk(KERN_WARNING "cciss%d: Sendcmd: Invalid command addr, "
		       "reject list overflow, command lost!\n", ctlr);
		return 1;
	}
	/* Save it for later */
	srl->complete[srl->ncompletions] = complete;
	srl->ncompletions++;
#endif
	return 0;
}

/*
 * Send a command to the controller, and wait for it to complete.
 * Only used at init time.
 */
static int sendcmd(__u8 cmd, int ctlr, void *buff, size_t size, unsigned int use_unit_num,	/* 0: address the controller,
												   1: address logical volume log_unit,
												   2: periph device address is scsi3addr */
		   unsigned int log_unit,
		   __u8 page_code, unsigned char *scsi3addr, int cmd_type)
{
	CommandList_struct *c;
	int i;
	unsigned long complete;
	ctlr_info_t *info_p = hba[ctlr];
	u64bit buff_dma_handle;
	int status, done = 0;

	if ((c = cmd_alloc(info_p, 1)) == NULL) {
		printk(KERN_WARNING "cciss: unable to get memory");
		return IO_ERROR;
	}
	status = fill_cmd(c, cmd, ctlr, buff, size, use_unit_num,
			  log_unit, page_code, scsi3addr, cmd_type);
	if (status != IO_OK) {
		cmd_free(info_p, c, 1);
		return status;
	}
      resend_cmd1:
	/*
	 * Disable interrupt
	 */
#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "cciss: turning intr off\n");
#endif				/* CCISS_DEBUG */
	info_p->access.set_intr_mask(info_p, CCISS_INTR_OFF);

	/* Make sure there is room in the command FIFO */
	/* Actually it should be completely empty at this time */
	/* unless we are in here doing error handling for the scsi */
	/* tape side of the driver. */
	for (i = 200000; i > 0; i--) {
		/* if fifo isn't full go */
		if (!(info_p->access.fifo_full(info_p))) {

			break;
		}
		udelay(10);
		printk(KERN_WARNING "cciss cciss%d: SendCmd FIFO full,"
		       " waiting!\n", ctlr);
	}
	/*
	 * Send the cmd
	 */
	info_p->access.submit_command(info_p, c);
	done = 0;
	do {
		complete = pollcomplete(ctlr);

#ifdef CCISS_DEBUG
		printk(KERN_DEBUG "cciss: command completed\n");
#endif				/* CCISS_DEBUG */

		if (complete == 1) {
			printk(KERN_WARNING
			       "cciss cciss%d: SendCmd Timeout out, "
			       "No command list address returned!\n", ctlr);
			status = IO_ERROR;
			done = 1;
			break;
		}

		/* This will need to change for direct lookup completions */
		if ((complete & CISS_ERROR_BIT)
		    && (complete & ~CISS_ERROR_BIT) == c->busaddr) {
			/* if data overrun or underun on Report command
			   ignore it
			 */
			if (((c->Request.CDB[0] == CISS_REPORT_LOG) ||
			     (c->Request.CDB[0] == CISS_REPORT_PHYS) ||
			     (c->Request.CDB[0] == CISS_INQUIRY)) &&
			    ((c->err_info->CommandStatus ==
			      CMD_DATA_OVERRUN) ||
			     (c->err_info->CommandStatus == CMD_DATA_UNDERRUN)
			    )) {
				complete = c->busaddr;
			} else {
				if (c->err_info->CommandStatus ==
				    CMD_UNSOLICITED_ABORT) {
					printk(KERN_WARNING "cciss%d: "
					       "unsolicited abort %p\n",
					       ctlr, c);
					if (c->retry_count < MAX_CMD_RETRIES) {
						printk(KERN_WARNING
						       "cciss%d: retrying %p\n",
						       ctlr, c);
						c->retry_count++;
						/* erase the old error */
						/* information */
						memset(c->err_info, 0,
						       sizeof
						       (ErrorInfo_struct));
						goto resend_cmd1;
					} else {
						printk(KERN_WARNING
						       "cciss%d: retried %p too "
						       "many times\n", ctlr, c);
						status = IO_ERROR;
						goto cleanup1;
					}
				} else if (c->err_info->CommandStatus ==
					   CMD_UNABORTABLE) {
					printk(KERN_WARNING
					       "cciss%d: command could not be aborted.\n",
					       ctlr);
					status = IO_ERROR;
					goto cleanup1;
				}
				printk(KERN_WARNING "ciss ciss%d: sendcmd"
				       " Error %x \n", ctlr,
				       c->err_info->CommandStatus);
				printk(KERN_WARNING "ciss ciss%d: sendcmd"
				       " offensive info\n"
				       "  size %x\n   num %x   value %x\n",
				       ctlr,
				       c->err_info->MoreErrInfo.Invalid_Cmd.
				       offense_size,
				       c->err_info->MoreErrInfo.Invalid_Cmd.
				       offense_num,
				       c->err_info->MoreErrInfo.Invalid_Cmd.
				       offense_value);
				status = IO_ERROR;
				goto cleanup1;
			}
		}
		/* This will need changing for direct lookup completions */
		if (complete != c->busaddr) {
			if (add_sendcmd_reject(cmd, ctlr, complete) != 0) {
				BUG();	/* we are pretty much hosed if we get here. */
			}
			continue;
		} else
			done = 1;
	} while (!done);

      cleanup1:
	/* unlock the data buffer from DMA */
	buff_dma_handle.val32.lower = c->SG[0].Addr.lower;
	buff_dma_handle.val32.upper = c->SG[0].Addr.upper;
	pci_unmap_single(info_p->pdev, (dma_addr_t) buff_dma_handle.val,
			 c->SG[0].Len, PCI_DMA_BIDIRECTIONAL);
#ifdef CONFIG_CISS_SCSI_TAPE
	/* if we saved some commands for later, process them now. */
	if (info_p->scsi_rejects.ncompletions > 0)
		do_cciss_intr(0, info_p);
#endif
	cmd_free(info_p, c, 1);
	return status;
}

/*
 * Map (physical) PCI mem into (virtual) kernel space
 */
static void __iomem *remap_pci_mem(ulong base, ulong size)
{
	ulong page_base = ((ulong) base) & PAGE_MASK;
	ulong page_offs = ((ulong) base) - page_base;
	void __iomem *page_remapped = ioremap(page_base, page_offs + size);

	return page_remapped ? (page_remapped + page_offs) : NULL;
}

/*
 * Takes jobs of the Q and sends them to the hardware, then puts it on
 * the Q to wait for completion.
 */
static void start_io(ctlr_info_t *h)
{
	CommandList_struct *c;

	while ((c = h->reqQ) != NULL) {
		/* can't do anything if fifo is full */
		if ((h->access.fifo_full(h))) {
			printk(KERN_WARNING "cciss: fifo full\n");
			break;
		}

		/* Get the first entry from the Request Q */
		removeQ(&(h->reqQ), c);
		h->Qdepth--;

		/* Tell the controller execute command */
		h->access.submit_command(h, c);

		/* Put job onto the completed Q */
		addQ(&(h->cmpQ), c);
	}
}

/* Assumes that CCISS_LOCK(h->ctlr) is held. */
/* Zeros out the error record and then resends the command back */
/* to the controller */
static inline void resend_cciss_cmd(ctlr_info_t *h, CommandList_struct *c)
{
	/* erase the old error information */
	memset(c->err_info, 0, sizeof(ErrorInfo_struct));

	/* add it to software queue and then send it to the controller */
	addQ(&(h->reqQ), c);
	h->Qdepth++;
	if (h->Qdepth > h->maxQsinceinit)
		h->maxQsinceinit = h->Qdepth;

	start_io(h);
}

static inline unsigned int make_status_bytes(unsigned int scsi_status_byte,
	unsigned int msg_byte, unsigned int host_byte,
	unsigned int driver_byte)
{
	/* inverse of macros in scsi.h */
	return (scsi_status_byte & 0xff) |
		((msg_byte & 0xff) << 8) |
		((host_byte & 0xff) << 16) |
		((driver_byte & 0xff) << 24);
}

static inline int evaluate_target_status(CommandList_struct *cmd)
{
	unsigned char sense_key;
	unsigned char status_byte, msg_byte, host_byte, driver_byte;
	int error_value;

	/* If we get in here, it means we got "target status", that is, scsi status */
	status_byte = cmd->err_info->ScsiStatus;
	driver_byte = DRIVER_OK;
	msg_byte = cmd->err_info->CommandStatus; /* correct?  seems too device specific */

	if (blk_pc_request(cmd->rq))
		host_byte = DID_PASSTHROUGH;
	else
		host_byte = DID_OK;

	error_value = make_status_bytes(status_byte, msg_byte,
		host_byte, driver_byte);

	if (cmd->err_info->ScsiStatus != SAM_STAT_CHECK_CONDITION) {
		if (!blk_pc_request(cmd->rq))
			printk(KERN_WARNING "cciss: cmd %p "
			       "has SCSI Status 0x%x\n",
			       cmd, cmd->err_info->ScsiStatus);
		return error_value;
	}

	/* check the sense key */
	sense_key = 0xf & cmd->err_info->SenseInfo[2];
	/* no status or recovered error */
	if (((sense_key == 0x0) || (sense_key == 0x1)) && !blk_pc_request(cmd->rq))
		error_value = 0;

	if (!blk_pc_request(cmd->rq)) { /* Not SG_IO or similar? */
		if (error_value != 0)
			printk(KERN_WARNING "cciss: cmd %p has CHECK CONDITION"
			       " sense key = 0x%x\n", cmd, sense_key);
		return error_value;
	}

	/* SG_IO or similar, copy sense data back */
	if (cmd->rq->sense) {
		if (cmd->rq->sense_len > cmd->err_info->SenseLen)
			cmd->rq->sense_len = cmd->err_info->SenseLen;
		memcpy(cmd->rq->sense, cmd->err_info->SenseInfo,
			cmd->rq->sense_len);
	} else
		cmd->rq->sense_len = 0;

	return error_value;
}

/* checks the status of the job and calls complete buffers to mark all
 * buffers for the completed job. Note that this function does not need
 * to hold the hba/queue lock.
 */
static inline void complete_command(ctlr_info_t *h, CommandList_struct *cmd,
				    int timeout)
{
	int retry_cmd = 0;
	struct request *rq = cmd->rq;

	rq->errors = 0;

	if (timeout)
		rq->errors = make_status_bytes(0, 0, 0, DRIVER_TIMEOUT);

	if (cmd->err_info->CommandStatus == 0)	/* no error has occurred */
		goto after_error_processing;

	switch (cmd->err_info->CommandStatus) {
	case CMD_TARGET_STATUS:
		rq->errors = evaluate_target_status(cmd);
		break;
	case CMD_DATA_UNDERRUN:
		if (blk_fs_request(cmd->rq)) {
			printk(KERN_WARNING "cciss: cmd %p has"
			       " completed with data underrun "
			       "reported\n", cmd);
			cmd->rq->data_len = cmd->err_info->ResidualCnt;
		}
		break;
	case CMD_DATA_OVERRUN:
		if (blk_fs_request(cmd->rq))
			printk(KERN_WARNING "cciss: cmd %p has"
			       " completed with data overrun "
			       "reported\n", cmd);
		break;
	case CMD_INVALID:
		printk(KERN_WARNING "cciss: cmd %p is "
		       "reported invalid\n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
		break;
	case CMD_PROTOCOL_ERR:
		printk(KERN_WARNING "cciss: cmd %p has "
		       "protocol error \n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
		break;
	case CMD_HARDWARE_ERR:
		printk(KERN_WARNING "cciss: cmd %p had "
		       " hardware error\n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
		break;
	case CMD_CONNECTION_LOST:
		printk(KERN_WARNING "cciss: cmd %p had "
		       "connection lost\n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
		break;
	case CMD_ABORTED:
		printk(KERN_WARNING "cciss: cmd %p was "
		       "aborted\n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ABORT);
		break;
	case CMD_ABORT_FAILED:
		printk(KERN_WARNING "cciss: cmd %p reports "
		       "abort failed\n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
		break;
	case CMD_UNSOLICITED_ABORT:
		printk(KERN_WARNING "cciss%d: unsolicited "
		       "abort %p\n", h->ctlr, cmd);
		if (cmd->retry_count < MAX_CMD_RETRIES) {
			retry_cmd = 1;
			printk(KERN_WARNING
			       "cciss%d: retrying %p\n", h->ctlr, cmd);
			cmd->retry_count++;
		} else
			printk(KERN_WARNING
			       "cciss%d: %p retried too "
			       "many times\n", h->ctlr, cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ABORT);
		break;
	case CMD_TIMEOUT:
		printk(KERN_WARNING "cciss: cmd %p timedout\n", cmd);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
		break;
	default:
		printk(KERN_WARNING "cciss: cmd %p returned "
		       "unknown status %x\n", cmd,
		       cmd->err_info->CommandStatus);
		rq->errors = make_status_bytes(SAM_STAT_GOOD,
			cmd->err_info->CommandStatus, DRIVER_OK,
			blk_pc_request(cmd->rq) ? DID_PASSTHROUGH : DID_ERROR);
	}

after_error_processing:

	/* We need to return this command */
	if (retry_cmd) {
		resend_cciss_cmd(h, cmd);
		return;
	}
	cmd->rq->completion_data = cmd;
	blk_complete_request(cmd->rq);
}

/*
 * Get a request and submit it to the controller.
 */
static void do_cciss_request(struct request_queue *q)
{
	ctlr_info_t *h = q->queuedata;
	CommandList_struct *c;
	sector_t start_blk;
	int seg;
	struct request *creq;
	u64bit temp64;
	struct scatterlist tmp_sg[MAXSGENTRIES];
	drive_info_struct *drv;
	int i, dir;

	/* We call start_io here in case there is a command waiting on the
	 * queue that has not been sent.
	 */
	if (blk_queue_plugged(q))
		goto startio;

      queue:
	creq = elv_next_request(q);
	if (!creq)
		goto startio;

	BUG_ON(creq->nr_phys_segments > MAXSGENTRIES);

	if ((c = cmd_alloc(h, 1)) == NULL)
		goto full;

	blkdev_dequeue_request(creq);

	spin_unlock_irq(q->queue_lock);

	c->cmd_type = CMD_RWREQ;
	c->rq = creq;

	/* fill in the request */
	drv = creq->rq_disk->private_data;
	c->Header.ReplyQueue = 0;	// unused in simple mode
	/* got command from pool, so use the command block index instead */
	/* for direct lookups. */
	/* The first 2 bits are reserved for controller error reporting. */
	c->Header.Tag.lower = (c->cmdindex << 3);
	c->Header.Tag.lower |= 0x04;	/* flag for direct lookup. */
	c->Header.LUN.LogDev.VolId = drv->LunID;
	c->Header.LUN.LogDev.Mode = 1;
	c->Request.CDBLen = 10;	// 12 byte commands not in FW yet;
	c->Request.Type.Type = TYPE_CMD;	// It is a command.
	c->Request.Type.Attribute = ATTR_SIMPLE;
	c->Request.Type.Direction =
	    (rq_data_dir(creq) == READ) ? XFER_READ : XFER_WRITE;
	c->Request.Timeout = 0;	// Don't time out
	c->Request.CDB[0] =
	    (rq_data_dir(creq) == READ) ? h->cciss_read : h->cciss_write;
	start_blk = creq->sector;
#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "ciss: sector =%d nr_sectors=%d\n", (int)creq->sector,
	       (int)creq->nr_sectors);
#endif				/* CCISS_DEBUG */

	sg_init_table(tmp_sg, MAXSGENTRIES);
	seg = blk_rq_map_sg(q, creq, tmp_sg);

	/* get the DMA records for the setup */
	if (c->Request.Type.Direction == XFER_READ)
		dir = PCI_DMA_FROMDEVICE;
	else
		dir = PCI_DMA_TODEVICE;

	for (i = 0; i < seg; i++) {
		c->SG[i].Len = tmp_sg[i].length;
		temp64.val = (__u64) pci_map_page(h->pdev, sg_page(&tmp_sg[i]),
						  tmp_sg[i].offset,
						  tmp_sg[i].length, dir);
		c->SG[i].Addr.lower = temp64.val32.lower;
		c->SG[i].Addr.upper = temp64.val32.upper;
		c->SG[i].Ext = 0;	// we are not chaining
	}
	/* track how many SG entries we are using */
	if (seg > h->maxSG)
		h->maxSG = seg;

#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "cciss: Submitting %d sectors in %d segments\n",
	       creq->nr_sectors, seg);
#endif				/* CCISS_DEBUG */

	c->Header.SGList = c->Header.SGTotal = seg;
	if (likely(blk_fs_request(creq))) {
		if(h->cciss_read == CCISS_READ_10) {
			c->Request.CDB[1] = 0;
			c->Request.CDB[2] = (start_blk >> 24) & 0xff;	//MSB
			c->Request.CDB[3] = (start_blk >> 16) & 0xff;
			c->Request.CDB[4] = (start_blk >> 8) & 0xff;
			c->Request.CDB[5] = start_blk & 0xff;
			c->Request.CDB[6] = 0;	// (sect >> 24) & 0xff; MSB
			c->Request.CDB[7] = (creq->nr_sectors >> 8) & 0xff;
			c->Request.CDB[8] = creq->nr_sectors & 0xff;
			c->Request.CDB[9] = c->Request.CDB[11] = c->Request.CDB[12] = 0;
		} else {
			u32 upper32 = upper_32_bits(start_blk);

			c->Request.CDBLen = 16;
			c->Request.CDB[1]= 0;
			c->Request.CDB[2]= (upper32 >> 24) & 0xff;	//MSB
			c->Request.CDB[3]= (upper32 >> 16) & 0xff;
			c->Request.CDB[4]= (upper32 >>  8) & 0xff;
			c->Request.CDB[5]= upper32 & 0xff;
			c->Request.CDB[6]= (start_blk >> 24) & 0xff;
			c->Request.CDB[7]= (start_blk >> 16) & 0xff;
			c->Request.CDB[8]= (start_blk >>  8) & 0xff;
			c->Request.CDB[9]= start_blk & 0xff;
			c->Request.CDB[10]= (creq->nr_sectors >>  24) & 0xff;
			c->Request.CDB[11]= (creq->nr_sectors >>  16) & 0xff;
			c->Request.CDB[12]= (creq->nr_sectors >>  8) & 0xff;
			c->Request.CDB[13]= creq->nr_sectors & 0xff;
			c->Request.CDB[14] = c->Request.CDB[15] = 0;
		}
	} else if (blk_pc_request(creq)) {
		c->Request.CDBLen = creq->cmd_len;
		memcpy(c->Request.CDB, creq->cmd, BLK_MAX_CDB);
	} else {
		printk(KERN_WARNING "cciss%d: bad request type %d\n", h->ctlr, creq->cmd_type);
		BUG();
	}

	spin_lock_irq(q->queue_lock);

	addQ(&(h->reqQ), c);
	h->Qdepth++;
	if (h->Qdepth > h->maxQsinceinit)
		h->maxQsinceinit = h->Qdepth;

	goto queue;
full:
	blk_stop_queue(q);
startio:
	/* We will already have the driver lock here so not need
	 * to lock it.
	 */
	start_io(h);
}

static inline unsigned long get_next_completion(ctlr_info_t *h)
{
#ifdef CONFIG_CISS_SCSI_TAPE
	/* Any rejects from sendcmd() lying around? Process them first */
	if (h->scsi_rejects.ncompletions == 0)
		return h->access.command_completed(h);
	else {
		struct sendcmd_reject_list *srl;
		int n;
		srl = &h->scsi_rejects;
		n = --srl->ncompletions;
		/* printk("cciss%d: processing saved reject\n", h->ctlr); */
		printk("p");
		return srl->complete[n];
	}
#else
	return h->access.command_completed(h);
#endif
}

static inline int interrupt_pending(ctlr_info_t *h)
{
#ifdef CONFIG_CISS_SCSI_TAPE
	return (h->access.intr_pending(h)
		|| (h->scsi_rejects.ncompletions > 0));
#else
	return h->access.intr_pending(h);
#endif
}

static inline long interrupt_not_for_us(ctlr_info_t *h)
{
#ifdef CONFIG_CISS_SCSI_TAPE
	return (((h->access.intr_pending(h) == 0) ||
		 (h->interrupts_enabled == 0))
		&& (h->scsi_rejects.ncompletions == 0));
#else
	return (((h->access.intr_pending(h) == 0) ||
		 (h->interrupts_enabled == 0)));
#endif
}

static irqreturn_t do_cciss_intr(int irq, void *dev_id)
{
	ctlr_info_t *h = dev_id;
	CommandList_struct *c;
	unsigned long flags;
	__u32 a, a1, a2;

	if (interrupt_not_for_us(h))
		return IRQ_NONE;
	/*
	 * If there are completed commands in the completion queue,
	 * we had better do something about it.
	 */
	spin_lock_irqsave(CCISS_LOCK(h->ctlr), flags);
	while (interrupt_pending(h)) {
		while ((a = get_next_completion(h)) != FIFO_EMPTY) {
			a1 = a;
			if ((a & 0x04)) {
				a2 = (a >> 3);
				if (a2 >= h->nr_cmds) {
					printk(KERN_WARNING
					       "cciss: controller cciss%d failed, stopping.\n",
					       h->ctlr);
					fail_all_cmds(h->ctlr);
					return IRQ_HANDLED;
				}

				c = h->cmd_pool + a2;
				a = c->busaddr;

			} else {
				a &= ~3;
				if ((c = h->cmpQ) == NULL) {
					printk(KERN_WARNING
					       "cciss: Completion of %08x ignored\n",
					       a1);
					continue;
				}
				while (c->busaddr != a) {
					c = c->next;
					if (c == h->cmpQ)
						break;
				}
			}
			/*
			 * If we've found the command, take it off the
			 * completion Q and free it
			 */
			if (c->busaddr == a) {
				removeQ(&h->cmpQ, c);
				if (c->cmd_type == CMD_RWREQ) {
					complete_command(h, c, 0);
				} else if (c->cmd_type == CMD_IOCTL_PEND) {
					complete(c->waiting);
				}
#				ifdef CONFIG_CISS_SCSI_TAPE
				else if (c->cmd_type == CMD_SCSI)
					complete_scsi_command(c, 0, a1);
#				endif
				continue;
			}
		}
	}

	spin_unlock_irqrestore(CCISS_LOCK(h->ctlr), flags);
	return IRQ_HANDLED;
}

/*
 *  We cannot read the structure directly, for portability we must use
 *   the io functions.
 *   This is for debug only.
 */
#ifdef CCISS_DEBUG
static void print_cfg_table(CfgTable_struct *tb)
{
	int i;
	char temp_name[17];

	printk("Controller Configuration information\n");
	printk("------------------------------------\n");
	for (i = 0; i < 4; i++)
		temp_name[i] = readb(&(tb->Signature[i]));
	temp_name[4] = '\0';
	printk("   Signature = %s\n", temp_name);
	printk("   Spec Number = %d\n", readl(&(tb->SpecValence)));
	printk("   Transport methods supported = 0x%x\n",
	       readl(&(tb->TransportSupport)));
	printk("   Transport methods active = 0x%x\n",
	       readl(&(tb->TransportActive)));
	printk("   Requested transport Method = 0x%x\n",
	       readl(&(tb->HostWrite.TransportRequest)));
	printk("   Coalesce Interrupt Delay = 0x%x\n",
	       readl(&(tb->HostWrite.CoalIntDelay)));
	printk("   Coalesce Interrupt Count = 0x%x\n",
	       readl(&(tb->HostWrite.CoalIntCount)));
	printk("   Max outstanding commands = 0x%d\n",
	       readl(&(tb->CmdsOutMax)));
	printk("   Bus Types = 0x%x\n", readl(&(tb->BusTypes)));
	for (i = 0; i < 16; i++)
		temp_name[i] = readb(&(tb->ServerName[i]));
	temp_name[16] = '\0';
	printk("   Server Name = %s\n", temp_name);
	printk("   Heartbeat Counter = 0x%x\n\n\n", readl(&(tb->HeartBeat)));
}
#endif				/* CCISS_DEBUG */

static int find_PCI_BAR_index(struct pci_dev *pdev, unsigned long pci_bar_addr)
{
	int i, offset, mem_type, bar_type;
	if (pci_bar_addr == PCI_BASE_ADDRESS_0)	/* looking for BAR zero? */
		return 0;
	offset = 0;
	for (i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
		bar_type = pci_resource_flags(pdev, i) & PCI_BASE_ADDRESS_SPACE;
		if (bar_type == PCI_BASE_ADDRESS_SPACE_IO)
			offset += 4;
		else {
			mem_type = pci_resource_flags(pdev, i) &
			    PCI_BASE_ADDRESS_MEM_TYPE_MASK;
			switch (mem_type) {
			case PCI_BASE_ADDRESS_MEM_TYPE_32:
			case PCI_BASE_ADDRESS_MEM_TYPE_1M:
				offset += 4;	/* 32 bit */
				break;
			case PCI_BASE_ADDRESS_MEM_TYPE_64:
				offset += 8;
				break;
			default:	/* reserved in PCI 2.2 */
				printk(KERN_WARNING
				       "Base address is invalid\n");
				return -1;
				break;
			}
		}
		if (offset == pci_bar_addr - PCI_BASE_ADDRESS_0)
			return i + 1;
	}
	return -1;
}

/* If MSI/MSI-X is supported by the kernel we will try to enable it on
 * controllers that are capable. If not, we use IO-APIC mode.
 */

static void __devinit cciss_interrupt_mode(ctlr_info_t *c,
					   struct pci_dev *pdev, __u32 board_id)
{
#ifdef CONFIG_PCI_MSI
	int err;
	struct msix_entry cciss_msix_entries[4] = { {0, 0}, {0, 1},
	{0, 2}, {0, 3}
	};

	/* Some boards advertise MSI but don't really support it */
	if ((board_id == 0x40700E11) ||
	    (board_id == 0x40800E11) ||
	    (board_id == 0x40820E11) || (board_id == 0x40830E11))
		goto default_int_mode;

	if (pci_find_capability(pdev, PCI_CAP_ID_MSIX)) {
		err = pci_enable_msix(pdev, cciss_msix_entries, 4);
		if (!err) {
			c->intr[0] = cciss_msix_entries[0].vector;
			c->intr[1] = cciss_msix_entries[1].vector;
			c->intr[2] = cciss_msix_entries[2].vector;
			c->intr[3] = cciss_msix_entries[3].vector;
			c->msix_vector = 1;
			return;
		}
		if (err > 0) {
			printk(KERN_WARNING "cciss: only %d MSI-X vectors "
			       "available\n", err);
			goto default_int_mode;
		} else {
			printk(KERN_WARNING "cciss: MSI-X init failed %d\n",
			       err);
			goto default_int_mode;
		}
	}
	if (pci_find_capability(pdev, PCI_CAP_ID_MSI)) {
		if (!pci_enable_msi(pdev)) {
			c->msi_vector = 1;
		} else {
			printk(KERN_WARNING "cciss: MSI init failed\n");
		}
	}
default_int_mode:
#endif				/* CONFIG_PCI_MSI */
	/* if we get here we're going to use the default interrupt mode */
	c->intr[SIMPLE_MODE_INT] = pdev->irq;
	return;
}

static int __devinit cciss_pci_init(ctlr_info_t *c, struct pci_dev *pdev)
{
	ushort subsystem_vendor_id, subsystem_device_id, command;
	__u32 board_id, scratchpad = 0;
	__u64 cfg_offset;
	__u32 cfg_base_addr;
	__u64 cfg_base_addr_index;
	int i, err;

	/* check to see if controller has been disabled */
	/* BEFORE trying to enable it */
	(void)pci_read_config_word(pdev, PCI_COMMAND, &command);
	if (!(command & 0x02)) {
		printk(KERN_WARNING
		       "cciss: controller appears to be disabled\n");
		return -ENODEV;
	}

	err = pci_enable_device(pdev);
	if (err) {
		printk(KERN_ERR "cciss: Unable to Enable PCI device\n");
		return err;
	}

	err = pci_request_regions(pdev, "cciss");
	if (err) {
		printk(KERN_ERR "cciss: Cannot obtain PCI resources, "
		       "aborting\n");
		return err;
	}

	subsystem_vendor_id = pdev->subsystem_vendor;
	subsystem_device_id = pdev->subsystem_device;
	board_id = (((__u32) (subsystem_device_id << 16) & 0xffff0000) |
		    subsystem_vendor_id);

#ifdef CCISS_DEBUG
	printk("command = %x\n", command);
	printk("irq = %x\n", pdev->irq);
	printk("board_id = %x\n", board_id);
#endif				/* CCISS_DEBUG */

/* If the kernel supports MSI/MSI-X we will try to enable that functionality,
 * else we use the IO-APIC interrupt assigned to us by system ROM.
 */
	cciss_interrupt_mode(c, pdev, board_id);

	/*
	 * Memory base addr is first addr , the second points to the config
	 *   table
	 */

	c->paddr = pci_resource_start(pdev, 0);	/* addressing mode bits already removed */
#ifdef CCISS_DEBUG
	printk("address 0 = %x\n", c->paddr);
#endif				/* CCISS_DEBUG */
	c->vaddr = remap_pci_mem(c->paddr, 0x250);

	/* Wait for the board to become ready.  (PCI hotplug needs this.)
	 * We poll for up to 120 secs, once per 100ms. */
	for (i = 0; i < 1200; i++) {
		scratchpad = readl(c->vaddr + SA5_SCRATCHPAD_OFFSET);
		if (scratchpad == CCISS_FIRMWARE_READY)
			break;
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 10);	/* wait 100ms */
	}
	if (scratchpad != CCISS_FIRMWARE_READY) {
		printk(KERN_WARNING "cciss: Board not ready.  Timed out.\n");
		err = -ENODEV;
		goto err_out_free_res;
	}

	/* get the address index number */
	cfg_base_addr = readl(c->vaddr + SA5_CTCFG_OFFSET);
	cfg_base_addr &= (__u32) 0x0000ffff;
#ifdef CCISS_DEBUG
	printk("cfg base address = %x\n", cfg_base_addr);
#endif				/* CCISS_DEBUG */
	cfg_base_addr_index = find_PCI_BAR_index(pdev, cfg_base_addr);
#ifdef CCISS_DEBUG
	printk("cfg base address index = %x\n", cfg_base_addr_index);
#endif				/* CCISS_DEBUG */
	if (cfg_base_addr_index == -1) {
		printk(KERN_WARNING "cciss: Cannot find cfg_base_addr_index\n");
		err = -ENODEV;
		goto err_out_free_res;
	}

	cfg_offset = readl(c->vaddr + SA5_CTMEM_OFFSET);
#ifdef CCISS_DEBUG
	printk("cfg offset = %x\n", cfg_offset);
#endif				/* CCISS_DEBUG */
	c->cfgtable = remap_pci_mem(pci_resource_start(pdev,
						       cfg_base_addr_index) +
				    cfg_offset, sizeof(CfgTable_struct));
	c->board_id = board_id;

#ifdef CCISS_DEBUG
	print_cfg_table(c->cfgtable);
#endif				/* CCISS_DEBUG */

	/* Some controllers support Zero Memory Raid (ZMR).
	 * When configured in ZMR mode the number of supported
	 * commands drops to 64. So instead of just setting an
	 * arbitrary value we make the driver a little smarter.
	 * We read the config table to tell us how many commands
	 * are supported on the controller then subtract 4 to
	 * leave a little room for ioctl calls.
	 */
	c->max_commands = readl(&(c->cfgtable->CmdsOutMax));
	for (i = 0; i < ARRAY_SIZE(products); i++) {
		if (board_id == products[i].board_id) {
			c->product_name = products[i].product_name;
			c->access = *(products[i].access);
			c->nr_cmds = c->max_commands - 4;
			break;
		}
	}
	if ((readb(&c->cfgtable->Signature[0]) != 'C') ||
	    (readb(&c->cfgtable->Signature[1]) != 'I') ||
	    (readb(&c->cfgtable->Signature[2]) != 'S') ||
	    (readb(&c->cfgtable->Signature[3]) != 'S')) {
		printk("Does not appear to be a valid CISS config table\n");
		err = -ENODEV;
		goto err_out_free_res;
	}
	/* We didn't find the controller in our list. We know the
	 * signature is valid. If it's an HP device let's try to
	 * bind to the device and fire it up. Otherwise we bail.
	 */
	if (i == ARRAY_SIZE(products)) {
		if (subsystem_vendor_id == PCI_VENDOR_ID_HP) {
			c->product_name = products[i-1].product_name;
			c->access = *(products[i-1].access);
			c->nr_cmds = c->max_commands - 4;
			printk(KERN_WARNING "cciss: This is an unknown "
				"Smart Array controller.\n"
				"cciss: Please update to the latest driver "
				"available from www.hp.com.\n");
		} else {
			printk(KERN_WARNING "cciss: Sorry, I don't know how"
				" to access the Smart Array controller %08lx\n"
					, (unsigned long)board_id);
			err = -ENODEV;
			goto err_out_free_res;
		}
	}
#ifdef CONFIG_X86
	{
		/* Need to enable prefetch in the SCSI core for 6400 in x86 */
		__u32 prefetch;
		prefetch = readl(&(c->cfgtable->SCSI_Prefetch));
		prefetch |= 0x100;
		writel(prefetch, &(c->cfgtable->SCSI_Prefetch));
	}
#endif

	/* Disabling DMA prefetch and refetch for the P600.
	 * An ASIC bug may result in accesses to invalid memory addresses.
	 * We've disabled prefetch for some time now. Testing with XEN
	 * kernels revealed a bug in the refetch if dom0 resides on a P600.
	 */
	if(board_id == 0x3225103C) {
		__u32 dma_prefetch;
		__u32 dma_refetch;
		dma_prefetch = readl(c->vaddr + I2O_DMA1_CFG);
		dma_prefetch |= 0x8000;
		writel(dma_prefetch, c->vaddr + I2O_DMA1_CFG);
		pci_read_config_dword(pdev, PCI_COMMAND_PARITY, &dma_refetch);
		dma_refetch |= 0x1;
		pci_write_config_dword(pdev, PCI_COMMAND_PARITY, dma_refetch);
	}

#ifdef CCISS_DEBUG
	printk("Trying to put board into Simple mode\n");
#endif				/* CCISS_DEBUG */
	c->max_commands = readl(&(c->cfgtable->CmdsOutMax));
	/* Update the field, and then ring the doorbell */
	writel(CFGTBL_Trans_Simple, &(c->cfgtable->HostWrite.TransportRequest));
	writel(CFGTBL_ChangeReq, c->vaddr + SA5_DOORBELL);

	/* under certain very rare conditions, this can take awhile.
	 * (e.g.: hot replace a failed 144GB drive in a RAID 5 set right
	 * as we enter this code.) */
	for (i = 0; i < MAX_CONFIG_WAIT; i++) {
		if (!(readl(c->vaddr + SA5_DOORBELL) & CFGTBL_ChangeReq))
			break;
		/* delay and try again */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(10);
	}

#ifdef CCISS_DEBUG
	printk(KERN_DEBUG "I counter got to %d %x\n", i,
	       readl(c->vaddr + SA5_DOORBELL));
#endif				/* CCISS_DEBUG */
#ifdef CCISS_DEBUG
	print_cfg_table(c->cfgtable);
#endif				/* CCISS_DEBUG */

	if (!(readl(&(c->cfgtable->TransportActive)) & CFGTBL_Trans_Simple)) {
		printk(KERN_WARNING "cciss: unable to get board into"
		       " simple mode\n");
		err = -ENODEV;
		goto err_out_free_res;
	}
	return 0;

err_out_free_res:
	/*
	 * Deliberately omit pci_disable_device(): it does something nasty to
	 * Smart Array controllers that pci_enable_device does not undo
	 */
	pci_release_regions(pdev);
	return err;
}

/* Function to find the first free pointer into our hba[] array
 * Returns -1 if no free entries are left.
 */
static int alloc_cciss_hba(void)
{
	int i;

	for (i = 0; i < MAX_CTLR; i++) {
		if (!hba[i]) {
			ctlr_info_t *p;

			p = kzalloc(sizeof(ctlr_info_t), GFP_KERNEL);
			if (!p)
				goto Enomem;
			hba[i] = p;
			return i;
		}
	}
	printk(KERN_WARNING "cciss: This driver supports a maximum"
	       " of %d controllers.\n", MAX_CTLR);
	return -1;
Enomem:
	printk(KERN_ERR "cciss: out of memory.\n");
	return -1;
}

static void free_hba(int i)
{
	ctlr_info_t *p = hba[i];
	int n;

	hba[i] = NULL;
	for (n = 0; n < CISS_MAX_LUN; n++)
		put_disk(p->gendisk[n]);
	kfree(p);
}

/*
 *  This is it.  Find all the controllers and register them.  I really hate
 *  stealing all these major device numbers.
 *  returns the number of block devices registered.
 */
static int __devinit cciss_init_one(struct pci_dev *pdev,
				    const struct pci_device_id *ent)
{
	int i;
	int j = 0;
	int rc;
	int dac, return_code;
	InquiryData_struct *inq_buff = NULL;

	i = alloc_cciss_hba();
	if (i < 0)
		return -1;

	hba[i]->busy_initializing = 1;

	if (cciss_pci_init(hba[i], pdev) != 0)
		goto clean1;

	sprintf(hba[i]->devname, "cciss%d", i);
	hba[i]->ctlr = i;
	hba[i]->pdev = pdev;

	/* configure PCI DMA stuff */
	if (!pci_set_dma_mask(pdev, DMA_64BIT_MASK))
		dac = 1;
	else if (!pci_set_dma_mask(pdev, DMA_32BIT_MASK))
		dac = 0;
	else {
		printk(KERN_ERR "cciss: no suitable DMA available\n");
		goto clean1;
	}

	/*
	 * register with the major number, or get a dynamic major number
	 * by passing 0 as argument.  This is done for greater than
	 * 8 controller support.
	 */
	if (i < MAX_CTLR_ORIG)
		hba[i]->major = COMPAQ_CISS_MAJOR + i;
	rc = register_blkdev(hba[i]->major, hba[i]->devname);
	if (rc == -EBUSY || rc == -EINVAL) {
		printk(KERN_ERR
		       "cciss:  Unable to get major number %d for %s "
		       "on hba %d\n", hba[i]->major, hba[i]->devname, i);
		goto clean1;
	} else {
		if (i >= MAX_CTLR_ORIG)
			hba[i]->major = rc;
	}

	/* make sure the board interrupts are off */
	hba[i]->access.set_intr_mask(hba[i], CCISS_INTR_OFF);
	if (request_irq(hba[i]->intr[SIMPLE_MODE_INT], do_cciss_intr,
			IRQF_DISABLED | IRQF_SHARED, hba[i]->devname, hba[i])) {
		printk(KERN_ERR "cciss: Unable to get irq %d for %s\n",
		       hba[i]->intr[SIMPLE_MODE_INT], hba[i]->devname);
		goto clean2;
	}

	printk(KERN_INFO "%s: <0x%x> at PCI %s IRQ %d%s using DAC\n",
	       hba[i]->devname, pdev->device, pci_name(pdev),
	       hba[i]->intr[SIMPLE_MODE_INT], dac ? "" : " not");

	hba[i]->cmd_pool_bits =
	    kmalloc(((hba[i]->nr_cmds + BITS_PER_LONG -
		      1) / BITS_PER_LONG) * sizeof(unsigned long), GFP_KERNEL);
	hba[i]->cmd_pool = (CommandList_struct *)
	    pci_alloc_consistent(hba[i]->pdev,
		    hba[i]->nr_cmds * sizeof(CommandList_struct),
		    &(hba[i]->cmd_pool_dhandle));
	hba[i]->errinfo_pool = (ErrorInfo_struct *)
	    pci_alloc_consistent(hba[i]->pdev,
		    hba[i]->nr_cmds * sizeof(ErrorInfo_struct),
		    &(hba[i]->errinfo_pool_dhandle));
	if ((hba[i]->cmd_pool_bits == NULL)
	    || (hba[i]->cmd_pool == NULL)
	    || (hba[i]->errinfo_pool == NULL)) {
		printk(KERN_ERR "cciss: out of memory");
		goto clean4;
	}
#ifdef CONFIG_CISS_SCSI_TAPE
	hba[i]->scsi_rejects.complete =
	    kmalloc(sizeof(hba[i]->scsi_rejects.complete[0]) *
		    (hba[i]->nr_cmds + 5), GFP_KERNEL);
	if (hba[i]->scsi_rejects.complete == NULL) {
		printk(KERN_ERR "cciss: out of memory");
		goto clean4;
	}
#endif
	spin_lock_init(&hba[i]->lock);

	/* Initialize the pdev driver private data.
	   have it point to hba[i].  */
	pci_set_drvdata(pdev, hba[i]);
	/* command and error info recs zeroed out before
	   they are used */
	memset(hba[i]->cmd_pool_bits, 0,
	       ((hba[i]->nr_cmds + BITS_PER_LONG -
		 1) / BITS_PER_LONG) * sizeof(unsigned long));

	hba[i]->num_luns = 0;
	hba[i]->highest_lun = -1;
	for (j = 0; j < CISS_MAX_LUN; j++) {
		hba[i]->drv[j].raid_level = -1;
		hba[i]->drv[j].queue = NULL;
		hba[i]->gendisk[j] = NULL;
	}

	cciss_scsi_setup(i);

	/* Turn the interrupts on so we can service requests */
	hba[i]->access.set_intr_mask(hba[i], CCISS_INTR_ON);

	/* Get the firmware version */
	inq_buff = kzalloc(sizeof(InquiryData_struct), GFP_KERNEL);
	if (inq_buff == NULL) {
		printk(KERN_ERR "cciss: out of memory\n");
		goto clean4;
	}

	return_code = sendcmd_withirq(CISS_INQUIRY, i, inq_buff,
		sizeof(InquiryData_struct), 0, 0 , 0, TYPE_CMD);
	if (return_code == IO_OK) {
		hba[i]->firm_ver[0] = inq_buff->data_byte[32];
		hba[i]->firm_ver[1] = inq_buff->data_byte[33];
		hba[i]->firm_ver[2] = inq_buff->data_byte[34];
		hba[i]->firm_ver[3] = inq_buff->data_byte[35];
	} else {	 /* send command failed */
		printk(KERN_WARNING "cciss: unable to determine firmware"
			" version of controller\n");
	}

	cciss_procinit(i);

	hba[i]->cciss_max_sectors = 2048;

	hba[i]->busy_initializing = 0;

	rebuild_lun_table(hba[i], 1);
	return 1;

clean4:
	kfree(inq_buff);
#ifdef CONFIG_CISS_SCSI_TAPE
	kfree(hba[i]->scsi_rejects.complete);
#endif
	kfree(hba[i]->cmd_pool_bits);
	if (hba[i]->cmd_pool)
		pci_free_consistent(hba[i]->pdev,
				    hba[i]->nr_cmds * sizeof(CommandList_struct),
				    hba[i]->cmd_pool, hba[i]->cmd_pool_dhandle);
	if (hba[i]->errinfo_pool)
		pci_free_consistent(hba[i]->pdev,
				    hba[i]->nr_cmds * sizeof(ErrorInfo_struct),
				    hba[i]->errinfo_pool,
				    hba[i]->errinfo_pool_dhandle);
	free_irq(hba[i]->intr[SIMPLE_MODE_INT], hba[i]);
clean2:
	unregister_blkdev(hba[i]->major, hba[i]->devname);
clean1:
	hba[i]->busy_initializing = 0;
	/* cleanup any queues that may have been initialized */
	for (j=0; j <= hba[i]->highest_lun; j++){
		drive_info_struct *drv = &(hba[i]->drv[j]);
		if (drv->queue)
			blk_cleanup_queue(drv->queue);
	}
	/*
	 * Deliberately omit pci_disable_device(): it does something nasty to
	 * Smart Array controllers that pci_enable_device does not undo
	 */
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);
	free_hba(i);
	return -1;
}

static void cciss_shutdown(struct pci_dev *pdev)
{
	ctlr_info_t *tmp_ptr;
	int i;
	char flush_buf[4];
	int return_code;

	tmp_ptr = pci_get_drvdata(pdev);
	if (tmp_ptr == NULL)
		return;
	i = tmp_ptr->ctlr;
	if (hba[i] == NULL)
		return;

	/* Turn board interrupts off  and send the flush cache command */
	/* sendcmd will turn off interrupt, and send the flush...
	 * To write all data in the battery backed cache to disks */
	memset(flush_buf, 0, 4);
	return_code = sendcmd(CCISS_CACHE_FLUSH, i, flush_buf, 4, 0, 0, 0, NULL,
			      TYPE_CMD);
	if (return_code == IO_OK) {
		printk(KERN_INFO "Completed flushing cache on controller %d\n", i);
	} else {
		printk(KERN_WARNING "Error flushing cache on controller %d\n", i);
	}
	free_irq(hba[i]->intr[2], hba[i]);
}

static void __devexit cciss_remove_one(struct pci_dev *pdev)
{
	ctlr_info_t *tmp_ptr;
	int i, j;

	if (pci_get_drvdata(pdev) == NULL) {
		printk(KERN_ERR "cciss: Unable to remove device \n");
		return;
	}
	tmp_ptr = pci_get_drvdata(pdev);
	i = tmp_ptr->ctlr;
	if (hba[i] == NULL) {
		printk(KERN_ERR "cciss: device appears to "
		       "already be removed \n");
		return;
	}

	remove_proc_entry(hba[i]->devname, proc_cciss);
	unregister_blkdev(hba[i]->major, hba[i]->devname);

	/* remove it from the disk list */
	for (j = 0; j < CISS_MAX_LUN; j++) {
		struct gendisk *disk = hba[i]->gendisk[j];
		if (disk) {
			struct request_queue *q = disk->queue;

			if (disk->flags & GENHD_FL_UP)
				del_gendisk(disk);
			if (q)
				blk_cleanup_queue(q);
		}
	}

#ifdef CONFIG_CISS_SCSI_TAPE
	cciss_unregister_scsi(i);	/* unhook from SCSI subsystem */
#endif

	cciss_shutdown(pdev);

#ifdef CONFIG_PCI_MSI
	if (hba[i]->msix_vector)
		pci_disable_msix(hba[i]->pdev);
	else if (hba[i]->msi_vector)
		pci_disable_msi(hba[i]->pdev);
#endif				/* CONFIG_PCI_MSI */

	iounmap(hba[i]->vaddr);

	pci_free_consistent(hba[i]->pdev, hba[i]->nr_cmds * sizeof(CommandList_struct),
			    hba[i]->cmd_pool, hba[i]->cmd_pool_dhandle);
	pci_free_consistent(hba[i]->pdev, hba[i]->nr_cmds * sizeof(ErrorInfo_struct),
			    hba[i]->errinfo_pool, hba[i]->errinfo_pool_dhandle);
	kfree(hba[i]->cmd_pool_bits);
#ifdef CONFIG_CISS_SCSI_TAPE
	kfree(hba[i]->scsi_rejects.complete);
#endif
	/*
	 * Deliberately omit pci_disable_device(): it does something nasty to
	 * Smart Array controllers that pci_enable_device does not undo
	 */
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);
	free_hba(i);
}

static struct pci_driver cciss_pci_driver = {
	.name = "cciss",
	.probe = cciss_init_one,
	.remove = __devexit_p(cciss_remove_one),
	.id_table = cciss_pci_device_id,	/* id_table */
	.shutdown = cciss_shutdown,
};

/*
 *  This is it.  Register the PCI driver information for the cards we control
 *  the OS will call our registered routines when it finds one of our cards.
 */
static int __init cciss_init(void)
{
	printk(KERN_INFO DRIVER_NAME "\n");

	/* Register for our PCI devices */
	return pci_register_driver(&cciss_pci_driver);
}

static void __exit cciss_cleanup(void)
{
	int i;

	pci_unregister_driver(&cciss_pci_driver);
	/* double check that all controller entrys have been removed */
	for (i = 0; i < MAX_CTLR; i++) {
		if (hba[i] != NULL) {
			printk(KERN_WARNING "cciss: had to remove"
			       " controller %d\n", i);
			cciss_remove_one(hba[i]->pdev);
		}
	}
	remove_proc_entry("driver/cciss", NULL);
}

static void fail_all_cmds(unsigned long ctlr)
{
	/* If we get here, the board is apparently dead. */
	ctlr_info_t *h = hba[ctlr];
	CommandList_struct *c;
	unsigned long flags;

	printk(KERN_WARNING "cciss%d: controller not responding.\n", h->ctlr);
	h->alive = 0;		/* the controller apparently died... */

	spin_lock_irqsave(CCISS_LOCK(ctlr), flags);

	pci_disable_device(h->pdev);	/* Make sure it is really dead. */

	/* move everything off the request queue onto the completed queue */
	while ((c = h->reqQ) != NULL) {
		removeQ(&(h->reqQ), c);
		h->Qdepth--;
		addQ(&(h->cmpQ), c);
	}

	/* Now, fail everything on the completed queue with a HW error */
	while ((c = h->cmpQ) != NULL) {
		removeQ(&h->cmpQ, c);
		c->err_info->CommandStatus = CMD_HARDWARE_ERR;
		if (c->cmd_type == CMD_RWREQ) {
			complete_command(h, c, 0);
		} else if (c->cmd_type == CMD_IOCTL_PEND)
			complete(c->waiting);
#ifdef CONFIG_CISS_SCSI_TAPE
		else if (c->cmd_type == CMD_SCSI)
			complete_scsi_command(c, 0, 0);
#endif
	}
	spin_unlock_irqrestore(CCISS_LOCK(ctlr), flags);
	return;
}

module_init(cciss_init);
module_exit(cciss_cleanup);
