/*
 * drivers/block/rvl-stsd.c
 *
 * Block driver for the Nintendo Wii SD front slot.
 * Copyright (C) 2008 The GameCube Linux Team
 * Copyright (C) 2008 Albert Herranz
 *
 * Based on drivers/block/gcn-sd.c
 *
 * Copyright (C) 2004-2008 The GameCube Linux Team
 * Copyright (C) 2004,2005 Rob Reylink
 * Copyright (C) 2005 Todd Jeffreys
 * Copyright (C) 2005,2006,2007,2008 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define DEBUG

#define DBG(fmt, arg...)	pr_debug(fmt, ##arg)
//#define DBG(fmt, arg...)	drv_printk(KERN_ERR, fmt, ##arg)

#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <asm/starlet.h>

/*
 * We are not a native MMC driver...
 * But anyway, we try to recycle here some of the available code.
 */
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>

#define DRV_MODULE_NAME "rvl-stsd"
#define DRV_DESCRIPTION "Block driver for the Nintendo Wii SD front slot"
#define DRV_AUTHOR      "Albert Herranz"

static char stsd_driver_version[] = "0.1i";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

/*
 * Driver settings.
 */
#define MMC_SHIFT		3	/* 8 partitions */

#define STSD_MAJOR		62
#define STSD_NAME		"rvlsd"

#define KERNEL_SECTOR_SHIFT     9
#define KERNEL_SECTOR_SIZE      (1 << KERNEL_SECTOR_SHIFT)	/*512 */


/*
 * IOS-related constants.
 */

/* ioctls */
#define STSD_IOCTL_SETHCREG	1
#define STSD_IOCTL_GETHCREG	2
#define STSD_IOCTL_RESET	4
#define STSD_IOCTL_SETCLOCK	6
#define STSD_IOCTL_SENDCMD	7
#define STSD_IOCTL_GETSTATUS	11
#define STSD_IOCTL_GETOCR	12

#define STSD_IOCTLV_READWRITE	7

/* SD command types */
#define STSD_CMDTYPE_BC		1
#define STSD_CMDTYPE_BCR	2
#define STSD_CMDTYPE_AC		3
#define STSD_CMDTYPE_ADTC	4

/* SD response types */
#define STSD_RSPTYPE_NONE	0
#define STSD_RSPTYPE_R1		1
#define STSD_RSPTYPE_R1B	2
#define STSD_RSPTYPE_R2		3
#define STSD_RSPTYPE_R3		4
#define STSD_RSPTYPE_R4		5
#define STSD_RSPTYPE_R5		6
#define STSD_RSPTYPE_R6		7
#define STSD_RSPTYPE_R7		8

/*
 * Hardware registers.
 */
#define STSD_HCR		0x28
#define STSD_HCR_4BIT		(1<<1)


static char stsd_dev_sdio_slot0[] = "/dev/sdio/slot0";

/*
 * Used to get/set the host controller hardware register values through IOS.
 */
struct stsd_reg_query {
	u32	addr;
	u32	_unk1;
	u32	_unk2;
	u32	size;
	void	*buf;
	u32	_unk3;
};

/*
 * Used to send commands to an SD card through IOS.
 */
struct stsd_command {
	u32 opcode;
	u32 cmdtype;
	u32 rsptype;
	u32 arg;
	u32 blk_count;
	u32 blk_size;
	dma_addr_t dma_addr;
	u32 _unk1;
	u32 _unk2;
};

struct stsd_transfer {
	struct stsd_command cmd;	/* must be the first member, aligned */

	dma_addr_t dma_addr;
	size_t size;
	enum dma_data_direction direction;

	struct scatterlist in[2], out[1];

	/* one-time initialized members */
	size_t blk_size;
	void *reply;
	size_t reply_len;
	struct stsd_host *host;
};

enum {
	__STSD_MEDIA_CHANGED = 0,
	__STSD_BAD_CARD,
};

struct stsd_host {
	spinlock_t	lock;
	unsigned long	flags;
#define STSD_MEDIA_CHANGED	(1<<__STSD_MEDIA_CHANGED)
#define STSD_BAD_CARD		(1<<__STSD_BAD_CARD)

	/* u32		ocr; */
	unsigned int	f_max;
	unsigned int	clock;
	u32		bus_width;

	u16		status;

	/* card related info */
	struct mmc_card	card;

	int refcnt;

	spinlock_t 		queue_lock;
	struct request_queue	*queue;
	struct gendisk 		*disk;
	unsigned int		max_phys_segments;

	struct stsd_transfer	*xfer;

	struct task_struct	*io_thread;
	struct mutex		io_mutex;

	int fd;
	struct device		*dev;
};


static const unsigned int tran_exp[] = {
	10000,		100000,		1000000,	10000000,
	0,		0,		0,		0
};

static const unsigned char tran_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};

static const unsigned int tacc_exp[] = {
	1,	10,	100,	1000,	10000,	100000,	1000000, 10000000,
};

static const unsigned int tacc_mant[] = {
	0,	10,	12,	13,	15,	20,	25,	30,
	35,	40,	45,	50,	55,	60,	70,	80,
};


/*
 * debug section
 *
 */

#if defined(DEBUG) && 0

#define __case_string(_s)	\
case _s:			\
	str = #_s;		\
	break;			

static char *stsd_opcode_string(u32 opcode)
{
	char *str = "unknown";

	switch(opcode) {
__case_string(MMC_GO_IDLE_STATE)
__case_string(MMC_SEND_OP_COND)
__case_string(MMC_ALL_SEND_CID)
__case_string(MMC_SET_RELATIVE_ADDR)
__case_string(MMC_SET_DSR)
__case_string(MMC_SWITCH)
__case_string(MMC_SELECT_CARD)
__case_string(MMC_SEND_EXT_CSD)
__case_string(MMC_SEND_CSD)
__case_string(MMC_SEND_CID)
__case_string(MMC_READ_DAT_UNTIL_STOP)
__case_string(MMC_STOP_TRANSMISSION)
__case_string(MMC_SEND_STATUS)
__case_string(MMC_GO_INACTIVE_STATE)
__case_string(MMC_SPI_READ_OCR)
__case_string(MMC_SPI_CRC_ON_OFF)
__case_string(MMC_SET_BLOCKLEN)
__case_string(MMC_READ_SINGLE_BLOCK)
__case_string(MMC_READ_MULTIPLE_BLOCK)
__case_string(MMC_WRITE_DAT_UNTIL_STOP)
__case_string(MMC_SET_BLOCK_COUNT)
__case_string(MMC_WRITE_BLOCK)
__case_string(MMC_WRITE_MULTIPLE_BLOCK)
__case_string(MMC_PROGRAM_CID)
__case_string(MMC_PROGRAM_CSD)
__case_string(MMC_SET_WRITE_PROT)
__case_string(MMC_CLR_WRITE_PROT)
__case_string(MMC_SEND_WRITE_PROT)
__case_string(MMC_ERASE_GROUP_START)
__case_string(MMC_ERASE_GROUP_END)
__case_string(MMC_ERASE)
__case_string(MMC_FAST_IO)
__case_string(MMC_GO_IRQ_STATE)
__case_string(MMC_LOCK_UNLOCK)
//__case_string(SD_SEND_RELATIVE_ADDR)
//__case_string(SD_SEND_IF_COND)
//__case_string(SD_SWITCH)
__case_string(SD_IO_SEND_OP_COND)
__case_string(SD_IO_RW_DIRECT)
__case_string(SD_IO_RW_EXTENDED)
	}

	return str;
}

static char *stsd_rsptype_string(u32 rsptype)
{
	char *str = "unknown";

	switch(rsptype) {
__case_string(STSD_RSPTYPE_NONE)
__case_string(STSD_RSPTYPE_R1)
__case_string(STSD_RSPTYPE_R1B)
__case_string(STSD_RSPTYPE_R2)
__case_string(STSD_RSPTYPE_R3)
__case_string(STSD_RSPTYPE_R4)
__case_string(STSD_RSPTYPE_R5)
__case_string(STSD_RSPTYPE_R6)
__case_string(STSD_RSPTYPE_R7)
	}

	return str;
}

static char *stsd_cmdtype_string(u32 cmdtype)
{
	char *str = "unknown";

	switch(cmdtype) {
__case_string(STSD_CMDTYPE_BC)
__case_string(STSD_CMDTYPE_BCR)
__case_string(STSD_CMDTYPE_AC)
__case_string(STSD_CMDTYPE_ADTC)
	}

	return str;
}

static char *stsd_statusbit_string(u32 statusbit)
{
	char *str = "unknown";

	switch(statusbit) {
__case_string(R1_OUT_OF_RANGE)
__case_string(R1_ADDRESS_ERROR)
__case_string(R1_BLOCK_LEN_ERROR)
__case_string(R1_ERASE_SEQ_ERROR)
__case_string(R1_ERASE_PARAM)
__case_string(R1_WP_VIOLATION)
__case_string(R1_CARD_IS_LOCKED)
__case_string(R1_LOCK_UNLOCK_FAILED)
__case_string(R1_COM_CRC_ERROR)
__case_string(R1_ILLEGAL_COMMAND)
__case_string(R1_CARD_ECC_FAILED)
__case_string(R1_CC_ERROR)
__case_string(R1_ERROR)
__case_string(R1_UNDERRUN)
__case_string(R1_OVERRUN)
__case_string(R1_CID_CSD_OVERWRITE)
__case_string(R1_WP_ERASE_SKIP)
__case_string(R1_CARD_ECC_DISABLED)
__case_string(R1_ERASE_RESET)
__case_string(R1_READY_FOR_DATA)
__case_string(R1_APP_CMD)
	}

	return str;
}

static char *stsd_card_state_string(u32 status)
{
	char *str = "unknown";

	switch(R1_CURRENT_STATE(status)) {
		case 0:
			str = "IDLE";
			break;
		case 1:
			str ="READY";
			break;
		case 2:
			str ="IDENT";
			break;
		case 3:
			str ="STANDBY";
			break;
		case 4:
			str ="TRANSFER";
			break;
		case 5:
			str = "SEND";
			break;
		case 6:
			str = "RECEIVE";
			break;
		case 7:
			str = "PROGRAM";
			break;
		case 8:
			str = "DISCONNECT";
			break;
	}

	return str;
}
static void stsd_print_status(u32 status)
{
	u32 i, bit;

	drv_printk(KERN_INFO, "card state %s\n", stsd_card_state_string(status));

	i = 13;
	for (i = 13; i <= 31; i++) {
		bit = 1 << i;
		if ((status & bit))
			drv_printk(KERN_INFO, "%02d %s\n", i,
				   stsd_statusbit_string(bit));
	}
	bit = 1 << 8;
	if ((status & bit))
		drv_printk(KERN_INFO, "%02d %s\n", 8,
			   stsd_statusbit_string(bit));
	bit = 1 << 5;
	if ((status & bit))
		drv_printk(KERN_INFO, "%02d %s\n", 5,
			   stsd_statusbit_string(bit));
}

static void stsd_print_cid(struct mmc_cid *cid)
{
	drv_printk(KERN_INFO,
		  "manfid = %d\n"
		  "oemid = %d\n"
		  "prod_name = %s\n"
		  "hwrev = %d\n"
		  "fwrev = %d\n"
		  "serial = %08x\n"
		  "year = %d\n"
		  "month = %d\n",
		  cid->manfid,
		  cid->oemid,
		  cid->prod_name,
		  cid->hwrev, cid->fwrev, cid->serial, cid->year, cid->month);
}

static void stsd_print_csd(struct mmc_csd *csd)
{
	drv_printk(KERN_INFO,
		  "mmca_vsn = %d\n"
		  "cmdclass = %d\n"
		  "tacc_clks = %d\n"
		  "tacc_ns = %d\n"
		  "r2w_factor = %d\n"
		  "max_dtr = %d\n"
		  "read_blkbits = %d\n"
		  "write_blkbits = %d\n"
		  "capacity = %d\n"
		  "read_partial = %d\n"
		  "read_misalign = %d\n"
		  "write_partial = %d\n"
		  "write_misalign = %d\n",
		  csd->mmca_vsn,
		  csd->cmdclass,
		  csd->tacc_clks,
		  csd->tacc_ns,
		  csd->r2w_factor,
		  csd->max_dtr,
		  csd->read_blkbits,
		  csd->write_blkbits,
		  csd->capacity,
		  csd->read_partial,
		  csd->read_misalign,
		  csd->write_partial,
		  csd->write_misalign);
}

#endif /* DEBUG */

/*
 *
 * MMC/SD data structures manipulation.
 * Borrowed from MMC layer.
 */

#define UNSTUFF_BITS(resp,start,size)					\
	({								\
		const int __size = size;				\
		const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1;	\
		const int __off = 3 - ((start) / 32);			\
		const int __shft = (start) & 31;			\
		u32 __res;						\
									\
		__res = resp[__off] >> __shft;				\
		if (__size + __shft > 32)				\
			__res |= resp[__off-1] << ((32 - __shft) % 32);	\
		__res & __mask;						\
	})

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
static void mmc_decode_cid(struct mmc_card *card)
{
	u32 *resp = card->raw_cid;

	memset(&card->cid, 0, sizeof(struct mmc_cid));

	/*
	 * SD doesn't currently have a version field so we will
	 * have to assume we can parse this.
	 */
	card->cid.manfid		= UNSTUFF_BITS(resp, 120, 8);
	card->cid.oemid			= UNSTUFF_BITS(resp, 104, 16);
	card->cid.prod_name[0]		= UNSTUFF_BITS(resp, 96, 8);
	card->cid.prod_name[1]		= UNSTUFF_BITS(resp, 88, 8);
	card->cid.prod_name[2]		= UNSTUFF_BITS(resp, 80, 8);
	card->cid.prod_name[3]		= UNSTUFF_BITS(resp, 72, 8);
	card->cid.prod_name[4]		= UNSTUFF_BITS(resp, 64, 8);
	card->cid.hwrev			= UNSTUFF_BITS(resp, 60, 4);
	card->cid.fwrev			= UNSTUFF_BITS(resp, 56, 4);
	card->cid.serial		= UNSTUFF_BITS(resp, 24, 32);
	card->cid.year			= UNSTUFF_BITS(resp, 12, 8);
	card->cid.month			= UNSTUFF_BITS(resp, 8, 4);

	card->cid.year += 2000; /* SD cards year offset */
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static int mmc_decode_csd(struct mmc_card *card)
{
	struct mmc_csd *csd = &card->csd;
	unsigned int e, m, csd_struct;
	u32 *resp = card->raw_csd;

	csd_struct = UNSTUFF_BITS(resp, 126, 2);

	switch (csd_struct) {
	case 0:
		m = UNSTUFF_BITS(resp, 115, 4);
		e = UNSTUFF_BITS(resp, 112, 3);
		csd->tacc_ns	 = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
		csd->tacc_clks	 = UNSTUFF_BITS(resp, 104, 8) * 100;

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

		e = UNSTUFF_BITS(resp, 47, 3);
		m = UNSTUFF_BITS(resp, 62, 12);
		csd->capacity	  = (1 + m) << (e + 2);

		csd->read_blkbits = UNSTUFF_BITS(resp, 80, 4);
		csd->read_partial = UNSTUFF_BITS(resp, 79, 1);
		csd->write_misalign = UNSTUFF_BITS(resp, 78, 1);
		csd->read_misalign = UNSTUFF_BITS(resp, 77, 1);
		csd->r2w_factor = UNSTUFF_BITS(resp, 26, 3);
		csd->write_blkbits = UNSTUFF_BITS(resp, 22, 4);
		csd->write_partial = UNSTUFF_BITS(resp, 21, 1);
		break;
	case 1:
		/*
		 * This is a block-addressed SDHC card. Most
		 * interesting fields are unused and have fixed
		 * values. To avoid getting tripped by buggy cards,
		 * we assume those fixed values ourselves.
		 */
		mmc_card_set_blockaddr(card);

		csd->tacc_ns	 = 0; /* Unused */
		csd->tacc_clks	 = 0; /* Unused */

		m = UNSTUFF_BITS(resp, 99, 4);
		e = UNSTUFF_BITS(resp, 96, 3);
		csd->max_dtr	  = tran_exp[e] * tran_mant[m];
		csd->cmdclass	  = UNSTUFF_BITS(resp, 84, 12);

		m = UNSTUFF_BITS(resp, 48, 22);
		csd->capacity     = (1 + m) << 10;

		csd->read_blkbits = 9;
		csd->read_partial = 0;
		csd->write_misalign = 0;
		csd->read_misalign = 0;
		csd->r2w_factor = 4; /* Unused */
		csd->write_blkbits = 9;
		csd->write_partial = 0;
		break;
	default:
		printk(KERN_ERR "unrecognised CSD structure version %d\n",
			csd_struct);
		return -EINVAL;
	}

	//stsd_print_csd(csd);

	return 0;
}

/*
 * REVISIT maybe get rid of this and specify the rsptype directly
 */
static u32 stsd_opcode_to_rsptype(u32 opcode)
{
	u32 rsptype = STSD_RSPTYPE_R1;

	switch (opcode) {
	case MMC_GO_IDLE_STATE:
	case MMC_SET_DSR:
	case MMC_GO_INACTIVE_STATE:
		rsptype = STSD_RSPTYPE_NONE;
		break;
	case MMC_SWITCH:
	case MMC_STOP_TRANSMISSION:
	case MMC_SET_WRITE_PROT:
	case MMC_CLR_WRITE_PROT:
	case MMC_ERASE:
	case MMC_LOCK_UNLOCK:
		rsptype = STSD_RSPTYPE_R1B;
		break;
	case MMC_ALL_SEND_CID:
	case MMC_SEND_CSD:
	case MMC_SEND_CID:
		rsptype = STSD_RSPTYPE_R2;
		break;
	case MMC_SEND_OP_COND:
	case SD_APP_OP_COND:
		rsptype = STSD_RSPTYPE_R3;
		break;
	case MMC_FAST_IO:
	case SD_IO_SEND_OP_COND:
		rsptype = STSD_RSPTYPE_R4;
		break;
	case MMC_GO_IRQ_STATE:
	case SD_IO_RW_DIRECT:
	case SD_IO_RW_EXTENDED:
		rsptype = STSD_RSPTYPE_R5;
		break;
	case SD_SEND_RELATIVE_ADDR:
		rsptype = STSD_RSPTYPE_R6;
		break;
	case SD_SEND_IF_COND:
		rsptype = STSD_RSPTYPE_R7;
		break;
	default:
		break;
	}

	return rsptype;
}

static void stsd_card_set_bad(struct stsd_host *host)
{
	set_bit(__STSD_BAD_CARD, &host->flags);
}

static int stsd_card_is_bad(struct stsd_host *host)
{
	return test_bit(__STSD_BAD_CARD, &host->flags);
}

/*
 * Hardware.
 *
 */

/*
 * @data must be aligned
 * @size must be between 1 and 4
 */
static int __stsd_inout(struct stsd_host *host, int write,
			u32 addr, u32 *data, size_t size)
{
	const unsigned int data_size = sizeof(u32);
	struct stsd_reg_query *query;
	int request;
	void *buf_a, *buf_b;
	int error;

	/*
	 * REVISIT maybe use a dma pool or small buffer for query data
	 */
	query = starlet_kzalloc(sizeof(*query), GFP_ATOMIC);
	if (!query)
		return -ENOMEM;

	if (write) {
		buf_a = NULL;
		buf_b = data;
		request = STSD_IOCTL_SETHCREG;
	} else {
		buf_a = data;
		buf_b = NULL;
		request = STSD_IOCTL_GETHCREG;
	}

	query->addr = addr;
	query->size = size;
	query->buf = buf_b; /* data to starlet */

	error = starlet_ioctl(host->fd, request,
				  query, sizeof(*query), buf_a, data_size);

	starlet_kfree(query);

	return error;
}


/*
 * Handy small buffer routines.
 * We use a small static aligned buffer to avoid allocations for short-lived
 * operations involving 1 to 4 byte data transfers to/from IOS.
 *
 */

static u32 stsd_small_buf[L1_CACHE_BYTES / sizeof(u32)]
		    __attribute__ ((aligned(STARLET_IPC_DMA_ALIGN + 1)));
static const size_t stsd_small_buf_size = sizeof(stsd_small_buf_size);
static DEFINE_MUTEX(stsd_small_buf_lock);

static u32 *stsd_small_buf_get(void)
{
	u32 *buf;

	if (!mutex_trylock(&stsd_small_buf_lock))
		buf = starlet_kzalloc(stsd_small_buf_size, GFP_NOIO);
	else {
		memset(stsd_small_buf, 0, stsd_small_buf_size);
		buf = stsd_small_buf;
	}

	return buf;
}

void stsd_small_buf_put(u32 *buf)
{
	if (buf == stsd_small_buf)
		mutex_unlock(&stsd_small_buf_lock);
	else
		starlet_kfree(buf);
}


/*
 * Host Controller hardware registers accessors.
 *
 */

static int stsd_in(struct stsd_host *host, 
		   u32 reg, void *buf, size_t size)
{
	void *local_buf;
	int error;

	/* we do 8, 16 and 32 bits reads */
	if (size > stsd_small_buf_size)
		return -EINVAL;

	local_buf = stsd_small_buf_get();
	if (!local_buf)
		return -ENOMEM;

	error = __stsd_inout(host, 0, reg, local_buf, size);
	if (!error)
		memcpy(buf, local_buf, size);

	stsd_small_buf_put(local_buf);

	return error;
}

static int stsd_out(struct stsd_host *host, 
		    u32 reg, void *buf, size_t size)
{
	void *local_buf;
	int error;

	/* we do 8, 16 and 32 bits writes */
	if (size > stsd_small_buf_size)
		return -EINVAL;

	local_buf = stsd_small_buf_get();
	if (!local_buf)
		return -ENOMEM;

	memcpy(local_buf, buf, size);
	error = __stsd_inout(host, 1, reg, local_buf, size);

	stsd_small_buf_put(local_buf);

	return error;
}

#define __declare_stsd_in(_type)					\
static inline u8 stsd_in_##_type(struct stsd_host *host, u32 reg)	\
{									\
	_type val;							\
									\
	stsd_in(host, reg, &val, sizeof(val));				\
	return val;							\
}

__declare_stsd_in(u8);
//__declare_stsd_in(u16);
//__declare_stsd_in(u32);

#define __declare_stsd_out(_type)					\
static inline void stsd_out_##_type(struct stsd_host *host, u32 reg, 	\
					 _type val)			\
{									\
	stsd_out(host, reg, &val, sizeof(val));				\
}

__declare_stsd_out(u8);
//__declare_stsd_out(u16);
//__declare_stsd_out(u32);


/*
 * Ioctl helpers.
 *
 */

static int stsd_ioctl_small_read(struct stsd_host *host, int request,
				 void *buf, size_t size)
{
	void *local_buf;
	int error;

	/* we do 8, 16 and 32 bits reads */
	if (size > stsd_small_buf_size) {
		error = -EINVAL;
		goto done;
	}

	local_buf = stsd_small_buf_get();
	if (!local_buf) {
		error = -ENOMEM;
		goto done;
	}

	error = starlet_ioctl(host->fd, request,
				   NULL, 0, local_buf, size);
	if (!error)
		memcpy(buf, local_buf, size);

	stsd_small_buf_put(local_buf);

done:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}

static int stsd_ioctl_small_write(struct stsd_host *host, int request,
				  void *buf, size_t size)
{
	void *local_buf;
	int error;

	/* we do 8, 16 and 32 bits writes */
	if (size > stsd_small_buf_size) {
		error = -EINVAL;
		goto done;
	}

	local_buf = stsd_small_buf_get();
	if (!local_buf) {
		error = -ENOMEM;
		goto done;
	}

	memcpy(local_buf, buf, size);
	error = starlet_ioctl(host->fd, request,
				   local_buf, size, NULL, 0);

	stsd_small_buf_put(local_buf);

done:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}


/*
 * Hardware interfaces.
 *
 */

static int stsd_get_status(struct stsd_host *host, u32 *status)
{
	int error;

	error = stsd_ioctl_small_read(host, STSD_IOCTL_GETSTATUS,
				       status, sizeof(*status));
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}

#if 0
static int stsd_get_ocr(struct stsd_host *host)
{
	int error;
	u32 ocr;

	error = stsd_ioctl_small_read(host, STSD_IOCTL_GETOCR,
				       &ocr, sizeof(ocr));
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	else
		host->ocr = ocr;

	return error;
}
#endif

static void stsd_set_bus_width(struct stsd_host *host, int width)
{
	u8 hcr;

	hcr = stsd_in_u8(host, STSD_HCR);
	if (width == 4) {
		hcr |= STSD_HCR_4BIT;
	} else {
		hcr &= ~STSD_HCR_4BIT;
		width = 1;
	}
	stsd_out_u8(host, STSD_HCR, hcr);
	host->bus_width = width;
}

static int stsd_set_clock(struct stsd_host *host, unsigned int clock)
{
	int error;
	u32 divisor;

	for (divisor = 1; divisor <= 32; divisor <<= 1) {
		if (host->f_max / divisor <= clock)
			break;
	}

	error = stsd_ioctl_small_write(host, STSD_IOCTL_SETCLOCK,
					&divisor, sizeof(divisor));
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	else
		host->clock = clock;

	return error;
}

static int stsd_reset_card(struct stsd_host *host)
{
	int error;
	u32 status;

	error = stsd_ioctl_small_read(host, STSD_IOCTL_RESET,
				       &status, sizeof(status));
	if (error) {
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	} else {
		host->card.rca = status >> 16;
		host->status = status & 0xffff;
	}

	return error;
}

static int stsd_send_command(struct stsd_host *host,
			     u32 opcode, u32 type, u32 arg,
			     void *buf, size_t buf_len)
{
	struct stsd_command *cmd;
	u32 *reply;
	size_t reply_len;
	int error;

	reply_len = 4 * sizeof(u32);
	if (buf_len > reply_len)
		return -EINVAL;
	
	cmd = starlet_kzalloc(sizeof(*cmd), GFP_NOIO);
	if (!cmd)
		return -ENOMEM;

	reply = starlet_kzalloc(reply_len, GFP_NOIO);
	if (!reply) {
		starlet_kfree(cmd);
		return -ENOMEM;
	}

	cmd->opcode = opcode;
	cmd->arg = arg;

	cmd->cmdtype = type;
	cmd->rsptype = stsd_opcode_to_rsptype(opcode);
	if (opcode == MMC_SELECT_CARD && arg == 0)
		cmd->rsptype = STSD_RSPTYPE_NONE;

	error = starlet_ioctl(host->fd, STSD_IOCTL_SENDCMD,
				   cmd, sizeof(*cmd), reply, reply_len);
	if (error) {
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	} else {
		memcpy(buf, reply, buf_len);
	}

	starlet_kfree(reply);
	starlet_kfree(cmd);

	return error;
}

static int stsd_read_cxd(struct stsd_host *host, int request, void *buf)
{
	int error;
	u32 *q, savedq;
	u8 *p, crc;
	const size_t size = 128/8*sizeof(u8);

	error = stsd_send_command(host, request, STSD_CMDTYPE_AC, 
				   host->card.rca << 16, buf, size);

	if (!error) {
		/*
		 * starlet sends CSD and CID contents in a very special way.
		 *
		 * If the 128 bit register value is:
		 *   0123456789abcdef
		 * starlet will send it as:
		 *   bcde789a3456f012
		 * with byte f (the crc field) zeroed.
		 */

		/* bcde789a3456f012 -> f0123456789abcde */
		q = buf;
		savedq = q[0];
		q[0] = q[3];
		q[3] = savedq;
		savedq = q[1];
		q[1] = q[2];
		q[2] = savedq;

		/* f0123456789abcde -> 0123456789abcdef */
		p = buf;
		crc = p[0];
		memcpy(p, p+1, size-1);
		p[size-1] = crc;
	} else {
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	}
	return error;
}

static int stsd_read_csd(struct stsd_host *host)
{
	return stsd_read_cxd(host, MMC_SEND_CSD, host->card.raw_csd);
}

static int stsd_read_cid(struct stsd_host *host)
{
	return stsd_read_cxd(host, MMC_SEND_CID, host->card.raw_cid);
}

static int stsd_select_card(struct stsd_host *host)
{
	return stsd_send_command(host, MMC_SELECT_CARD, STSD_CMDTYPE_AC,
				 host->card.rca << 16,
				 NULL, 0);
}

static int stsd_deselect_card(struct stsd_host *host)
{
	return stsd_send_command(host, MMC_SELECT_CARD, STSD_CMDTYPE_AC,
				 0,
				 NULL, 0);
}

static int stsd_set_block_len(struct stsd_host *host, unsigned int len)
{
	return stsd_send_command(host, MMC_SET_BLOCKLEN, STSD_CMDTYPE_AC,
				 len,
				 NULL, 0);
}

static int stsd_welcome_card(struct stsd_host *host)
{
	u32 status;
	int i;
	int error;

	mutex_lock(&host->io_mutex);

	/*
	 * Re-open the sdio device if things look wrong.
	 */
	error = stsd_get_status(host, &status);
	if (error == STARLET_EINVAL) {
		starlet_close(host->fd);
		host->fd = starlet_open(stsd_dev_sdio_slot0, 0);
		if (host->fd < 0) {
			drv_printk(KERN_ERR, "unable to re-open %s\n",
				   stsd_dev_sdio_slot0);
			error = -ENODEV;
			goto err_bad_card;
		}
	}

	/* reset the card, maybe several times before giving up */
	for (i = 0; i < 3; i++) {
		error = stsd_reset_card(host);
		if (!error)
			break;
	}
	if (error)
		goto err_bad_card;

#if 0
	/* read Operating Conditions Register */
	error = stsd_get_ocr(host);
	if (error < 0)
		goto err_bad_card;
#endif

	error = stsd_deselect_card(host);
	if (error)
		goto err_bad_card;

	/* read and decode the Card Specific Data */
	error = stsd_read_csd(host);
	if (error)
		goto err_bad_card;
	mmc_decode_csd(&host->card);

	/* read and decode the Card Identification Data */
	error = stsd_read_cid(host);
	if (error)
		goto err_bad_card;
	mmc_decode_cid(&host->card);

	error = stsd_select_card(host);
	if (error)
		goto err_bad_card;

	mutex_unlock(&host->io_mutex);

	error = stsd_set_clock(host, host->card.csd.max_dtr);

	/* FIXME check if card supports 4 bit bus width */
	stsd_set_bus_width(host, 4);

	mmc_card_set_present(&host->card);

	drv_printk(KERN_INFO, "descr \"%s\", size %luk, block %ub,"
		  " serial %08x\n",
		  host->card.cid.prod_name,
		  (unsigned long)((host->card.csd.capacity *
			  (1 << host->card.csd.read_blkbits)) / 1024),
	          1 << host->card.csd.read_blkbits,
		  host->card.cid.serial);

	error = 0;
	goto out;

err_bad_card:
	mutex_unlock(&host->io_mutex);
	stsd_card_set_bad(host);
out:
	return error;
}


/*
 * Block layer helper routines.
 *
 */

static int stsd_do_block_transfer(struct stsd_host *host, int write,
				  unsigned long start,
				  void *buf, size_t nr_blocks)
{
	struct stsd_transfer *xfer = host->xfer;
	struct stsd_command *cmd = &xfer->cmd;
	struct scatterlist *sgl;
	int error;

	/*
	 * This is stupid.
	 * Starlet expects the buffer to be an input iovec (from starlet
	 * point of view) even for reads. Thus, map the buffer explicitly here.
	 */

	xfer->direction = (write)?DMA_TO_DEVICE:DMA_FROM_DEVICE;
	xfer->size = nr_blocks * xfer->blk_size;
	xfer->dma_addr = dma_map_single(host->dev, buf,
					xfer->size, xfer->direction);

	sgl = xfer->in;
	sg_init_table(sgl, 2);
	sg_set_buf(sgl, cmd, sizeof(*cmd));
	sg_set_buf(sgl+1, buf, xfer->size);

	sgl = xfer->out;
	sg_init_table(sgl, 1);
	sg_set_buf(sgl, xfer->reply, xfer->reply_len);

	cmd->opcode = (write)?MMC_WRITE_MULTIPLE_BLOCK:MMC_READ_MULTIPLE_BLOCK;
	cmd->arg = start;
	cmd->cmdtype = STSD_CMDTYPE_AC; /* STSD_CMDTYPE_ADTC */
	cmd->rsptype = stsd_opcode_to_rsptype(cmd->opcode);
	cmd->blk_count = nr_blocks;
	cmd->blk_size = xfer->blk_size;
	cmd->dma_addr = xfer->dma_addr;
	cmd->_unk1 = 1;

	error = starlet_ioctlv(host->fd, STSD_IOCTLV_READWRITE,
				    2, xfer->in, 1, xfer->out);

	dma_unmap_single(xfer->host->dev,
			 xfer->dma_addr, xfer->size, xfer->direction);

	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);

	return error;
}

/*
 * Returns >0 if a request should be dispatched.
 */
static int stsd_check_request(struct stsd_host *host, struct request *req)
{
	unsigned long nr_sectors;

	if (test_bit(__STSD_MEDIA_CHANGED, &host->flags)) {
		drv_printk(KERN_ERR, "media changed, aborting\n");
		return -ENOMEDIUM;
	}

	/* unit is kernel sectors */
	nr_sectors =
	    host->card.csd.capacity << (host->card.csd.read_blkbits -
					KERNEL_SECTOR_SHIFT);

	/* keep our reads within limits */
	if (req->sector + req->current_nr_sectors > nr_sectors) {
		drv_printk(KERN_ERR, "reading past end, aborting\n");
		return -EINVAL;
	}

	if (!blk_fs_request(req))
		return 0;

	return 1;
}

static int stsd_do_request(struct stsd_host *host, struct request *req)
{
	unsigned long nr_blocks; /* in card blocks */
	size_t block_len; /* in bytes */
	unsigned long start;
	int write;
	int uptodate;
	int error;

	uptodate = stsd_check_request(host, req);
	if (uptodate <= 0)
		return uptodate;

	write = (rq_data_dir(req) == READ)?0:1;

	block_len = KERNEL_SECTOR_SIZE;
	stsd_set_block_len(host, block_len);

	start = req->sector << KERNEL_SECTOR_SHIFT;
	nr_blocks = req->current_nr_sectors;

	uptodate = 1;
	error = stsd_do_block_transfer(host, write,
					start, req->buffer, nr_blocks);
	if (error) {
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
		uptodate = error;
	}

	return uptodate;
}

static int stsd_io_thread(void *param)
{
        struct stsd_host *host = param;
	struct request *req;
	int uptodate;
	unsigned long flags;

        current->flags |= PF_NOFREEZE|PF_MEMALLOC;

	mutex_lock(&host->io_mutex);
	for(;;) {
		req = NULL;
		set_current_state(TASK_INTERRUPTIBLE);

		spin_lock_irqsave(&host->queue_lock, flags);
		if (!blk_queue_plugged(host->queue))
			req = elv_next_request(host->queue);
		spin_unlock_irqrestore(&host->queue_lock, flags);

		if (!req) {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
			mutex_unlock(&host->io_mutex);
			schedule();
			mutex_lock(&host->io_mutex);
			continue;
		}
		set_current_state(TASK_INTERRUPTIBLE);
		uptodate = stsd_do_request(host, req);

		spin_lock_irqsave(&host->queue_lock, flags);
		end_queued_request(req, uptodate);
		spin_unlock_irqrestore(&host->queue_lock, flags);
	}
	mutex_unlock(&host->io_mutex);

	return 0;
}

static void stsd_request_func(struct request_queue *q)
{
	struct stsd_host *host = q->queuedata;

	wake_up_process(host->io_thread);
}

/*
 * Block device hooks.
 *
 */

static DECLARE_MUTEX(open_lock);

static int stsd_open(struct inode *inode, struct file *filp)
{
	struct stsd_host *host = inode->i_bdev->bd_disk->private_data;
	int error = 0;

	if (!host || host->fd < 0)
		return -ENXIO;

	/* honor exclusive open mode */
	if (host->refcnt == -1 ||
	    (host->refcnt && (filp->f_flags & O_EXCL))) {
		error = -EBUSY;
		goto out;
	}

	/* this takes care of revalidating the media if needed */
	check_disk_change(inode->i_bdev);
	if (!host->card.csd.capacity) {
		error = -ENOMEDIUM;
		goto out;
	}

	down(&open_lock);

	if ((filp->f_flags & O_EXCL))
		host->refcnt = -1;
	else
		host->refcnt++;

	up(&open_lock);

out:
	return error;

}

static int stsd_release(struct inode *inode, struct file *filp)
{
	struct stsd_host *host = inode->i_bdev->bd_disk->private_data;

	if (!host)
		return -ENXIO;

	down(&open_lock);

	if (host->refcnt > 0)
		host->refcnt--;
	else {
		host->refcnt = 0;
	}

	up(&open_lock);

        if (!host->refcnt && host->fd == -1)
                kfree(host);

	return 0;
}

static int stsd_media_changed(struct gendisk *disk)
{
	struct stsd_host *host = disk->private_data;
	unsigned int last_serial;
	int error;

	/* report a media change for zombies */
	if (!host)
		return 1;

	/* report a media change if someone forced it */
	if (test_bit(__STSD_MEDIA_CHANGED, &host->flags))
		return 1;

	/* REVISIT use the starlet provided iotcl to check the status */

	mutex_lock(&host->io_mutex);

	/* check if the serial number of the card changed */
	last_serial = host->card.cid.serial;
	error = stsd_deselect_card(host);
	if (!error) {
		error = stsd_read_cid(host);
		if (!error)
			error = stsd_select_card(host);
	}

	mutex_unlock(&host->io_mutex);

	if (!error && last_serial == host->card.cid.serial && last_serial) {
		clear_bit(__STSD_MEDIA_CHANGED, &host->flags);
	} else {
		set_bit(__STSD_MEDIA_CHANGED, &host->flags);
	}

	return (host->flags & STSD_MEDIA_CHANGED) ? 1 : 0;
}

static int stsd_revalidate_disk(struct gendisk *disk)
{
	struct stsd_host *host = disk->private_data;
	int error = 0;

	/* report missing medium for zombies */
	if (!host) {
		error = -ENOMEDIUM;
		goto out;
	}

	/* the block layer likes to call us multiple times... */
	if (!stsd_media_changed(host->disk))
		goto out;

	/* get the card into a known status */
	error = stsd_welcome_card(host);
	if (error < 0 || stsd_card_is_bad(host)) {
		drv_printk(KERN_ERR, "card welcome failed\n");
		if (stsd_card_is_bad(host))
			drv_printk(KERN_ERR, "stsd_card_is_bad() true\n");
		if (error < 0)
			drv_printk(KERN_ERR, "error = %d\n", error);
		error = -ENOMEDIUM;
		goto out;
	}

	/* inform the block layer about various sizes */
	blk_queue_hardsect_size(host->queue, KERNEL_SECTOR_SIZE);
	set_capacity(host->disk, host->card.csd.capacity <<
			 (host->card.csd.read_blkbits - KERNEL_SECTOR_SHIFT));

	clear_bit(__STSD_MEDIA_CHANGED, &host->flags);

out:
	if (error)
		DBG("%s: error=%d (%08x)\n", __func__, error, error);
	return error;
}

static int stsd_ioctl(struct inode *inode, struct file *filp,
		    unsigned int cmd, unsigned long arg)
{
	struct block_device *bdev = inode->i_bdev;
	struct hd_geometry geo;

	switch (cmd) {
	case HDIO_GETGEO:
		/* fake the entries */
		geo.cylinders = get_capacity(bdev->bd_disk) / (4 * 16);
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = get_start_sect(bdev);

		if (copy_to_user((void __user *)arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
	return -EINVAL;
}

static struct block_device_operations stsd_fops = {
	.owner = THIS_MODULE,
	.open = stsd_open,
	.release = stsd_release,
	.revalidate_disk = stsd_revalidate_disk,
	.media_changed = stsd_media_changed,
	.ioctl = stsd_ioctl,
};

/*
 * Setup routines.
 *
 */

static int stsd_init_xfer(struct stsd_host *host)
{
	struct stsd_transfer *xfer;

	xfer = starlet_kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return -ENOMEM;

	xfer->reply_len = 4 * sizeof(u32);
	xfer->reply = starlet_kzalloc(xfer->reply_len, GFP_KERNEL);
	if (!xfer->reply) {
		starlet_kfree(xfer);
		return -ENOMEM;
	}

	xfer->host = host;
	xfer->blk_size = KERNEL_SECTOR_SIZE;

	host->xfer = xfer;

	return 0;
}

static void stsd_exit_xfer(struct stsd_host *host)
{
	struct stsd_transfer *xfer = host->xfer;

	starlet_kfree(xfer->reply);
	starlet_kfree(host->xfer);
}

static int stsd_init_blk_dev(struct stsd_host *host)
{
	struct gendisk *disk;
	struct request_queue *queue;
	int error;

	mutex_init(&host->io_mutex);

	/* queue */
	error = -ENOMEM;
	spin_lock_init(&host->queue_lock);
	queue = blk_init_queue(stsd_request_func, &host->queue_lock);
	if (!queue) {
		drv_printk(KERN_ERR, "error initializing queue\n");
		goto err_blk_init_queue;
	}
	host->max_phys_segments = 1;
	blk_queue_max_phys_segments(queue, host->max_phys_segments);
	blk_queue_max_hw_segments(queue, host->max_phys_segments);
	blk_queue_max_sectors(queue, 16); /* 16 * 512 = 8K */
	blk_queue_dma_alignment(queue, STARLET_IPC_DMA_ALIGN);
	queue->queuedata = host;
	host->queue = queue;

	/* disk */
	disk = alloc_disk(1 << MMC_SHIFT);
	if (!disk) {
		drv_printk(KERN_ERR, "error allocating disk\n");
		goto err_alloc_disk;
	}
	disk->major = STSD_MAJOR;
	disk->first_minor = 0 << MMC_SHIFT;
	disk->fops = &stsd_fops;
	sprintf(disk->disk_name, "%s%c", STSD_NAME, 'a');
	disk->private_data = host;
	disk->queue = host->queue;
	host->disk = disk;

	error = 0;
	goto out;

err_alloc_disk:
	blk_cleanup_queue(host->queue);
	host->queue = NULL;
err_blk_init_queue:
out:
	return error;
}

static void stsd_exit_blk_dev(struct stsd_host *host)
{
	blk_cleanup_queue(host->queue);
	put_disk(host->disk);
}

static int stsd_init_io_thread(struct stsd_host *host)
{
	int result = 0;

	host->io_thread = kthread_run(stsd_io_thread, host, "ksdio");
	if (IS_ERR(host->io_thread)) {
		drv_printk(KERN_ERR, "error creating io thread\n");
		result = PTR_ERR(host->io_thread);
	}
	return result;
}

static void stsd_exit_io_thread(struct stsd_host *host)
{
	if (!IS_ERR(host->io_thread)) {
		wake_up_process(host->io_thread);
		kthread_stop(host->io_thread);
		host->io_thread = ERR_PTR(-EINVAL);
	}
}

static int stsd_init(struct stsd_host *host)
{
	int error;

	host->refcnt = 0;
	spin_lock_init(&host->lock);
	set_bit(__STSD_MEDIA_CHANGED, &host->flags);
	host->f_max = 25000000; /* 25MHz */

	host->fd = starlet_open(stsd_dev_sdio_slot0, 0);
	if (host->fd < 0) {
		drv_printk(KERN_ERR, "unable to open %s\n",
			   stsd_dev_sdio_slot0);
		return -ENODEV;
	}

	error = stsd_init_blk_dev(host);
	if (error)
		goto out;

	error = stsd_init_xfer(host);
	if (error)
		goto err_blk_dev;

	error = stsd_revalidate_disk(host->disk);
#if 0
	if (error < 0 || !mmc_card_present(&host->card)) {
		error = -ENODEV;
		goto err_xfer;
	}
#endif

	error = stsd_init_io_thread(host);
	if (error) 
		goto err_xfer;

	add_disk(host->disk);

	return 0;

err_xfer:
	stsd_exit_xfer(host);
err_blk_dev:
	stsd_exit_blk_dev(host);
out:
	return error;
}

static void stsd_exit(struct stsd_host *host)
{
	del_gendisk(host->disk);
	stsd_exit_io_thread(host);
	stsd_exit_xfer(host);
	stsd_exit_blk_dev(host);
	if (host->fd >= 0)
		starlet_close(host->fd);
	host->fd = -1;

}

static void stsd_kill(struct stsd_host *host)
{
	if (host->refcnt > 0) {
		drv_printk(KERN_ERR, "hey! card removed while in use!\n");
		set_bit(__STSD_MEDIA_CHANGED, &host->flags);
	}

	stsd_exit(host);

	/* release the host immediately when not in use */
	if (!host->refcnt)
		kfree(host);
}

/*
 * Driver model helper routines.
 *
 */

static int __devinit stsd_do_probe(struct device *dev)
{
	struct stsd_host *host;
	int error;

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host) {
		drv_printk(KERN_ERR, "%s: failed to allocate stsd_host\n",
			    __func__);
		return -ENOMEM;
	}
	dev_set_drvdata(dev, host);
	host->dev = dev;

	error = stsd_init(host);
	if (error) {
		kfree(host);
		dev_set_drvdata(dev, NULL);
	}

	return error;
}

static int __devexit stsd_do_remove(struct device *dev)
{
	struct stsd_host *host = dev_get_drvdata(dev);

	if (!host)
		return -ENODEV;

	stsd_kill(host);
	dev_set_drvdata(dev, NULL);

	return 0;
}

/*
 * OF platform device routines.
 *
 */

static int __init stsd_of_probe(struct of_device *odev,
				const struct of_device_id *match)
{
	return stsd_do_probe(&odev->dev);
}

static int __exit stsd_of_remove(struct of_device *odev)
{
	return stsd_do_remove(&odev->dev);
}

static struct of_device_id stsd_of_match[] = {
	{ .compatible = "nintendo,starlet-sd" },
	{ },
};

MODULE_DEVICE_TABLE(of, stsd_of_match);

static struct of_platform_driver stsd_of_driver = {
	.owner = THIS_MODULE,
	.name = DRV_MODULE_NAME,
	.match_table = stsd_of_match,
	.probe = stsd_of_probe,
	.remove = stsd_of_remove,
};


/*
 * Kernel module interface.
 *
 */

static int __init stsd_init_module(void)
{
        drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   stsd_driver_version);

	if (register_blkdev(STSD_MAJOR, DRV_MODULE_NAME)) {
		drv_printk(KERN_ERR, "unable to register major %d\n",
			   STSD_MAJOR);
		return -EIO;
	}

	return of_register_platform_driver(&stsd_of_driver);
}

static void __exit stsd_exit_module(void)
{
	of_unregister_platform_driver(&stsd_of_driver);
	unregister_blkdev(STSD_MAJOR, DRV_MODULE_NAME);
}

module_init(stsd_init_module);
module_exit(stsd_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

