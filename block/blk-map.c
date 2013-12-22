/*
 * Functions related to mapping data to requests
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <scsi/sg.h>		/* for struct sg_iovec */

#include "blk.h"

int blk_rq_append_bio(struct request_queue *q, struct request *rq,
		      struct bio *bio)
{
	if (!rq->bio)
		blk_rq_bio_prep(q, rq, bio);
	else if (!ll_back_merge_fn(q, rq, bio))
		return -EINVAL;
	else {
		rq->biotail->bi_next = bio;
		rq->biotail = bio;

		rq->data_len += bio->bi_size;
	}
	return 0;
}
EXPORT_SYMBOL(blk_rq_append_bio);

static int __blk_rq_unmap_user(struct bio *bio)
{
	int ret = 0;

	if (bio) {
		if (bio_flagged(bio, BIO_USER_MAPPED))
			bio_unmap_user(bio);
		else
			ret = bio_uncopy_user(bio);
	}

	return ret;
}

static int __blk_rq_map_user(struct request_queue *q, struct request *rq,
			     void __user *ubuf, unsigned int len)
{
	unsigned long uaddr;
	unsigned int alignment;
	struct bio *bio, *orig_bio;
	int reading, ret;

	reading = rq_data_dir(rq) == READ;

	/*
	 * if alignment requirement is satisfied, map in user pages for
	 * direct dma. else, set up kernel bounce buffers
	 */
	uaddr = (unsigned long) ubuf;
	alignment = queue_dma_alignment(q) | q->dma_pad_mask;
	if (!(uaddr & alignment) && !(len & alignment))
		bio = bio_map_user(q, NULL, uaddr, len, reading);
	else
		bio = bio_copy_user(q, uaddr, len, reading);

	if (IS_ERR(bio))
		return PTR_ERR(bio);

	orig_bio = bio;
	blk_queue_bounce(q, &bio);

	/*
	 * We link the bounce buffer in and could have to traverse it
	 * later so we have to get a ref to prevent it from being freed
	 */
	bio_get(bio);

	ret = blk_rq_append_bio(q, rq, bio);
	if (!ret)
		return bio->bi_size;

	/* if it was boucned we must call the end io function */
	bio_endio(bio, 0);
	__blk_rq_unmap_user(orig_bio);
	bio_put(bio);
	return ret;
}

/**
 * blk_rq_map_user - map user data to a request, for REQ_BLOCK_PC usage
 * @q:		request queue where request should be inserted
 * @rq:		request structure to fill
 * @ubuf:	the user buffer
 * @len:	length of user data
 *
 * Description:
 *    Data will be mapped directly for zero copy io, if possible. Otherwise
 *    a kernel bounce buffer is used.
 *
 *    A matching blk_rq_unmap_user() must be issued at the end of io, while
 *    still in process context.
 *
 *    Note: The mapped bio may need to be bounced through blk_queue_bounce()
 *    before being submitted to the device, as pages mapped may be out of
 *    reach. It's the callers responsibility to make sure this happens. The
 *    original bio must be passed back in to blk_rq_unmap_user() for proper
 *    unmapping.
 */
int blk_rq_map_user(struct request_queue *q, struct request *rq,
		    void __user *ubuf, unsigned long len)
{
	unsigned long bytes_read = 0;
	struct bio *bio = NULL;
	int ret;

	if (len > (q->max_hw_sectors << 9))
		return -EINVAL;
	if (!len || !ubuf)
		return -EINVAL;

	while (bytes_read != len) {
		unsigned long map_len, end, start;

		map_len = min_t(unsigned long, len - bytes_read, BIO_MAX_SIZE);
		end = ((unsigned long)ubuf + map_len + PAGE_SIZE - 1)
								>> PAGE_SHIFT;
		start = (unsigned long)ubuf >> PAGE_SHIFT;

		/*
		 * A bad offset could cause us to require BIO_MAX_PAGES + 1
		 * pages. If this happens we just lower the requested
		 * mapping len by a page so that we can fit
		 */
		if (end - start > BIO_MAX_PAGES)
			map_len -= PAGE_SIZE;

		ret = __blk_rq_map_user(q, rq, ubuf, map_len);
		if (ret < 0)
			goto unmap_rq;
		if (!bio)
			bio = rq->bio;
		bytes_read += ret;
		ubuf += ret;
	}

	if (!bio_flagged(bio, BIO_USER_MAPPED))
		rq->cmd_flags |= REQ_COPY_USER;

	rq->buffer = rq->data = NULL;
	return 0;
unmap_rq:
	blk_rq_unmap_user(bio);
	rq->bio = NULL;
	return ret;
}
EXPORT_SYMBOL(blk_rq_map_user);

/**
 * blk_rq_map_user_iov - map user data to a request, for REQ_BLOCK_PC usage
 * @q:		request queue where request should be inserted
 * @rq:		request to map data to
 * @iov:	pointer to the iovec
 * @iov_count:	number of elements in the iovec
 * @len:	I/O byte count
 *
 * Description:
 *    Data will be mapped directly for zero copy io, if possible. Otherwise
 *    a kernel bounce buffer is used.
 *
 *    A matching blk_rq_unmap_user() must be issued at the end of io, while
 *    still in process context.
 *
 *    Note: The mapped bio may need to be bounced through blk_queue_bounce()
 *    before being submitted to the device, as pages mapped may be out of
 *    reach. It's the callers responsibility to make sure this happens. The
 *    original bio must be passed back in to blk_rq_unmap_user() for proper
 *    unmapping.
 */
int blk_rq_map_user_iov(struct request_queue *q, struct request *rq,
			struct sg_iovec *iov, int iov_count, unsigned int len)
{
	struct bio *bio;
	int i, read = rq_data_dir(rq) == READ;
	int unaligned = 0;

	if (!iov || iov_count <= 0)
		return -EINVAL;

	for (i = 0; i < iov_count; i++) {
		unsigned long uaddr = (unsigned long)iov[i].iov_base;

		if (uaddr & queue_dma_alignment(q)) {
			unaligned = 1;
			break;
		}
		if (!iov[i].iov_len)
			return -EINVAL;
	}

	if (unaligned || (q->dma_pad_mask & len))
		bio = bio_copy_user_iov(q, iov, iov_count, read);
	else
		bio = bio_map_user_iov(q, NULL, iov, iov_count, read);

	if (IS_ERR(bio))
		return PTR_ERR(bio);

	if (bio->bi_size != len) {
		bio_endio(bio, 0);
		bio_unmap_user(bio);
		return -EINVAL;
	}

	if (!bio_flagged(bio, BIO_USER_MAPPED))
		rq->cmd_flags |= REQ_COPY_USER;

	blk_queue_bounce(q, &bio);
	bio_get(bio);
	blk_rq_bio_prep(q, rq, bio);
	rq->buffer = rq->data = NULL;
	return 0;
}

/**
 * blk_rq_unmap_user - unmap a request with user data
 * @bio:	       start of bio list
 *
 * Description:
 *    Unmap a rq previously mapped by blk_rq_map_user(). The caller must
 *    supply the original rq->bio from the blk_rq_map_user() return, since
 *    the io completion may have changed rq->bio.
 */
int blk_rq_unmap_user(struct bio *bio)
{
	struct bio *mapped_bio;
	int ret = 0, ret2;

	while (bio) {
		mapped_bio = bio;
		if (unlikely(bio_flagged(bio, BIO_BOUNCED)))
			mapped_bio = bio->bi_private;

		ret2 = __blk_rq_unmap_user(mapped_bio);
		if (ret2 && !ret)
			ret = ret2;

		mapped_bio = bio;
		bio = bio->bi_next;
		bio_put(mapped_bio);
	}

	return ret;
}
EXPORT_SYMBOL(blk_rq_unmap_user);

/**
 * blk_rq_map_kern - map kernel data to a request, for REQ_BLOCK_PC usage
 * @q:		request queue where request should be inserted
 * @rq:		request to fill
 * @kbuf:	the kernel buffer
 * @len:	length of user data
 * @gfp_mask:	memory allocation flags
 *
 * Description:
 *    Data will be mapped directly if possible. Otherwise a bounce
 *    buffer is used.
 */
int blk_rq_map_kern(struct request_queue *q, struct request *rq, void *kbuf,
		    unsigned int len, gfp_t gfp_mask)
{
	unsigned long kaddr;
	unsigned int alignment;
	int reading = rq_data_dir(rq) == READ;
	int do_copy = 0;
	struct bio *bio;

	if (len > (q->max_hw_sectors << 9))
		return -EINVAL;
	if (!len || !kbuf)
		return -EINVAL;

	kaddr = (unsigned long)kbuf;
	alignment = queue_dma_alignment(q) | q->dma_pad_mask;
	do_copy = ((kaddr & alignment) || (len & alignment) ||
		   object_is_on_stack(kbuf));

	if (do_copy)
		bio = bio_copy_kern(q, kbuf, len, gfp_mask, reading);
	else
		bio = bio_map_kern(q, kbuf, len, gfp_mask);

	if (IS_ERR(bio))
		return PTR_ERR(bio);

	if (rq_data_dir(rq) == WRITE)
		bio->bi_rw |= (1 << BIO_RW);

	if (do_copy)
		rq->cmd_flags |= REQ_COPY_USER;

	blk_rq_bio_prep(q, rq, bio);
	blk_queue_bounce(q, &rq->bio);
	rq->buffer = rq->data = NULL;
	return 0;
}
EXPORT_SYMBOL(blk_rq_map_kern);
