/*
 * Engenio/LSI RDAC SCSI Device Handler
 *
 * Copyright (C) 2005 Mike Christie. All rights reserved.
 * Copyright (C) Chandra Seetharaman, IBM Corp. 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */
#include <scsi/scsi.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_dh.h>

#define RDAC_NAME "rdac"

/*
 * LSI mode page stuff
 *
 * These struct definitions and the forming of the
 * mode page were taken from the LSI RDAC 2.4 GPL'd
 * driver, and then converted to Linux conventions.
 */
#define RDAC_QUIESCENCE_TIME 20;
/*
 * Page Codes
 */
#define RDAC_PAGE_CODE_REDUNDANT_CONTROLLER 0x2c

/*
 * Controller modes definitions
 */
#define RDAC_MODE_TRANSFER_SPECIFIED_LUNS	0x02

/*
 * RDAC Options field
 */
#define RDAC_FORCED_QUIESENCE 0x02

#define RDAC_TIMEOUT	(60 * HZ)
#define RDAC_RETRIES	3

struct rdac_mode_6_hdr {
	u8	data_len;
	u8	medium_type;
	u8	device_params;
	u8	block_desc_len;
};

struct rdac_mode_10_hdr {
	u16	data_len;
	u8	medium_type;
	u8	device_params;
	u16	reserved;
	u16	block_desc_len;
};

struct rdac_mode_common {
	u8	controller_serial[16];
	u8	alt_controller_serial[16];
	u8	rdac_mode[2];
	u8	alt_rdac_mode[2];
	u8	quiescence_timeout;
	u8	rdac_options;
};

struct rdac_pg_legacy {
	struct rdac_mode_6_hdr hdr;
	u8	page_code;
	u8	page_len;
	struct rdac_mode_common common;
#define MODE6_MAX_LUN	32
	u8	lun_table[MODE6_MAX_LUN];
	u8	reserved2[32];
	u8	reserved3;
	u8	reserved4;
};

struct rdac_pg_expanded {
	struct rdac_mode_10_hdr hdr;
	u8	page_code;
	u8	subpage_code;
	u8	page_len[2];
	struct rdac_mode_common common;
	u8	lun_table[256];
	u8	reserved3;
	u8	reserved4;
};

struct c9_inquiry {
	u8	peripheral_info;
	u8	page_code;	/* 0xC9 */
	u8	reserved1;
	u8	page_len;
	u8	page_id[4];	/* "vace" */
	u8	avte_cvp;
	u8	path_prio;
	u8	reserved2[38];
};

#define SUBSYS_ID_LEN	16
#define SLOT_ID_LEN	2

struct c4_inquiry {
	u8	peripheral_info;
	u8	page_code;	/* 0xC4 */
	u8	reserved1;
	u8	page_len;
	u8	page_id[4];	/* "subs" */
	u8	subsys_id[SUBSYS_ID_LEN];
	u8	revision[4];
	u8	slot_id[SLOT_ID_LEN];
	u8	reserved[2];
};

struct rdac_controller {
	u8			subsys_id[SUBSYS_ID_LEN];
	u8			slot_id[SLOT_ID_LEN];
	int			use_ms10;
	struct kref		kref;
	struct list_head	node; /* list of all controllers */
	union			{
		struct rdac_pg_legacy legacy;
		struct rdac_pg_expanded expanded;
	} mode_select;
};
struct c8_inquiry {
	u8	peripheral_info;
	u8	page_code; /* 0xC8 */
	u8	reserved1;
	u8	page_len;
	u8	page_id[4]; /* "edid" */
	u8	reserved2[3];
	u8	vol_uniq_id_len;
	u8	vol_uniq_id[16];
	u8	vol_user_label_len;
	u8	vol_user_label[60];
	u8	array_uniq_id_len;
	u8	array_unique_id[16];
	u8	array_user_label_len;
	u8	array_user_label[60];
	u8	lun[8];
};

struct c2_inquiry {
	u8	peripheral_info;
	u8	page_code;	/* 0xC2 */
	u8	reserved1;
	u8	page_len;
	u8	page_id[4];	/* "swr4" */
	u8	sw_version[3];
	u8	sw_date[3];
	u8	features_enabled;
	u8	max_lun_supported;
	u8	partitions[239]; /* Total allocation length should be 0xFF */
};

struct rdac_dh_data {
	struct rdac_controller	*ctlr;
#define UNINITIALIZED_LUN	(1 << 8)
	unsigned		lun;
#define RDAC_STATE_ACTIVE	0
#define RDAC_STATE_PASSIVE	1
	unsigned char		state;

#define RDAC_LUN_UNOWNED	0
#define RDAC_LUN_OWNED		1
#define RDAC_LUN_AVT		2
	char			lun_state;
	unsigned char		sense[SCSI_SENSE_BUFFERSIZE];
	union			{
		struct c2_inquiry c2;
		struct c4_inquiry c4;
		struct c8_inquiry c8;
		struct c9_inquiry c9;
	} inq;
};

static const char *lun_state[] =
{
	"unowned",
	"owned",
	"owned (AVT mode)",
};

static LIST_HEAD(ctlr_list);
static DEFINE_SPINLOCK(list_lock);

static inline struct rdac_dh_data *get_rdac_data(struct scsi_device *sdev)
{
	struct scsi_dh_data *scsi_dh_data = sdev->scsi_dh_data;
	BUG_ON(scsi_dh_data == NULL);
	return ((struct rdac_dh_data *) scsi_dh_data->buf);
}

static struct request *get_rdac_req(struct scsi_device *sdev,
			void *buffer, unsigned buflen, int rw)
{
	struct request *rq;
	struct request_queue *q = sdev->request_queue;

	rq = blk_get_request(q, rw, GFP_NOIO);

	if (!rq) {
		sdev_printk(KERN_INFO, sdev,
				"get_rdac_req: blk_get_request failed.\n");
		return NULL;
	}

	if (buflen && blk_rq_map_kern(q, rq, buffer, buflen, GFP_NOIO)) {
		blk_put_request(rq);
		sdev_printk(KERN_INFO, sdev,
				"get_rdac_req: blk_rq_map_kern failed.\n");
		return NULL;
	}

	memset(rq->cmd, 0, BLK_MAX_CDB);

	rq->cmd_type = REQ_TYPE_BLOCK_PC;
	rq->cmd_flags |= REQ_FAILFAST | REQ_NOMERGE;
	rq->retries = RDAC_RETRIES;
	rq->timeout = RDAC_TIMEOUT;

	return rq;
}

static struct request *rdac_failover_get(struct scsi_device *sdev,
					 struct rdac_dh_data *h)
{
	struct request *rq;
	struct rdac_mode_common *common;
	unsigned data_size;

	if (h->ctlr->use_ms10) {
		struct rdac_pg_expanded *rdac_pg;

		data_size = sizeof(struct rdac_pg_expanded);
		rdac_pg = &h->ctlr->mode_select.expanded;
		memset(rdac_pg, 0, data_size);
		common = &rdac_pg->common;
		rdac_pg->page_code = RDAC_PAGE_CODE_REDUNDANT_CONTROLLER + 0x40;
		rdac_pg->subpage_code = 0x1;
		rdac_pg->page_len[0] = 0x01;
		rdac_pg->page_len[1] = 0x28;
		rdac_pg->lun_table[h->lun] = 0x81;
	} else {
		struct rdac_pg_legacy *rdac_pg;

		data_size = sizeof(struct rdac_pg_legacy);
		rdac_pg = &h->ctlr->mode_select.legacy;
		memset(rdac_pg, 0, data_size);
		common = &rdac_pg->common;
		rdac_pg->page_code = RDAC_PAGE_CODE_REDUNDANT_CONTROLLER;
		rdac_pg->page_len = 0x68;
		rdac_pg->lun_table[h->lun] = 0x81;
	}
	common->rdac_mode[1] = RDAC_MODE_TRANSFER_SPECIFIED_LUNS;
	common->quiescence_timeout = RDAC_QUIESCENCE_TIME;
	common->rdac_options = RDAC_FORCED_QUIESENCE;

	/* get request for block layer packet command */
	rq = get_rdac_req(sdev, &h->ctlr->mode_select, data_size, WRITE);
	if (!rq)
		return NULL;

	/* Prepare the command. */
	if (h->ctlr->use_ms10) {
		rq->cmd[0] = MODE_SELECT_10;
		rq->cmd[7] = data_size >> 8;
		rq->cmd[8] = data_size & 0xff;
	} else {
		rq->cmd[0] = MODE_SELECT;
		rq->cmd[4] = data_size;
	}
	rq->cmd_len = COMMAND_SIZE(rq->cmd[0]);

	rq->sense = h->sense;
	memset(rq->sense, 0, SCSI_SENSE_BUFFERSIZE);
	rq->sense_len = 0;

	return rq;
}

static void release_controller(struct kref *kref)
{
	struct rdac_controller *ctlr;
	ctlr = container_of(kref, struct rdac_controller, kref);

	spin_lock(&list_lock);
	list_del(&ctlr->node);
	spin_unlock(&list_lock);
	kfree(ctlr);
}

static struct rdac_controller *get_controller(u8 *subsys_id, u8 *slot_id)
{
	struct rdac_controller *ctlr, *tmp;

	spin_lock(&list_lock);

	list_for_each_entry(tmp, &ctlr_list, node) {
		if ((memcmp(tmp->subsys_id, subsys_id, SUBSYS_ID_LEN) == 0) &&
			  (memcmp(tmp->slot_id, slot_id, SLOT_ID_LEN) == 0)) {
			kref_get(&tmp->kref);
			spin_unlock(&list_lock);
			return tmp;
		}
	}
	ctlr = kmalloc(sizeof(*ctlr), GFP_ATOMIC);
	if (!ctlr)
		goto done;

	/* initialize fields of controller */
	memcpy(ctlr->subsys_id, subsys_id, SUBSYS_ID_LEN);
	memcpy(ctlr->slot_id, slot_id, SLOT_ID_LEN);
	kref_init(&ctlr->kref);
	ctlr->use_ms10 = -1;
	list_add(&ctlr->node, &ctlr_list);
done:
	spin_unlock(&list_lock);
	return ctlr;
}

static int submit_inquiry(struct scsi_device *sdev, int page_code,
			  unsigned int len, struct rdac_dh_data *h)
{
	struct request *rq;
	struct request_queue *q = sdev->request_queue;
	int err = SCSI_DH_RES_TEMP_UNAVAIL;

	rq = get_rdac_req(sdev, &h->inq, len, READ);
	if (!rq)
		goto done;

	/* Prepare the command. */
	rq->cmd[0] = INQUIRY;
	rq->cmd[1] = 1;
	rq->cmd[2] = page_code;
	rq->cmd[4] = len;
	rq->cmd_len = COMMAND_SIZE(INQUIRY);

	rq->sense = h->sense;
	memset(rq->sense, 0, SCSI_SENSE_BUFFERSIZE);
	rq->sense_len = 0;

	err = blk_execute_rq(q, NULL, rq, 1);
	if (err == -EIO)
		err = SCSI_DH_IO;

	blk_put_request(rq);
done:
	return err;
}

static int get_lun(struct scsi_device *sdev, struct rdac_dh_data *h)
{
	int err;
	struct c8_inquiry *inqp;

	err = submit_inquiry(sdev, 0xC8, sizeof(struct c8_inquiry), h);
	if (err == SCSI_DH_OK) {
		inqp = &h->inq.c8;
		if (inqp->page_code != 0xc8)
			return SCSI_DH_NOSYS;
		if (inqp->page_id[0] != 'e' || inqp->page_id[1] != 'd' ||
		    inqp->page_id[2] != 'i' || inqp->page_id[3] != 'd')
			return SCSI_DH_NOSYS;
		h->lun = inqp->lun[7]; /* Uses only the last byte */
	}
	return err;
}

static int check_ownership(struct scsi_device *sdev, struct rdac_dh_data *h)
{
	int err;
	struct c9_inquiry *inqp;

	h->lun_state = RDAC_LUN_UNOWNED;
	err = submit_inquiry(sdev, 0xC9, sizeof(struct c9_inquiry), h);
	if (err == SCSI_DH_OK) {
		inqp = &h->inq.c9;
		if ((inqp->avte_cvp >> 7) == 0x1) {
			/* LUN in AVT mode */
			sdev_printk(KERN_NOTICE, sdev,
				    "%s: AVT mode detected\n",
				    RDAC_NAME);
			h->lun_state = RDAC_LUN_AVT;
		} else if ((inqp->avte_cvp & 0x1) != 0) {
			/* LUN was owned by the controller */
			h->lun_state = RDAC_LUN_OWNED;
		}
	}

	return err;
}

static int initialize_controller(struct scsi_device *sdev,
				 struct rdac_dh_data *h)
{
	int err;
	struct c4_inquiry *inqp;

	err = submit_inquiry(sdev, 0xC4, sizeof(struct c4_inquiry), h);
	if (err == SCSI_DH_OK) {
		inqp = &h->inq.c4;
		h->ctlr = get_controller(inqp->subsys_id, inqp->slot_id);
		if (!h->ctlr)
			err = SCSI_DH_RES_TEMP_UNAVAIL;
	}
	return err;
}

static int set_mode_select(struct scsi_device *sdev, struct rdac_dh_data *h)
{
	int err;
	struct c2_inquiry *inqp;

	err = submit_inquiry(sdev, 0xC2, sizeof(struct c2_inquiry), h);
	if (err == SCSI_DH_OK) {
		inqp = &h->inq.c2;
		/*
		 * If more than MODE6_MAX_LUN luns are supported, use
		 * mode select 10
		 */
		if (inqp->max_lun_supported >= MODE6_MAX_LUN)
			h->ctlr->use_ms10 = 1;
		else
			h->ctlr->use_ms10 = 0;
	}
	return err;
}

static int mode_select_handle_sense(struct scsi_device *sdev,
				    unsigned char *sensebuf)
{
	struct scsi_sense_hdr sense_hdr;
	int sense, err = SCSI_DH_IO, ret;

	ret = scsi_normalize_sense(sensebuf, SCSI_SENSE_BUFFERSIZE, &sense_hdr);
	if (!ret)
		goto done;

	err = SCSI_DH_OK;
	sense = (sense_hdr.sense_key << 16) | (sense_hdr.asc << 8) |
			sense_hdr.ascq;
	/* If it is retryable failure, submit the c9 inquiry again */
	if (sense == 0x59136 || sense == 0x68b02 || sense == 0xb8b02 ||
			    sense == 0x62900) {
		/* 0x59136    - Command lock contention
		 * 0x[6b]8b02 - Quiesense in progress or achieved
		 * 0x62900    - Power On, Reset, or Bus Device Reset
		 */
		err = SCSI_DH_RETRY;
	}

	if (sense)
		sdev_printk(KERN_INFO, sdev,
			"MODE_SELECT failed with sense 0x%x.\n", sense);
done:
	return err;
}

static int send_mode_select(struct scsi_device *sdev, struct rdac_dh_data *h)
{
	struct request *rq;
	struct request_queue *q = sdev->request_queue;
	int err = SCSI_DH_RES_TEMP_UNAVAIL;

	rq = rdac_failover_get(sdev, h);
	if (!rq)
		goto done;

	sdev_printk(KERN_INFO, sdev, "queueing MODE_SELECT command.\n");

	err = blk_execute_rq(q, NULL, rq, 1);
	if (err != SCSI_DH_OK)
		err = mode_select_handle_sense(sdev, h->sense);
	if (err == SCSI_DH_OK)
		h->state = RDAC_STATE_ACTIVE;

	blk_put_request(rq);
done:
	return err;
}

static int rdac_activate(struct scsi_device *sdev)
{
	struct rdac_dh_data *h = get_rdac_data(sdev);
	int err = SCSI_DH_OK;

	err = check_ownership(sdev, h);
	if (err != SCSI_DH_OK)
		goto done;

	if (!h->ctlr) {
		err = initialize_controller(sdev, h);
		if (err != SCSI_DH_OK)
			goto done;
	}

	if (h->ctlr->use_ms10 == -1) {
		err = set_mode_select(sdev, h);
		if (err != SCSI_DH_OK)
			goto done;
	}
	if (h->lun_state == RDAC_LUN_UNOWNED)
		err = send_mode_select(sdev, h);
done:
	return err;
}

static int rdac_prep_fn(struct scsi_device *sdev, struct request *req)
{
	struct rdac_dh_data *h = get_rdac_data(sdev);
	int ret = BLKPREP_OK;

	if (h->state != RDAC_STATE_ACTIVE) {
		ret = BLKPREP_KILL;
		req->cmd_flags |= REQ_QUIET;
	}
	return ret;

}

static int rdac_check_sense(struct scsi_device *sdev,
				struct scsi_sense_hdr *sense_hdr)
{
	struct rdac_dh_data *h = get_rdac_data(sdev);
	switch (sense_hdr->sense_key) {
	case NOT_READY:
		if (sense_hdr->asc == 0x04 && sense_hdr->ascq == 0x81)
			/* LUN Not Ready - Storage firmware incompatible
			 * Manual code synchonisation required.
			 *
			 * Nothing we can do here. Try to bypass the path.
			 */
			return SUCCESS;
		if (sense_hdr->asc == 0x04 && sense_hdr->ascq == 0xA1)
			/* LUN Not Ready - Quiescense in progress
			 *
			 * Just retry and wait.
			 */
			return ADD_TO_MLQUEUE;
		break;
	case ILLEGAL_REQUEST:
		if (sense_hdr->asc == 0x94 && sense_hdr->ascq == 0x01) {
			/* Invalid Request - Current Logical Unit Ownership.
			 * Controller is not the current owner of the LUN,
			 * Fail the path, so that the other path be used.
			 */
			h->state = RDAC_STATE_PASSIVE;
			return SUCCESS;
		}
		break;
	case UNIT_ATTENTION:
		if (sense_hdr->asc == 0x29 && sense_hdr->ascq == 0x00)
			/*
			 * Power On, Reset, or Bus Device Reset, just retry.
			 */
			return ADD_TO_MLQUEUE;
		break;
	}
	/* success just means we do not care what scsi-ml does */
	return SCSI_RETURN_NOT_HANDLED;
}

static const struct scsi_dh_devlist rdac_dev_list[] = {
	{"IBM", "1722"},
	{"IBM", "1724"},
	{"IBM", "1726"},
	{"IBM", "1742"},
	{"IBM", "1814"},
	{"IBM", "1815"},
	{"IBM", "1818"},
	{"IBM", "3526"},
	{"SGI", "TP9400"},
	{"SGI", "TP9500"},
	{"SGI", "IS"},
	{"STK", "OPENstorage D280"},
	{"SUN", "CSM200_R"},
	{"SUN", "LCSM100_F"},
	{"DELL", "MD3000"},
	{"DELL", "MD3000i"},
	{NULL, NULL},
};

static int rdac_bus_attach(struct scsi_device *sdev);
static void rdac_bus_detach(struct scsi_device *sdev);

static struct scsi_device_handler rdac_dh = {
	.name = RDAC_NAME,
	.module = THIS_MODULE,
	.devlist = rdac_dev_list,
	.prep_fn = rdac_prep_fn,
	.check_sense = rdac_check_sense,
	.attach = rdac_bus_attach,
	.detach = rdac_bus_detach,
	.activate = rdac_activate,
};

static int rdac_bus_attach(struct scsi_device *sdev)
{
	struct scsi_dh_data *scsi_dh_data;
	struct rdac_dh_data *h;
	unsigned long flags;
	int err;

	scsi_dh_data = kzalloc(sizeof(struct scsi_device_handler *)
			       + sizeof(*h) , GFP_KERNEL);
	if (!scsi_dh_data) {
		sdev_printk(KERN_ERR, sdev, "%s: Attach failed\n",
			    RDAC_NAME);
		return 0;
	}

	scsi_dh_data->scsi_dh = &rdac_dh;
	h = (struct rdac_dh_data *) scsi_dh_data->buf;
	h->lun = UNINITIALIZED_LUN;
	h->state = RDAC_STATE_ACTIVE;

	err = get_lun(sdev, h);
	if (err != SCSI_DH_OK)
		goto failed;

	err = check_ownership(sdev, h);
	if (err != SCSI_DH_OK)
		goto failed;

	if (!try_module_get(THIS_MODULE))
		goto failed;

	spin_lock_irqsave(sdev->request_queue->queue_lock, flags);
	sdev->scsi_dh_data = scsi_dh_data;
	spin_unlock_irqrestore(sdev->request_queue->queue_lock, flags);

	sdev_printk(KERN_NOTICE, sdev,
		    "%s: LUN %d (%s)\n",
		    RDAC_NAME, h->lun, lun_state[(int)h->lun_state]);

	return 0;

failed:
	kfree(scsi_dh_data);
	sdev_printk(KERN_ERR, sdev, "%s: not attached\n",
		    RDAC_NAME);
	return -EINVAL;
}

static void rdac_bus_detach( struct scsi_device *sdev )
{
	struct scsi_dh_data *scsi_dh_data;
	struct rdac_dh_data *h;
	unsigned long flags;

	spin_lock_irqsave(sdev->request_queue->queue_lock, flags);
	scsi_dh_data = sdev->scsi_dh_data;
	sdev->scsi_dh_data = NULL;
	spin_unlock_irqrestore(sdev->request_queue->queue_lock, flags);

	h = (struct rdac_dh_data *) scsi_dh_data->buf;
	if (h->ctlr)
		kref_put(&h->ctlr->kref, release_controller);
	kfree(scsi_dh_data);
	module_put(THIS_MODULE);
	sdev_printk(KERN_NOTICE, sdev, "%s: Detached\n", RDAC_NAME);
}



static int __init rdac_init(void)
{
	int r;

	r = scsi_register_device_handler(&rdac_dh);
	if (r != 0)
		printk(KERN_ERR "Failed to register scsi device handler.");
	return r;
}

static void __exit rdac_exit(void)
{
	scsi_unregister_device_handler(&rdac_dh);
}

module_init(rdac_init);
module_exit(rdac_exit);

MODULE_DESCRIPTION("Multipath LSI/Engenio RDAC driver");
MODULE_AUTHOR("Mike Christie, Chandra Seetharaman");
MODULE_LICENSE("GPL");
