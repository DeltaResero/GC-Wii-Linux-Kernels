/*
 * drivers/block/gcn-di/gcn-di.c
 *
 * Nintendo GameCube DVD Interface driver
 * Copyright (C) 2005 The GameCube Linux Team
 * Copyright (C) 2005 Albert Herranz
 *
 * Portions based on previous work by Scream|CT.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/blkdev.h>
#include <linux/fcntl.h>
#include <linux/hdreg.h>
#include <linux/cdrom.h>

#include <asm/io.h>

#define DI_DEBUG

#define DRV_MODULE_NAME	"gcn-di"
#define DRV_DESCRIPTION	"Nintendo GameCube DVD Interface driver"
#define DRV_AUTHOR	"Albert Herranz"

static char di_driver_version[] = "0.4";

#define di_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

#ifdef DI_DEBUG
#  define DBG(fmt, args...) \
          printk(KERN_ERR "%s: " fmt, __FUNCTION__ , ## args)
#else
#  define DBG(fmt, args...)
#endif


/*
 * Hardware.
 */
#define DI_IRQ			2

#define DI_DMA_ALIGN		0x1f /* 32 bytes */

#define DI_BASE			0xcc006000
#define DI_SIZE			0x40

#define DI_IO_BASE		((void __iomem *)DI_BASE)

/* DI Status Register */
#define DI_SR			0x00
#define  DI_SR_BRK		(1<<0)
#define  DI_SR_DEINTMASK	(1<<1)
#define  DI_SR_DEINT		(1<<2)
#define  DI_SR_TCINTMASK	(1<<3)
#define  DI_SR_TCINT		(1<<4)
#define  DI_SR_BRKINTMASK	(1<<5)
#define  DI_SR_BRKINT		(1<<6)

/* DI Cover Register */
#define DI_CVR			0x04
#define  DI_CVR_CVR		(1<<0)
#define  DI_CVR_CVRINTMASK	(1<<1)
#define  DI_CVR_CVRINT		(1<<2)

/* DI Command Buffers */
#define DI_CMDBUF0		0x08
#define DI_CMDBUF1		0x0c
#define DI_CMDBUF2		0x10

/* DI DMA Memory Address Register */
#define DI_MAR			0x14

/* DI DMA Transfer Length Register */
#define DI_LENGTH		0x18

/* DI Control Register */
#define DI_CR			0x1c
#define  DI_CR_TSTART		(1<<0)
#define  DI_CR_DMA		(1<<1)
#define  DI_CR_RW		(1<<2)

/* DI Immediate Data Buffer */
#define DI_DATA			0x20

/* DI Configuration Register */
#define DI_CFG			0x24


/* drive status, status */
#define DI_STATUS(s)		((u8)((s)>>24))

#define DI_STATUS_READY			0x00
#define DI_STATUS_COVER_OPENED		0x01
#define DI_STATUS_DISK_CHANGE		0x02
#define DI_STATUS_NO_DISK		0x03
#define DI_STATUS_MOTOR_STOP		0x04
#define DI_STATUS_DISK_ID_NOT_READ	0x05

/* drive status, error */
#define DI_ERROR(s)		((u32)((s)&0x00ffffff))

#define DI_ERROR_NO_ERROR		0x000000
#define DI_ERROR_MOTOR_STOPPED		0x020400
#define DI_ERROR_DISK_ID_NOT_READ	0x020401
#define DI_ERROR_MEDIUM_NOT_PRESENT	0x023a00
#define DI_ERROR_SEEK_INCOMPLETE	0x030200
#define DI_ERROR_UNRECOVERABLE_READ	0x031100
#define DI_ERROR_INVALID_COMMAND	0x052000
#define DI_ERROR_BLOCK_OUT_OF_RANGE	0x052100
#define DI_ERROR_INVALID_FIELD		0x052400
#define DI_ERROR_MEDIUM_CHANGED		0x062800

#define di_may_retry(s)		((DI_STATUS(s) == DI_STATUS_READY || \
				  DI_STATUS(s) == DI_STATUS_DISK_ID_NOT_READ) \
				 && \
				 (DI_ERROR(s) != DI_ERROR_SEEK_INCOMPLETE))

/* DI Sector Size */
#define DI_SECTOR_SHIFT		11
#define DI_SECTOR_SIZE		(1 << DI_SECTOR_SHIFT) /*2048*/
#define DI_MAX_SECTORS		712880


/* Driver Settings */
#define DI_NAME			"di"
#define DI_MAJOR		60

#define DI_COMMAND_TIMEOUT	20 /* seconds */
#define DI_COMMAND_RETRIES	10 /* times */

#define DI_MOTOR_OFF_TIMEOUT	10

#define KERNEL_SECTOR_SHIFT	9
#define KERNEL_SECTOR_SIZE	(1 << KERNEL_SECTOR_SHIFT) /*512*/


/*
 * Drive Information.
 */
struct di_drive_info {
	u16				rev;
	u16				code;
	u32				date;
	u8				pad[0x18];
};

/*
 * Disk ID.
 */
struct di_disk_id {
	u8				id[32];
};

/*
 * An operation code.
 */
struct di_opcode {
	u16				op;
#define DI_OP(id,flags)		(((u8)(id)<<8)|((u8)(flags)))
#define DI_OP_ID(op)		((u8)((op)>>8))
#define DI_OP_FLAGS(op)		((u8)(op))

#define   DI_DIR_READ		0x00
#define   DI_DIR_WRITE		DI_CR_RW
#define   DI_MODE_IMMED		0x00
#define   DI_MODE_DMA		DI_CR_DMA
#define   DI_IGNORE_ERRORS	(1<<7)

	char				*name;

	u32				cmdbuf0;
};

/*
 * Drive code container.
 */
struct di_drive_code {
	u32				address;
	size_t				len;
	void				*code;
};

struct di_device;

/*
 * A DVD Interface command.
 */
struct di_command {
	u16				opidx;

	u32				cmdbuf0;
	u32				cmdbuf1;
	u32				cmdbuf2;

	void				*data;
	size_t				len;

	dma_addr_t			dma_addr;
	size_t				dma_len;

	void				*done_data;
	void				(*done)(struct di_command *cmd);

	u16				retries;
	u16				max_retries;

	u32				result;

	struct di_device		*ddev;
};

#define di_result_ok(result)	((result) == DI_SR_TCINT)
#define di_command_ok(cmd)	(di_result_ok((cmd)->result))

enum {
	__DI_INTEROPERABLE = 0,
	__DI_MEDIA_CHANGED,
	__DI_START_QUEUE,
	__DI_RESETTING
};

/*
 * The DVD Interface device.
 */
struct di_device {
	spinlock_t			lock;

	int				irq;

	spinlock_t			io_lock;
	void __iomem			*io_base;

	struct di_command		*cmd;
	struct di_command		*failed_cmd;

	struct di_command		status;

	struct gendisk                  *disk;
	struct request_queue            *queue;
	spinlock_t			queue_lock;

	struct request                  *req;
	struct di_command		req_cmd;

	u32				drive_status;

	unsigned long			flags;
#define DI_INTEROPERABLE	(1<<__DI_INTEROPERABLE)
#define DI_MEDIA_CHANGED	(1<<__DI_MEDIA_CHANGED)
#define DI_START_QUEUE		(1<<__DI_START_QUEUE)
#define DI_RESETTING		(1<<__DI_RESETTING)

	unsigned long			nr_sectors;

	struct timer_list		motor_off_timer;

#ifdef CONFIG_PROC_FS
	struct proc_dir_entry		*proc;
#endif /* CONFIG_PROC_FS */

	int				ref_count;

	struct platform_device		pdev;  /* must be last member */
};

/* get the di device given the platform device of a di device */
#define to_di_device(n) container_of(n,struct di_device,pdev)


/*
 * We do not accept original media with this driver, as there is currently no
 * general need for that.
 * If you ever develop an application (a media player for example) which works
 * with original media, just change di_accept_gods and recompile. 
 */
static const int di_accept_gods = 0;


/*
 * Drive firmware extensions.
 */

#define DI_DRIVE_CODE_BASE	0x40d000
#define DI_DRIVE_IRQ_VECTOR	0x00804c

static u32 generic_drive_code_entry_point = DI_DRIVE_CODE_BASE;
static struct di_drive_code generic_drive_code_trigger = {
	.address = DI_DRIVE_IRQ_VECTOR,
	.len = sizeof(generic_drive_code_entry_point),
	.code = (u8 *)&generic_drive_code_entry_point,
};

/*
 * Drive 04 (20020402) firmware extensions.
 */

#include "drive_20020402.h"

static struct di_drive_code drive_20020402[] = {
	[0] = {
		.address = DI_DRIVE_CODE_BASE,
		.len = sizeof(drive_20020402_firmware),
		.code = (u8 *)drive_20020402_firmware,
	},
};

/*
 * Drive 06 (20010608) firmware extensions.
 */

#include "drive_20010608.h"

static struct di_drive_code drive_20010608[] = {
	[0] = {
		.address = DI_DRIVE_CODE_BASE,
		.len = sizeof(drive_20010608_firmware),
		.code = (u8 *)drive_20010608_firmware,
	},
};

/*
 * Drive 08 (20020823) firmware extensions.
 */

#include "drive_20020823.h"

static struct di_drive_code drive_20020823[] = {
	[0] = {
		.address = DI_DRIVE_CODE_BASE,
		.len = sizeof(drive_20020823_firmware),
		.code = (u8 *)drive_20020823_firmware,
	},
};

/*
 * Panasonic Q (20010831) firmware extensions.
 */

#include "drive_20010831.h"

static struct di_drive_code drive_20010831[] = {
	[0] = {
		.address = DI_DRIVE_CODE_BASE,
		.len = sizeof(drive_20010831_firmware),
		.code = (u8 *)drive_20010831_firmware,
	},
};


/*
 * Drive operations table, incomplete.
 * We just include here some of the available functions, in no particular
 * order.
 */
#define CMDBUF(a,b,c,d) (((a)<<24)|((b)<<16)|((c)<<8)|(d))

static struct di_opcode di_opcodes[] = {

#define DI_OP_NOP		0
	[DI_OP_NOP] = {
		.op = DI_OP(DI_OP_NOP, 0),
		.name = "NOP",
		.cmdbuf0 = 0,
	},

#define DI_OP_INQ		(DI_OP_NOP+1)
	[DI_OP_INQ] = {
		.op = DI_OP(DI_OP_INQ, DI_DIR_READ | DI_MODE_DMA),
		.name = "INQ",
		.cmdbuf0 = 0x12000000,
	},

#define DI_OP_STOPMOTOR		(DI_OP_INQ+1)
	[DI_OP_STOPMOTOR] = {
		.op = DI_OP(DI_OP_STOPMOTOR, DI_DIR_READ | DI_MODE_IMMED),
		.name = "STOPMOTOR",
		.cmdbuf0 = 0xe3000000,
	},

#define DI_OP_READDISKID	(DI_OP_STOPMOTOR+1)
	[DI_OP_READDISKID] = {
		.op = DI_OP(DI_OP_READDISKID, DI_DIR_READ | DI_MODE_DMA),
		.name = "READDISKID",
		.cmdbuf0 = 0xa8000040,
	},

#define DI_OP_READSECTOR	(DI_OP_READDISKID+1)
	[DI_OP_READSECTOR] = {
		.op = DI_OP(DI_OP_READSECTOR, DI_DIR_READ | DI_MODE_DMA),
		.name = "READSECTOR",
		.cmdbuf0 = 0xa8000000,
	},

#define DI_OP_ENABLE1		(DI_OP_READSECTOR+1)
	[DI_OP_ENABLE1] = {
		.op = DI_OP(DI_OP_ENABLE1, DI_DIR_READ | DI_MODE_IMMED),
		.name = "MATSHITA",
		.cmdbuf0 = 0,
	},

#define DI_OP_ENABLE2		(DI_OP_ENABLE1+1)
	[DI_OP_ENABLE2] = {
		.op = DI_OP(DI_OP_ENABLE2, DI_DIR_READ | DI_MODE_IMMED),
		.name = "DVD-GAME",
		.cmdbuf0 = 0,
	},

#define DI_OP_READMEM		(DI_OP_ENABLE2+1)
	[DI_OP_READMEM] = {
		.op = DI_OP(DI_OP_READMEM, DI_DIR_READ | DI_MODE_IMMED),
		.name = "READMEM",
		.cmdbuf0 = 0xfe010000,
	},

#define DI_OP_WRITEMEM		(DI_OP_READMEM+1)
	[DI_OP_WRITEMEM] = {
		.op = DI_OP(DI_OP_WRITEMEM, DI_DIR_READ | DI_MODE_DMA),
		.name = "WRITEMEM",
		.cmdbuf0 = 0xfe010100,
	},

#define DI_OP_GETSTATUS		(DI_OP_WRITEMEM+1)
	[DI_OP_GETSTATUS] = {
		.op = DI_OP(DI_OP_GETSTATUS, DI_DIR_READ | DI_MODE_IMMED),
		.name = "GETSTATUS",
		.cmdbuf0 = 0xe0000000,
	},

/* thanks to blackcheck for pointing this one */
#define DI_OP_SPINMOTOR		(DI_OP_GETSTATUS+1)
	[DI_OP_SPINMOTOR] = {
		.op = DI_OP(DI_OP_SPINMOTOR, DI_DIR_READ | DI_MODE_IMMED),
		.name = "SPINMOTOR",
		.cmdbuf0 = 0xfe110000,
#define DI_SPINMOTOR_MASK	0x0000ff00
#define DI_SPINMOTOR_DOWN	0x00000000
#define DI_SPINMOTOR_UP		0x00000100
#define DI_SPINMOTOR_CHECKDISK	0x00008000
	},

/*
 * The following commands are part of the firmware extensions.
 */

#define DI_OP_SETSTATUS		(DI_OP_SPINMOTOR+1)
	[DI_OP_SETSTATUS] = {
		.op = DI_OP(DI_OP_SETSTATUS, DI_DIR_READ | DI_MODE_IMMED),
		.name = "SETSTATUS",
		.cmdbuf0 = 0xee000000,
#define DI_SETSTATUS_MASK	0x00ff0000
#define DI_SETSTATUS_SHIFT	16
	},

#define DI_OP_ENABLEEXTENSIONS	(DI_OP_SETSTATUS+1)
	[DI_OP_ENABLEEXTENSIONS] = {
		.op = DI_OP(DI_OP_ENABLEEXTENSIONS, DI_DIR_READ|DI_MODE_IMMED|
							DI_IGNORE_ERRORS),
		.name = "ENABLEEXTENSIONS",
		.cmdbuf0 = 0x55000000,
#define DI_ENABLEEXTENSIONS_MASK	0x00ff0000
#define DI_ENABLEEXTENSIONS_SHIFT	16
	},

#define DI_OP_MAXOP		DI_OP_ENABLEEXTENSIONS
};

#define DI_OP_CUSTOM		((u16)~0)


static void di_reset(struct di_device *ddev);
static int di_run_command(struct di_command *cmd);

/*
 * Returns the operation code related data for a command.
 */
static inline struct di_opcode *di_get_opcode(struct di_command *cmd)
{
	BUG_ON(cmd->opidx > DI_OP_MAXOP && cmd->opidx != DI_OP_CUSTOM);

	if (cmd->opidx == DI_OP_CUSTOM) {
		return cmd->data;
	} else {
		return &di_opcodes[cmd->opidx];
	}
}

/*
 * Returns the operation code for a command.
 */
static inline u16 di_op(struct di_command *cmd)
{
	return di_get_opcode(cmd)->op;
}


/*
 * Basic initialization for all commands.
 */
static void di_op_basic(struct di_command *cmd,
			struct di_device *ddev, u16 opidx)
{
	struct di_opcode *opcode;

	memset(cmd, 0, sizeof(*cmd));
	cmd->ddev = ddev;
	cmd->opidx = opidx;
	cmd->max_retries = cmd->retries = 0;
	opcode = di_get_opcode(cmd);
	if (opcode) {
		cmd->cmdbuf0 = opcode->cmdbuf0;
	}
}

/*
 * Builds an "Inquiry" command.
 */
static void di_op_inq(struct di_command *cmd,
		      struct di_device *ddev,
		      struct di_drive_info *drive_info)
{
	di_op_basic(cmd, ddev, DI_OP_INQ);
	cmd->cmdbuf2 = sizeof(*drive_info);
	cmd->data = drive_info;
	cmd->len = sizeof(*drive_info);
}

/*
 * Builds a "Stop Motor" command.
 */
static inline void di_op_stopmotor(struct di_command *cmd,
				   struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_STOPMOTOR);
}

/*
 * Builds a "Read Disc ID" command.
 */
static void di_op_readdiskid(struct di_command *cmd,
			     struct di_device *ddev,
			     struct di_disk_id *disk_id)
{
	di_op_basic(cmd, ddev, DI_OP_READDISKID);
	cmd->cmdbuf2 = sizeof(*disk_id);
	cmd->data = disk_id;
	cmd->len = sizeof(*disk_id);
	cmd->max_retries = cmd->retries = DI_COMMAND_RETRIES;
}

/*
 * Builds a "Read Sector" command.
 */
static void di_op_readsector(struct di_command *cmd,
			     struct di_device *ddev,
			     u32 sector, void *data, size_t len)
{
	di_op_basic(cmd, ddev, DI_OP_READSECTOR);
	cmd->cmdbuf1 = sector;
	cmd->cmdbuf2 = len;
	cmd->data = data;
	cmd->len = len;
	cmd->max_retries = cmd->retries = DI_COMMAND_RETRIES;
}

/*
 * Builds the first enable command.
 */
static void di_op_enable1(struct di_command *cmd, struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_ENABLE1);
	cmd->cmdbuf0 = CMDBUF(0xff, 0x01, 'M', 'A');
	cmd->cmdbuf1 = CMDBUF('T', 'S', 'H', 'I');
	cmd->cmdbuf2 = CMDBUF('T', 'A', 0x02, 0x00);
}

/*
 * Builds the second enable command.
 */
static void di_op_enable2(struct di_command *cmd, struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_ENABLE2);
	cmd->cmdbuf0 = CMDBUF(0xff, 0x00, 'D', 'V');
	cmd->cmdbuf1 = CMDBUF('D', '-', 'G', 'A');
	cmd->cmdbuf2 = CMDBUF('M', 'E', 0x03, 0x00);
}

/*
 * Builds a "Read Memory" command.
 */
static inline void di_op_readmem(struct di_command *cmd,
				 struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_READMEM);
	cmd->cmdbuf2 = 0x00010000;
}

/*
 * Builds a "Write Memory" command.
 */
static inline void di_op_writemem(struct di_command *cmd,
				 struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_WRITEMEM);
}

/*
 * Builds a "get drive status" command.
 */
static inline void di_op_getstatus(struct di_command *cmd,
				   struct di_device *ddev)
{
	di_op_basic(cmd, ddev, DI_OP_GETSTATUS);
}

/*
 * Builds a "spin motor" command.
 */
static void di_op_spinmotor(struct di_command *cmd,
			    struct di_device *ddev, u32 flags)
{
	di_op_basic(cmd, ddev, DI_OP_SPINMOTOR);
	cmd->cmdbuf0 |= (flags & DI_SPINMOTOR_MASK);
}

/*
 * Builds a "set drive status" command.
 */
static void di_op_setstatus(struct di_command *cmd,
			    struct di_device *ddev, u8 status)
{
	di_op_basic(cmd, ddev, DI_OP_SETSTATUS);
	cmd->cmdbuf0 |= ((status << DI_SETSTATUS_SHIFT) & DI_SETSTATUS_MASK);
}

/*
 * Builds a "enable extensions" command.
 * The extended firmware will transparently disable the extensions when
 * original media is found.
 */
static void di_op_enableextensions(struct di_command *cmd,
				  struct di_device *ddev, u8 enable)
{
	di_op_basic(cmd, ddev, DI_OP_ENABLEEXTENSIONS);
	cmd->cmdbuf0 |= ((enable << DI_ENABLEEXTENSIONS_SHIFT) &
			DI_ENABLEEXTENSIONS_MASK);
}

/*
 * Builds a customized command.
 */
static inline void di_op_custom(struct di_command *cmd,
				struct di_device *ddev,
				struct di_opcode *opcode)
{
	di_op_basic(cmd, ddev, DI_OP_NOP);
	cmd->opidx = DI_OP_CUSTOM;
	cmd->data = opcode;
}


/*
 * Converts a request direction into a DMA data direction.
 */
static inline
enum dma_data_direction di_opidx_to_dma_dir(struct di_command *cmd)
{
	u16 op = di_op(cmd);

	if ((op & DI_DIR_WRITE)) {
		return DMA_TO_DEVICE;
	} else {
		return DMA_FROM_DEVICE;
	}
}

/*
 * Returns the printable form of the status part of a drive status.
 */
static char *di_printable_status(u32 drive_status)
{
	char *s = "unknown";

	switch(DI_STATUS(drive_status)) {
		case DI_STATUS_READY:
			s = "ready";
			break;
		case DI_STATUS_COVER_OPENED:
			s = "cover opened";
			break;
		case DI_STATUS_DISK_CHANGE:
			s = "disk change";
			break;
		case DI_STATUS_NO_DISK:
			s = "no disk";
			break;
		case DI_STATUS_MOTOR_STOP:
			s = "motor stop";
			break;
		case DI_STATUS_DISK_ID_NOT_READ:
			s = "disk id not read";
			break;
	}
	return s;
}

/*
 * Returns the printable form of the error part of a drive status.
 */
static char *di_printable_error(u32 drive_status)
{
	char *s = "unknown";

	switch(DI_ERROR(drive_status)) {
		case DI_ERROR_NO_ERROR:
			s = "no error";
		case DI_ERROR_MOTOR_STOPPED:
			s = "motor stopped";
			break;
		case DI_ERROR_DISK_ID_NOT_READ:
			s = "disk id not read";
			break;
		case DI_ERROR_MEDIUM_NOT_PRESENT:
			s = "medium not present";
			break;
		case DI_ERROR_SEEK_INCOMPLETE:
			s = "seek incomplete";
			break;
		case DI_ERROR_UNRECOVERABLE_READ:
			s = "unrecoverable read";
			break;
		case DI_ERROR_INVALID_COMMAND:
			s = "invalid command";
			break;
		case DI_ERROR_BLOCK_OUT_OF_RANGE:
			s = "block out of range";
			break;
		case DI_ERROR_INVALID_FIELD:
			s = "invalid field";
			break;
		case DI_ERROR_MEDIUM_CHANGED:
			s = "medium changed";
			break;
	}

	return s;
}

/*
 * Prints the given drive status, only if debug enabled.
 */
static inline void di_debug_print_drive_status(u32 drive_status)
{
	DBG("%08x, [%s, %s]\n", drive_status,
	    di_printable_status(drive_status),
	    di_printable_error(drive_status));
}

/*
 * Prints the given drive status.
 */
static void di_print_drive_status(u32 drive_status)
{
	di_printk(KERN_INFO, "drive_status=%08x, [%s, %s]\n", drive_status,
		  di_printable_status(drive_status),
		  di_printable_error(drive_status));
}

/*
 * Prints the given disk identifier.
 */
static void di_print_disk_id(struct di_disk_id *disk_id)
{
	di_printk(KERN_INFO, "disk_id = [%s]\n", disk_id->id);
}


/*
 * Starts a DMA transfer.
 */
static void di_start_dma_transfer_raw(struct di_device *ddev,
				      dma_addr_t data, size_t len, int mode)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *sr_reg = io_base + DI_SR;
	unsigned long flags;

	BUG_ON((data & DI_DMA_ALIGN) != 0 ||
	       (len & DI_DMA_ALIGN) != 0);

	/* setup address and length of transfer */
	writel(len, io_base + DI_LENGTH);
	writel(data, io_base + DI_MAR);

	/* enable the Transfer Complete interrupt */
	spin_lock_irqsave(&ddev->io_lock, flags);
	writel(readl(sr_reg) | DI_SR_TCINTMASK, sr_reg);
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/* start the transfer */
	writel(DI_CR_TSTART | DI_CR_DMA | (mode&0x4), io_base + DI_CR);
}

/*
 * Internal. Busy-waits until a DMA transfer finishes or timeouts.
 */
static int __wait_for_dma_transfer_or_timeout(u32 __iomem *cr_reg,
					      int secs)
{
	unsigned long timeout = jiffies + secs*HZ;

	/* busy-wait for transfer complete */
	while((readl(cr_reg) & DI_CR_TSTART) && time_before(jiffies, timeout)) {
		cpu_relax();
	}

	return (readl(cr_reg) & DI_CR_TSTART)?-EBUSY:0;
}

/*
 * Busy-waits until DMA transfers are finished.
 */
static void di_wait_for_dma_transfer_raw(struct di_device *ddev)
{
	u32 __iomem *cr_reg = ddev->io_base + DI_CR;
	u32 __iomem *sr_reg = ddev->io_base + DI_SR;
	unsigned long flags;

	/* we don't want TCINTs to disturb us while waiting */
	spin_lock_irqsave(&ddev->io_lock, flags);
	writel(readl(sr_reg) & ~DI_SR_TCINTMASK, sr_reg);
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/* if the drive got stuck, reset it */
	if (__wait_for_dma_transfer_or_timeout(cr_reg, DI_COMMAND_TIMEOUT)) {
		DBG("dvd stuck!\n");
		di_reset(ddev);
	}

	/* ack and enable the Transfer Complete interrupt */
	spin_lock_irqsave(&ddev->io_lock, flags);
	writel(readl(sr_reg) | (DI_SR_TCINT|DI_SR_TCINTMASK), sr_reg);
	spin_unlock_irqrestore(&ddev->io_lock, flags);

	return;
}

/*
 * Outputs the command buffers, and optionally starts a transfer.
 */
static void di_prepare_command(struct di_command *cmd, int tstart)
{
	struct di_opcode *opcode = di_get_opcode(cmd);
	void __iomem *io_base = cmd->ddev->io_base;

	/*DBG("buf0 = 0x%08x, buf1 = 0x%08x, buf2 = 0x%08x\n",
	    cmd->cmdbuf0, cmd->cmdbuf1, cmd->cmdbuf2);*/

	writel(cmd->cmdbuf0, io_base + DI_CMDBUF0);
	writel(cmd->cmdbuf1, io_base + DI_CMDBUF1);
	writel(cmd->cmdbuf2, io_base + DI_CMDBUF2);

	cmd->ddev->drive_status = 0;

	if (tstart) {
		writel(DI_CR_TSTART | (opcode->op & 0x6), io_base + DI_CR);
	}
}

static void di_command_done(struct di_command *cmd);

/*
 * Starts a command by using the immediate mode.
 */
static int di_start_command(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	unsigned long flags;
	int retval = 1;

	spin_lock_irqsave(&ddev->lock, flags);

	BUG_ON(ddev->cmd);

	ddev->cmd = cmd;
	cmd->dma_len = 0; /* no dma here */
	di_prepare_command(cmd, 1);

	spin_unlock_irqrestore(&ddev->lock, flags);

	return retval;
}

/*
 * Starts a command by using the DMA mode.
 */
static int di_start_dma_command(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	unsigned long flags;
	int retval = 1;

	spin_lock_irqsave(&ddev->lock, flags);

	BUG_ON(ddev->cmd);

	ddev->cmd = cmd;
	cmd->dma_len = cmd->len;
	cmd->dma_addr = dma_map_single(&ddev->pdev.dev,
				       cmd->data, cmd->len,
				       di_opidx_to_dma_dir(cmd));

	di_prepare_command(cmd, 0);
	di_start_dma_transfer_raw(ddev, cmd->dma_addr, cmd->dma_len,
				  di_op(cmd) & DI_DIR_WRITE);

	spin_unlock_irqrestore(&ddev->lock, flags);

	return retval;
}

/*
 * Completes a "get drive status" command, after a failed command.
 */
static void di_complete_getstatus(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *data_reg = io_base + DI_DATA;

	ddev->drive_status = readl(data_reg);
}

/*
 * Called after a transfer is completed.
 */
static void di_complete_transfer(struct di_device *ddev, u32 result)
{
	struct di_command *cmd;
	struct di_opcode *opcode;
	u32 drive_status;
	unsigned long flags;

	spin_lock_irqsave(&ddev->lock, flags);

	/* do nothing if we have nothing to complete */
	cmd = ddev->cmd;
	if (!cmd) {
		spin_unlock_irqrestore(&ddev->lock, flags);
		goto out;
	}

	/* free the command slot */
	ddev->cmd = NULL;
	spin_unlock_irqrestore(&ddev->lock, flags);

	/* deal with caches after a dma transfer */
	if (cmd->dma_len) {
		dma_unmap_single(&ddev->pdev.dev,
				 cmd->dma_addr, cmd->dma_len,
				 di_opidx_to_dma_dir(cmd));
	}

	opcode = di_get_opcode(cmd);

	/*
	 * If a command fails we check the drive status. Depending on that
	 * we may or not retry later the command.
	 */
	cmd->result = result;
	if (!di_command_ok(cmd)) {
		/* the MATSHITA command always reports failure, ignore it */
		if (DI_OP_ID(opcode->op) != DI_OP_ENABLE1) {
			BUG_ON(ddev->failed_cmd != NULL);

			ddev->failed_cmd = cmd;

			/*
			 * Issue immediately a "get drive status"
			 * after a failed command.
			 */
			cmd = &ddev->status;
			di_op_getstatus(cmd, ddev);
			cmd->done = di_complete_getstatus;
			di_run_command(cmd);
			goto out;
		}
	} else {
		if (cmd->retries != cmd->max_retries) {
			DBG("command %s succeeded after %d retries :-)\n",
			    opcode->name, cmd->max_retries - cmd->retries);
		}
	}

	/* complete a successful command, or the MATSHITA one */
	di_command_done(cmd);

	spin_lock_irqsave(&ddev->lock, flags);
	if (ddev->failed_cmd) {
		cmd = ddev->failed_cmd;
		ddev->failed_cmd = NULL;
		spin_unlock_irqrestore(&ddev->lock, flags);

		drive_status = ddev->drive_status;
		opcode = di_get_opcode(cmd);

		/* retry a previously failed command if appropiate */
		if (cmd->retries > 0) {
			if (di_may_retry(drive_status)) {
				DBG("command %s failed, %d retries left\n",
				    opcode->name, cmd->retries);
				di_debug_print_drive_status(drive_status);
				
				cmd->retries--;
				di_run_command(cmd);
				goto out;
			} else {
				DBG("command %s failed,"
				    " aborting due to drive status\n",
				    opcode->name);
			}
		} else {
			if (!(opcode->op & DI_IGNORE_ERRORS))
				DBG("command %s failed\n", opcode->name);
		}

		if (!(opcode->op & DI_IGNORE_ERRORS))
			di_print_drive_status(drive_status);

		/* complete the failed command */
		di_command_done(cmd);

		/* update the driver status */
		switch(DI_ERROR(drive_status)) {
			case DI_ERROR_MOTOR_STOPPED:
			case DI_ERROR_MEDIUM_NOT_PRESENT:
			case DI_ERROR_MEDIUM_CHANGED:
				set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
				break;
			default:
				break;
		}

	} else {
		spin_unlock_irqrestore(&ddev->lock, flags);
	}

	/* start the block layer queue if someone requested it */
	if (test_and_clear_bit(__DI_START_QUEUE, &ddev->flags)) {
		spin_lock(&ddev->queue_lock);
		blk_start_queue(ddev->queue);
		spin_unlock(&ddev->queue_lock);
	}

out:
	return;
}

/*
 * Calls any done hooks.
 */
static void di_command_done(struct di_command *cmd)
{
	/* if specified, call the completion routine */
	if (cmd->done) {
		cmd->done(cmd);
	}
}

/*
 * Completion routine.
 */
static void di_wait_done(struct di_command *cmd)
{
	complete(cmd->done_data);
}

/*
 * Runs a command.
 */
static int di_run_command(struct di_command *cmd)
{
	struct di_opcode *opcode = di_get_opcode(cmd);
	int retval;

	if (cmd->retries > cmd->max_retries)
		cmd->retries = cmd->max_retries;

	if (!(opcode->op & DI_MODE_DMA)) {
		retval = di_start_command(cmd);
	} else {
		retval = di_start_dma_command(cmd);
	}
	return retval;
}

/*
 * Runs a command and waits.
 * Might sleep if called from user context.
 */
static int di_run_command_and_wait(struct di_command *cmd)
{
	DECLARE_COMPLETION(complete);

	cmd->done_data = &complete;
	cmd->done = di_wait_done;
	if (di_run_command(cmd) > 0) {
		wait_for_completion(&complete);
	}
	return cmd->result;
}

/*
 * Interrupt handler for DI interrupts.
 */
static irqreturn_t di_irq_handler(int irq, void *dev0, struct pt_regs *regs)
{
	struct di_device *ddev = dev0;
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *sr_reg = io_base + DI_SR;
	u32 __iomem *cvr_reg = io_base + DI_CVR;
	u32 sr, cvr, reason, mask;
	unsigned long flags;

	spin_lock_irqsave(&ddev->io_lock, flags);

	sr = readl(sr_reg);
	mask = sr & (DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINTMASK);
	reason = sr; /* & (mask << 1); */
	if (reason) {
		writel(sr | reason, sr_reg);
		spin_unlock_irqrestore(&ddev->io_lock, flags);

		if (reason & DI_SR_TCINT) {
			di_complete_transfer(ddev, DI_SR_TCINT);
		}
		if (reason & DI_SR_BRKINT) {
			DBG("BRKINT\n");
			di_complete_transfer(ddev, DI_SR_BRKINT);
		}
		if (reason & DI_SR_DEINT) {
			di_complete_transfer(ddev, DI_SR_DEINT);
		}

		spin_lock_irqsave(&ddev->io_lock, flags);
	}

	cvr = readl(cvr_reg);
	mask = cvr & DI_CVR_CVRINTMASK;
	reason = cvr; /* & (mask << 1); */
	if ((reason & DI_CVR_CVRINT)) {
		writel(cvr | DI_CVR_CVRINT, cvr_reg);
		set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
		if (test_and_clear_bit(__DI_RESETTING, &ddev->flags)) {
			if (ddev->flags & DI_INTEROPERABLE) {
				DBG("extensions loaded"
				    " and hopefully working\n");
			} else {
				DBG("drive reset, no extensions loaded yet\n");
			}
		} else {
			DBG("dvd cover interrupt\n");
		}
	}

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	return IRQ_HANDLED;
}

/*
 * Resets the drive (hard).
 */
static void di_reset(struct di_device *ddev)
{
	u32 __iomem *reset_reg = (u32 __iomem *)0xcc003024;
	u32 reset;

#define FLIPPER_RESET_DVD 0x00000004

	ddev->flags = DI_RESETTING|DI_MEDIA_CHANGED;

	reset = readl(reset_reg);
	writel((reset & ~FLIPPER_RESET_DVD) | 1, reset_reg);
	mdelay(500);
	writel((reset | FLIPPER_RESET_DVD) | 1, reset_reg);
	mdelay(500);
}

/*
 * Gets the current drive status.
 */
static u32 di_get_drive_status(struct di_device *ddev)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *data_reg = io_base + DI_DATA;
	struct di_command cmd;
	u32 drive_status;

	di_op_getstatus(&cmd, ddev);
	di_run_command_and_wait(&cmd);
	drive_status = readl(data_reg);

	return drive_status;
}

/*
 * Enables the "privileged" command set.
 */
static int di_enable_privileged_commands(struct di_device *ddev)
{
	struct di_command cmd;

	/* send these two consecutive enable commands */
	di_op_enable1(&cmd, ddev);
	di_run_command_and_wait(&cmd);
	di_op_enable2(&cmd, ddev);
	return di_run_command_and_wait(&cmd);
}

/*
 * Patches drive addressable memory.
 */
static void di_patch_mem(struct di_device *ddev, u32 address,
			 void *data, size_t len)
{
	struct di_command cmd;
	struct di_opcode opcode;
	int chunk_size;
	const int max_chunk_size = 3 * sizeof(cmd.cmdbuf0);

	while(len > 0) {
		/* we can write in groups of 12 bytes at max */
		if (len > max_chunk_size)
			chunk_size = max_chunk_size;
		else
			chunk_size = len;

		/* prepare for writing to drive's memory ... */
		di_op_writemem(&cmd, ddev);
		cmd.cmdbuf1 = address;
		cmd.cmdbuf2 = chunk_size << 16;
		di_run_command_and_wait(&cmd);

		/* ... and actually write to it */
		opcode.op = DI_OP(DI_OP_CUSTOM, DI_DIR_READ | DI_MODE_IMMED);
		opcode.name = "custom write";
		di_op_custom(&cmd, ddev, &opcode);
		memcpy(&cmd.cmdbuf0, data, chunk_size);
		di_run_command(&cmd);

		/*
		 * We can't rely on drive operating as expected here, so we
		 * explicitly poll for end of transfer and timeout eventually.
		 * Anyway, we assume everything was ok.
		 */
		di_wait_for_dma_transfer_raw(ddev);
		di_complete_transfer(ddev, DI_SR_TCINT);

		/* ok, next chunk */
		address += chunk_size;
		data += chunk_size;
		len -= chunk_size;
	}
}

/*
 * Runs a series of patches.
 */
static void di_patch(struct di_device *ddev,
		     struct di_drive_code *section, int nr_sections)
{
	while(nr_sections > 0) {
		di_patch_mem(ddev, section->address,
			     section->code, section->len);
		section++;
		nr_sections--;
	}
}

/*
 * Configures the drive to accept DVD-R and DVD+R media.
 */
static void di_make_interoperable(struct di_device *ddev)
{
	static struct di_drive_info drive_info
			 __attribute__ ((aligned (DI_DMA_ALIGN+1)));
	struct di_command cmd;
	int result;

	/* check that no one is banning access to the privileged command set */
	result = di_enable_privileged_commands(ddev);
	if (!di_result_ok(result)) {
		DBG("Uhmm, looks like a bad-mannered drive code is"
		    " already in place\n");
		DBG("Let's hard reset the drive then...\n");

		di_reset(ddev);
	}

	/* we'll use the drive model to select the appropiate firmware */
	memset(&drive_info, 0, sizeof(drive_info));
	di_op_inq(&cmd, ddev, &drive_info);
	di_run_command_and_wait(&cmd);

	DBG("drive_info: rev=%x, code=%x, date=%x\n",
	    drive_info.rev, drive_info.code, drive_info.date);

	/* we require here the privileged command set */
	di_enable_privileged_commands(ddev);

	/* extend the firmware to allow use of normal media */
	di_printk(KERN_INFO, "loading drive %x extensions\n",
		  drive_info.date);

	switch(drive_info.date) {
		case 0x20020402:
			di_patch(ddev, drive_20020402,
				 ARRAY_SIZE(drive_20020402));
			break;
		case 0x20010608:
			di_patch(ddev, drive_20010608,
				 ARRAY_SIZE(drive_20010608));
			break;
		case 0x20020823:
			di_patch(ddev, drive_20020823,
				 ARRAY_SIZE(drive_20020823));
			break;
		case 0x20010831:
			di_patch(ddev, drive_20010831,
				 ARRAY_SIZE(drive_20010831));
			break;
		default:
			di_printk(KERN_ERR, "sorry, drive %x is not yet"
				  " supported\n",
				  drive_info.date);
			goto out;
			break;
	}

	/* the drive will become interoperable, and will go through a reset */
	set_bit(__DI_INTEROPERABLE, &ddev->flags);
	set_bit(__DI_RESETTING, &ddev->flags);

	/* here we go ... */
	di_patch(ddev, &generic_drive_code_trigger, 1);

out:
	return;
}

/*
 * Stops the drive's motor, according to a previous schedule.
 */
static void di_motor_off(unsigned long ddev0)
{
	struct di_device *ddev = (struct di_device *)ddev0;
	struct di_command *cmd;
	unsigned long flags;

	/* postpone a bit the motor off if there are pending commands */
	spin_lock_irqsave(&ddev->lock, flags);
	if (!ddev->cmd) {
	        ddev->cmd = cmd = &ddev->status;
		spin_unlock_irqrestore(&ddev->lock, flags);
		di_op_stopmotor(cmd, ddev);
	        di_prepare_command(cmd, 1);
	} else {
		spin_unlock_irqrestore(&ddev->lock, flags);
		mod_timer(&ddev->motor_off_timer, jiffies + 1*HZ);
	}
}

/*
 * Cancels a previously scheduled motor off.
 */
static inline void di_cancel_motor_off(struct di_device *ddev)
{
	del_timer(&ddev->motor_off_timer);
}

/*
 * Stops the drive's motor after the specified amount of seconds has elapsed.
 */
static void di_schedule_motor_off(struct di_device *ddev, unsigned int secs)
{
	del_timer(&ddev->motor_off_timer);
	ddev->motor_off_timer.expires = jiffies + secs*HZ;
	ddev->motor_off_timer.data = (unsigned long)ddev;
	add_timer(&ddev->motor_off_timer);
}

/*
 * Spins down the drive, immediatelly.
 */
static void di_spin_down_drive(struct di_device *ddev)
{
	struct di_command cmd;

	di_op_stopmotor(&cmd, ddev);
	di_run_command_and_wait(&cmd);
}

/*
 * Spins up the drive.
 */
static void di_spin_up_drive(struct di_device *ddev, u8 enable_extensions)
{
	struct di_command cmd;
	u32 drive_status;

	/* first, make sure the drive is interoperable */
	if (!(ddev->flags & DI_INTEROPERABLE)) {
		di_spin_down_drive(ddev);

		/* this actually will reset and spin up the drive */
		di_make_interoperable(ddev);
	} else {
		/*
		 * We only re-enable the extensions if the drive is not
		 * in a peding read disk id state. Otherwise, we assume the
		 * drive has already accepted the disk.
		 */
		drive_status = di_get_drive_status(ddev);
		if (DI_STATUS(drive_status) != DI_STATUS_DISK_ID_NOT_READ) {
			di_op_enableextensions(&cmd, ddev, enable_extensions);
			di_run_command_and_wait(&cmd);
		}
	}

	/* the spin motor command requires the privileged mode */
	di_enable_privileged_commands(ddev);

	di_op_spinmotor(&cmd, ddev, DI_SPINMOTOR_UP);
	di_run_command_and_wait(&cmd);

	if (!ddev->drive_status) {
		di_op_setstatus(&cmd, ddev, DI_STATUS_DISK_ID_NOT_READ+1);
		cmd.cmdbuf0 |= 0x00000300; /* XXX cheqmate */
		di_run_command_and_wait(&cmd);
	}
}

/*
 * Determines media type and accepts accordingly.
 */
static int di_read_toc(struct di_device *ddev)
{
	static struct di_disk_id disk_id
			 __attribute__ ((aligned (DI_DMA_ALIGN+1)));
	struct di_command cmd;
	int accepted_media = 0;
	int retval = 0;
	const u8 enable_extensions = 1;

	di_cancel_motor_off(ddev);

	/* spin up the drive if needed */
	if ((ddev->flags & DI_MEDIA_CHANGED)) {
		di_spin_up_drive(ddev, enable_extensions);
	}

	/* check that disk id can be read and that the media is appropiate */
	memset(&disk_id, 0, sizeof(disk_id));
	di_op_readdiskid(&cmd, ddev, &disk_id);
	di_run_command_and_wait(&cmd);
	if (di_command_ok(&cmd)) {
		if (disk_id.id[0] && !di_accept_gods) {
			di_print_disk_id(&disk_id);
			di_printk(KERN_ERR, "sorry, gamecube media"
				  " support is disabled\n");
		} else {
			accepted_media = 1;
		}
	} else {
		set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
	}

	if (accepted_media) {
		/*
		 * This is currently hardcoded. Scream|CT got this number
		 * by reading up to where the lens physically allowed.
		 *
		 * This is currently causing us problems.
		 * For example, recent 'mount' versions will read 4k from
		 * the end of the device when guessing filesystem types.
		 * The end of device we are reporting is not the real one,
		 * so the drive will fail to read that part if it was not
		 * burned.
		 *
		 * As a temporary solution, specify always a filesystem
		 * type when using mount, or fill the whole disk when
		 * burning.
		 */
		ddev->nr_sectors = DI_MAX_SECTORS; /* in DVD sectors */
		clear_bit(__DI_MEDIA_CHANGED, &ddev->flags);

		DBG("media ready for operation\n");
	} else {
		ddev->nr_sectors = 0;
		retval = -ENOMEDIUM;

		di_spin_down_drive(ddev);

		DBG("media NOT ready\n");
	}

	/* transform to kernel sectors */
	ddev->nr_sectors <<= (DI_SECTOR_SHIFT - KERNEL_SECTOR_SHIFT);
	set_capacity(ddev->disk, ddev->nr_sectors);

	return retval;
}


/*
 * Finishes a block layer request.
 */
static void di_request_done(struct di_command *cmd)
{
	struct di_device *ddev = cmd->ddev;
	struct request *req;
	unsigned long flags;

	spin_lock_irqsave(&ddev->lock, flags);

	req = ddev->req;
	ddev->req = NULL;

	spin_unlock_irqrestore(&ddev->lock, flags);

	if (req) {
		if (!end_that_request_first(req,
					    (cmd->result & DI_SR_TCINT)?1:0,
					    req->current_nr_sectors)) {
			add_disk_randomness(req->rq_disk);
			end_that_request_last(req);
		}
		spin_lock(&ddev->queue_lock);
		blk_start_queue(ddev->queue);
		spin_unlock(&ddev->queue_lock);
	}
}

/*
 * Processes a block layer request.
 */
static void di_do_request(request_queue_t *q)
{
	struct di_device *ddev = q->queuedata;
	struct di_command *cmd = &ddev->req_cmd;
	struct request *req;
	unsigned long start;
	unsigned long flags;
	size_t len;

	while ((req = elv_next_request(q))) {
		/* keep our reads within limits */
		if (req->sector + req->current_nr_sectors > ddev->nr_sectors) {
			di_printk(KERN_ERR, "reading past end\n");
			end_request(req, 0);
			continue;
		}

		/* it doesn't make sense to write to this device */
		if (rq_data_dir(req) == WRITE) {
			di_printk(KERN_ERR, "write attempted\n");
			end_request(req, 0);
			continue;
		}

		/* it is not a good idea to open the lid ... */
		if ((ddev->flags & DI_MEDIA_CHANGED)) {
			di_printk(KERN_ERR, "media changed, aborting\n");
			end_request(req, 0);
			continue;
		}

		spin_lock_irqsave(&ddev->lock, flags);

		/* we can schedule just a single request each time */
		if (ddev->req || ddev->cmd) {
			blk_stop_queue(q);
			if (ddev->cmd) {
				set_bit(__DI_START_QUEUE, &ddev->flags);
			}
			spin_unlock_irqrestore(&ddev->lock, flags);
			break;
		}

		blkdev_dequeue_request(req);

		/* ignore requests that we can't handle */
		if (!blk_fs_request(req)) {
			spin_unlock_irqrestore(&ddev->lock, flags);
			continue;
		}

		/* store the request being handled ... */
		ddev->req = req;
		blk_stop_queue(q);

		spin_unlock_irqrestore(&ddev->lock, flags);

		/* ... and launch the corresponding read sector command */
		start = req->sector << KERNEL_SECTOR_SHIFT;
		len = req->current_nr_sectors << KERNEL_SECTOR_SHIFT;

		di_op_readsector(cmd, ddev, start >> 2,
				 req->buffer, len);
		cmd->done_data = cmd;
		cmd->done = di_request_done;
		di_run_command(cmd);
	}
}

/*
 * Opens the drive device.
 */
static int di_open(struct inode *inode, struct file *filp)
{
	struct di_device *ddev = inode->i_bdev->bd_disk->private_data;
	struct di_command *cmd;
	DECLARE_COMPLETION(complete);
	unsigned long flags;
	int retval = 0;

	/* this is a read only device */
	if (filp->f_mode & FMODE_WRITE) {
		retval = -EROFS;
		goto out;
	}

	/* only allow a minor of 0 to be opened */
	if (iminor(inode)) {
		retval =  -ENODEV;
		goto out;
	}

        spin_lock_irqsave(&ddev->queue_lock, flags);

	/* honor exclusive open mode */
	if (ddev->ref_count == -1 ||
	    (ddev->ref_count && (filp->f_flags & O_EXCL))) {
		retval = -EBUSY;
		goto out_unlock;
	}

	/*
	 * If we have a pending command, that's a previously scheduled
	 * motor off. Wait for it to terminate before going on.
	 */
	if (ddev->cmd && ddev->ref_count == 0) {
		cmd = ddev->cmd;
		cmd->done_data = &complete;
		cmd->done = di_wait_done;
		spin_unlock_irqrestore(&ddev->queue_lock, flags);
		wait_for_completion(&complete);
        	spin_lock_irqsave(&ddev->queue_lock, flags);
	}

	/* this will take care of validating the media */
	check_disk_change(inode->i_bdev);
	if (!ddev->nr_sectors) {
		retval = -ENOMEDIUM;
		goto out_unlock;
	}

	if ((filp->f_flags & O_EXCL))
		ddev->ref_count = -1;
	else
		ddev->ref_count++;

out_unlock:
	spin_unlock_irqrestore(&ddev->queue_lock, flags);
out:
	return retval;

}

/*
 * Releases the drive device.
 */
static int di_release(struct inode *inode, struct file *filp)
{
	struct di_device *ddev = inode->i_bdev->bd_disk->private_data;
	unsigned long flags;

	spin_lock_irqsave(&ddev->queue_lock, flags);

	if (ddev->ref_count > 0)
		ddev->ref_count--;
	else
		ddev->ref_count = 0;

	spin_unlock_irqrestore(&ddev->queue_lock, flags);

	if (ddev->ref_count == 0) {
		/*
		 * We do not immediately stop the motor, which saves us
		 * a spin down/spin up in applications that re-open quickly
		 * the device, like mount when -t is not specified.
		 */
		di_schedule_motor_off(ddev, 1);

		set_bit(__DI_MEDIA_CHANGED, &ddev->flags);
	}

	return 0;
}

/*
 * Checks if media is still valid.
 */
static int di_revalidate_disk(struct gendisk *disk)
{
	struct di_device *ddev = disk->private_data;
	di_read_toc(ddev);
	return 0;
}

/*
 * Checks if media changed.
 */
static int di_media_changed(struct gendisk *disk)
{
	struct di_device *ddev = disk->private_data;
	return (ddev->flags & DI_MEDIA_CHANGED) ? 1 : 0;
}

/*
 * Ioctl. Specific CDROM stuff is pending support.
 */
static int di_ioctl(struct inode *inode, struct file *filp,
		    unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
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
		return -EINVAL;
	}
	return -EINVAL;
}

static struct block_device_operations di_fops = {
	.owner = THIS_MODULE,
	.open = di_open,
	.release = di_release,
	.revalidate_disk = di_revalidate_disk,
	.media_changed = di_media_changed,
	.ioctl = di_ioctl,
};

/*
 * Quiesces the hardware to a calm and known state.
 */
static void di_quiesce(struct di_device *ddev)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *cr_reg = io_base + DI_CR;
	u32 __iomem *sr_reg = io_base + DI_SR;
	u32 __iomem *cvr_reg = io_base + DI_CVR;
	u32 sr, cvr;
	unsigned long flags;

	spin_lock_irqsave(&ddev->io_lock, flags);

	/* ack and mask dvd io interrupts */
	sr = readl(sr_reg);
	sr |= DI_SR_BRKINT | DI_SR_TCINT | DI_SR_DEINT;
	sr &= ~(DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINTMASK);
	writel(sr, sr_reg);

	/* ack and mask dvd cover interrupts */
	cvr = readl(cvr_reg);
	writel((cvr | DI_CVR_CVRINT) & ~DI_CVR_CVRINTMASK, cvr_reg);

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/* busy-wait for transfer complete */
	__wait_for_dma_transfer_or_timeout(cr_reg, DI_COMMAND_TIMEOUT);
}

/*
 * Initializes the hardware.
 */
static int di_init_irq(struct di_device *ddev)
{
	void __iomem *io_base = ddev->io_base;
	u32 __iomem *sr_reg = io_base + DI_SR;
	u32 __iomem *cvr_reg = io_base + DI_CVR;
	u32 sr, cvr;
	struct di_command cmd;
	unsigned long flags;
	int retval;

	init_timer(&ddev->motor_off_timer);
	ddev->motor_off_timer.function =
		(void (*)(unsigned long))di_motor_off;

	set_bit(__DI_MEDIA_CHANGED, &ddev->flags);

	/* calm down things a bit first */
	di_quiesce(ddev);

	/* request interrupt */
	retval = request_irq(ddev->irq, di_irq_handler, 0,
			     DRV_MODULE_NAME, ddev);
	if (retval) {
		di_printk(KERN_ERR, "request of irq%d failed\n", ddev->irq);
		goto out;
	}

	spin_lock_irqsave(&ddev->io_lock, flags);

	sr = readl(sr_reg);
	sr |= DI_SR_BRKINT | DI_SR_TCINT | DI_SR_DEINT;
	sr |= DI_SR_BRKINTMASK | DI_SR_TCINTMASK | DI_SR_DEINTMASK;
	writel(sr, sr_reg);

	cvr = readl(cvr_reg);
	writel(cvr | DI_CVR_CVRINT | DI_CVR_CVRINTMASK, cvr_reg);

	spin_unlock_irqrestore(&ddev->io_lock, flags);

	/*
	 * We check if the drive is already interoperable by issuing one of
	 * the extended commands.
	 */
	di_op_enableextensions(&cmd, ddev, 1);
	di_run_command_and_wait(&cmd);
	if (ddev->drive_status) {
		di_make_interoperable(ddev);
	} else {
		set_bit(__DI_INTEROPERABLE, &ddev->flags);
	}

	di_schedule_motor_off(ddev, DI_MOTOR_OFF_TIMEOUT);

out:
	return retval;
}

/*
 * Relinquishes control of the hardware.
 */
static void di_exit_irq(struct di_device *ddev)
{
	/* stop DVD motor */
	di_cancel_motor_off(ddev);
	di_spin_down_drive(ddev);

	di_quiesce(ddev);

	free_irq(ddev->irq, ddev);
}


/*
 * Initializes the block layer interfaces.
 */
static int di_init_blk_dev(struct di_device *ddev)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int retval;

	spin_lock_init(&ddev->lock);
	spin_lock_init(&ddev->io_lock);

	ddev->ref_count = 0;

	retval = register_blkdev(DI_MAJOR, DI_NAME);
	if (retval) {
		di_printk(KERN_ERR, "error registering major %d\n", DI_MAJOR);
		goto err_register_blkdev;
	}

	retval = -ENOMEM;
	spin_lock_init(&ddev->queue_lock);
	queue = blk_init_queue(di_do_request, &ddev->queue_lock);
	if (!queue) {
		di_printk(KERN_ERR, "error initializing queue\n");
		goto err_blk_init_queue;
	}

	blk_queue_hardsect_size(queue, DI_SECTOR_SIZE);
	blk_queue_dma_alignment(queue, DI_DMA_ALIGN);
	blk_queue_max_phys_segments(queue, 1);
	blk_queue_max_hw_segments(queue, 1);
	queue->queuedata = ddev;
	ddev->queue = queue;

	disk = alloc_disk(1);
	if (!disk) {
		di_printk(KERN_ERR, "error allocating disk\n");
		goto err_alloc_disk;
	}

	disk->major = DI_MAJOR;
	disk->first_minor = 0;
	disk->fops = &di_fops;
	strcpy(disk->disk_name, DI_NAME);
	strcpy(disk->devfs_name, disk->disk_name);
	disk->queue = ddev->queue;
	disk->private_data = ddev;
	ddev->disk = disk;

	set_disk_ro(ddev->disk, 1);
	add_disk(ddev->disk);

	retval = 0;
	goto out;

err_alloc_disk:
        blk_cleanup_queue(ddev->queue);
err_blk_init_queue:
        unregister_blkdev(DI_MAJOR, DI_NAME);
err_register_blkdev:
out:
	return retval;
}

/*
 * Exits the block layer interfaces.
 */
static void di_exit_blk_dev(struct di_device *ddev)
{
	if (ddev->disk) {
		del_gendisk(ddev->disk);
		put_disk(ddev->disk);
	}
	if (ddev->queue)
		blk_cleanup_queue(ddev->queue);
	unregister_blkdev(DI_MAJOR, DI_NAME);
}

/*
 * Initializes /proc filesystem support.
 */
static int di_init_proc(struct di_device *ddev)
{
#ifdef CONFIG_PROC_FS
#endif /* CONFIG_PROC_FS */
	return 0;
}

/*
 * Exits /proc filesystem support.
 */
static void di_exit_proc(struct di_device *ddev)
{
#ifdef CONFIG_PROC_FS
#endif /* CONFIG_PROC_FS */
}


/*
 * Initializes the device.
 */
static int di_init(struct di_device *ddev, struct resource *mem, int irq)
{
	int retval;

	cpu_to_le32s(&generic_drive_code_entry_point);

	memset(ddev, 0, sizeof(*ddev) - sizeof(ddev->pdev));

	ddev->io_base = (void __iomem *)mem->start;
	ddev->irq = irq;

	retval = di_init_blk_dev(ddev);
	if (!retval) {
		retval = di_init_irq(ddev);
		if (retval) {
			di_exit_blk_dev(ddev);
		} else {
			di_init_proc(ddev);
		}
	}
	return retval;
}

/*
 * Exits the device.
 */
static void di_exit(struct di_device *ddev)
{
        di_exit_blk_dev(ddev);
        di_exit_irq(ddev);
	di_exit_proc(ddev);
}


/*
 * Needed for platform devices.
 */
static void di_dev_release(struct device *dev)
{
}

/*
 * Set of resources used by the disk interface device.
 */
static struct resource di_resources[] = {
        [0] = {
                .start = DI_BASE,
                .end = DI_BASE + DI_SIZE -1,
                .flags = IORESOURCE_MEM,
        },
        [1] = {
                .start = DI_IRQ,
                .end = DI_IRQ,
                .flags = IORESOURCE_IRQ,
        },
};


/*
 * The disk interface device.
 */
static struct di_device di_device = {
	.pdev = {
		.name = DI_NAME,
		.id = 0,
		.num_resources = ARRAY_SIZE(di_resources),
		.resource = di_resources,
		.dev = {
			.release = di_dev_release,
		},
	},
};


/*
 * Drive model probe function for our device.
 */
static int di_probe(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct di_device *ddev = to_di_device(pdev);
	struct resource *mem;
	int irq;
	int retval;

	retval = -ENODEV;
	irq = platform_get_irq(pdev, 0);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (mem) {
		retval = di_init(ddev, mem, irq);
	}
	return retval;
}

/*
 * Drive model remove function for our device.
 */
static int di_remove(struct device *device)
{
        struct platform_device *pdev = to_platform_device(device);
        struct di_device *ddev = to_di_device(pdev);
                                                                                
        di_exit(ddev);
                                                                                
        return 0;
}

/*
 * Drive model shutdown function for our device.
 */
static void di_shutdown(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct di_device *ddev = to_di_device(pdev);

	di_quiesce(ddev);
}


/*
 * The disk interface driver.
 */
static struct device_driver di_driver = {
	.name = DI_NAME,
	.bus = &platform_bus_type,
	.probe = di_probe,
	.remove = di_remove,
	.shutdown = di_shutdown,
};

/*
 * Module initialization routine.
 */
static int __init di_init_module(void)
{
	int retval = 0;

	di_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		  di_driver_version);

	retval = driver_register(&di_driver);
	if (!retval) {
		retval = platform_device_register(&di_device.pdev);
	}

	return retval;
}

/*
 * Module de-initialization routine.
 */
static void __exit di_exit_module(void)
{
        platform_device_unregister(&di_device.pdev);
        driver_unregister(&di_driver);
}

module_init(di_init_module);
module_exit(di_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

