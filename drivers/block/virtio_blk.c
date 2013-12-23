//#define DEBUG
#include <linux/spinlock.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/virtio.h>
#include <linux/virtio_blk.h>
#include <linux/scatterlist.h>

#define PART_BITS 4

static int major, index;

struct virtio_blk
{
	spinlock_t lock;

	struct virtio_device *vdev;
	struct virtqueue *vq;

	/* The disk structure for the kernel. */
	struct gendisk *disk;

	/* Request tracking. */
	struct list_head reqs;

	mempool_t *pool;

	/* What host tells us, plus 2 for header & tailer. */
	unsigned int sg_elems;

	/* Scatterlist: can be too big for stack. */
	struct scatterlist sg[/*sg_elems*/];
};

struct virtblk_req
{
	struct list_head list;
	struct request *req;
	struct virtio_blk_outhdr out_hdr;
	u8 status;
};

static void blk_done(struct virtqueue *vq)
{
	struct virtio_blk *vblk = vq->vdev->priv;
	struct virtblk_req *vbr;
	unsigned int len;
	unsigned long flags;

	spin_lock_irqsave(&vblk->lock, flags);
	while ((vbr = vblk->vq->vq_ops->get_buf(vblk->vq, &len)) != NULL) {
		int error;
		switch (vbr->status) {
		case VIRTIO_BLK_S_OK:
			error = 0;
			break;
		case VIRTIO_BLK_S_UNSUPP:
			error = -ENOTTY;
			break;
		default:
			error = -EIO;
			break;
		}

		__blk_end_request(vbr->req, error, blk_rq_bytes(vbr->req));
		list_del(&vbr->list);
		mempool_free(vbr, vblk->pool);
	}
	/* In case queue is stopped waiting for more buffers. */
	blk_start_queue(vblk->disk->queue);
	spin_unlock_irqrestore(&vblk->lock, flags);
}

static bool do_req(struct request_queue *q, struct virtio_blk *vblk,
		   struct request *req)
{
	unsigned long num, out, in;
	struct virtblk_req *vbr;

	vbr = mempool_alloc(vblk->pool, GFP_ATOMIC);
	if (!vbr)
		/* When another request finishes we'll try again. */
		return false;

	vbr->req = req;
	if (blk_fs_request(vbr->req)) {
		vbr->out_hdr.type = 0;
		vbr->out_hdr.sector = vbr->req->sector;
		vbr->out_hdr.ioprio = req_get_ioprio(vbr->req);
	} else if (blk_pc_request(vbr->req)) {
		vbr->out_hdr.type = VIRTIO_BLK_T_SCSI_CMD;
		vbr->out_hdr.sector = 0;
		vbr->out_hdr.ioprio = req_get_ioprio(vbr->req);
	} else {
		/* We don't put anything else in the queue. */
		BUG();
	}

	if (blk_barrier_rq(vbr->req))
		vbr->out_hdr.type |= VIRTIO_BLK_T_BARRIER;

	sg_set_buf(&vblk->sg[0], &vbr->out_hdr, sizeof(vbr->out_hdr));
	num = blk_rq_map_sg(q, vbr->req, vblk->sg+1);
	sg_set_buf(&vblk->sg[num+1], &vbr->status, sizeof(vbr->status));

	if (rq_data_dir(vbr->req) == WRITE) {
		vbr->out_hdr.type |= VIRTIO_BLK_T_OUT;
		out = 1 + num;
		in = 1;
	} else {
		vbr->out_hdr.type |= VIRTIO_BLK_T_IN;
		out = 1;
		in = 1 + num;
	}

	if (vblk->vq->vq_ops->add_buf(vblk->vq, vblk->sg, out, in, vbr)) {
		mempool_free(vbr, vblk->pool);
		return false;
	}

	list_add_tail(&vbr->list, &vblk->reqs);
	return true;
}

static void do_virtblk_request(struct request_queue *q)
{
	struct virtio_blk *vblk = NULL;
	struct request *req;
	unsigned int issued = 0;

	while ((req = elv_next_request(q)) != NULL) {
		vblk = req->rq_disk->private_data;
		BUG_ON(req->nr_phys_segments + 2 > vblk->sg_elems);

		/* If this request fails, stop queue and wait for something to
		   finish to restart it. */
		if (!do_req(q, vblk, req)) {
			blk_stop_queue(q);
			break;
		}
		blkdev_dequeue_request(req);
		issued++;
	}

	if (issued)
		vblk->vq->vq_ops->kick(vblk->vq);
}

static int virtblk_ioctl(struct block_device *bdev, fmode_t mode,
			 unsigned cmd, unsigned long data)
{
	return scsi_cmd_ioctl(bdev->bd_disk->queue,
			      bdev->bd_disk, mode, cmd,
			      (void __user *)data);
}

/* We provide getgeo only to please some old bootloader/partitioning tools */
static int virtblk_getgeo(struct block_device *bd, struct hd_geometry *geo)
{
	struct virtio_blk *vblk = bd->bd_disk->private_data;
	struct virtio_blk_geometry vgeo;
	int err;

	/* see if the host passed in geometry config */
	err = virtio_config_val(vblk->vdev, VIRTIO_BLK_F_GEOMETRY,
				offsetof(struct virtio_blk_config, geometry),
				&vgeo);

	if (!err) {
		geo->heads = vgeo.heads;
		geo->sectors = vgeo.sectors;
		geo->cylinders = vgeo.cylinders;
	} else {
		/* some standard values, similar to sd */
		geo->heads = 1 << 6;
		geo->sectors = 1 << 5;
		geo->cylinders = get_capacity(bd->bd_disk) >> 11;
	}
	return 0;
}

static struct block_device_operations virtblk_fops = {
	.locked_ioctl = virtblk_ioctl,
	.owner  = THIS_MODULE,
	.getgeo = virtblk_getgeo,
};

static int index_to_minor(int index)
{
	return index << PART_BITS;
}

static int virtblk_probe(struct virtio_device *vdev)
{
	struct virtio_blk *vblk;
	int err;
	u64 cap;
	u32 v;
	u32 blk_size, sg_elems;

	if (index_to_minor(index) >= 1 << MINORBITS)
		return -ENOSPC;

	/* We need to know how many segments before we allocate. */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_SEG_MAX,
				offsetof(struct virtio_blk_config, seg_max),
				&sg_elems);
	if (err)
		sg_elems = 1;

	/* We need an extra sg elements at head and tail. */
	sg_elems += 2;
	vdev->priv = vblk = kmalloc(sizeof(*vblk) +
				    sizeof(vblk->sg[0]) * sg_elems, GFP_KERNEL);
	if (!vblk) {
		err = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&vblk->reqs);
	spin_lock_init(&vblk->lock);
	vblk->vdev = vdev;
	vblk->sg_elems = sg_elems;
	sg_init_table(vblk->sg, vblk->sg_elems);

	/* We expect one virtqueue, for output. */
	vblk->vq = vdev->config->find_vq(vdev, 0, blk_done);
	if (IS_ERR(vblk->vq)) {
		err = PTR_ERR(vblk->vq);
		goto out_free_vblk;
	}

	vblk->pool = mempool_create_kmalloc_pool(1,sizeof(struct virtblk_req));
	if (!vblk->pool) {
		err = -ENOMEM;
		goto out_free_vq;
	}

	/* FIXME: How many partitions?  How long is a piece of string? */
	vblk->disk = alloc_disk(1 << PART_BITS);
	if (!vblk->disk) {
		err = -ENOMEM;
		goto out_mempool;
	}

	vblk->disk->queue = blk_init_queue(do_virtblk_request, &vblk->lock);
	if (!vblk->disk->queue) {
		err = -ENOMEM;
		goto out_put_disk;
	}

	queue_flag_set_unlocked(QUEUE_FLAG_VIRT, vblk->disk->queue);

	if (index < 26) {
		sprintf(vblk->disk->disk_name, "vd%c", 'a' + index % 26);
	} else if (index < (26 + 1) * 26) {
		sprintf(vblk->disk->disk_name, "vd%c%c",
			'a' + index / 26 - 1, 'a' + index % 26);
	} else {
		const unsigned int m1 = (index / 26 - 1) / 26 - 1;
		const unsigned int m2 = (index / 26 - 1) % 26;
		const unsigned int m3 =  index % 26;
		sprintf(vblk->disk->disk_name, "vd%c%c%c",
			'a' + m1, 'a' + m2, 'a' + m3);
	}

	vblk->disk->major = major;
	vblk->disk->first_minor = index_to_minor(index);
	vblk->disk->private_data = vblk;
	vblk->disk->fops = &virtblk_fops;
	vblk->disk->driverfs_dev = &vdev->dev;
	index++;

	/* If barriers are supported, tell block layer that queue is ordered */
	if (virtio_has_feature(vdev, VIRTIO_BLK_F_BARRIER))
		blk_queue_ordered(vblk->disk->queue, QUEUE_ORDERED_TAG, NULL);

	/* If disk is read-only in the host, the guest should obey */
	if (virtio_has_feature(vdev, VIRTIO_BLK_F_RO))
		set_disk_ro(vblk->disk, 1);

	/* Host must always specify the capacity. */
	vdev->config->get(vdev, offsetof(struct virtio_blk_config, capacity),
			  &cap, sizeof(cap));

	/* If capacity is too big, truncate with warning. */
	if ((sector_t)cap != cap) {
		dev_warn(&vdev->dev, "Capacity %llu too large: truncating\n",
			 (unsigned long long)cap);
		cap = (sector_t)-1;
	}
	set_capacity(vblk->disk, cap);

	/* We can handle whatever the host told us to handle. */
	blk_queue_max_phys_segments(vblk->disk->queue, vblk->sg_elems-2);
	blk_queue_max_hw_segments(vblk->disk->queue, vblk->sg_elems-2);

	/* No real sector limit. */
	blk_queue_max_sectors(vblk->disk->queue, -1U);

	/* Host can optionally specify maximum segment size and number of
	 * segments. */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_SIZE_MAX,
				offsetof(struct virtio_blk_config, size_max),
				&v);
	if (!err)
		blk_queue_max_segment_size(vblk->disk->queue, v);
	else
		blk_queue_max_segment_size(vblk->disk->queue, -1U);

	/* No need to bounce any requests */
	blk_queue_bounce_limit(vblk->disk->queue, BLK_BOUNCE_ANY);

	/* Host can optionally specify the block size of the device */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_BLK_SIZE,
				offsetof(struct virtio_blk_config, blk_size),
				&blk_size);
	if (!err)
		blk_queue_hardsect_size(vblk->disk->queue, blk_size);

	add_disk(vblk->disk);
	return 0;

out_put_disk:
	put_disk(vblk->disk);
out_mempool:
	mempool_destroy(vblk->pool);
out_free_vq:
	vdev->config->del_vq(vblk->vq);
out_free_vblk:
	kfree(vblk);
out:
	return err;
}

static void virtblk_remove(struct virtio_device *vdev)
{
	struct virtio_blk *vblk = vdev->priv;

	/* Nothing should be pending. */
	BUG_ON(!list_empty(&vblk->reqs));

	/* Stop all the virtqueues. */
	vdev->config->reset(vdev);

	del_gendisk(vblk->disk);
	blk_cleanup_queue(vblk->disk->queue);
	put_disk(vblk->disk);
	mempool_destroy(vblk->pool);
	vdev->config->del_vq(vblk->vq);
	kfree(vblk);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BLOCK, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_BLK_F_BARRIER, VIRTIO_BLK_F_SEG_MAX, VIRTIO_BLK_F_SIZE_MAX,
	VIRTIO_BLK_F_GEOMETRY, VIRTIO_BLK_F_RO, VIRTIO_BLK_F_BLK_SIZE,
};

static struct virtio_driver virtio_blk = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtblk_probe,
	.remove =	__devexit_p(virtblk_remove),
};

static int __init init(void)
{
	major = register_blkdev(0, "virtblk");
	if (major < 0)
		return major;
	return register_virtio_driver(&virtio_blk);
}

static void __exit fini(void)
{
	unregister_blkdev(major, "virtblk");
	unregister_virtio_driver(&virtio_blk);
}
module_init(init);
module_exit(fini);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio block driver");
MODULE_LICENSE("GPL");
