/*
   md.c : Multiple Devices driver for Linux
	  Copyright (C) 1998, 1999, 2000 Ingo Molnar

     completely rewritten, based on the MD driver code from Marc Zyngier

   Changes:

   - RAID-1/RAID-5 extensions by Miguel de Icaza, Gadi Oxman, Ingo Molnar
   - RAID-6 extensions by H. Peter Anvin <hpa@zytor.com>
   - boot support for linear and striped mode by Harald Hoyer <HarryH@Royal.Net>
   - kerneld support by Boris Tobotras <boris@xtalk.msk.su>
   - kmod support by: Cyrus Durgin
   - RAID0 bugfixes: Mark Anthony Lisher <markal@iname.com>
   - Devfs support by Richard Gooch <rgooch@atnf.csiro.au>

   - lots of fixes and improvements to the RAID1/RAID5 and generic
     RAID code (such as request based resynchronization):

     Neil Brown <neilb@cse.unsw.edu.au>.

   - persistent bitmap code
     Copyright (C) 2003-2004, Paul Clements, SteelEye Technology, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   You should have received a copy of the GNU General Public License
   (for example /usr/src/linux/COPYING); if not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/module.h>
#include <linux/config.h>
#include <linux/kthread.h>
#include <linux/linkage.h>
#include <linux/raid/md.h>
#include <linux/raid/bitmap.h>
#include <linux/sysctl.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/buffer_head.h> /* for invalidate_bdev */
#include <linux/suspend.h>

#include <linux/init.h>

#include <linux/file.h>

#ifdef CONFIG_KMOD
#include <linux/kmod.h>
#endif

#include <asm/unaligned.h>

#define MAJOR_NR MD_MAJOR
#define MD_DRIVER

/* 63 partitions with the alternate major number (mdp) */
#define MdpMinorShift 6

#define DEBUG 0
#define dprintk(x...) ((void)(DEBUG && printk(x)))


#ifndef MODULE
static void autostart_arrays (int part);
#endif

static mdk_personality_t *pers[MAX_PERSONALITY];
static DEFINE_SPINLOCK(pers_lock);

/*
 * Current RAID-1,4,5 parallel reconstruction 'guaranteed speed limit'
 * is 1000 KB/sec, so the extra system load does not show up that much.
 * Increase it if you want to have more _guaranteed_ speed. Note that
 * the RAID driver will use the maximum available bandwidth if the IO
 * subsystem is idle. There is also an 'absolute maximum' reconstruction
 * speed limit - in case reconstruction slows down your system despite
 * idle IO detection.
 *
 * you can change it via /proc/sys/dev/raid/speed_limit_min and _max.
 */

static int sysctl_speed_limit_min = 1000;
static int sysctl_speed_limit_max = 200000;

static struct ctl_table_header *raid_table_header;

static ctl_table raid_table[] = {
	{
		.ctl_name	= DEV_RAID_SPEED_LIMIT_MIN,
		.procname	= "speed_limit_min",
		.data		= &sysctl_speed_limit_min,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec,
	},
	{
		.ctl_name	= DEV_RAID_SPEED_LIMIT_MAX,
		.procname	= "speed_limit_max",
		.data		= &sysctl_speed_limit_max,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec,
	},
	{ .ctl_name = 0 }
};

static ctl_table raid_dir_table[] = {
	{
		.ctl_name	= DEV_RAID,
		.procname	= "raid",
		.maxlen		= 0,
		.mode		= 0555,
		.child		= raid_table,
	},
	{ .ctl_name = 0 }
};

static ctl_table raid_root_table[] = {
	{
		.ctl_name	= CTL_DEV,
		.procname	= "dev",
		.maxlen		= 0,
		.mode		= 0555,
		.child		= raid_dir_table,
	},
	{ .ctl_name = 0 }
};

static struct block_device_operations md_fops;

static int start_readonly;

/*
 * Enables to iterate over all existing md arrays
 * all_mddevs_lock protects this list.
 */
static LIST_HEAD(all_mddevs);
static DEFINE_SPINLOCK(all_mddevs_lock);


/*
 * iterates through all used mddevs in the system.
 * We take care to grab the all_mddevs_lock whenever navigating
 * the list, and to always hold a refcount when unlocked.
 * Any code which breaks out of this loop while own
 * a reference to the current mddev and must mddev_put it.
 */
#define ITERATE_MDDEV(mddev,tmp)					\
									\
	for (({ spin_lock(&all_mddevs_lock); 				\
		tmp = all_mddevs.next;					\
		mddev = NULL;});					\
	     ({ if (tmp != &all_mddevs)					\
			mddev_get(list_entry(tmp, mddev_t, all_mddevs));\
		spin_unlock(&all_mddevs_lock);				\
		if (mddev) mddev_put(mddev);				\
		mddev = list_entry(tmp, mddev_t, all_mddevs);		\
		tmp != &all_mddevs;});					\
	     ({ spin_lock(&all_mddevs_lock);				\
		tmp = tmp->next;})					\
		)


static int md_fail_request (request_queue_t *q, struct bio *bio)
{
	bio_io_error(bio, bio->bi_size);
	return 0;
}

static inline mddev_t *mddev_get(mddev_t *mddev)
{
	atomic_inc(&mddev->active);
	return mddev;
}

static void mddev_put(mddev_t *mddev)
{
	if (!atomic_dec_and_lock(&mddev->active, &all_mddevs_lock))
		return;
	if (!mddev->raid_disks && list_empty(&mddev->disks)) {
		list_del(&mddev->all_mddevs);
		blk_put_queue(mddev->queue);
		kobject_unregister(&mddev->kobj);
	}
	spin_unlock(&all_mddevs_lock);
}

static mddev_t * mddev_find(dev_t unit)
{
	mddev_t *mddev, *new = NULL;

 retry:
	spin_lock(&all_mddevs_lock);
	list_for_each_entry(mddev, &all_mddevs, all_mddevs)
		if (mddev->unit == unit) {
			mddev_get(mddev);
			spin_unlock(&all_mddevs_lock);
			kfree(new);
			return mddev;
		}

	if (new) {
		list_add(&new->all_mddevs, &all_mddevs);
		spin_unlock(&all_mddevs_lock);
		return new;
	}
	spin_unlock(&all_mddevs_lock);

	new = (mddev_t *) kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return NULL;

	memset(new, 0, sizeof(*new));

	new->unit = unit;
	if (MAJOR(unit) == MD_MAJOR)
		new->md_minor = MINOR(unit);
	else
		new->md_minor = MINOR(unit) >> MdpMinorShift;

	init_MUTEX(&new->reconfig_sem);
	INIT_LIST_HEAD(&new->disks);
	INIT_LIST_HEAD(&new->all_mddevs);
	init_timer(&new->safemode_timer);
	atomic_set(&new->active, 1);
	spin_lock_init(&new->write_lock);
	init_waitqueue_head(&new->sb_wait);

	new->queue = blk_alloc_queue(GFP_KERNEL);
	if (!new->queue) {
		kfree(new);
		return NULL;
	}

	blk_queue_make_request(new->queue, md_fail_request);

	goto retry;
}

static inline int mddev_lock(mddev_t * mddev)
{
	return down_interruptible(&mddev->reconfig_sem);
}

static inline void mddev_lock_uninterruptible(mddev_t * mddev)
{
	down(&mddev->reconfig_sem);
}

static inline int mddev_trylock(mddev_t * mddev)
{
	return down_trylock(&mddev->reconfig_sem);
}

static inline void mddev_unlock(mddev_t * mddev)
{
	up(&mddev->reconfig_sem);

	md_wakeup_thread(mddev->thread);
}

mdk_rdev_t * find_rdev_nr(mddev_t *mddev, int nr)
{
	mdk_rdev_t * rdev;
	struct list_head *tmp;

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev->desc_nr == nr)
			return rdev;
	}
	return NULL;
}

static mdk_rdev_t * find_rdev(mddev_t * mddev, dev_t dev)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev->bdev->bd_dev == dev)
			return rdev;
	}
	return NULL;
}

static inline sector_t calc_dev_sboffset(struct block_device *bdev)
{
	sector_t size = bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;
	return MD_NEW_SIZE_BLOCKS(size);
}

static sector_t calc_dev_size(mdk_rdev_t *rdev, unsigned chunk_size)
{
	sector_t size;

	size = rdev->sb_offset;

	if (chunk_size)
		size &= ~((sector_t)chunk_size/1024 - 1);
	return size;
}

static int alloc_disk_sb(mdk_rdev_t * rdev)
{
	if (rdev->sb_page)
		MD_BUG();

	rdev->sb_page = alloc_page(GFP_KERNEL);
	if (!rdev->sb_page) {
		printk(KERN_ALERT "md: out of memory.\n");
		return -EINVAL;
	}

	return 0;
}

static void free_disk_sb(mdk_rdev_t * rdev)
{
	if (rdev->sb_page) {
		page_cache_release(rdev->sb_page);
		rdev->sb_loaded = 0;
		rdev->sb_page = NULL;
		rdev->sb_offset = 0;
		rdev->size = 0;
	}
}


static int super_written(struct bio *bio, unsigned int bytes_done, int error)
{
	mdk_rdev_t *rdev = bio->bi_private;
	mddev_t *mddev = rdev->mddev;
	if (bio->bi_size)
		return 1;

	if (error || !test_bit(BIO_UPTODATE, &bio->bi_flags))
		md_error(mddev, rdev);

	if (atomic_dec_and_test(&mddev->pending_writes))
		wake_up(&mddev->sb_wait);
	bio_put(bio);
	return 0;
}

static int super_written_barrier(struct bio *bio, unsigned int bytes_done, int error)
{
	struct bio *bio2 = bio->bi_private;
	mdk_rdev_t *rdev = bio2->bi_private;
	mddev_t *mddev = rdev->mddev;
	if (bio->bi_size)
		return 1;

	if (!test_bit(BIO_UPTODATE, &bio->bi_flags) &&
	    error == -EOPNOTSUPP) {
		unsigned long flags;
		/* barriers don't appear to be supported :-( */
		set_bit(BarriersNotsupp, &rdev->flags);
		mddev->barriers_work = 0;
		spin_lock_irqsave(&mddev->write_lock, flags);
		bio2->bi_next = mddev->biolist;
		mddev->biolist = bio2;
		spin_unlock_irqrestore(&mddev->write_lock, flags);
		wake_up(&mddev->sb_wait);
		bio_put(bio);
		return 0;
	}
	bio_put(bio2);
	bio->bi_private = rdev;
	return super_written(bio, bytes_done, error);
}

void md_super_write(mddev_t *mddev, mdk_rdev_t *rdev,
		   sector_t sector, int size, struct page *page)
{
	/* write first size bytes of page to sector of rdev
	 * Increment mddev->pending_writes before returning
	 * and decrement it on completion, waking up sb_wait
	 * if zero is reached.
	 * If an error occurred, call md_error
	 *
	 * As we might need to resubmit the request if BIO_RW_BARRIER
	 * causes ENOTSUPP, we allocate a spare bio...
	 */
	struct bio *bio = bio_alloc(GFP_NOIO, 1);
	int rw = (1<<BIO_RW) | (1<<BIO_RW_SYNC);

	bio->bi_bdev = rdev->bdev;
	bio->bi_sector = sector;
	bio_add_page(bio, page, size, 0);
	bio->bi_private = rdev;
	bio->bi_end_io = super_written;
	bio->bi_rw = rw;

	atomic_inc(&mddev->pending_writes);
	if (!test_bit(BarriersNotsupp, &rdev->flags)) {
		struct bio *rbio;
		rw |= (1<<BIO_RW_BARRIER);
		rbio = bio_clone(bio, GFP_NOIO);
		rbio->bi_private = bio;
		rbio->bi_end_io = super_written_barrier;
		submit_bio(rw, rbio);
	} else
		submit_bio(rw, bio);
}

void md_super_wait(mddev_t *mddev)
{
	/* wait for all superblock writes that were scheduled to complete.
	 * if any had to be retried (due to BARRIER problems), retry them
	 */
	DEFINE_WAIT(wq);
	for(;;) {
		prepare_to_wait(&mddev->sb_wait, &wq, TASK_UNINTERRUPTIBLE);
		if (atomic_read(&mddev->pending_writes)==0)
			break;
		while (mddev->biolist) {
			struct bio *bio;
			spin_lock_irq(&mddev->write_lock);
			bio = mddev->biolist;
			mddev->biolist = bio->bi_next ;
			bio->bi_next = NULL;
			spin_unlock_irq(&mddev->write_lock);
			submit_bio(bio->bi_rw, bio);
		}
		schedule();
	}
	finish_wait(&mddev->sb_wait, &wq);
}

static int bi_complete(struct bio *bio, unsigned int bytes_done, int error)
{
	if (bio->bi_size)
		return 1;

	complete((struct completion*)bio->bi_private);
	return 0;
}

int sync_page_io(struct block_device *bdev, sector_t sector, int size,
		   struct page *page, int rw)
{
	struct bio *bio = bio_alloc(GFP_NOIO, 1);
	struct completion event;
	int ret;

	rw |= (1 << BIO_RW_SYNC);

	bio->bi_bdev = bdev;
	bio->bi_sector = sector;
	bio_add_page(bio, page, size, 0);
	init_completion(&event);
	bio->bi_private = &event;
	bio->bi_end_io = bi_complete;
	submit_bio(rw, bio);
	wait_for_completion(&event);

	ret = test_bit(BIO_UPTODATE, &bio->bi_flags);
	bio_put(bio);
	return ret;
}

static int read_disk_sb(mdk_rdev_t * rdev, int size)
{
	char b[BDEVNAME_SIZE];
	if (!rdev->sb_page) {
		MD_BUG();
		return -EINVAL;
	}
	if (rdev->sb_loaded)
		return 0;


	if (!sync_page_io(rdev->bdev, rdev->sb_offset<<1, size, rdev->sb_page, READ))
		goto fail;
	rdev->sb_loaded = 1;
	return 0;

fail:
	printk(KERN_WARNING "md: disabled device %s, could not read superblock.\n",
		bdevname(rdev->bdev,b));
	return -EINVAL;
}

static int uuid_equal(mdp_super_t *sb1, mdp_super_t *sb2)
{
	if (	(sb1->set_uuid0 == sb2->set_uuid0) &&
		(sb1->set_uuid1 == sb2->set_uuid1) &&
		(sb1->set_uuid2 == sb2->set_uuid2) &&
		(sb1->set_uuid3 == sb2->set_uuid3))

		return 1;

	return 0;
}


static int sb_equal(mdp_super_t *sb1, mdp_super_t *sb2)
{
	int ret;
	mdp_super_t *tmp1, *tmp2;

	tmp1 = kmalloc(sizeof(*tmp1),GFP_KERNEL);
	tmp2 = kmalloc(sizeof(*tmp2),GFP_KERNEL);

	if (!tmp1 || !tmp2) {
		ret = 0;
		printk(KERN_INFO "md.c: sb1 is not equal to sb2!\n");
		goto abort;
	}

	*tmp1 = *sb1;
	*tmp2 = *sb2;

	/*
	 * nr_disks is not constant
	 */
	tmp1->nr_disks = 0;
	tmp2->nr_disks = 0;

	if (memcmp(tmp1, tmp2, MD_SB_GENERIC_CONSTANT_WORDS * 4))
		ret = 0;
	else
		ret = 1;

abort:
	kfree(tmp1);
	kfree(tmp2);
	return ret;
}

static unsigned int calc_sb_csum(mdp_super_t * sb)
{
	unsigned int disk_csum, csum;

	disk_csum = sb->sb_csum;
	sb->sb_csum = 0;
	csum = csum_partial((void *)sb, MD_SB_BYTES, 0);
	sb->sb_csum = disk_csum;
	return csum;
}


/*
 * Handle superblock details.
 * We want to be able to handle multiple superblock formats
 * so we have a common interface to them all, and an array of
 * different handlers.
 * We rely on user-space to write the initial superblock, and support
 * reading and updating of superblocks.
 * Interface methods are:
 *   int load_super(mdk_rdev_t *dev, mdk_rdev_t *refdev, int minor_version)
 *      loads and validates a superblock on dev.
 *      if refdev != NULL, compare superblocks on both devices
 *    Return:
 *      0 - dev has a superblock that is compatible with refdev
 *      1 - dev has a superblock that is compatible and newer than refdev
 *          so dev should be used as the refdev in future
 *     -EINVAL superblock incompatible or invalid
 *     -othererror e.g. -EIO
 *
 *   int validate_super(mddev_t *mddev, mdk_rdev_t *dev)
 *      Verify that dev is acceptable into mddev.
 *       The first time, mddev->raid_disks will be 0, and data from
 *       dev should be merged in.  Subsequent calls check that dev
 *       is new enough.  Return 0 or -EINVAL
 *
 *   void sync_super(mddev_t *mddev, mdk_rdev_t *dev)
 *     Update the superblock for rdev with data in mddev
 *     This does not write to disc.
 *
 */

struct super_type  {
	char 		*name;
	struct module	*owner;
	int		(*load_super)(mdk_rdev_t *rdev, mdk_rdev_t *refdev, int minor_version);
	int		(*validate_super)(mddev_t *mddev, mdk_rdev_t *rdev);
	void		(*sync_super)(mddev_t *mddev, mdk_rdev_t *rdev);
};

/*
 * load_super for 0.90.0 
 */
static int super_90_load(mdk_rdev_t *rdev, mdk_rdev_t *refdev, int minor_version)
{
	char b[BDEVNAME_SIZE], b2[BDEVNAME_SIZE];
	mdp_super_t *sb;
	int ret;
	sector_t sb_offset;

	/*
	 * Calculate the position of the superblock,
	 * it's at the end of the disk.
	 *
	 * It also happens to be a multiple of 4Kb.
	 */
	sb_offset = calc_dev_sboffset(rdev->bdev);
	rdev->sb_offset = sb_offset;

	ret = read_disk_sb(rdev, MD_SB_BYTES);
	if (ret) return ret;

	ret = -EINVAL;

	bdevname(rdev->bdev, b);
	sb = (mdp_super_t*)page_address(rdev->sb_page);

	if (sb->md_magic != MD_SB_MAGIC) {
		printk(KERN_ERR "md: invalid raid superblock magic on %s\n",
		       b);
		goto abort;
	}

	if (sb->major_version != 0 ||
	    sb->minor_version != 90) {
		printk(KERN_WARNING "Bad version number %d.%d on %s\n",
			sb->major_version, sb->minor_version,
			b);
		goto abort;
	}

	if (sb->raid_disks <= 0)
		goto abort;

	if (csum_fold(calc_sb_csum(sb)) != csum_fold(sb->sb_csum)) {
		printk(KERN_WARNING "md: invalid superblock checksum on %s\n",
			b);
		goto abort;
	}

	rdev->preferred_minor = sb->md_minor;
	rdev->data_offset = 0;
	rdev->sb_size = MD_SB_BYTES;

	if (sb->level == LEVEL_MULTIPATH)
		rdev->desc_nr = -1;
	else
		rdev->desc_nr = sb->this_disk.number;

	if (refdev == 0)
		ret = 1;
	else {
		__u64 ev1, ev2;
		mdp_super_t *refsb = (mdp_super_t*)page_address(refdev->sb_page);
		if (!uuid_equal(refsb, sb)) {
			printk(KERN_WARNING "md: %s has different UUID to %s\n",
				b, bdevname(refdev->bdev,b2));
			goto abort;
		}
		if (!sb_equal(refsb, sb)) {
			printk(KERN_WARNING "md: %s has same UUID"
			       " but different superblock to %s\n",
			       b, bdevname(refdev->bdev, b2));
			goto abort;
		}
		ev1 = md_event(sb);
		ev2 = md_event(refsb);
		if (ev1 > ev2)
			ret = 1;
		else 
			ret = 0;
	}
	rdev->size = calc_dev_size(rdev, sb->chunk_size);

 abort:
	return ret;
}

/*
 * validate_super for 0.90.0
 */
static int super_90_validate(mddev_t *mddev, mdk_rdev_t *rdev)
{
	mdp_disk_t *desc;
	mdp_super_t *sb = (mdp_super_t *)page_address(rdev->sb_page);

	rdev->raid_disk = -1;
	rdev->flags = 0;
	if (mddev->raid_disks == 0) {
		mddev->major_version = 0;
		mddev->minor_version = sb->minor_version;
		mddev->patch_version = sb->patch_version;
		mddev->persistent = ! sb->not_persistent;
		mddev->chunk_size = sb->chunk_size;
		mddev->ctime = sb->ctime;
		mddev->utime = sb->utime;
		mddev->level = sb->level;
		mddev->layout = sb->layout;
		mddev->raid_disks = sb->raid_disks;
		mddev->size = sb->size;
		mddev->events = md_event(sb);
		mddev->bitmap_offset = 0;
		mddev->default_bitmap_offset = MD_SB_BYTES >> 9;

		if (sb->state & (1<<MD_SB_CLEAN))
			mddev->recovery_cp = MaxSector;
		else {
			if (sb->events_hi == sb->cp_events_hi && 
				sb->events_lo == sb->cp_events_lo) {
				mddev->recovery_cp = sb->recovery_cp;
			} else
				mddev->recovery_cp = 0;
		}

		memcpy(mddev->uuid+0, &sb->set_uuid0, 4);
		memcpy(mddev->uuid+4, &sb->set_uuid1, 4);
		memcpy(mddev->uuid+8, &sb->set_uuid2, 4);
		memcpy(mddev->uuid+12,&sb->set_uuid3, 4);

		mddev->max_disks = MD_SB_DISKS;

		if (sb->state & (1<<MD_SB_BITMAP_PRESENT) &&
		    mddev->bitmap_file == NULL) {
			if (mddev->level != 1 && mddev->level != 5 && mddev->level != 6) {
				/* FIXME use a better test */
				printk(KERN_WARNING "md: bitmaps only support for raid1\n");
				return -EINVAL;
			}
			mddev->bitmap_offset = mddev->default_bitmap_offset;
		}

	} else if (mddev->pers == NULL) {
		/* Insist on good event counter while assembling */
		__u64 ev1 = md_event(sb);
		++ev1;
		if (ev1 < mddev->events) 
			return -EINVAL;
	} else if (mddev->bitmap) {
		/* if adding to array with a bitmap, then we can accept an
		 * older device ... but not too old.
		 */
		__u64 ev1 = md_event(sb);
		if (ev1 < mddev->bitmap->events_cleared)
			return 0;
	} else /* just a hot-add of a new device, leave raid_disk at -1 */
		return 0;

	if (mddev->level != LEVEL_MULTIPATH) {
		desc = sb->disks + rdev->desc_nr;

		if (desc->state & (1<<MD_DISK_FAULTY))
			set_bit(Faulty, &rdev->flags);
		else if (desc->state & (1<<MD_DISK_SYNC) &&
			 desc->raid_disk < mddev->raid_disks) {
			set_bit(In_sync, &rdev->flags);
			rdev->raid_disk = desc->raid_disk;
		}
		if (desc->state & (1<<MD_DISK_WRITEMOSTLY))
			set_bit(WriteMostly, &rdev->flags);
	} else /* MULTIPATH are always insync */
		set_bit(In_sync, &rdev->flags);
	return 0;
}

/*
 * sync_super for 0.90.0
 */
static void super_90_sync(mddev_t *mddev, mdk_rdev_t *rdev)
{
	mdp_super_t *sb;
	struct list_head *tmp;
	mdk_rdev_t *rdev2;
	int next_spare = mddev->raid_disks;


	/* make rdev->sb match mddev data..
	 *
	 * 1/ zero out disks
	 * 2/ Add info for each disk, keeping track of highest desc_nr (next_spare);
	 * 3/ any empty disks < next_spare become removed
	 *
	 * disks[0] gets initialised to REMOVED because
	 * we cannot be sure from other fields if it has
	 * been initialised or not.
	 */
	int i;
	int active=0, working=0,failed=0,spare=0,nr_disks=0;

	rdev->sb_size = MD_SB_BYTES;

	sb = (mdp_super_t*)page_address(rdev->sb_page);

	memset(sb, 0, sizeof(*sb));

	sb->md_magic = MD_SB_MAGIC;
	sb->major_version = mddev->major_version;
	sb->minor_version = mddev->minor_version;
	sb->patch_version = mddev->patch_version;
	sb->gvalid_words  = 0; /* ignored */
	memcpy(&sb->set_uuid0, mddev->uuid+0, 4);
	memcpy(&sb->set_uuid1, mddev->uuid+4, 4);
	memcpy(&sb->set_uuid2, mddev->uuid+8, 4);
	memcpy(&sb->set_uuid3, mddev->uuid+12,4);

	sb->ctime = mddev->ctime;
	sb->level = mddev->level;
	sb->size  = mddev->size;
	sb->raid_disks = mddev->raid_disks;
	sb->md_minor = mddev->md_minor;
	sb->not_persistent = !mddev->persistent;
	sb->utime = mddev->utime;
	sb->state = 0;
	sb->events_hi = (mddev->events>>32);
	sb->events_lo = (u32)mddev->events;

	if (mddev->in_sync)
	{
		sb->recovery_cp = mddev->recovery_cp;
		sb->cp_events_hi = (mddev->events>>32);
		sb->cp_events_lo = (u32)mddev->events;
		if (mddev->recovery_cp == MaxSector)
			sb->state = (1<< MD_SB_CLEAN);
	} else
		sb->recovery_cp = 0;

	sb->layout = mddev->layout;
	sb->chunk_size = mddev->chunk_size;

	if (mddev->bitmap && mddev->bitmap_file == NULL)
		sb->state |= (1<<MD_SB_BITMAP_PRESENT);

	sb->disks[0].state = (1<<MD_DISK_REMOVED);
	ITERATE_RDEV(mddev,rdev2,tmp) {
		mdp_disk_t *d;
		int desc_nr;
		if (rdev2->raid_disk >= 0 && test_bit(In_sync, &rdev2->flags)
		    && !test_bit(Faulty, &rdev2->flags))
			desc_nr = rdev2->raid_disk;
		else
			desc_nr = next_spare++;
		rdev2->desc_nr = desc_nr;
		d = &sb->disks[rdev2->desc_nr];
		nr_disks++;
		d->number = rdev2->desc_nr;
		d->major = MAJOR(rdev2->bdev->bd_dev);
		d->minor = MINOR(rdev2->bdev->bd_dev);
		if (rdev2->raid_disk >= 0 && test_bit(In_sync, &rdev2->flags)
		    && !test_bit(Faulty, &rdev2->flags))
			d->raid_disk = rdev2->raid_disk;
		else
			d->raid_disk = rdev2->desc_nr; /* compatibility */
		if (test_bit(Faulty, &rdev2->flags)) {
			d->state = (1<<MD_DISK_FAULTY);
			failed++;
		} else if (test_bit(In_sync, &rdev2->flags)) {
			d->state = (1<<MD_DISK_ACTIVE);
			d->state |= (1<<MD_DISK_SYNC);
			active++;
			working++;
		} else {
			d->state = 0;
			spare++;
			working++;
		}
		if (test_bit(WriteMostly, &rdev2->flags))
			d->state |= (1<<MD_DISK_WRITEMOSTLY);
	}
	/* now set the "removed" and "faulty" bits on any missing devices */
	for (i=0 ; i < mddev->raid_disks ; i++) {
		mdp_disk_t *d = &sb->disks[i];
		if (d->state == 0 && d->number == 0) {
			d->number = i;
			d->raid_disk = i;
			d->state = (1<<MD_DISK_REMOVED);
			d->state |= (1<<MD_DISK_FAULTY);
			failed++;
		}
	}
	sb->nr_disks = nr_disks;
	sb->active_disks = active;
	sb->working_disks = working;
	sb->failed_disks = failed;
	sb->spare_disks = spare;

	sb->this_disk = sb->disks[rdev->desc_nr];
	sb->sb_csum = calc_sb_csum(sb);
}

/*
 * version 1 superblock
 */

static unsigned int calc_sb_1_csum(struct mdp_superblock_1 * sb)
{
	unsigned int disk_csum, csum;
	unsigned long long newcsum;
	int size = 256 + le32_to_cpu(sb->max_dev)*2;
	unsigned int *isuper = (unsigned int*)sb;
	int i;

	disk_csum = sb->sb_csum;
	sb->sb_csum = 0;
	newcsum = 0;
	for (i=0; size>=4; size -= 4 )
		newcsum += le32_to_cpu(*isuper++);

	if (size == 2)
		newcsum += le16_to_cpu(*(unsigned short*) isuper);

	csum = (newcsum & 0xffffffff) + (newcsum >> 32);
	sb->sb_csum = disk_csum;
	return cpu_to_le32(csum);
}

static int super_1_load(mdk_rdev_t *rdev, mdk_rdev_t *refdev, int minor_version)
{
	struct mdp_superblock_1 *sb;
	int ret;
	sector_t sb_offset;
	char b[BDEVNAME_SIZE], b2[BDEVNAME_SIZE];
	int bmask;

	/*
	 * Calculate the position of the superblock.
	 * It is always aligned to a 4K boundary and
	 * depeding on minor_version, it can be:
	 * 0: At least 8K, but less than 12K, from end of device
	 * 1: At start of device
	 * 2: 4K from start of device.
	 */
	switch(minor_version) {
	case 0:
		sb_offset = rdev->bdev->bd_inode->i_size >> 9;
		sb_offset -= 8*2;
		sb_offset &= ~(sector_t)(4*2-1);
		/* convert from sectors to K */
		sb_offset /= 2;
		break;
	case 1:
		sb_offset = 0;
		break;
	case 2:
		sb_offset = 4;
		break;
	default:
		return -EINVAL;
	}
	rdev->sb_offset = sb_offset;

	/* superblock is rarely larger than 1K, but it can be larger,
	 * and it is safe to read 4k, so we do that
	 */
	ret = read_disk_sb(rdev, 4096);
	if (ret) return ret;


	sb = (struct mdp_superblock_1*)page_address(rdev->sb_page);

	if (sb->magic != cpu_to_le32(MD_SB_MAGIC) ||
	    sb->major_version != cpu_to_le32(1) ||
	    le32_to_cpu(sb->max_dev) > (4096-256)/2 ||
	    le64_to_cpu(sb->super_offset) != (rdev->sb_offset<<1) ||
	    (le32_to_cpu(sb->feature_map) & ~MD_FEATURE_ALL) != 0)
		return -EINVAL;

	if (calc_sb_1_csum(sb) != sb->sb_csum) {
		printk("md: invalid superblock checksum on %s\n",
			bdevname(rdev->bdev,b));
		return -EINVAL;
	}
	if (le64_to_cpu(sb->data_size) < 10) {
		printk("md: data_size too small on %s\n",
		       bdevname(rdev->bdev,b));
		return -EINVAL;
	}
	rdev->preferred_minor = 0xffff;
	rdev->data_offset = le64_to_cpu(sb->data_offset);

	rdev->sb_size = le32_to_cpu(sb->max_dev) * 2 + 256;
	bmask = queue_hardsect_size(rdev->bdev->bd_disk->queue)-1;
	if (rdev->sb_size & bmask)
		rdev-> sb_size = (rdev->sb_size | bmask)+1;

	if (refdev == 0)
		return 1;
	else {
		__u64 ev1, ev2;
		struct mdp_superblock_1 *refsb = 
			(struct mdp_superblock_1*)page_address(refdev->sb_page);

		if (memcmp(sb->set_uuid, refsb->set_uuid, 16) != 0 ||
		    sb->level != refsb->level ||
		    sb->layout != refsb->layout ||
		    sb->chunksize != refsb->chunksize) {
			printk(KERN_WARNING "md: %s has strangely different"
				" superblock to %s\n",
				bdevname(rdev->bdev,b),
				bdevname(refdev->bdev,b2));
			return -EINVAL;
		}
		ev1 = le64_to_cpu(sb->events);
		ev2 = le64_to_cpu(refsb->events);

		if (ev1 > ev2)
			return 1;
	}
	if (minor_version) 
		rdev->size = ((rdev->bdev->bd_inode->i_size>>9) - le64_to_cpu(sb->data_offset)) / 2;
	else
		rdev->size = rdev->sb_offset;
	if (rdev->size < le64_to_cpu(sb->data_size)/2)
		return -EINVAL;
	rdev->size = le64_to_cpu(sb->data_size)/2;
	if (le32_to_cpu(sb->chunksize))
		rdev->size &= ~((sector_t)le32_to_cpu(sb->chunksize)/2 - 1);
	return 0;
}

static int super_1_validate(mddev_t *mddev, mdk_rdev_t *rdev)
{
	struct mdp_superblock_1 *sb = (struct mdp_superblock_1*)page_address(rdev->sb_page);

	rdev->raid_disk = -1;
	rdev->flags = 0;
	if (mddev->raid_disks == 0) {
		mddev->major_version = 1;
		mddev->patch_version = 0;
		mddev->persistent = 1;
		mddev->chunk_size = le32_to_cpu(sb->chunksize) << 9;
		mddev->ctime = le64_to_cpu(sb->ctime) & ((1ULL << 32)-1);
		mddev->utime = le64_to_cpu(sb->utime) & ((1ULL << 32)-1);
		mddev->level = le32_to_cpu(sb->level);
		mddev->layout = le32_to_cpu(sb->layout);
		mddev->raid_disks = le32_to_cpu(sb->raid_disks);
		mddev->size = le64_to_cpu(sb->size)/2;
		mddev->events = le64_to_cpu(sb->events);
		mddev->bitmap_offset = 0;
		mddev->default_bitmap_offset = 1024;
		
		mddev->recovery_cp = le64_to_cpu(sb->resync_offset);
		memcpy(mddev->uuid, sb->set_uuid, 16);

		mddev->max_disks =  (4096-256)/2;

		if ((le32_to_cpu(sb->feature_map) & MD_FEATURE_BITMAP_OFFSET) &&
		    mddev->bitmap_file == NULL ) {
			if (mddev->level != 1) {
				printk(KERN_WARNING "md: bitmaps only supported for raid1\n");
				return -EINVAL;
			}
			mddev->bitmap_offset = (__s32)le32_to_cpu(sb->bitmap_offset);
		}
	} else if (mddev->pers == NULL) {
		/* Insist of good event counter while assembling */
		__u64 ev1 = le64_to_cpu(sb->events);
		++ev1;
		if (ev1 < mddev->events)
			return -EINVAL;
	} else if (mddev->bitmap) {
		/* If adding to array with a bitmap, then we can accept an
		 * older device, but not too old.
		 */
		__u64 ev1 = le64_to_cpu(sb->events);
		if (ev1 < mddev->bitmap->events_cleared)
			return 0;
	} else /* just a hot-add of a new device, leave raid_disk at -1 */
		return 0;

	if (mddev->level != LEVEL_MULTIPATH) {
		int role;
		rdev->desc_nr = le32_to_cpu(sb->dev_number);
		role = le16_to_cpu(sb->dev_roles[rdev->desc_nr]);
		switch(role) {
		case 0xffff: /* spare */
			break;
		case 0xfffe: /* faulty */
			set_bit(Faulty, &rdev->flags);
			break;
		default:
			set_bit(In_sync, &rdev->flags);
			rdev->raid_disk = role;
			break;
		}
		if (sb->devflags & WriteMostly1)
			set_bit(WriteMostly, &rdev->flags);
	} else /* MULTIPATH are always insync */
		set_bit(In_sync, &rdev->flags);

	return 0;
}

static void super_1_sync(mddev_t *mddev, mdk_rdev_t *rdev)
{
	struct mdp_superblock_1 *sb;
	struct list_head *tmp;
	mdk_rdev_t *rdev2;
	int max_dev, i;
	/* make rdev->sb match mddev and rdev data. */

	sb = (struct mdp_superblock_1*)page_address(rdev->sb_page);

	sb->feature_map = 0;
	sb->pad0 = 0;
	memset(sb->pad1, 0, sizeof(sb->pad1));
	memset(sb->pad2, 0, sizeof(sb->pad2));
	memset(sb->pad3, 0, sizeof(sb->pad3));

	sb->utime = cpu_to_le64((__u64)mddev->utime);
	sb->events = cpu_to_le64(mddev->events);
	if (mddev->in_sync)
		sb->resync_offset = cpu_to_le64(mddev->recovery_cp);
	else
		sb->resync_offset = cpu_to_le64(0);

	if (mddev->bitmap && mddev->bitmap_file == NULL) {
		sb->bitmap_offset = cpu_to_le32((__u32)mddev->bitmap_offset);
		sb->feature_map = cpu_to_le32(MD_FEATURE_BITMAP_OFFSET);
	}

	max_dev = 0;
	ITERATE_RDEV(mddev,rdev2,tmp)
		if (rdev2->desc_nr+1 > max_dev)
			max_dev = rdev2->desc_nr+1;
	
	sb->max_dev = cpu_to_le32(max_dev);
	for (i=0; i<max_dev;i++)
		sb->dev_roles[i] = cpu_to_le16(0xfffe);
	
	ITERATE_RDEV(mddev,rdev2,tmp) {
		i = rdev2->desc_nr;
		if (test_bit(Faulty, &rdev2->flags))
			sb->dev_roles[i] = cpu_to_le16(0xfffe);
		else if (test_bit(In_sync, &rdev2->flags))
			sb->dev_roles[i] = cpu_to_le16(rdev2->raid_disk);
		else
			sb->dev_roles[i] = cpu_to_le16(0xffff);
	}

	sb->recovery_offset = cpu_to_le64(0); /* not supported yet */
	sb->sb_csum = calc_sb_1_csum(sb);
}


static struct super_type super_types[] = {
	[0] = {
		.name	= "0.90.0",
		.owner	= THIS_MODULE,
		.load_super	= super_90_load,
		.validate_super	= super_90_validate,
		.sync_super	= super_90_sync,
	},
	[1] = {
		.name	= "md-1",
		.owner	= THIS_MODULE,
		.load_super	= super_1_load,
		.validate_super	= super_1_validate,
		.sync_super	= super_1_sync,
	},
};
	
static mdk_rdev_t * match_dev_unit(mddev_t *mddev, mdk_rdev_t *dev)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev,rdev,tmp)
		if (rdev->bdev->bd_contains == dev->bdev->bd_contains)
			return rdev;

	return NULL;
}

static int match_mddev_units(mddev_t *mddev1, mddev_t *mddev2)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev1,rdev,tmp)
		if (match_dev_unit(mddev2, rdev))
			return 1;

	return 0;
}

static LIST_HEAD(pending_raid_disks);

static int bind_rdev_to_array(mdk_rdev_t * rdev, mddev_t * mddev)
{
	mdk_rdev_t *same_pdev;
	char b[BDEVNAME_SIZE], b2[BDEVNAME_SIZE];
	struct kobject *ko;
	char *s;

	if (rdev->mddev) {
		MD_BUG();
		return -EINVAL;
	}
	same_pdev = match_dev_unit(mddev, rdev);
	if (same_pdev)
		printk(KERN_WARNING
			"%s: WARNING: %s appears to be on the same physical"
	 		" disk as %s. True\n     protection against single-disk"
			" failure might be compromised.\n",
			mdname(mddev), bdevname(rdev->bdev,b),
			bdevname(same_pdev->bdev,b2));

	/* Verify rdev->desc_nr is unique.
	 * If it is -1, assign a free number, else
	 * check number is not in use
	 */
	if (rdev->desc_nr < 0) {
		int choice = 0;
		if (mddev->pers) choice = mddev->raid_disks;
		while (find_rdev_nr(mddev, choice))
			choice++;
		rdev->desc_nr = choice;
	} else {
		if (find_rdev_nr(mddev, rdev->desc_nr))
			return -EBUSY;
	}
	bdevname(rdev->bdev,b);
	if (kobject_set_name(&rdev->kobj, "dev-%s", b) < 0)
		return -ENOMEM;
	while ( (s=strchr(rdev->kobj.k_name, '/')) != NULL)
		*s = '!';
			
	list_add(&rdev->same_set, &mddev->disks);
	rdev->mddev = mddev;
	printk(KERN_INFO "md: bind<%s>\n", b);

	rdev->kobj.parent = &mddev->kobj;
	kobject_add(&rdev->kobj);

	if (rdev->bdev->bd_part)
		ko = &rdev->bdev->bd_part->kobj;
	else
		ko = &rdev->bdev->bd_disk->kobj;
	sysfs_create_link(&rdev->kobj, ko, "block");
	return 0;
}

static void unbind_rdev_from_array(mdk_rdev_t * rdev)
{
	char b[BDEVNAME_SIZE];
	if (!rdev->mddev) {
		MD_BUG();
		return;
	}
	list_del_init(&rdev->same_set);
	printk(KERN_INFO "md: unbind<%s>\n", bdevname(rdev->bdev,b));
	rdev->mddev = NULL;
	sysfs_remove_link(&rdev->kobj, "block");
	kobject_del(&rdev->kobj);
}

/*
 * prevent the device from being mounted, repartitioned or
 * otherwise reused by a RAID array (or any other kernel
 * subsystem), by bd_claiming the device.
 */
static int lock_rdev(mdk_rdev_t *rdev, dev_t dev)
{
	int err = 0;
	struct block_device *bdev;
	char b[BDEVNAME_SIZE];

	bdev = open_by_devnum(dev, FMODE_READ|FMODE_WRITE);
	if (IS_ERR(bdev)) {
		printk(KERN_ERR "md: could not open %s.\n",
			__bdevname(dev, b));
		return PTR_ERR(bdev);
	}
	err = bd_claim(bdev, rdev);
	if (err) {
		printk(KERN_ERR "md: could not bd_claim %s.\n",
			bdevname(bdev, b));
		blkdev_put(bdev);
		return err;
	}
	rdev->bdev = bdev;
	return err;
}

static void unlock_rdev(mdk_rdev_t *rdev)
{
	struct block_device *bdev = rdev->bdev;
	rdev->bdev = NULL;
	if (!bdev)
		MD_BUG();
	bd_release(bdev);
	blkdev_put(bdev);
}

void md_autodetect_dev(dev_t dev);

static void export_rdev(mdk_rdev_t * rdev)
{
	char b[BDEVNAME_SIZE];
	printk(KERN_INFO "md: export_rdev(%s)\n",
		bdevname(rdev->bdev,b));
	if (rdev->mddev)
		MD_BUG();
	free_disk_sb(rdev);
	list_del_init(&rdev->same_set);
#ifndef MODULE
	md_autodetect_dev(rdev->bdev->bd_dev);
#endif
	unlock_rdev(rdev);
	kobject_put(&rdev->kobj);
}

static void kick_rdev_from_array(mdk_rdev_t * rdev)
{
	unbind_rdev_from_array(rdev);
	export_rdev(rdev);
}

static void export_array(mddev_t *mddev)
{
	struct list_head *tmp;
	mdk_rdev_t *rdev;

	ITERATE_RDEV(mddev,rdev,tmp) {
		if (!rdev->mddev) {
			MD_BUG();
			continue;
		}
		kick_rdev_from_array(rdev);
	}
	if (!list_empty(&mddev->disks))
		MD_BUG();
	mddev->raid_disks = 0;
	mddev->major_version = 0;
}

static void print_desc(mdp_disk_t *desc)
{
	printk(" DISK<N:%d,(%d,%d),R:%d,S:%d>\n", desc->number,
		desc->major,desc->minor,desc->raid_disk,desc->state);
}

static void print_sb(mdp_super_t *sb)
{
	int i;

	printk(KERN_INFO 
		"md:  SB: (V:%d.%d.%d) ID:<%08x.%08x.%08x.%08x> CT:%08x\n",
		sb->major_version, sb->minor_version, sb->patch_version,
		sb->set_uuid0, sb->set_uuid1, sb->set_uuid2, sb->set_uuid3,
		sb->ctime);
	printk(KERN_INFO "md:     L%d S%08d ND:%d RD:%d md%d LO:%d CS:%d\n",
		sb->level, sb->size, sb->nr_disks, sb->raid_disks,
		sb->md_minor, sb->layout, sb->chunk_size);
	printk(KERN_INFO "md:     UT:%08x ST:%d AD:%d WD:%d"
		" FD:%d SD:%d CSUM:%08x E:%08lx\n",
		sb->utime, sb->state, sb->active_disks, sb->working_disks,
		sb->failed_disks, sb->spare_disks,
		sb->sb_csum, (unsigned long)sb->events_lo);

	printk(KERN_INFO);
	for (i = 0; i < MD_SB_DISKS; i++) {
		mdp_disk_t *desc;

		desc = sb->disks + i;
		if (desc->number || desc->major || desc->minor ||
		    desc->raid_disk || (desc->state && (desc->state != 4))) {
			printk("     D %2d: ", i);
			print_desc(desc);
		}
	}
	printk(KERN_INFO "md:     THIS: ");
	print_desc(&sb->this_disk);

}

static void print_rdev(mdk_rdev_t *rdev)
{
	char b[BDEVNAME_SIZE];
	printk(KERN_INFO "md: rdev %s, SZ:%08llu F:%d S:%d DN:%u\n",
		bdevname(rdev->bdev,b), (unsigned long long)rdev->size,
	        test_bit(Faulty, &rdev->flags), test_bit(In_sync, &rdev->flags),
	        rdev->desc_nr);
	if (rdev->sb_loaded) {
		printk(KERN_INFO "md: rdev superblock:\n");
		print_sb((mdp_super_t*)page_address(rdev->sb_page));
	} else
		printk(KERN_INFO "md: no rdev superblock!\n");
}

void md_print_devices(void)
{
	struct list_head *tmp, *tmp2;
	mdk_rdev_t *rdev;
	mddev_t *mddev;
	char b[BDEVNAME_SIZE];

	printk("\n");
	printk("md:	**********************************\n");
	printk("md:	* <COMPLETE RAID STATE PRINTOUT> *\n");
	printk("md:	**********************************\n");
	ITERATE_MDDEV(mddev,tmp) {

		if (mddev->bitmap)
			bitmap_print_sb(mddev->bitmap);
		else
			printk("%s: ", mdname(mddev));
		ITERATE_RDEV(mddev,rdev,tmp2)
			printk("<%s>", bdevname(rdev->bdev,b));
		printk("\n");

		ITERATE_RDEV(mddev,rdev,tmp2)
			print_rdev(rdev);
	}
	printk("md:	**********************************\n");
	printk("\n");
}


static void sync_sbs(mddev_t * mddev)
{
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	ITERATE_RDEV(mddev,rdev,tmp) {
		super_types[mddev->major_version].
			sync_super(mddev, rdev);
		rdev->sb_loaded = 1;
	}
}

static void md_update_sb(mddev_t * mddev)
{
	int err;
	struct list_head *tmp;
	mdk_rdev_t *rdev;
	int sync_req;

repeat:
	spin_lock_irq(&mddev->write_lock);
	sync_req = mddev->in_sync;
	mddev->utime = get_seconds();
	mddev->events ++;

	if (!mddev->events) {
		/*
		 * oops, this 64-bit counter should never wrap.
		 * Either we are in around ~1 trillion A.C., assuming
		 * 1 reboot per second, or we have a bug:
		 */
		MD_BUG();
		mddev->events --;
	}
	mddev->sb_dirty = 2;
	sync_sbs(mddev);

	/*
	 * do not write anything to disk if using
	 * nonpersistent superblocks
	 */
	if (!mddev->persistent) {
		mddev->sb_dirty = 0;
		spin_unlock_irq(&mddev->write_lock);
		wake_up(&mddev->sb_wait);
		return;
	}
	spin_unlock_irq(&mddev->write_lock);

	dprintk(KERN_INFO 
		"md: updating %s RAID superblock on device (in sync %d)\n",
		mdname(mddev),mddev->in_sync);

	err = bitmap_update_sb(mddev->bitmap);
	ITERATE_RDEV(mddev,rdev,tmp) {
		char b[BDEVNAME_SIZE];
		dprintk(KERN_INFO "md: ");
		if (test_bit(Faulty, &rdev->flags))
			dprintk("(skipping faulty ");

		dprintk("%s ", bdevname(rdev->bdev,b));
		if (!test_bit(Faulty, &rdev->flags)) {
			md_super_write(mddev,rdev,
				       rdev->sb_offset<<1, rdev->sb_size,
				       rdev->sb_page);
			dprintk(KERN_INFO "(write) %s's sb offset: %llu\n",
				bdevname(rdev->bdev,b),
				(unsigned long long)rdev->sb_offset);

		} else
			dprintk(")\n");
		if (mddev->level == LEVEL_MULTIPATH)
			/* only need to write one superblock... */
			break;
	}
	md_super_wait(mddev);
	/* if there was a failure, sb_dirty was set to 1, and we re-write super */

	spin_lock_irq(&mddev->write_lock);
	if (mddev->in_sync != sync_req|| mddev->sb_dirty == 1) {
		/* have to write it out again */
		spin_unlock_irq(&mddev->write_lock);
		goto repeat;
	}
	mddev->sb_dirty = 0;
	spin_unlock_irq(&mddev->write_lock);
	wake_up(&mddev->sb_wait);

}

struct rdev_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(mdk_rdev_t *, char *);
	ssize_t (*store)(mdk_rdev_t *, const char *, size_t);
};

static ssize_t
state_show(mdk_rdev_t *rdev, char *page)
{
	char *sep = "";
	int len=0;

	if (test_bit(Faulty, &rdev->flags)) {
		len+= sprintf(page+len, "%sfaulty",sep);
		sep = ",";
	}
	if (test_bit(In_sync, &rdev->flags)) {
		len += sprintf(page+len, "%sin_sync",sep);
		sep = ",";
	}
	if (!test_bit(Faulty, &rdev->flags) &&
	    !test_bit(In_sync, &rdev->flags)) {
		len += sprintf(page+len, "%sspare", sep);
		sep = ",";
	}
	return len+sprintf(page+len, "\n");
}

static struct rdev_sysfs_entry
rdev_state = __ATTR_RO(state);

static ssize_t
super_show(mdk_rdev_t *rdev, char *page)
{
	if (rdev->sb_loaded && rdev->sb_size) {
		memcpy(page, page_address(rdev->sb_page), rdev->sb_size);
		return rdev->sb_size;
	} else
		return 0;
}
static struct rdev_sysfs_entry rdev_super = __ATTR_RO(super);

static struct attribute *rdev_default_attrs[] = {
	&rdev_state.attr,
	&rdev_super.attr,
	NULL,
};
static ssize_t
rdev_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct rdev_sysfs_entry *entry = container_of(attr, struct rdev_sysfs_entry, attr);
	mdk_rdev_t *rdev = container_of(kobj, mdk_rdev_t, kobj);

	if (!entry->show)
		return -EIO;
	return entry->show(rdev, page);
}

static ssize_t
rdev_attr_store(struct kobject *kobj, struct attribute *attr,
	      const char *page, size_t length)
{
	struct rdev_sysfs_entry *entry = container_of(attr, struct rdev_sysfs_entry, attr);
	mdk_rdev_t *rdev = container_of(kobj, mdk_rdev_t, kobj);

	if (!entry->store)
		return -EIO;
	return entry->store(rdev, page, length);
}

static void rdev_free(struct kobject *ko)
{
	mdk_rdev_t *rdev = container_of(ko, mdk_rdev_t, kobj);
	kfree(rdev);
}
static struct sysfs_ops rdev_sysfs_ops = {
	.show		= rdev_attr_show,
	.store		= rdev_attr_store,
};
static struct kobj_type rdev_ktype = {
	.release	= rdev_free,
	.sysfs_ops	= &rdev_sysfs_ops,
	.default_attrs	= rdev_default_attrs,
};

/*
 * Import a device. If 'super_format' >= 0, then sanity check the superblock
 *
 * mark the device faulty if:
 *
 *   - the device is nonexistent (zero size)
 *   - the device has no valid superblock
 *
 * a faulty rdev _never_ has rdev->sb set.
 */
static mdk_rdev_t *md_import_device(dev_t newdev, int super_format, int super_minor)
{
	char b[BDEVNAME_SIZE];
	int err;
	mdk_rdev_t *rdev;
	sector_t size;

	rdev = (mdk_rdev_t *) kmalloc(sizeof(*rdev), GFP_KERNEL);
	if (!rdev) {
		printk(KERN_ERR "md: could not alloc mem for new device!\n");
		return ERR_PTR(-ENOMEM);
	}
	memset(rdev, 0, sizeof(*rdev));

	if ((err = alloc_disk_sb(rdev)))
		goto abort_free;

	err = lock_rdev(rdev, newdev);
	if (err)
		goto abort_free;

	rdev->kobj.parent = NULL;
	rdev->kobj.ktype = &rdev_ktype;
	kobject_init(&rdev->kobj);

	rdev->desc_nr = -1;
	rdev->flags = 0;
	rdev->data_offset = 0;
	atomic_set(&rdev->nr_pending, 0);
	atomic_set(&rdev->read_errors, 0);

	size = rdev->bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;
	if (!size) {
		printk(KERN_WARNING 
			"md: %s has zero or unknown size, marking faulty!\n",
			bdevname(rdev->bdev,b));
		err = -EINVAL;
		goto abort_free;
	}

	if (super_format >= 0) {
		err = super_types[super_format].
			load_super(rdev, NULL, super_minor);
		if (err == -EINVAL) {
			printk(KERN_WARNING 
				"md: %s has invalid sb, not importing!\n",
				bdevname(rdev->bdev,b));
			goto abort_free;
		}
		if (err < 0) {
			printk(KERN_WARNING 
				"md: could not read %s's sb, not importing!\n",
				bdevname(rdev->bdev,b));
			goto abort_free;
		}
	}
	INIT_LIST_HEAD(&rdev->same_set);

	return rdev;

abort_free:
	if (rdev->sb_page) {
		if (rdev->bdev)
			unlock_rdev(rdev);
		free_disk_sb(rdev);
	}
	kfree(rdev);
	return ERR_PTR(err);
}

/*
 * Check a full RAID array for plausibility
 */


static void analyze_sbs(mddev_t * mddev)
{
	int i;
	struct list_head *tmp;
	mdk_rdev_t *rdev, *freshest;
	char b[BDEVNAME_SIZE];

	freshest = NULL;
	ITERATE_RDEV(mddev,rdev,tmp)
		switch (super_types[mddev->major_version].
			load_super(rdev, freshest, mddev->minor_version)) {
		case 1:
			freshest = rdev;
			break;
		case 0:
			break;
		default:
			printk( KERN_ERR \
				"md: fatal superblock inconsistency in %s"
				" -- removing from array\n", 
				bdevname(rdev->bdev,b));
			kick_rdev_from_array(rdev);
		}


	super_types[mddev->major_version].
		validate_super(mddev, freshest);

	i = 0;
	ITERATE_RDEV(mddev,rdev,tmp) {
		if (rdev != freshest)
			if (super_types[mddev->major_version].
			    validate_super(mddev, rdev)) {
				printk(KERN_WARNING "md: kicking non-fresh %s"
					" from array!\n",
					bdevname(rdev->bdev,b));
				kick_rdev_from_array(rdev);
				continue;
			}
		if (mddev->level == LEVEL_MULTIPATH) {
			rdev->desc_nr = i++;
			rdev->raid_disk = rdev->desc_nr;
			set_bit(In_sync, &rdev->flags);
		}
	}



	if (mddev->recovery_cp != MaxSector &&
	    mddev->level >= 1)
		printk(KERN_ERR "md: %s: raid array is not clean"
		       " -- starting background reconstruction\n",
		       mdname(mddev));

}

static ssize_t
level_show(mddev_t *mddev, char *page)
{
	mdk_personality_t *p = mddev->pers;
	if (p == NULL && mddev->raid_disks == 0)
		return 0;
	if (mddev->level >= 0)
		return sprintf(page, "raid%d\n", mddev->level);
	else
		return sprintf(page, "%s\n", p->name);
}

static struct md_sysfs_entry md_level = __ATTR_RO(level);

static ssize_t
raid_disks_show(mddev_t *mddev, char *page)
{
	if (mddev->raid_disks == 0)
		return 0;
	return sprintf(page, "%d\n", mddev->raid_disks);
}

static struct md_sysfs_entry md_raid_disks = __ATTR_RO(raid_disks);

static ssize_t
action_show(mddev_t *mddev, char *page)
{
	char *type = "idle";
	if (test_bit(MD_RECOVERY_RUNNING, &mddev->recovery) ||
	    test_bit(MD_RECOVERY_NEEDED, &mddev->recovery)) {
		if (test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
			if (!test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery))
				type = "resync";
			else if (test_bit(MD_RECOVERY_CHECK, &mddev->recovery))
				type = "check";
			else
				type = "repair";
		} else
			type = "recover";
	}
	return sprintf(page, "%s\n", type);
}

static ssize_t
action_store(mddev_t *mddev, const char *page, size_t len)
{
	if (!mddev->pers || !mddev->pers->sync_request)
		return -EINVAL;

	if (strcmp(page, "idle")==0 || strcmp(page, "idle\n")==0) {
		if (mddev->sync_thread) {
			set_bit(MD_RECOVERY_INTR, &mddev->recovery);
			md_unregister_thread(mddev->sync_thread);
			mddev->sync_thread = NULL;
			mddev->recovery = 0;
		}
		return len;
	}

	if (test_bit(MD_RECOVERY_RUNNING, &mddev->recovery) ||
	    test_bit(MD_RECOVERY_NEEDED, &mddev->recovery))
		return -EBUSY;
	if (strcmp(page, "resync")==0 || strcmp(page, "resync\n")==0 ||
	    strcmp(page, "recover")==0 || strcmp(page, "recover\n")==0)
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	else {
		if (strcmp(page, "check")==0 || strcmp(page, "check\n")==0)
			set_bit(MD_RECOVERY_CHECK, &mddev->recovery);
		else if (strcmp(page, "repair")!=0 && strcmp(page, "repair\n")!=0)
			return -EINVAL;
		set_bit(MD_RECOVERY_REQUESTED, &mddev->recovery);
		set_bit(MD_RECOVERY_SYNC, &mddev->recovery);
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	}
	md_wakeup_thread(mddev->thread);
	return len;
}

static ssize_t
mismatch_cnt_show(mddev_t *mddev, char *page)
{
	return sprintf(page, "%llu\n",
		       (unsigned long long) mddev->resync_mismatches);
}

static struct md_sysfs_entry
md_scan_mode = __ATTR(sync_action, S_IRUGO|S_IWUSR, action_show, action_store);


static struct md_sysfs_entry
md_mismatches = __ATTR_RO(mismatch_cnt);

static struct attribute *md_default_attrs[] = {
	&md_level.attr,
	&md_raid_disks.attr,
	NULL,
};

static struct attribute *md_redundancy_attrs[] = {
	&md_scan_mode.attr,
	&md_mismatches.attr,
	NULL,
};
static struct attribute_group md_redundancy_group = {
	.name = NULL,
	.attrs = md_redundancy_attrs,
};


static ssize_t
md_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct md_sysfs_entry *entry = container_of(attr, struct md_sysfs_entry, attr);
	mddev_t *mddev = container_of(kobj, struct mddev_s, kobj);
	ssize_t rv;

	if (!entry->show)
		return -EIO;
	mddev_lock(mddev);
	rv = entry->show(mddev, page);
	mddev_unlock(mddev);
	return rv;
}

static ssize_t
md_attr_store(struct kobject *kobj, struct attribute *attr,
	      const char *page, size_t length)
{
	struct md_sysfs_entry *entry = container_of(attr, struct md_sysfs_entry, attr);
	mddev_t *mddev = container_of(kobj, struct mddev_s, kobj);
	ssize_t rv;

	if (!entry->store)
		return -EIO;
	mddev_lock(mddev);
	rv = entry->store(mddev, page, length);
	mddev_unlock(mddev);
	return rv;
}

static void md_free(struct kobject *ko)
{
	mddev_t *mddev = container_of(ko, mddev_t, kobj);
	kfree(mddev);
}

static struct sysfs_ops md_sysfs_ops = {
	.show	= md_attr_show,
	.store	= md_attr_store,
};
static struct kobj_type md_ktype = {
	.release	= md_free,
	.sysfs_ops	= &md_sysfs_ops,
	.default_attrs	= md_default_attrs,
};

int mdp_major = 0;

static struct kobject *md_probe(dev_t dev, int *part, void *data)
{
	static DECLARE_MUTEX(disks_sem);
	mddev_t *mddev = mddev_find(dev);
	struct gendisk *disk;
	int partitioned = (MAJOR(dev) != MD_MAJOR);
	int shift = partitioned ? MdpMinorShift : 0;
	int unit = MINOR(dev) >> shift;

	if (!mddev)
		return NULL;

	down(&disks_sem);
	if (mddev->gendisk) {
		up(&disks_sem);
		mddev_put(mddev);
		return NULL;
	}
	disk = alloc_disk(1 << shift);
	if (!disk) {
		up(&disks_sem);
		mddev_put(mddev);
		return NULL;
	}
	disk->major = MAJOR(dev);
	disk->first_minor = unit << shift;
	if (partitioned) {
		sprintf(disk->disk_name, "md_d%d", unit);
		sprintf(disk->devfs_name, "md/d%d", unit);
	} else {
		sprintf(disk->disk_name, "md%d", unit);
		sprintf(disk->devfs_name, "md/%d", unit);
	}
	disk->fops = &md_fops;
	disk->private_data = mddev;
	disk->queue = mddev->queue;
	add_disk(disk);
	mddev->gendisk = disk;
	up(&disks_sem);
	mddev->kobj.parent = &disk->kobj;
	mddev->kobj.k_name = NULL;
	snprintf(mddev->kobj.name, KOBJ_NAME_LEN, "%s", "md");
	mddev->kobj.ktype = &md_ktype;
	kobject_register(&mddev->kobj);
	return NULL;
}

void md_wakeup_thread(mdk_thread_t *thread);

static void md_safemode_timeout(unsigned long data)
{
	mddev_t *mddev = (mddev_t *) data;

	mddev->safemode = 1;
	md_wakeup_thread(mddev->thread);
}


static int do_md_run(mddev_t * mddev)
{
	int pnum, err;
	int chunk_size;
	struct list_head *tmp;
	mdk_rdev_t *rdev;
	struct gendisk *disk;
	char b[BDEVNAME_SIZE];

	if (list_empty(&mddev->disks))
		/* cannot run an array with no devices.. */
		return -EINVAL;

	if (mddev->pers)
		return -EBUSY;

	/*
	 * Analyze all RAID superblock(s)
	 */
	if (!mddev->raid_disks)
		analyze_sbs(mddev);

	chunk_size = mddev->chunk_size;
	pnum = level_to_pers(mddev->level);

	if ((pnum != MULTIPATH) && (pnum != RAID1)) {
		if (!chunk_size) {
			/*
			 * 'default chunksize' in the old md code used to
			 * be PAGE_SIZE, baaad.
			 * we abort here to be on the safe side. We don't
			 * want to continue the bad practice.
			 */
			printk(KERN_ERR 
				"no chunksize specified, see 'man raidtab'\n");
			return -EINVAL;
		}
		if (chunk_size > MAX_CHUNK_SIZE) {
			printk(KERN_ERR "too big chunk_size: %d > %d\n",
				chunk_size, MAX_CHUNK_SIZE);
			return -EINVAL;
		}
		/*
		 * chunk-size has to be a power of 2 and multiples of PAGE_SIZE
		 */
		if ( (1 << ffz(~chunk_size)) != chunk_size) {
			printk(KERN_ERR "chunk_size of %d not valid\n", chunk_size);
			return -EINVAL;
		}
		if (chunk_size < PAGE_SIZE) {
			printk(KERN_ERR "too small chunk_size: %d < %ld\n",
				chunk_size, PAGE_SIZE);
			return -EINVAL;
		}

		/* devices must have minimum size of one chunk */
		ITERATE_RDEV(mddev,rdev,tmp) {
			if (test_bit(Faulty, &rdev->flags))
				continue;
			if (rdev->size < chunk_size / 1024) {
				printk(KERN_WARNING
					"md: Dev %s smaller than chunk_size:"
					" %lluk < %dk\n",
					bdevname(rdev->bdev,b),
					(unsigned long long)rdev->size,
					chunk_size / 1024);
				return -EINVAL;
			}
		}
	}

#ifdef CONFIG_KMOD
	if (!pers[pnum])
	{
		request_module("md-personality-%d", pnum);
	}
#endif

	/*
	 * Drop all container device buffers, from now on
	 * the only valid external interface is through the md
	 * device.
	 * Also find largest hardsector size
	 */
	ITERATE_RDEV(mddev,rdev,tmp) {
		if (test_bit(Faulty, &rdev->flags))
			continue;
		sync_blockdev(rdev->bdev);
		invalidate_bdev(rdev->bdev, 0);
	}

	md_probe(mddev->unit, NULL, NULL);
	disk = mddev->gendisk;
	if (!disk)
		return -ENOMEM;

	spin_lock(&pers_lock);
	if (!pers[pnum] || !try_module_get(pers[pnum]->owner)) {
		spin_unlock(&pers_lock);
		printk(KERN_WARNING "md: personality %d is not loaded!\n",
		       pnum);
		return -EINVAL;
	}

	mddev->pers = pers[pnum];
	spin_unlock(&pers_lock);

	mddev->recovery = 0;
	mddev->resync_max_sectors = mddev->size << 1; /* may be over-ridden by personality */
	mddev->barriers_work = 1;

	if (start_readonly)
		mddev->ro = 2; /* read-only, but switch on first write */

	/* before we start the array running, initialise the bitmap */
	err = bitmap_create(mddev);
	if (err)
		printk(KERN_ERR "%s: failed to create bitmap (%d)\n",
			mdname(mddev), err);
	else
		err = mddev->pers->run(mddev);
	if (err) {
		printk(KERN_ERR "md: pers->run() failed ...\n");
		module_put(mddev->pers->owner);
		mddev->pers = NULL;
		bitmap_destroy(mddev);
		return err;
	}
	if (mddev->pers->sync_request)
		sysfs_create_group(&mddev->kobj, &md_redundancy_group);
	else if (mddev->ro == 2) /* auto-readonly not meaningful */
		mddev->ro = 0;

 	atomic_set(&mddev->writes_pending,0);
	mddev->safemode = 0;
	mddev->safemode_timer.function = md_safemode_timeout;
	mddev->safemode_timer.data = (unsigned long) mddev;
	mddev->safemode_delay = (20 * HZ)/1000 +1; /* 20 msec delay */
	mddev->in_sync = 1;

	ITERATE_RDEV(mddev,rdev,tmp)
		if (rdev->raid_disk >= 0) {
			char nm[20];
			sprintf(nm, "rd%d", rdev->raid_disk);
			sysfs_create_link(&mddev->kobj, &rdev->kobj, nm);
		}
	
	set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	md_wakeup_thread(mddev->thread);
	
	if (mddev->sb_dirty)
		md_update_sb(mddev);

	set_capacity(disk, mddev->array_size<<1);

	/* If we call blk_queue_make_request here, it will
	 * re-initialise max_sectors etc which may have been
	 * refined inside -> run.  So just set the bits we need to set.
	 * Most initialisation happended when we called
	 * blk_queue_make_request(..., md_fail_request)
	 * earlier.
	 */
	mddev->queue->queuedata = mddev;
	mddev->queue->make_request_fn = mddev->pers->make_request;

	mddev->changed = 1;
	return 0;
}

static int restart_array(mddev_t *mddev)
{
	struct gendisk *disk = mddev->gendisk;
	int err;

	/*
	 * Complain if it has no devices
	 */
	err = -ENXIO;
	if (list_empty(&mddev->disks))
		goto out;

	if (mddev->pers) {
		err = -EBUSY;
		if (!mddev->ro)
			goto out;

		mddev->safemode = 0;
		mddev->ro = 0;
		set_disk_ro(disk, 0);

		printk(KERN_INFO "md: %s switched to read-write mode.\n",
			mdname(mddev));
		/*
		 * Kick recovery or resync if necessary
		 */
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		md_wakeup_thread(mddev->thread);
		err = 0;
	} else {
		printk(KERN_ERR "md: %s has no personality assigned.\n",
			mdname(mddev));
		err = -EINVAL;
	}

out:
	return err;
}

static int do_md_stop(mddev_t * mddev, int ro)
{
	int err = 0;
	struct gendisk *disk = mddev->gendisk;

	if (mddev->pers) {
		if (atomic_read(&mddev->active)>2) {
			printk("md: %s still in use.\n",mdname(mddev));
			return -EBUSY;
		}

		if (mddev->sync_thread) {
			set_bit(MD_RECOVERY_INTR, &mddev->recovery);
			md_unregister_thread(mddev->sync_thread);
			mddev->sync_thread = NULL;
		}

		del_timer_sync(&mddev->safemode_timer);

		invalidate_partition(disk, 0);

		if (ro) {
			err  = -ENXIO;
			if (mddev->ro==1)
				goto out;
			mddev->ro = 1;
		} else {
			bitmap_flush(mddev);
			md_super_wait(mddev);
			if (mddev->ro)
				set_disk_ro(disk, 0);
			blk_queue_make_request(mddev->queue, md_fail_request);
			mddev->pers->stop(mddev);
			if (mddev->pers->sync_request)
				sysfs_remove_group(&mddev->kobj, &md_redundancy_group);

			module_put(mddev->pers->owner);
			mddev->pers = NULL;
			if (mddev->ro)
				mddev->ro = 0;
		}
		if (!mddev->in_sync) {
			/* mark array as shutdown cleanly */
			mddev->in_sync = 1;
			md_update_sb(mddev);
		}
		if (ro)
			set_disk_ro(disk, 1);
	}

	bitmap_destroy(mddev);
	if (mddev->bitmap_file) {
		atomic_set(&mddev->bitmap_file->f_dentry->d_inode->i_writecount, 1);
		fput(mddev->bitmap_file);
		mddev->bitmap_file = NULL;
	}
	mddev->bitmap_offset = 0;

	/*
	 * Free resources if final stop
	 */
	if (!ro) {
		mdk_rdev_t *rdev;
		struct list_head *tmp;
		struct gendisk *disk;
		printk(KERN_INFO "md: %s stopped.\n", mdname(mddev));

		ITERATE_RDEV(mddev,rdev,tmp)
			if (rdev->raid_disk >= 0) {
				char nm[20];
				sprintf(nm, "rd%d", rdev->raid_disk);
				sysfs_remove_link(&mddev->kobj, nm);
			}

		export_array(mddev);

		mddev->array_size = 0;
		disk = mddev->gendisk;
		if (disk)
			set_capacity(disk, 0);
		mddev->changed = 1;
	} else
		printk(KERN_INFO "md: %s switched to read-only mode.\n",
			mdname(mddev));
	err = 0;
out:
	return err;
}

static void autorun_array(mddev_t *mddev)
{
	mdk_rdev_t *rdev;
	struct list_head *tmp;
	int err;

	if (list_empty(&mddev->disks))
		return;

	printk(KERN_INFO "md: running: ");

	ITERATE_RDEV(mddev,rdev,tmp) {
		char b[BDEVNAME_SIZE];
		printk("<%s>", bdevname(rdev->bdev,b));
	}
	printk("\n");

	err = do_md_run (mddev);
	if (err) {
		printk(KERN_WARNING "md: do_md_run() returned %d\n", err);
		do_md_stop (mddev, 0);
	}
}

/*
 * lets try to run arrays based on all disks that have arrived
 * until now. (those are in pending_raid_disks)
 *
 * the method: pick the first pending disk, collect all disks with
 * the same UUID, remove all from the pending list and put them into
 * the 'same_array' list. Then order this list based on superblock
 * update time (freshest comes first), kick out 'old' disks and
 * compare superblocks. If everything's fine then run it.
 *
 * If "unit" is allocated, then bump its reference count
 */
static void autorun_devices(int part)
{
	struct list_head candidates;
	struct list_head *tmp;
	mdk_rdev_t *rdev0, *rdev;
	mddev_t *mddev;
	char b[BDEVNAME_SIZE];

	printk(KERN_INFO "md: autorun ...\n");
	while (!list_empty(&pending_raid_disks)) {
		dev_t dev;
		rdev0 = list_entry(pending_raid_disks.next,
					 mdk_rdev_t, same_set);

		printk(KERN_INFO "md: considering %s ...\n",
			bdevname(rdev0->bdev,b));
		INIT_LIST_HEAD(&candidates);
		ITERATE_RDEV_PENDING(rdev,tmp)
			if (super_90_load(rdev, rdev0, 0) >= 0) {
				printk(KERN_INFO "md:  adding %s ...\n",
					bdevname(rdev->bdev,b));
				list_move(&rdev->same_set, &candidates);
			}
		/*
		 * now we have a set of devices, with all of them having
		 * mostly sane superblocks. It's time to allocate the
		 * mddev.
		 */
		if (rdev0->preferred_minor < 0 || rdev0->preferred_minor >= MAX_MD_DEVS) {
			printk(KERN_INFO "md: unit number in %s is bad: %d\n",
			       bdevname(rdev0->bdev, b), rdev0->preferred_minor);
			break;
		}
		if (part)
			dev = MKDEV(mdp_major,
				    rdev0->preferred_minor << MdpMinorShift);
		else
			dev = MKDEV(MD_MAJOR, rdev0->preferred_minor);

		md_probe(dev, NULL, NULL);
		mddev = mddev_find(dev);
		if (!mddev) {
			printk(KERN_ERR 
				"md: cannot allocate memory for md drive.\n");
			break;
		}
		if (mddev_lock(mddev)) 
			printk(KERN_WARNING "md: %s locked, cannot run\n",
			       mdname(mddev));
		else if (mddev->raid_disks || mddev->major_version
			 || !list_empty(&mddev->disks)) {
			printk(KERN_WARNING 
				"md: %s already running, cannot run %s\n",
				mdname(mddev), bdevname(rdev0->bdev,b));
			mddev_unlock(mddev);
		} else {
			printk(KERN_INFO "md: created %s\n", mdname(mddev));
			ITERATE_RDEV_GENERIC(candidates,rdev,tmp) {
				list_del_init(&rdev->same_set);
				if (bind_rdev_to_array(rdev, mddev))
					export_rdev(rdev);
			}
			autorun_array(mddev);
			mddev_unlock(mddev);
		}
		/* on success, candidates will be empty, on error
		 * it won't...
		 */
		ITERATE_RDEV_GENERIC(candidates,rdev,tmp)
			export_rdev(rdev);
		mddev_put(mddev);
	}
	printk(KERN_INFO "md: ... autorun DONE.\n");
}

/*
 * import RAID devices based on one partition
 * if possible, the array gets run as well.
 */

static int autostart_array(dev_t startdev)
{
	char b[BDEVNAME_SIZE];
	int err = -EINVAL, i;
	mdp_super_t *sb = NULL;
	mdk_rdev_t *start_rdev = NULL, *rdev;

	start_rdev = md_import_device(startdev, 0, 0);
	if (IS_ERR(start_rdev))
		return err;


	/* NOTE: this can only work for 0.90.0 superblocks */
	sb = (mdp_super_t*)page_address(start_rdev->sb_page);
	if (sb->major_version != 0 ||
	    sb->minor_version != 90 ) {
		printk(KERN_WARNING "md: can only autostart 0.90.0 arrays\n");
		export_rdev(start_rdev);
		return err;
	}

	if (test_bit(Faulty, &start_rdev->flags)) {
		printk(KERN_WARNING 
			"md: can not autostart based on faulty %s!\n",
			bdevname(start_rdev->bdev,b));
		export_rdev(start_rdev);
		return err;
	}
	list_add(&start_rdev->same_set, &pending_raid_disks);

	for (i = 0; i < MD_SB_DISKS; i++) {
		mdp_disk_t *desc = sb->disks + i;
		dev_t dev = MKDEV(desc->major, desc->minor);

		if (!dev)
			continue;
		if (dev == startdev)
			continue;
		if (MAJOR(dev) != desc->major || MINOR(dev) != desc->minor)
			continue;
		rdev = md_import_device(dev, 0, 0);
		if (IS_ERR(rdev))
			continue;

		list_add(&rdev->same_set, &pending_raid_disks);
	}

	/*
	 * possibly return codes
	 */
	autorun_devices(0);
	return 0;

}


static int get_version(void __user * arg)
{
	mdu_version_t ver;

	ver.major = MD_MAJOR_VERSION;
	ver.minor = MD_MINOR_VERSION;
	ver.patchlevel = MD_PATCHLEVEL_VERSION;

	if (copy_to_user(arg, &ver, sizeof(ver)))
		return -EFAULT;

	return 0;
}

static int get_array_info(mddev_t * mddev, void __user * arg)
{
	mdu_array_info_t info;
	int nr,working,active,failed,spare;
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	nr=working=active=failed=spare=0;
	ITERATE_RDEV(mddev,rdev,tmp) {
		nr++;
		if (test_bit(Faulty, &rdev->flags))
			failed++;
		else {
			working++;
			if (test_bit(In_sync, &rdev->flags))
				active++;	
			else
				spare++;
		}
	}

	info.major_version = mddev->major_version;
	info.minor_version = mddev->minor_version;
	info.patch_version = MD_PATCHLEVEL_VERSION;
	info.ctime         = mddev->ctime;
	info.level         = mddev->level;
	info.size          = mddev->size;
	info.nr_disks      = nr;
	info.raid_disks    = mddev->raid_disks;
	info.md_minor      = mddev->md_minor;
	info.not_persistent= !mddev->persistent;

	info.utime         = mddev->utime;
	info.state         = 0;
	if (mddev->in_sync)
		info.state = (1<<MD_SB_CLEAN);
	if (mddev->bitmap && mddev->bitmap_offset)
		info.state = (1<<MD_SB_BITMAP_PRESENT);
	info.active_disks  = active;
	info.working_disks = working;
	info.failed_disks  = failed;
	info.spare_disks   = spare;

	info.layout        = mddev->layout;
	info.chunk_size    = mddev->chunk_size;

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int get_bitmap_file(mddev_t * mddev, void __user * arg)
{
	mdu_bitmap_file_t *file = NULL; /* too big for stack allocation */
	char *ptr, *buf = NULL;
	int err = -ENOMEM;

	file = kmalloc(sizeof(*file), GFP_KERNEL);
	if (!file)
		goto out;

	/* bitmap disabled, zero the first byte and copy out */
	if (!mddev->bitmap || !mddev->bitmap->file) {
		file->pathname[0] = '\0';
		goto copy_out;
	}

	buf = kmalloc(sizeof(file->pathname), GFP_KERNEL);
	if (!buf)
		goto out;

	ptr = file_path(mddev->bitmap->file, buf, sizeof(file->pathname));
	if (!ptr)
		goto out;

	strcpy(file->pathname, ptr);

copy_out:
	err = 0;
	if (copy_to_user(arg, file, sizeof(*file)))
		err = -EFAULT;
out:
	kfree(buf);
	kfree(file);
	return err;
}

static int get_disk_info(mddev_t * mddev, void __user * arg)
{
	mdu_disk_info_t info;
	unsigned int nr;
	mdk_rdev_t *rdev;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	nr = info.number;

	rdev = find_rdev_nr(mddev, nr);
	if (rdev) {
		info.major = MAJOR(rdev->bdev->bd_dev);
		info.minor = MINOR(rdev->bdev->bd_dev);
		info.raid_disk = rdev->raid_disk;
		info.state = 0;
		if (test_bit(Faulty, &rdev->flags))
			info.state |= (1<<MD_DISK_FAULTY);
		else if (test_bit(In_sync, &rdev->flags)) {
			info.state |= (1<<MD_DISK_ACTIVE);
			info.state |= (1<<MD_DISK_SYNC);
		}
		if (test_bit(WriteMostly, &rdev->flags))
			info.state |= (1<<MD_DISK_WRITEMOSTLY);
	} else {
		info.major = info.minor = 0;
		info.raid_disk = -1;
		info.state = (1<<MD_DISK_REMOVED);
	}

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static int add_new_disk(mddev_t * mddev, mdu_disk_info_t *info)
{
	char b[BDEVNAME_SIZE], b2[BDEVNAME_SIZE];
	mdk_rdev_t *rdev;
	dev_t dev = MKDEV(info->major,info->minor);

	if (info->major != MAJOR(dev) || info->minor != MINOR(dev))
		return -EOVERFLOW;

	if (!mddev->raid_disks) {
		int err;
		/* expecting a device which has a superblock */
		rdev = md_import_device(dev, mddev->major_version, mddev->minor_version);
		if (IS_ERR(rdev)) {
			printk(KERN_WARNING 
				"md: md_import_device returned %ld\n",
				PTR_ERR(rdev));
			return PTR_ERR(rdev);
		}
		if (!list_empty(&mddev->disks)) {
			mdk_rdev_t *rdev0 = list_entry(mddev->disks.next,
							mdk_rdev_t, same_set);
			int err = super_types[mddev->major_version]
				.load_super(rdev, rdev0, mddev->minor_version);
			if (err < 0) {
				printk(KERN_WARNING 
					"md: %s has different UUID to %s\n",
					bdevname(rdev->bdev,b), 
					bdevname(rdev0->bdev,b2));
				export_rdev(rdev);
				return -EINVAL;
			}
		}
		err = bind_rdev_to_array(rdev, mddev);
		if (err)
			export_rdev(rdev);
		return err;
	}

	/*
	 * add_new_disk can be used once the array is assembled
	 * to add "hot spares".  They must already have a superblock
	 * written
	 */
	if (mddev->pers) {
		int err;
		if (!mddev->pers->hot_add_disk) {
			printk(KERN_WARNING 
				"%s: personality does not support diskops!\n",
			       mdname(mddev));
			return -EINVAL;
		}
		if (mddev->persistent)
			rdev = md_import_device(dev, mddev->major_version,
						mddev->minor_version);
		else
			rdev = md_import_device(dev, -1, -1);
		if (IS_ERR(rdev)) {
			printk(KERN_WARNING 
				"md: md_import_device returned %ld\n",
				PTR_ERR(rdev));
			return PTR_ERR(rdev);
		}
		/* set save_raid_disk if appropriate */
		if (!mddev->persistent) {
			if (info->state & (1<<MD_DISK_SYNC)  &&
			    info->raid_disk < mddev->raid_disks)
				rdev->raid_disk = info->raid_disk;
			else
				rdev->raid_disk = -1;
		} else
			super_types[mddev->major_version].
				validate_super(mddev, rdev);
		rdev->saved_raid_disk = rdev->raid_disk;

		clear_bit(In_sync, &rdev->flags); /* just to be sure */
		if (info->state & (1<<MD_DISK_WRITEMOSTLY))
			set_bit(WriteMostly, &rdev->flags);

		rdev->raid_disk = -1;
		err = bind_rdev_to_array(rdev, mddev);
		if (err)
			export_rdev(rdev);

		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		md_wakeup_thread(mddev->thread);
		return err;
	}

	/* otherwise, add_new_disk is only allowed
	 * for major_version==0 superblocks
	 */
	if (mddev->major_version != 0) {
		printk(KERN_WARNING "%s: ADD_NEW_DISK not supported\n",
		       mdname(mddev));
		return -EINVAL;
	}

	if (!(info->state & (1<<MD_DISK_FAULTY))) {
		int err;
		rdev = md_import_device (dev, -1, 0);
		if (IS_ERR(rdev)) {
			printk(KERN_WARNING 
				"md: error, md_import_device() returned %ld\n",
				PTR_ERR(rdev));
			return PTR_ERR(rdev);
		}
		rdev->desc_nr = info->number;
		if (info->raid_disk < mddev->raid_disks)
			rdev->raid_disk = info->raid_disk;
		else
			rdev->raid_disk = -1;

		rdev->flags = 0;

		if (rdev->raid_disk < mddev->raid_disks)
			if (info->state & (1<<MD_DISK_SYNC))
				set_bit(In_sync, &rdev->flags);

		if (info->state & (1<<MD_DISK_WRITEMOSTLY))
			set_bit(WriteMostly, &rdev->flags);

		err = bind_rdev_to_array(rdev, mddev);
		if (err) {
			export_rdev(rdev);
			return err;
		}

		if (!mddev->persistent) {
			printk(KERN_INFO "md: nonpersistent superblock ...\n");
			rdev->sb_offset = rdev->bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;
		} else 
			rdev->sb_offset = calc_dev_sboffset(rdev->bdev);
		rdev->size = calc_dev_size(rdev, mddev->chunk_size);

		if (!mddev->size || (mddev->size > rdev->size))
			mddev->size = rdev->size;
	}

	return 0;
}

static int hot_remove_disk(mddev_t * mddev, dev_t dev)
{
	char b[BDEVNAME_SIZE];
	mdk_rdev_t *rdev;

	if (!mddev->pers)
		return -ENODEV;

	rdev = find_rdev(mddev, dev);
	if (!rdev)
		return -ENXIO;

	if (rdev->raid_disk >= 0)
		goto busy;

	kick_rdev_from_array(rdev);
	md_update_sb(mddev);

	return 0;
busy:
	printk(KERN_WARNING "md: cannot remove active disk %s from %s ... \n",
		bdevname(rdev->bdev,b), mdname(mddev));
	return -EBUSY;
}

static int hot_add_disk(mddev_t * mddev, dev_t dev)
{
	char b[BDEVNAME_SIZE];
	int err;
	unsigned int size;
	mdk_rdev_t *rdev;

	if (!mddev->pers)
		return -ENODEV;

	if (mddev->major_version != 0) {
		printk(KERN_WARNING "%s: HOT_ADD may only be used with"
			" version-0 superblocks.\n",
			mdname(mddev));
		return -EINVAL;
	}
	if (!mddev->pers->hot_add_disk) {
		printk(KERN_WARNING 
			"%s: personality does not support diskops!\n",
			mdname(mddev));
		return -EINVAL;
	}

	rdev = md_import_device (dev, -1, 0);
	if (IS_ERR(rdev)) {
		printk(KERN_WARNING 
			"md: error, md_import_device() returned %ld\n",
			PTR_ERR(rdev));
		return -EINVAL;
	}

	if (mddev->persistent)
		rdev->sb_offset = calc_dev_sboffset(rdev->bdev);
	else
		rdev->sb_offset =
			rdev->bdev->bd_inode->i_size >> BLOCK_SIZE_BITS;

	size = calc_dev_size(rdev, mddev->chunk_size);
	rdev->size = size;

	if (size < mddev->size) {
		printk(KERN_WARNING 
			"%s: disk size %llu blocks < array size %llu\n",
			mdname(mddev), (unsigned long long)size,
			(unsigned long long)mddev->size);
		err = -ENOSPC;
		goto abort_export;
	}

	if (test_bit(Faulty, &rdev->flags)) {
		printk(KERN_WARNING 
			"md: can not hot-add faulty %s disk to %s!\n",
			bdevname(rdev->bdev,b), mdname(mddev));
		err = -EINVAL;
		goto abort_export;
	}
	clear_bit(In_sync, &rdev->flags);
	rdev->desc_nr = -1;
	bind_rdev_to_array(rdev, mddev);

	/*
	 * The rest should better be atomic, we can have disk failures
	 * noticed in interrupt contexts ...
	 */

	if (rdev->desc_nr == mddev->max_disks) {
		printk(KERN_WARNING "%s: can not hot-add to full array!\n",
			mdname(mddev));
		err = -EBUSY;
		goto abort_unbind_export;
	}

	rdev->raid_disk = -1;

	md_update_sb(mddev);

	/*
	 * Kick recovery, maybe this spare has to be added to the
	 * array immediately.
	 */
	set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	md_wakeup_thread(mddev->thread);

	return 0;

abort_unbind_export:
	unbind_rdev_from_array(rdev);

abort_export:
	export_rdev(rdev);
	return err;
}

/* similar to deny_write_access, but accounts for our holding a reference
 * to the file ourselves */
static int deny_bitmap_write_access(struct file * file)
{
	struct inode *inode = file->f_mapping->host;

	spin_lock(&inode->i_lock);
	if (atomic_read(&inode->i_writecount) > 1) {
		spin_unlock(&inode->i_lock);
		return -ETXTBSY;
	}
	atomic_set(&inode->i_writecount, -1);
	spin_unlock(&inode->i_lock);

	return 0;
}

static int set_bitmap_file(mddev_t *mddev, int fd)
{
	int err;

	if (mddev->pers) {
		if (!mddev->pers->quiesce)
			return -EBUSY;
		if (mddev->recovery || mddev->sync_thread)
			return -EBUSY;
		/* we should be able to change the bitmap.. */
	}


	if (fd >= 0) {
		if (mddev->bitmap)
			return -EEXIST; /* cannot add when bitmap is present */
		mddev->bitmap_file = fget(fd);

		if (mddev->bitmap_file == NULL) {
			printk(KERN_ERR "%s: error: failed to get bitmap file\n",
			       mdname(mddev));
			return -EBADF;
		}

		err = deny_bitmap_write_access(mddev->bitmap_file);
		if (err) {
			printk(KERN_ERR "%s: error: bitmap file is already in use\n",
			       mdname(mddev));
			fput(mddev->bitmap_file);
			mddev->bitmap_file = NULL;
			return err;
		}
		mddev->bitmap_offset = 0; /* file overrides offset */
	} else if (mddev->bitmap == NULL)
		return -ENOENT; /* cannot remove what isn't there */
	err = 0;
	if (mddev->pers) {
		mddev->pers->quiesce(mddev, 1);
		if (fd >= 0)
			err = bitmap_create(mddev);
		if (fd < 0 || err)
			bitmap_destroy(mddev);
		mddev->pers->quiesce(mddev, 0);
	} else if (fd < 0) {
		if (mddev->bitmap_file)
			fput(mddev->bitmap_file);
		mddev->bitmap_file = NULL;
	}

	return err;
}

/*
 * set_array_info is used two different ways
 * The original usage is when creating a new array.
 * In this usage, raid_disks is > 0 and it together with
 *  level, size, not_persistent,layout,chunksize determine the
 *  shape of the array.
 *  This will always create an array with a type-0.90.0 superblock.
 * The newer usage is when assembling an array.
 *  In this case raid_disks will be 0, and the major_version field is
 *  use to determine which style super-blocks are to be found on the devices.
 *  The minor and patch _version numbers are also kept incase the
 *  super_block handler wishes to interpret them.
 */
static int set_array_info(mddev_t * mddev, mdu_array_info_t *info)
{

	if (info->raid_disks == 0) {
		/* just setting version number for superblock loading */
		if (info->major_version < 0 ||
		    info->major_version >= sizeof(super_types)/sizeof(super_types[0]) ||
		    super_types[info->major_version].name == NULL) {
			/* maybe try to auto-load a module? */
			printk(KERN_INFO 
				"md: superblock version %d not known\n",
				info->major_version);
			return -EINVAL;
		}
		mddev->major_version = info->major_version;
		mddev->minor_version = info->minor_version;
		mddev->patch_version = info->patch_version;
		return 0;
	}
	mddev->major_version = MD_MAJOR_VERSION;
	mddev->minor_version = MD_MINOR_VERSION;
	mddev->patch_version = MD_PATCHLEVEL_VERSION;
	mddev->ctime         = get_seconds();

	mddev->level         = info->level;
	mddev->size          = info->size;
	mddev->raid_disks    = info->raid_disks;
	/* don't set md_minor, it is determined by which /dev/md* was
	 * openned
	 */
	if (info->state & (1<<MD_SB_CLEAN))
		mddev->recovery_cp = MaxSector;
	else
		mddev->recovery_cp = 0;
	mddev->persistent    = ! info->not_persistent;

	mddev->layout        = info->layout;
	mddev->chunk_size    = info->chunk_size;

	mddev->max_disks     = MD_SB_DISKS;

	mddev->sb_dirty      = 1;

	mddev->default_bitmap_offset = MD_SB_BYTES >> 9;
	mddev->bitmap_offset = 0;

	/*
	 * Generate a 128 bit UUID
	 */
	get_random_bytes(mddev->uuid, 16);

	return 0;
}

/*
 * update_array_info is used to change the configuration of an
 * on-line array.
 * The version, ctime,level,size,raid_disks,not_persistent, layout,chunk_size
 * fields in the info are checked against the array.
 * Any differences that cannot be handled will cause an error.
 * Normally, only one change can be managed at a time.
 */
static int update_array_info(mddev_t *mddev, mdu_array_info_t *info)
{
	int rv = 0;
	int cnt = 0;
	int state = 0;

	/* calculate expected state,ignoring low bits */
	if (mddev->bitmap && mddev->bitmap_offset)
		state |= (1 << MD_SB_BITMAP_PRESENT);

	if (mddev->major_version != info->major_version ||
	    mddev->minor_version != info->minor_version ||
/*	    mddev->patch_version != info->patch_version || */
	    mddev->ctime         != info->ctime         ||
	    mddev->level         != info->level         ||
/*	    mddev->layout        != info->layout        || */
	    !mddev->persistent	 != info->not_persistent||
	    mddev->chunk_size    != info->chunk_size    ||
	    /* ignore bottom 8 bits of state, and allow SB_BITMAP_PRESENT to change */
	    ((state^info->state) & 0xfffffe00)
		)
		return -EINVAL;
	/* Check there is only one change */
	if (mddev->size != info->size) cnt++;
	if (mddev->raid_disks != info->raid_disks) cnt++;
	if (mddev->layout != info->layout) cnt++;
	if ((state ^ info->state) & (1<<MD_SB_BITMAP_PRESENT)) cnt++;
	if (cnt == 0) return 0;
	if (cnt > 1) return -EINVAL;

	if (mddev->layout != info->layout) {
		/* Change layout
		 * we don't need to do anything at the md level, the
		 * personality will take care of it all.
		 */
		if (mddev->pers->reconfig == NULL)
			return -EINVAL;
		else
			return mddev->pers->reconfig(mddev, info->layout, -1);
	}
	if (mddev->size != info->size) {
		mdk_rdev_t * rdev;
		struct list_head *tmp;
		if (mddev->pers->resize == NULL)
			return -EINVAL;
		/* The "size" is the amount of each device that is used.
		 * This can only make sense for arrays with redundancy.
		 * linear and raid0 always use whatever space is available
		 * We can only consider changing the size if no resync
		 * or reconstruction is happening, and if the new size
		 * is acceptable. It must fit before the sb_offset or,
		 * if that is <data_offset, it must fit before the
		 * size of each device.
		 * If size is zero, we find the largest size that fits.
		 */
		if (mddev->sync_thread)
			return -EBUSY;
		ITERATE_RDEV(mddev,rdev,tmp) {
			sector_t avail;
			int fit = (info->size == 0);
			if (rdev->sb_offset > rdev->data_offset)
				avail = (rdev->sb_offset*2) - rdev->data_offset;
			else
				avail = get_capacity(rdev->bdev->bd_disk)
					- rdev->data_offset;
			if (fit && (info->size == 0 || info->size > avail/2))
				info->size = avail/2;
			if (avail < ((sector_t)info->size << 1))
				return -ENOSPC;
		}
		rv = mddev->pers->resize(mddev, (sector_t)info->size *2);
		if (!rv) {
			struct block_device *bdev;

			bdev = bdget_disk(mddev->gendisk, 0);
			if (bdev) {
				down(&bdev->bd_inode->i_sem);
				i_size_write(bdev->bd_inode, mddev->array_size << 10);
				up(&bdev->bd_inode->i_sem);
				bdput(bdev);
			}
		}
	}
	if (mddev->raid_disks    != info->raid_disks) {
		/* change the number of raid disks */
		if (mddev->pers->reshape == NULL)
			return -EINVAL;
		if (info->raid_disks <= 0 ||
		    info->raid_disks >= mddev->max_disks)
			return -EINVAL;
		if (mddev->sync_thread)
			return -EBUSY;
		rv = mddev->pers->reshape(mddev, info->raid_disks);
		if (!rv) {
			struct block_device *bdev;

			bdev = bdget_disk(mddev->gendisk, 0);
			if (bdev) {
				down(&bdev->bd_inode->i_sem);
				i_size_write(bdev->bd_inode, mddev->array_size << 10);
				up(&bdev->bd_inode->i_sem);
				bdput(bdev);
			}
		}
	}
	if ((state ^ info->state) & (1<<MD_SB_BITMAP_PRESENT)) {
		if (mddev->pers->quiesce == NULL)
			return -EINVAL;
		if (mddev->recovery || mddev->sync_thread)
			return -EBUSY;
		if (info->state & (1<<MD_SB_BITMAP_PRESENT)) {
			/* add the bitmap */
			if (mddev->bitmap)
				return -EEXIST;
			if (mddev->default_bitmap_offset == 0)
				return -EINVAL;
			mddev->bitmap_offset = mddev->default_bitmap_offset;
			mddev->pers->quiesce(mddev, 1);
			rv = bitmap_create(mddev);
			if (rv)
				bitmap_destroy(mddev);
			mddev->pers->quiesce(mddev, 0);
		} else {
			/* remove the bitmap */
			if (!mddev->bitmap)
				return -ENOENT;
			if (mddev->bitmap->file)
				return -EINVAL;
			mddev->pers->quiesce(mddev, 1);
			bitmap_destroy(mddev);
			mddev->pers->quiesce(mddev, 0);
			mddev->bitmap_offset = 0;
		}
	}
	md_update_sb(mddev);
	return rv;
}

static int set_disk_faulty(mddev_t *mddev, dev_t dev)
{
	mdk_rdev_t *rdev;

	if (mddev->pers == NULL)
		return -ENODEV;

	rdev = find_rdev(mddev, dev);
	if (!rdev)
		return -ENODEV;

	md_error(mddev, rdev);
	return 0;
}

static int md_ioctl(struct inode *inode, struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int err = 0;
	void __user *argp = (void __user *)arg;
	struct hd_geometry __user *loc = argp;
	mddev_t *mddev = NULL;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/*
	 * Commands dealing with the RAID driver but not any
	 * particular array:
	 */
	switch (cmd)
	{
		case RAID_VERSION:
			err = get_version(argp);
			goto done;

		case PRINT_RAID_DEBUG:
			err = 0;
			md_print_devices();
			goto done;

#ifndef MODULE
		case RAID_AUTORUN:
			err = 0;
			autostart_arrays(arg);
			goto done;
#endif
		default:;
	}

	/*
	 * Commands creating/starting a new array:
	 */

	mddev = inode->i_bdev->bd_disk->private_data;

	if (!mddev) {
		BUG();
		goto abort;
	}


	if (cmd == START_ARRAY) {
		/* START_ARRAY doesn't need to lock the array as autostart_array
		 * does the locking, and it could even be a different array
		 */
		static int cnt = 3;
		if (cnt > 0 ) {
			printk(KERN_WARNING
			       "md: %s(pid %d) used deprecated START_ARRAY ioctl. "
			       "This will not be supported beyond July 2006\n",
			       current->comm, current->pid);
			cnt--;
		}
		err = autostart_array(new_decode_dev(arg));
		if (err) {
			printk(KERN_WARNING "md: autostart failed!\n");
			goto abort;
		}
		goto done;
	}

	err = mddev_lock(mddev);
	if (err) {
		printk(KERN_INFO 
			"md: ioctl lock interrupted, reason %d, cmd %d\n",
			err, cmd);
		goto abort;
	}

	switch (cmd)
	{
		case SET_ARRAY_INFO:
			{
				mdu_array_info_t info;
				if (!arg)
					memset(&info, 0, sizeof(info));
				else if (copy_from_user(&info, argp, sizeof(info))) {
					err = -EFAULT;
					goto abort_unlock;
				}
				if (mddev->pers) {
					err = update_array_info(mddev, &info);
					if (err) {
						printk(KERN_WARNING "md: couldn't update"
						       " array info. %d\n", err);
						goto abort_unlock;
					}
					goto done_unlock;
				}
				if (!list_empty(&mddev->disks)) {
					printk(KERN_WARNING
					       "md: array %s already has disks!\n",
					       mdname(mddev));
					err = -EBUSY;
					goto abort_unlock;
				}
				if (mddev->raid_disks) {
					printk(KERN_WARNING
					       "md: array %s already initialised!\n",
					       mdname(mddev));
					err = -EBUSY;
					goto abort_unlock;
				}
				err = set_array_info(mddev, &info);
				if (err) {
					printk(KERN_WARNING "md: couldn't set"
					       " array info. %d\n", err);
					goto abort_unlock;
				}
			}
			goto done_unlock;

		default:;
	}

	/*
	 * Commands querying/configuring an existing array:
	 */
	/* if we are not initialised yet, only ADD_NEW_DISK, STOP_ARRAY,
	 * RUN_ARRAY, and SET_BITMAP_FILE are allowed */
	if (!mddev->raid_disks && cmd != ADD_NEW_DISK && cmd != STOP_ARRAY
			&& cmd != RUN_ARRAY && cmd != SET_BITMAP_FILE) {
		err = -ENODEV;
		goto abort_unlock;
	}

	/*
	 * Commands even a read-only array can execute:
	 */
	switch (cmd)
	{
		case GET_ARRAY_INFO:
			err = get_array_info(mddev, argp);
			goto done_unlock;

		case GET_BITMAP_FILE:
			err = get_bitmap_file(mddev, argp);
			goto done_unlock;

		case GET_DISK_INFO:
			err = get_disk_info(mddev, argp);
			goto done_unlock;

		case RESTART_ARRAY_RW:
			err = restart_array(mddev);
			goto done_unlock;

		case STOP_ARRAY:
			err = do_md_stop (mddev, 0);
			goto done_unlock;

		case STOP_ARRAY_RO:
			err = do_md_stop (mddev, 1);
			goto done_unlock;

	/*
	 * We have a problem here : there is no easy way to give a CHS
	 * virtual geometry. We currently pretend that we have a 2 heads
	 * 4 sectors (with a BIG number of cylinders...). This drives
	 * dosfs just mad... ;-)
	 */
		case HDIO_GETGEO:
			if (!loc) {
				err = -EINVAL;
				goto abort_unlock;
			}
			err = put_user (2, (char __user *) &loc->heads);
			if (err)
				goto abort_unlock;
			err = put_user (4, (char __user *) &loc->sectors);
			if (err)
				goto abort_unlock;
			err = put_user(get_capacity(mddev->gendisk)/8,
					(short __user *) &loc->cylinders);
			if (err)
				goto abort_unlock;
			err = put_user (get_start_sect(inode->i_bdev),
						(long __user *) &loc->start);
			goto done_unlock;
	}

	/*
	 * The remaining ioctls are changing the state of the
	 * superblock, so we do not allow them on read-only arrays.
	 * However non-MD ioctls (e.g. get-size) will still come through
	 * here and hit the 'default' below, so only disallow
	 * 'md' ioctls, and switch to rw mode if started auto-readonly.
	 */
	if (_IOC_TYPE(cmd) == MD_MAJOR &&
	    mddev->ro && mddev->pers) {
		if (mddev->ro == 2) {
			mddev->ro = 0;
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		md_wakeup_thread(mddev->thread);

		} else {
			err = -EROFS;
			goto abort_unlock;
		}
	}

	switch (cmd)
	{
		case ADD_NEW_DISK:
		{
			mdu_disk_info_t info;
			if (copy_from_user(&info, argp, sizeof(info)))
				err = -EFAULT;
			else
				err = add_new_disk(mddev, &info);
			goto done_unlock;
		}

		case HOT_REMOVE_DISK:
			err = hot_remove_disk(mddev, new_decode_dev(arg));
			goto done_unlock;

		case HOT_ADD_DISK:
			err = hot_add_disk(mddev, new_decode_dev(arg));
			goto done_unlock;

		case SET_DISK_FAULTY:
			err = set_disk_faulty(mddev, new_decode_dev(arg));
			goto done_unlock;

		case RUN_ARRAY:
			err = do_md_run (mddev);
			goto done_unlock;

		case SET_BITMAP_FILE:
			err = set_bitmap_file(mddev, (int)arg);
			goto done_unlock;

		default:
			if (_IOC_TYPE(cmd) == MD_MAJOR)
				printk(KERN_WARNING "md: %s(pid %d) used"
					" obsolete MD ioctl, upgrade your"
					" software to use new ictls.\n",
					current->comm, current->pid);
			err = -EINVAL;
			goto abort_unlock;
	}

done_unlock:
abort_unlock:
	mddev_unlock(mddev);

	return err;
done:
	if (err)
		MD_BUG();
abort:
	return err;
}

static int md_open(struct inode *inode, struct file *file)
{
	/*
	 * Succeed if we can lock the mddev, which confirms that
	 * it isn't being stopped right now.
	 */
	mddev_t *mddev = inode->i_bdev->bd_disk->private_data;
	int err;

	if ((err = mddev_lock(mddev)))
		goto out;

	err = 0;
	mddev_get(mddev);
	mddev_unlock(mddev);

	check_disk_change(inode->i_bdev);
 out:
	return err;
}

static int md_release(struct inode *inode, struct file * file)
{
 	mddev_t *mddev = inode->i_bdev->bd_disk->private_data;

	if (!mddev)
		BUG();
	mddev_put(mddev);

	return 0;
}

static int md_media_changed(struct gendisk *disk)
{
	mddev_t *mddev = disk->private_data;

	return mddev->changed;
}

static int md_revalidate(struct gendisk *disk)
{
	mddev_t *mddev = disk->private_data;

	mddev->changed = 0;
	return 0;
}
static struct block_device_operations md_fops =
{
	.owner		= THIS_MODULE,
	.open		= md_open,
	.release	= md_release,
	.ioctl		= md_ioctl,
	.media_changed	= md_media_changed,
	.revalidate_disk= md_revalidate,
};

static int md_thread(void * arg)
{
	mdk_thread_t *thread = arg;

	/*
	 * md_thread is a 'system-thread', it's priority should be very
	 * high. We avoid resource deadlocks individually in each
	 * raid personality. (RAID5 does preallocation) We also use RR and
	 * the very same RT priority as kswapd, thus we will never get
	 * into a priority inversion deadlock.
	 *
	 * we definitely have to have equal or higher priority than
	 * bdflush, otherwise bdflush will deadlock if there are too
	 * many dirty RAID5 blocks.
	 */

	allow_signal(SIGKILL);
	while (!kthread_should_stop()) {

		/* We need to wait INTERRUPTIBLE so that
		 * we don't add to the load-average.
		 * That means we need to be sure no signals are
		 * pending
		 */
		if (signal_pending(current))
			flush_signals(current);

		wait_event_interruptible_timeout
			(thread->wqueue,
			 test_bit(THREAD_WAKEUP, &thread->flags)
			 || kthread_should_stop(),
			 thread->timeout);
		try_to_freeze();

		clear_bit(THREAD_WAKEUP, &thread->flags);

		thread->run(thread->mddev);
	}

	return 0;
}

void md_wakeup_thread(mdk_thread_t *thread)
{
	if (thread) {
		dprintk("md: waking up MD thread %s.\n", thread->tsk->comm);
		set_bit(THREAD_WAKEUP, &thread->flags);
		wake_up(&thread->wqueue);
	}
}

mdk_thread_t *md_register_thread(void (*run) (mddev_t *), mddev_t *mddev,
				 const char *name)
{
	mdk_thread_t *thread;

	thread = kmalloc(sizeof(mdk_thread_t), GFP_KERNEL);
	if (!thread)
		return NULL;

	memset(thread, 0, sizeof(mdk_thread_t));
	init_waitqueue_head(&thread->wqueue);

	thread->run = run;
	thread->mddev = mddev;
	thread->timeout = MAX_SCHEDULE_TIMEOUT;
	thread->tsk = kthread_run(md_thread, thread, name, mdname(thread->mddev));
	if (IS_ERR(thread->tsk)) {
		kfree(thread);
		return NULL;
	}
	return thread;
}

void md_unregister_thread(mdk_thread_t *thread)
{
	dprintk("interrupting MD-thread pid %d\n", thread->tsk->pid);

	kthread_stop(thread->tsk);
	kfree(thread);
}

void md_error(mddev_t *mddev, mdk_rdev_t *rdev)
{
	if (!mddev) {
		MD_BUG();
		return;
	}

	if (!rdev || test_bit(Faulty, &rdev->flags))
		return;
/*
	dprintk("md_error dev:%s, rdev:(%d:%d), (caller: %p,%p,%p,%p).\n",
		mdname(mddev),
		MAJOR(rdev->bdev->bd_dev), MINOR(rdev->bdev->bd_dev),
		__builtin_return_address(0),__builtin_return_address(1),
		__builtin_return_address(2),__builtin_return_address(3));
*/
	if (!mddev->pers->error_handler)
		return;
	mddev->pers->error_handler(mddev,rdev);
	set_bit(MD_RECOVERY_INTR, &mddev->recovery);
	set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	md_wakeup_thread(mddev->thread);
}

/* seq_file implementation /proc/mdstat */

static void status_unused(struct seq_file *seq)
{
	int i = 0;
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	seq_printf(seq, "unused devices: ");

	ITERATE_RDEV_PENDING(rdev,tmp) {
		char b[BDEVNAME_SIZE];
		i++;
		seq_printf(seq, "%s ",
			      bdevname(rdev->bdev,b));
	}
	if (!i)
		seq_printf(seq, "<none>");

	seq_printf(seq, "\n");
}


static void status_resync(struct seq_file *seq, mddev_t * mddev)
{
	unsigned long max_blocks, resync, res, dt, db, rt;

	resync = (mddev->curr_resync - atomic_read(&mddev->recovery_active))/2;

	if (test_bit(MD_RECOVERY_SYNC, &mddev->recovery))
		max_blocks = mddev->resync_max_sectors >> 1;
	else
		max_blocks = mddev->size;

	/*
	 * Should not happen.
	 */
	if (!max_blocks) {
		MD_BUG();
		return;
	}
	res = (resync/1024)*1000/(max_blocks/1024 + 1);
	{
		int i, x = res/50, y = 20-x;
		seq_printf(seq, "[");
		for (i = 0; i < x; i++)
			seq_printf(seq, "=");
		seq_printf(seq, ">");
		for (i = 0; i < y; i++)
			seq_printf(seq, ".");
		seq_printf(seq, "] ");
	}
	seq_printf(seq, " %s =%3lu.%lu%% (%lu/%lu)",
		      (test_bit(MD_RECOVERY_SYNC, &mddev->recovery) ?
		       "resync" : "recovery"),
		      res/10, res % 10, resync, max_blocks);

	/*
	 * We do not want to overflow, so the order of operands and
	 * the * 100 / 100 trick are important. We do a +1 to be
	 * safe against division by zero. We only estimate anyway.
	 *
	 * dt: time from mark until now
	 * db: blocks written from mark until now
	 * rt: remaining time
	 */
	dt = ((jiffies - mddev->resync_mark) / HZ);
	if (!dt) dt++;
	db = resync - (mddev->resync_mark_cnt/2);
	rt = (dt * ((max_blocks-resync) / (db/100+1)))/100;

	seq_printf(seq, " finish=%lu.%lumin", rt / 60, (rt % 60)/6);

	seq_printf(seq, " speed=%ldK/sec", db/dt);
}

static void *md_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct list_head *tmp;
	loff_t l = *pos;
	mddev_t *mddev;

	if (l >= 0x10000)
		return NULL;
	if (!l--)
		/* header */
		return (void*)1;

	spin_lock(&all_mddevs_lock);
	list_for_each(tmp,&all_mddevs)
		if (!l--) {
			mddev = list_entry(tmp, mddev_t, all_mddevs);
			mddev_get(mddev);
			spin_unlock(&all_mddevs_lock);
			return mddev;
		}
	spin_unlock(&all_mddevs_lock);
	if (!l--)
		return (void*)2;/* tail */
	return NULL;
}

static void *md_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct list_head *tmp;
	mddev_t *next_mddev, *mddev = v;
	
	++*pos;
	if (v == (void*)2)
		return NULL;

	spin_lock(&all_mddevs_lock);
	if (v == (void*)1)
		tmp = all_mddevs.next;
	else
		tmp = mddev->all_mddevs.next;
	if (tmp != &all_mddevs)
		next_mddev = mddev_get(list_entry(tmp,mddev_t,all_mddevs));
	else {
		next_mddev = (void*)2;
		*pos = 0x10000;
	}		
	spin_unlock(&all_mddevs_lock);

	if (v != (void*)1)
		mddev_put(mddev);
	return next_mddev;

}

static void md_seq_stop(struct seq_file *seq, void *v)
{
	mddev_t *mddev = v;

	if (mddev && v != (void*)1 && v != (void*)2)
		mddev_put(mddev);
}

static int md_seq_show(struct seq_file *seq, void *v)
{
	mddev_t *mddev = v;
	sector_t size;
	struct list_head *tmp2;
	mdk_rdev_t *rdev;
	int i;
	struct bitmap *bitmap;

	if (v == (void*)1) {
		seq_printf(seq, "Personalities : ");
		spin_lock(&pers_lock);
		for (i = 0; i < MAX_PERSONALITY; i++)
			if (pers[i])
				seq_printf(seq, "[%s] ", pers[i]->name);

		spin_unlock(&pers_lock);
		seq_printf(seq, "\n");
		return 0;
	}
	if (v == (void*)2) {
		status_unused(seq);
		return 0;
	}

	if (mddev_lock(mddev)!=0) 
		return -EINTR;
	if (mddev->pers || mddev->raid_disks || !list_empty(&mddev->disks)) {
		seq_printf(seq, "%s : %sactive", mdname(mddev),
						mddev->pers ? "" : "in");
		if (mddev->pers) {
			if (mddev->ro==1)
				seq_printf(seq, " (read-only)");
			if (mddev->ro==2)
				seq_printf(seq, "(auto-read-only)");
			seq_printf(seq, " %s", mddev->pers->name);
		}

		size = 0;
		ITERATE_RDEV(mddev,rdev,tmp2) {
			char b[BDEVNAME_SIZE];
			seq_printf(seq, " %s[%d]",
				bdevname(rdev->bdev,b), rdev->desc_nr);
			if (test_bit(WriteMostly, &rdev->flags))
				seq_printf(seq, "(W)");
			if (test_bit(Faulty, &rdev->flags)) {
				seq_printf(seq, "(F)");
				continue;
			} else if (rdev->raid_disk < 0)
				seq_printf(seq, "(S)"); /* spare */
			size += rdev->size;
		}

		if (!list_empty(&mddev->disks)) {
			if (mddev->pers)
				seq_printf(seq, "\n      %llu blocks",
					(unsigned long long)mddev->array_size);
			else
				seq_printf(seq, "\n      %llu blocks",
					(unsigned long long)size);
		}
		if (mddev->persistent) {
			if (mddev->major_version != 0 ||
			    mddev->minor_version != 90) {
				seq_printf(seq," super %d.%d",
					   mddev->major_version,
					   mddev->minor_version);
			}
		} else
			seq_printf(seq, " super non-persistent");

		if (mddev->pers) {
			mddev->pers->status (seq, mddev);
	 		seq_printf(seq, "\n      ");
			if (mddev->pers->sync_request) {
				if (mddev->curr_resync > 2) {
					status_resync (seq, mddev);
					seq_printf(seq, "\n      ");
				} else if (mddev->curr_resync == 1 || mddev->curr_resync == 2)
					seq_printf(seq, "\tresync=DELAYED\n      ");
				else if (mddev->recovery_cp < MaxSector)
					seq_printf(seq, "\tresync=PENDING\n      ");
			}
		} else
			seq_printf(seq, "\n       ");

		if ((bitmap = mddev->bitmap)) {
			unsigned long chunk_kb;
			unsigned long flags;
			spin_lock_irqsave(&bitmap->lock, flags);
			chunk_kb = bitmap->chunksize >> 10;
			seq_printf(seq, "bitmap: %lu/%lu pages [%luKB], "
				"%lu%s chunk",
				bitmap->pages - bitmap->missing_pages,
				bitmap->pages,
				(bitmap->pages - bitmap->missing_pages)
					<< (PAGE_SHIFT - 10),
				chunk_kb ? chunk_kb : bitmap->chunksize,
				chunk_kb ? "KB" : "B");
			if (bitmap->file) {
				seq_printf(seq, ", file: ");
				seq_path(seq, bitmap->file->f_vfsmnt,
					 bitmap->file->f_dentry," \t\n");
			}

			seq_printf(seq, "\n");
			spin_unlock_irqrestore(&bitmap->lock, flags);
		}

		seq_printf(seq, "\n");
	}
	mddev_unlock(mddev);
	
	return 0;
}

static struct seq_operations md_seq_ops = {
	.start  = md_seq_start,
	.next   = md_seq_next,
	.stop   = md_seq_stop,
	.show   = md_seq_show,
};

static int md_seq_open(struct inode *inode, struct file *file)
{
	int error;

	error = seq_open(file, &md_seq_ops);
	return error;
}

static struct file_operations md_seq_fops = {
	.open           = md_seq_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release	= seq_release,
};

int register_md_personality(int pnum, mdk_personality_t *p)
{
	if (pnum >= MAX_PERSONALITY) {
		printk(KERN_ERR
		       "md: tried to install personality %s as nr %d, but max is %lu\n",
		       p->name, pnum, MAX_PERSONALITY-1);
		return -EINVAL;
	}

	spin_lock(&pers_lock);
	if (pers[pnum]) {
		spin_unlock(&pers_lock);
		return -EBUSY;
	}

	pers[pnum] = p;
	printk(KERN_INFO "md: %s personality registered as nr %d\n", p->name, pnum);
	spin_unlock(&pers_lock);
	return 0;
}

int unregister_md_personality(int pnum)
{
	if (pnum >= MAX_PERSONALITY)
		return -EINVAL;

	printk(KERN_INFO "md: %s personality unregistered\n", pers[pnum]->name);
	spin_lock(&pers_lock);
	pers[pnum] = NULL;
	spin_unlock(&pers_lock);
	return 0;
}

static int is_mddev_idle(mddev_t *mddev)
{
	mdk_rdev_t * rdev;
	struct list_head *tmp;
	int idle;
	unsigned long curr_events;

	idle = 1;
	ITERATE_RDEV(mddev,rdev,tmp) {
		struct gendisk *disk = rdev->bdev->bd_contains->bd_disk;
		curr_events = disk_stat_read(disk, sectors[0]) + 
				disk_stat_read(disk, sectors[1]) - 
				atomic_read(&disk->sync_io);
		/* The difference between curr_events and last_events
		 * will be affected by any new non-sync IO (making
		 * curr_events bigger) and any difference in the amount of
		 * in-flight syncio (making current_events bigger or smaller)
		 * The amount in-flight is currently limited to
		 * 32*64K in raid1/10 and 256*PAGE_SIZE in raid5/6
		 * which is at most 4096 sectors.
		 * These numbers are fairly fragile and should be made
		 * more robust, probably by enforcing the
		 * 'window size' that md_do_sync sort-of uses.
		 *
		 * Note: the following is an unsigned comparison.
		 */
		if ((curr_events - rdev->last_events + 4096) > 8192) {
			rdev->last_events = curr_events;
			idle = 0;
		}
	}
	return idle;
}

void md_done_sync(mddev_t *mddev, int blocks, int ok)
{
	/* another "blocks" (512byte) blocks have been synced */
	atomic_sub(blocks, &mddev->recovery_active);
	wake_up(&mddev->recovery_wait);
	if (!ok) {
		set_bit(MD_RECOVERY_ERR, &mddev->recovery);
		md_wakeup_thread(mddev->thread);
		// stop recovery, signal do_sync ....
	}
}


/* md_write_start(mddev, bi)
 * If we need to update some array metadata (e.g. 'active' flag
 * in superblock) before writing, schedule a superblock update
 * and wait for it to complete.
 */
void md_write_start(mddev_t *mddev, struct bio *bi)
{
	if (bio_data_dir(bi) != WRITE)
		return;

	BUG_ON(mddev->ro == 1);
	if (mddev->ro == 2) {
		/* need to switch to read/write */
		mddev->ro = 0;
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		md_wakeup_thread(mddev->thread);
	}
	atomic_inc(&mddev->writes_pending);
	if (mddev->in_sync) {
		spin_lock_irq(&mddev->write_lock);
		if (mddev->in_sync) {
			mddev->in_sync = 0;
			mddev->sb_dirty = 1;
			md_wakeup_thread(mddev->thread);
		}
		spin_unlock_irq(&mddev->write_lock);
	}
	wait_event(mddev->sb_wait, mddev->sb_dirty==0);
}

void md_write_end(mddev_t *mddev)
{
	if (atomic_dec_and_test(&mddev->writes_pending)) {
		if (mddev->safemode == 2)
			md_wakeup_thread(mddev->thread);
		else
			mod_timer(&mddev->safemode_timer, jiffies + mddev->safemode_delay);
	}
}

static DECLARE_WAIT_QUEUE_HEAD(resync_wait);

#define SYNC_MARKS	10
#define	SYNC_MARK_STEP	(3*HZ)
static void md_do_sync(mddev_t *mddev)
{
	mddev_t *mddev2;
	unsigned int currspeed = 0,
		 window;
	sector_t max_sectors,j, io_sectors;
	unsigned long mark[SYNC_MARKS];
	sector_t mark_cnt[SYNC_MARKS];
	int last_mark,m;
	struct list_head *tmp;
	sector_t last_check;
	int skipped = 0;

	/* just incase thread restarts... */
	if (test_bit(MD_RECOVERY_DONE, &mddev->recovery))
		return;

	/* we overload curr_resync somewhat here.
	 * 0 == not engaged in resync at all
	 * 2 == checking that there is no conflict with another sync
	 * 1 == like 2, but have yielded to allow conflicting resync to
	 *		commense
	 * other == active in resync - this many blocks
	 *
	 * Before starting a resync we must have set curr_resync to
	 * 2, and then checked that every "conflicting" array has curr_resync
	 * less than ours.  When we find one that is the same or higher
	 * we wait on resync_wait.  To avoid deadlock, we reduce curr_resync
	 * to 1 if we choose to yield (based arbitrarily on address of mddev structure).
	 * This will mean we have to start checking from the beginning again.
	 *
	 */

	do {
		mddev->curr_resync = 2;

	try_again:
		if (kthread_should_stop()) {
			set_bit(MD_RECOVERY_INTR, &mddev->recovery);
			goto skip;
		}
		ITERATE_MDDEV(mddev2,tmp) {
			if (mddev2 == mddev)
				continue;
			if (mddev2->curr_resync && 
			    match_mddev_units(mddev,mddev2)) {
				DEFINE_WAIT(wq);
				if (mddev < mddev2 && mddev->curr_resync == 2) {
					/* arbitrarily yield */
					mddev->curr_resync = 1;
					wake_up(&resync_wait);
				}
				if (mddev > mddev2 && mddev->curr_resync == 1)
					/* no need to wait here, we can wait the next
					 * time 'round when curr_resync == 2
					 */
					continue;
				prepare_to_wait(&resync_wait, &wq, TASK_UNINTERRUPTIBLE);
				if (!kthread_should_stop() &&
				    mddev2->curr_resync >= mddev->curr_resync) {
					printk(KERN_INFO "md: delaying resync of %s"
					       " until %s has finished resync (they"
					       " share one or more physical units)\n",
					       mdname(mddev), mdname(mddev2));
					mddev_put(mddev2);
					schedule();
					finish_wait(&resync_wait, &wq);
					goto try_again;
				}
				finish_wait(&resync_wait, &wq);
			}
		}
	} while (mddev->curr_resync < 2);

	if (test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
		/* resync follows the size requested by the personality,
		 * which defaults to physical size, but can be virtual size
		 */
		max_sectors = mddev->resync_max_sectors;
		mddev->resync_mismatches = 0;
	} else
		/* recovery follows the physical size of devices */
		max_sectors = mddev->size << 1;

	printk(KERN_INFO "md: syncing RAID array %s\n", mdname(mddev));
	printk(KERN_INFO "md: minimum _guaranteed_ reconstruction speed:"
		" %d KB/sec/disc.\n", sysctl_speed_limit_min);
	printk(KERN_INFO "md: using maximum available idle IO bandwidth "
	       "(but not more than %d KB/sec) for reconstruction.\n",
	       sysctl_speed_limit_max);

	is_mddev_idle(mddev); /* this also initializes IO event counters */
	/* we don't use the checkpoint if there's a bitmap */
	if (test_bit(MD_RECOVERY_SYNC, &mddev->recovery) && !mddev->bitmap
	    && ! test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery))
		j = mddev->recovery_cp;
	else
		j = 0;
	io_sectors = 0;
	for (m = 0; m < SYNC_MARKS; m++) {
		mark[m] = jiffies;
		mark_cnt[m] = io_sectors;
	}
	last_mark = 0;
	mddev->resync_mark = mark[last_mark];
	mddev->resync_mark_cnt = mark_cnt[last_mark];

	/*
	 * Tune reconstruction:
	 */
	window = 32*(PAGE_SIZE/512);
	printk(KERN_INFO "md: using %dk window, over a total of %llu blocks.\n",
		window/2,(unsigned long long) max_sectors/2);

	atomic_set(&mddev->recovery_active, 0);
	init_waitqueue_head(&mddev->recovery_wait);
	last_check = 0;

	if (j>2) {
		printk(KERN_INFO 
			"md: resuming recovery of %s from checkpoint.\n",
			mdname(mddev));
		mddev->curr_resync = j;
	}

	while (j < max_sectors) {
		sector_t sectors;

		skipped = 0;
		sectors = mddev->pers->sync_request(mddev, j, &skipped,
					    currspeed < sysctl_speed_limit_min);
		if (sectors == 0) {
			set_bit(MD_RECOVERY_ERR, &mddev->recovery);
			goto out;
		}

		if (!skipped) { /* actual IO requested */
			io_sectors += sectors;
			atomic_add(sectors, &mddev->recovery_active);
		}

		j += sectors;
		if (j>1) mddev->curr_resync = j;


		if (last_check + window > io_sectors || j == max_sectors)
			continue;

		last_check = io_sectors;

		if (test_bit(MD_RECOVERY_INTR, &mddev->recovery) ||
		    test_bit(MD_RECOVERY_ERR, &mddev->recovery))
			break;

	repeat:
		if (time_after_eq(jiffies, mark[last_mark] + SYNC_MARK_STEP )) {
			/* step marks */
			int next = (last_mark+1) % SYNC_MARKS;

			mddev->resync_mark = mark[next];
			mddev->resync_mark_cnt = mark_cnt[next];
			mark[next] = jiffies;
			mark_cnt[next] = io_sectors - atomic_read(&mddev->recovery_active);
			last_mark = next;
		}


		if (kthread_should_stop()) {
			/*
			 * got a signal, exit.
			 */
			printk(KERN_INFO 
				"md: md_do_sync() got signal ... exiting\n");
			set_bit(MD_RECOVERY_INTR, &mddev->recovery);
			goto out;
		}

		/*
		 * this loop exits only if either when we are slower than
		 * the 'hard' speed limit, or the system was IO-idle for
		 * a jiffy.
		 * the system might be non-idle CPU-wise, but we only care
		 * about not overloading the IO subsystem. (things like an
		 * e2fsck being done on the RAID array should execute fast)
		 */
		mddev->queue->unplug_fn(mddev->queue);
		cond_resched();

		currspeed = ((unsigned long)(io_sectors-mddev->resync_mark_cnt))/2
			/((jiffies-mddev->resync_mark)/HZ +1) +1;

		if (currspeed > sysctl_speed_limit_min) {
			if ((currspeed > sysctl_speed_limit_max) ||
					!is_mddev_idle(mddev)) {
				msleep(500);
				goto repeat;
			}
		}
	}
	printk(KERN_INFO "md: %s: sync done.\n",mdname(mddev));
	/*
	 * this also signals 'finished resyncing' to md_stop
	 */
 out:
	mddev->queue->unplug_fn(mddev->queue);

	wait_event(mddev->recovery_wait, !atomic_read(&mddev->recovery_active));

	/* tell personality that we are finished */
	mddev->pers->sync_request(mddev, max_sectors, &skipped, 1);

	if (!test_bit(MD_RECOVERY_ERR, &mddev->recovery) &&
	    mddev->curr_resync > 2 &&
	    mddev->curr_resync >= mddev->recovery_cp) {
		if (test_bit(MD_RECOVERY_INTR, &mddev->recovery)) {
			printk(KERN_INFO 
				"md: checkpointing recovery of %s.\n",
				mdname(mddev));
			mddev->recovery_cp = mddev->curr_resync;
		} else
			mddev->recovery_cp = MaxSector;
	}

 skip:
	mddev->curr_resync = 0;
	wake_up(&resync_wait);
	set_bit(MD_RECOVERY_DONE, &mddev->recovery);
	md_wakeup_thread(mddev->thread);
}


/*
 * This routine is regularly called by all per-raid-array threads to
 * deal with generic issues like resync and super-block update.
 * Raid personalities that don't have a thread (linear/raid0) do not
 * need this as they never do any recovery or update the superblock.
 *
 * It does not do any resync itself, but rather "forks" off other threads
 * to do that as needed.
 * When it is determined that resync is needed, we set MD_RECOVERY_RUNNING in
 * "->recovery" and create a thread at ->sync_thread.
 * When the thread finishes it sets MD_RECOVERY_DONE (and might set MD_RECOVERY_ERR)
 * and wakeups up this thread which will reap the thread and finish up.
 * This thread also removes any faulty devices (with nr_pending == 0).
 *
 * The overall approach is:
 *  1/ if the superblock needs updating, update it.
 *  2/ If a recovery thread is running, don't do anything else.
 *  3/ If recovery has finished, clean up, possibly marking spares active.
 *  4/ If there are any faulty devices, remove them.
 *  5/ If array is degraded, try to add spares devices
 *  6/ If array has spares or is not in-sync, start a resync thread.
 */
void md_check_recovery(mddev_t *mddev)
{
	mdk_rdev_t *rdev;
	struct list_head *rtmp;


	if (mddev->bitmap)
		bitmap_daemon_work(mddev->bitmap);

	if (mddev->ro)
		return;

	if (signal_pending(current)) {
		if (mddev->pers->sync_request) {
			printk(KERN_INFO "md: %s in immediate safe mode\n",
			       mdname(mddev));
			mddev->safemode = 2;
		}
		flush_signals(current);
	}

	if ( ! (
		mddev->sb_dirty ||
		test_bit(MD_RECOVERY_NEEDED, &mddev->recovery) ||
		test_bit(MD_RECOVERY_DONE, &mddev->recovery) ||
		(mddev->safemode == 1) ||
		(mddev->safemode == 2 && ! atomic_read(&mddev->writes_pending)
		 && !mddev->in_sync && mddev->recovery_cp == MaxSector)
		))
		return;

	if (mddev_trylock(mddev)==0) {
		int spares =0;

		spin_lock_irq(&mddev->write_lock);
		if (mddev->safemode && !atomic_read(&mddev->writes_pending) &&
		    !mddev->in_sync && mddev->recovery_cp == MaxSector) {
			mddev->in_sync = 1;
			mddev->sb_dirty = 1;
		}
		if (mddev->safemode == 1)
			mddev->safemode = 0;
		spin_unlock_irq(&mddev->write_lock);

		if (mddev->sb_dirty)
			md_update_sb(mddev);


		if (test_bit(MD_RECOVERY_RUNNING, &mddev->recovery) &&
		    !test_bit(MD_RECOVERY_DONE, &mddev->recovery)) {
			/* resync/recovery still happening */
			clear_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
			goto unlock;
		}
		if (mddev->sync_thread) {
			/* resync has finished, collect result */
			md_unregister_thread(mddev->sync_thread);
			mddev->sync_thread = NULL;
			if (!test_bit(MD_RECOVERY_ERR, &mddev->recovery) &&
			    !test_bit(MD_RECOVERY_INTR, &mddev->recovery)) {
				/* success...*/
				/* activate any spares */
				mddev->pers->spare_active(mddev);
			}
			md_update_sb(mddev);

			/* if array is no-longer degraded, then any saved_raid_disk
			 * information must be scrapped
			 */
			if (!mddev->degraded)
				ITERATE_RDEV(mddev,rdev,rtmp)
					rdev->saved_raid_disk = -1;

			mddev->recovery = 0;
			/* flag recovery needed just to double check */
			set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
			goto unlock;
		}
		/* Clear some bits that don't mean anything, but
		 * might be left set
		 */
		clear_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		clear_bit(MD_RECOVERY_ERR, &mddev->recovery);
		clear_bit(MD_RECOVERY_INTR, &mddev->recovery);
		clear_bit(MD_RECOVERY_DONE, &mddev->recovery);

		/* no recovery is running.
		 * remove any failed drives, then
		 * add spares if possible.
		 * Spare are also removed and re-added, to allow
		 * the personality to fail the re-add.
		 */
		ITERATE_RDEV(mddev,rdev,rtmp)
			if (rdev->raid_disk >= 0 &&
			    (test_bit(Faulty, &rdev->flags) || ! test_bit(In_sync, &rdev->flags)) &&
			    atomic_read(&rdev->nr_pending)==0) {
				if (mddev->pers->hot_remove_disk(mddev, rdev->raid_disk)==0) {
					char nm[20];
					sprintf(nm,"rd%d", rdev->raid_disk);
					sysfs_remove_link(&mddev->kobj, nm);
					rdev->raid_disk = -1;
				}
			}

		if (mddev->degraded) {
			ITERATE_RDEV(mddev,rdev,rtmp)
				if (rdev->raid_disk < 0
				    && !test_bit(Faulty, &rdev->flags)) {
					if (mddev->pers->hot_add_disk(mddev,rdev)) {
						char nm[20];
						sprintf(nm, "rd%d", rdev->raid_disk);
						sysfs_create_link(&mddev->kobj, &rdev->kobj, nm);
						spares++;
					} else
						break;
				}
		}

		if (spares) {
			clear_bit(MD_RECOVERY_SYNC, &mddev->recovery);
			clear_bit(MD_RECOVERY_CHECK, &mddev->recovery);
		} else if (mddev->recovery_cp < MaxSector) {
			set_bit(MD_RECOVERY_SYNC, &mddev->recovery);
		} else if (!test_bit(MD_RECOVERY_SYNC, &mddev->recovery))
			/* nothing to be done ... */
			goto unlock;

		if (mddev->pers->sync_request) {
			set_bit(MD_RECOVERY_RUNNING, &mddev->recovery);
			if (spares && mddev->bitmap && ! mddev->bitmap->file) {
				/* We are adding a device or devices to an array
				 * which has the bitmap stored on all devices.
				 * So make sure all bitmap pages get written
				 */
				bitmap_write_all(mddev->bitmap);
			}
			mddev->sync_thread = md_register_thread(md_do_sync,
								mddev,
								"%s_resync");
			if (!mddev->sync_thread) {
				printk(KERN_ERR "%s: could not start resync"
					" thread...\n", 
					mdname(mddev));
				/* leave the spares where they are, it shouldn't hurt */
				mddev->recovery = 0;
			} else {
				md_wakeup_thread(mddev->sync_thread);
			}
		}
	unlock:
		mddev_unlock(mddev);
	}
}

static int md_notify_reboot(struct notifier_block *this,
			    unsigned long code, void *x)
{
	struct list_head *tmp;
	mddev_t *mddev;

	if ((code == SYS_DOWN) || (code == SYS_HALT) || (code == SYS_POWER_OFF)) {

		printk(KERN_INFO "md: stopping all md devices.\n");

		ITERATE_MDDEV(mddev,tmp)
			if (mddev_trylock(mddev)==0)
				do_md_stop (mddev, 1);
		/*
		 * certain more exotic SCSI devices are known to be
		 * volatile wrt too early system reboots. While the
		 * right place to handle this issue is the given
		 * driver, we do want to have a safe RAID driver ...
		 */
		mdelay(1000*1);
	}
	return NOTIFY_DONE;
}

static struct notifier_block md_notifier = {
	.notifier_call	= md_notify_reboot,
	.next		= NULL,
	.priority	= INT_MAX, /* before any real devices */
};

static void md_geninit(void)
{
	struct proc_dir_entry *p;

	dprintk("md: sizeof(mdp_super_t) = %d\n", (int)sizeof(mdp_super_t));

	p = create_proc_entry("mdstat", S_IRUGO, NULL);
	if (p)
		p->proc_fops = &md_seq_fops;
}

static int __init md_init(void)
{
	int minor;

	printk(KERN_INFO "md: md driver %d.%d.%d MAX_MD_DEVS=%d,"
			" MD_SB_DISKS=%d\n",
			MD_MAJOR_VERSION, MD_MINOR_VERSION,
			MD_PATCHLEVEL_VERSION, MAX_MD_DEVS, MD_SB_DISKS);
	printk(KERN_INFO "md: bitmap version %d.%d\n", BITMAP_MAJOR_HI,
			BITMAP_MINOR);

	if (register_blkdev(MAJOR_NR, "md"))
		return -1;
	if ((mdp_major=register_blkdev(0, "mdp"))<=0) {
		unregister_blkdev(MAJOR_NR, "md");
		return -1;
	}
	devfs_mk_dir("md");
	blk_register_region(MKDEV(MAJOR_NR, 0), MAX_MD_DEVS, THIS_MODULE,
				md_probe, NULL, NULL);
	blk_register_region(MKDEV(mdp_major, 0), MAX_MD_DEVS<<MdpMinorShift, THIS_MODULE,
			    md_probe, NULL, NULL);

	for (minor=0; minor < MAX_MD_DEVS; ++minor)
		devfs_mk_bdev(MKDEV(MAJOR_NR, minor),
				S_IFBLK|S_IRUSR|S_IWUSR,
				"md/%d", minor);

	for (minor=0; minor < MAX_MD_DEVS; ++minor)
		devfs_mk_bdev(MKDEV(mdp_major, minor<<MdpMinorShift),
			      S_IFBLK|S_IRUSR|S_IWUSR,
			      "md/mdp%d", minor);


	register_reboot_notifier(&md_notifier);
	raid_table_header = register_sysctl_table(raid_root_table, 1);

	md_geninit();
	return (0);
}


#ifndef MODULE

/*
 * Searches all registered partitions for autorun RAID arrays
 * at boot time.
 */
static dev_t detected_devices[128];
static int dev_cnt;

void md_autodetect_dev(dev_t dev)
{
	if (dev_cnt >= 0 && dev_cnt < 127)
		detected_devices[dev_cnt++] = dev;
}


static void autostart_arrays(int part)
{
	mdk_rdev_t *rdev;
	int i;

	printk(KERN_INFO "md: Autodetecting RAID arrays.\n");

	for (i = 0; i < dev_cnt; i++) {
		dev_t dev = detected_devices[i];

		rdev = md_import_device(dev,0, 0);
		if (IS_ERR(rdev))
			continue;

		if (test_bit(Faulty, &rdev->flags)) {
			MD_BUG();
			continue;
		}
		list_add(&rdev->same_set, &pending_raid_disks);
	}
	dev_cnt = 0;

	autorun_devices(part);
}

#endif

static __exit void md_exit(void)
{
	mddev_t *mddev;
	struct list_head *tmp;
	int i;
	blk_unregister_region(MKDEV(MAJOR_NR,0), MAX_MD_DEVS);
	blk_unregister_region(MKDEV(mdp_major,0), MAX_MD_DEVS << MdpMinorShift);
	for (i=0; i < MAX_MD_DEVS; i++)
		devfs_remove("md/%d", i);
	for (i=0; i < MAX_MD_DEVS; i++)
		devfs_remove("md/d%d", i);

	devfs_remove("md");

	unregister_blkdev(MAJOR_NR,"md");
	unregister_blkdev(mdp_major, "mdp");
	unregister_reboot_notifier(&md_notifier);
	unregister_sysctl_table(raid_table_header);
	remove_proc_entry("mdstat", NULL);
	ITERATE_MDDEV(mddev,tmp) {
		struct gendisk *disk = mddev->gendisk;
		if (!disk)
			continue;
		export_array(mddev);
		del_gendisk(disk);
		put_disk(disk);
		mddev->gendisk = NULL;
		mddev_put(mddev);
	}
}

module_init(md_init)
module_exit(md_exit)

static int get_ro(char *buffer, struct kernel_param *kp)
{
	return sprintf(buffer, "%d", start_readonly);
}
static int set_ro(const char *val, struct kernel_param *kp)
{
	char *e;
	int num = simple_strtoul(val, &e, 10);
	if (*val && (*e == '\0' || *e == '\n')) {
		start_readonly = num;
		return 0;;
	}
	return -EINVAL;
}

module_param_call(start_ro, set_ro, get_ro, NULL, 0600);

EXPORT_SYMBOL(register_md_personality);
EXPORT_SYMBOL(unregister_md_personality);
EXPORT_SYMBOL(md_error);
EXPORT_SYMBOL(md_done_sync);
EXPORT_SYMBOL(md_write_start);
EXPORT_SYMBOL(md_write_end);
EXPORT_SYMBOL(md_register_thread);
EXPORT_SYMBOL(md_unregister_thread);
EXPORT_SYMBOL(md_wakeup_thread);
EXPORT_SYMBOL(md_print_devices);
EXPORT_SYMBOL(md_check_recovery);
MODULE_LICENSE("GPL");
MODULE_ALIAS("md");
MODULE_ALIAS_BLOCKDEV_MAJOR(MD_MAJOR);
