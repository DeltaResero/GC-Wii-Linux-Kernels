/*
 *  linux/fs/ioctl.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/syscalls.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>

#include <asm/ioctls.h>

/* So that the fiemap access checks can't overflow on 32 bit machines. */
#define FIEMAP_MAX_EXTENTS	(UINT_MAX / sizeof(struct fiemap_extent))

/**
 * vfs_ioctl - call filesystem specific ioctl methods
 * @filp:	open file to invoke ioctl method on
 * @cmd:	ioctl command to execute
 * @arg:	command-specific argument for ioctl
 *
 * Invokes filesystem specific ->unlocked_ioctl, if one exists; otherwise
 * invokes filesystem specific ->ioctl method.  If neither method exists,
 * returns -ENOTTY.
 *
 * Returns 0 on success, -errno on error.
 */
static long vfs_ioctl(struct file *filp, unsigned int cmd,
		      unsigned long arg)
{
	int error = -ENOTTY;

	if (!filp->f_op)
		goto out;

	if (filp->f_op->unlocked_ioctl) {
		error = filp->f_op->unlocked_ioctl(filp, cmd, arg);
		if (error == -ENOIOCTLCMD)
			error = -EINVAL;
		goto out;
	} else if (filp->f_op->ioctl) {
		lock_kernel();
		error = filp->f_op->ioctl(filp->f_path.dentry->d_inode,
					  filp, cmd, arg);
		unlock_kernel();
	}

 out:
	return error;
}

static int ioctl_fibmap(struct file *filp, int __user *p)
{
	struct address_space *mapping = filp->f_mapping;
	int res, block;

	/* do we support this mess? */
	if (!mapping->a_ops->bmap)
		return -EINVAL;
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	res = get_user(block, p);
	if (res)
		return res;
	lock_kernel();
	res = mapping->a_ops->bmap(mapping, block);
	unlock_kernel();
	return put_user(res, p);
}

/**
 * fiemap_fill_next_extent - Fiemap helper function
 * @fieinfo:	Fiemap context passed into ->fiemap
 * @logical:	Extent logical start offset, in bytes
 * @phys:	Extent physical start offset, in bytes
 * @len:	Extent length, in bytes
 * @flags:	FIEMAP_EXTENT flags that describe this extent
 *
 * Called from file system ->fiemap callback. Will populate extent
 * info as passed in via arguments and copy to user memory. On
 * success, extent count on fieinfo is incremented.
 *
 * Returns 0 on success, -errno on error, 1 if this was the last
 * extent that will fit in user array.
 */
#define SET_UNKNOWN_FLAGS	(FIEMAP_EXTENT_DELALLOC)
#define SET_NO_UNMOUNTED_IO_FLAGS	(FIEMAP_EXTENT_DATA_ENCRYPTED)
#define SET_NOT_ALIGNED_FLAGS	(FIEMAP_EXTENT_DATA_TAIL|FIEMAP_EXTENT_DATA_INLINE)
int fiemap_fill_next_extent(struct fiemap_extent_info *fieinfo, u64 logical,
			    u64 phys, u64 len, u32 flags)
{
	struct fiemap_extent extent;
	struct fiemap_extent *dest = fieinfo->fi_extents_start;

	/* only count the extents */
	if (fieinfo->fi_extents_max == 0) {
		fieinfo->fi_extents_mapped++;
		return (flags & FIEMAP_EXTENT_LAST) ? 1 : 0;
	}

	if (fieinfo->fi_extents_mapped >= fieinfo->fi_extents_max)
		return 1;

	if (flags & SET_UNKNOWN_FLAGS)
		flags |= FIEMAP_EXTENT_UNKNOWN;
	if (flags & SET_NO_UNMOUNTED_IO_FLAGS)
		flags |= FIEMAP_EXTENT_ENCODED;
	if (flags & SET_NOT_ALIGNED_FLAGS)
		flags |= FIEMAP_EXTENT_NOT_ALIGNED;

	memset(&extent, 0, sizeof(extent));
	extent.fe_logical = logical;
	extent.fe_physical = phys;
	extent.fe_length = len;
	extent.fe_flags = flags;

	dest += fieinfo->fi_extents_mapped;
	if (copy_to_user(dest, &extent, sizeof(extent)))
		return -EFAULT;

	fieinfo->fi_extents_mapped++;
	if (fieinfo->fi_extents_mapped == fieinfo->fi_extents_max)
		return 1;
	return (flags & FIEMAP_EXTENT_LAST) ? 1 : 0;
}
EXPORT_SYMBOL(fiemap_fill_next_extent);

/**
 * fiemap_check_flags - check validity of requested flags for fiemap
 * @fieinfo:	Fiemap context passed into ->fiemap
 * @fs_flags:	Set of fiemap flags that the file system understands
 *
 * Called from file system ->fiemap callback. This will compute the
 * intersection of valid fiemap flags and those that the fs supports. That
 * value is then compared against the user supplied flags. In case of bad user
 * flags, the invalid values will be written into the fieinfo structure, and
 * -EBADR is returned, which tells ioctl_fiemap() to return those values to
 * userspace. For this reason, a return code of -EBADR should be preserved.
 *
 * Returns 0 on success, -EBADR on bad flags.
 */
int fiemap_check_flags(struct fiemap_extent_info *fieinfo, u32 fs_flags)
{
	u32 incompat_flags;

	incompat_flags = fieinfo->fi_flags & ~(FIEMAP_FLAGS_COMPAT & fs_flags);
	if (incompat_flags) {
		fieinfo->fi_flags = incompat_flags;
		return -EBADR;
	}
	return 0;
}
EXPORT_SYMBOL(fiemap_check_flags);

static int fiemap_check_ranges(struct super_block *sb,
			       u64 start, u64 len, u64 *new_len)
{
	*new_len = len;

	if (len == 0)
		return -EINVAL;

	if (start > sb->s_maxbytes)
		return -EFBIG;

	/*
	 * Shrink request scope to what the fs can actually handle.
	 */
	if ((len > sb->s_maxbytes) ||
	    (sb->s_maxbytes - len) < start)
		*new_len = sb->s_maxbytes - start;

	return 0;
}

static int ioctl_fiemap(struct file *filp, unsigned long arg)
{
	struct fiemap fiemap;
	struct fiemap_extent_info fieinfo = { 0, };
	struct inode *inode = filp->f_path.dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	u64 len;
	int error;

	if (!inode->i_op->fiemap)
		return -EOPNOTSUPP;

	if (copy_from_user(&fiemap, (struct fiemap __user *)arg,
			   sizeof(struct fiemap)))
		return -EFAULT;

	if (fiemap.fm_extent_count > FIEMAP_MAX_EXTENTS)
		return -EINVAL;

	error = fiemap_check_ranges(sb, fiemap.fm_start, fiemap.fm_length,
				    &len);
	if (error)
		return error;

	fieinfo.fi_flags = fiemap.fm_flags;
	fieinfo.fi_extents_max = fiemap.fm_extent_count;
	fieinfo.fi_extents_start = (struct fiemap_extent *)(arg + sizeof(fiemap));

	if (fiemap.fm_extent_count != 0 &&
	    !access_ok(VERIFY_WRITE, fieinfo.fi_extents_start,
		       fieinfo.fi_extents_max * sizeof(struct fiemap_extent)))
		return -EFAULT;

	if (fieinfo.fi_flags & FIEMAP_FLAG_SYNC)
		filemap_write_and_wait(inode->i_mapping);

	error = inode->i_op->fiemap(inode, &fieinfo, fiemap.fm_start, len);
	fiemap.fm_flags = fieinfo.fi_flags;
	fiemap.fm_mapped_extents = fieinfo.fi_extents_mapped;
	if (copy_to_user((char *)arg, &fiemap, sizeof(fiemap)))
		error = -EFAULT;

	return error;
}

#ifdef CONFIG_BLOCK

#define blk_to_logical(inode, blk) (blk << (inode)->i_blkbits)
#define logical_to_blk(inode, offset) (offset >> (inode)->i_blkbits);

/**
 * __generic_block_fiemap - FIEMAP for block based inodes (no locking)
 * @inode - the inode to map
 * @arg - the pointer to userspace where we copy everything to
 * @get_block - the fs's get_block function
 *
 * This does FIEMAP for block based inodes.  Basically it will just loop
 * through get_block until we hit the number of extents we want to map, or we
 * go past the end of the file and hit a hole.
 *
 * If it is possible to have data blocks beyond a hole past @inode->i_size, then
 * please do not use this function, it will stop at the first unmapped block
 * beyond i_size.
 *
 * If you use this function directly, you need to do your own locking. Use
 * generic_block_fiemap if you want the locking done for you.
 */

int __generic_block_fiemap(struct inode *inode,
			   struct fiemap_extent_info *fieinfo, u64 start,
			   u64 len, get_block_t *get_block)
{
	struct buffer_head tmp;
	unsigned int start_blk;
	long long length = 0, map_len = 0;
	u64 logical = 0, phys = 0, size = 0;
	u32 flags = FIEMAP_EXTENT_MERGED;
	int ret = 0, past_eof = 0, whole_file = 0;

	if ((ret = fiemap_check_flags(fieinfo, FIEMAP_FLAG_SYNC)))
		return ret;

	start_blk = logical_to_blk(inode, start);

	length = (long long)min_t(u64, len, i_size_read(inode));
	if (length < len)
		whole_file = 1;

	map_len = length;

	do {
		/*
		 * we set b_size to the total size we want so it will map as
		 * many contiguous blocks as possible at once
		 */
		memset(&tmp, 0, sizeof(struct buffer_head));
		tmp.b_size = map_len;

		ret = get_block(inode, start_blk, &tmp, 0);
		if (ret)
			break;

		/* HOLE */
		if (!buffer_mapped(&tmp)) {
			length -= blk_to_logical(inode, 1);
			start_blk++;

			/*
			 * we want to handle the case where there is an
			 * allocated block at the front of the file, and then
			 * nothing but holes up to the end of the file properly,
			 * to make sure that extent at the front gets properly
			 * marked with FIEMAP_EXTENT_LAST
			 */
			if (!past_eof &&
			    blk_to_logical(inode, start_blk) >=
			    blk_to_logical(inode, 0)+i_size_read(inode))
				past_eof = 1;

			/*
			 * first hole after going past the EOF, this is our
			 * last extent
			 */
			if (past_eof && size) {
				flags = FIEMAP_EXTENT_MERGED|FIEMAP_EXTENT_LAST;
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size,
							      flags);
				break;
			}

			/* if we have holes up to/past EOF then we're done */
			if (length <= 0 || past_eof)
				break;
		} else {
			/*
			 * we have gone over the length of what we wanted to
			 * map, and it wasn't the entire file, so add the extent
			 * we got last time and exit.
			 *
			 * This is for the case where say we want to map all the
			 * way up to the second to the last block in a file, but
			 * the last block is a hole, making the second to last
			 * block FIEMAP_EXTENT_LAST.  In this case we want to
			 * see if there is a hole after the second to last block
			 * so we can mark it properly.  If we found data after
			 * we exceeded the length we were requesting, then we
			 * are good to go, just add the extent to the fieinfo
			 * and break
			 */
			if (length <= 0 && !whole_file) {
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size,
							      flags);
				break;
			}

			/*
			 * if size != 0 then we know we already have an extent
			 * to add, so add it.
			 */
			if (size) {
				ret = fiemap_fill_next_extent(fieinfo, logical,
							      phys, size,
							      flags);
				if (ret)
					break;
			}

			logical = blk_to_logical(inode, start_blk);
			phys = blk_to_logical(inode, tmp.b_blocknr);
			size = tmp.b_size;
			flags = FIEMAP_EXTENT_MERGED;

			length -= tmp.b_size;
			start_blk += logical_to_blk(inode, size);

			/*
			 * If we are past the EOF, then we need to make sure as
			 * soon as we find a hole that the last extent we found
			 * is marked with FIEMAP_EXTENT_LAST
			 */
			if (!past_eof &&
			    logical+size >=
			    blk_to_logical(inode, 0)+i_size_read(inode))
				past_eof = 1;
		}
		cond_resched();
	} while (1);

	/* if ret is 1 then we just hit the end of the extent array */
	if (ret == 1)
		ret = 0;

	return ret;
}
EXPORT_SYMBOL(__generic_block_fiemap);

/**
 * generic_block_fiemap - FIEMAP for block based inodes
 * @inode: The inode to map
 * @fieinfo: The mapping information
 * @start: The initial block to map
 * @len: The length of the extect to attempt to map
 * @get_block: The block mapping function for the fs
 *
 * Calls __generic_block_fiemap to map the inode, after taking
 * the inode's mutex lock.
 */

int generic_block_fiemap(struct inode *inode,
			 struct fiemap_extent_info *fieinfo, u64 start,
			 u64 len, get_block_t *get_block)
{
	int ret;
	mutex_lock(&inode->i_mutex);
	ret = __generic_block_fiemap(inode, fieinfo, start, len, get_block);
	mutex_unlock(&inode->i_mutex);
	return ret;
}
EXPORT_SYMBOL(generic_block_fiemap);

#endif  /*  CONFIG_BLOCK  */

static int file_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct inode *inode = filp->f_path.dentry->d_inode;
	int __user *p = (int __user *)arg;

	switch (cmd) {
	case FIBMAP:
		return ioctl_fibmap(filp, p);
	case FS_IOC_FIEMAP:
		return ioctl_fiemap(filp, arg);
	case FIGETBSZ:
		return put_user(inode->i_sb->s_blocksize, p);
	case FIONREAD:
		return put_user(i_size_read(inode) - filp->f_pos, p);
	}

	return vfs_ioctl(filp, cmd, arg);
}

static int ioctl_fionbio(struct file *filp, int __user *argp)
{
	unsigned int flag;
	int on, error;

	error = get_user(on, argp);
	if (error)
		return error;
	flag = O_NONBLOCK;
#ifdef __sparc__
	/* SunOS compatibility item. */
	if (O_NONBLOCK != O_NDELAY)
		flag |= O_NDELAY;
#endif
	if (on)
		filp->f_flags |= flag;
	else
		filp->f_flags &= ~flag;
	return error;
}

static int ioctl_fioasync(unsigned int fd, struct file *filp,
			  int __user *argp)
{
	unsigned int flag;
	int on, error;

	error = get_user(on, argp);
	if (error)
		return error;
	flag = on ? FASYNC : 0;

	/* Did FASYNC state change ? */
	if ((flag ^ filp->f_flags) & FASYNC) {
		if (filp->f_op && filp->f_op->fasync)
			error = filp->f_op->fasync(fd, filp, on);
		else
			error = -ENOTTY;
	}
	if (error)
		return error;

	if (on)
		filp->f_flags |= FASYNC;
	else
		filp->f_flags &= ~FASYNC;
	return error;
}

static int ioctl_fsfreeze(struct file *filp)
{
	struct super_block *sb = filp->f_path.dentry->d_inode->i_sb;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/* If filesystem doesn't support freeze feature, return. */
	if (sb->s_op->freeze_fs == NULL)
		return -EOPNOTSUPP;

	/* If a blockdevice-backed filesystem isn't specified, return. */
	if (sb->s_bdev == NULL)
		return -EINVAL;

	/* Freeze */
	sb = freeze_bdev(sb->s_bdev);
	if (IS_ERR(sb))
		return PTR_ERR(sb);
	return 0;
}

static int ioctl_fsthaw(struct file *filp)
{
	struct super_block *sb = filp->f_path.dentry->d_inode->i_sb;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/* If a blockdevice-backed filesystem isn't specified, return EINVAL. */
	if (sb->s_bdev == NULL)
		return -EINVAL;

	/* Thaw */
	return thaw_bdev(sb->s_bdev, sb);
}

/*
 * When you add any new common ioctls to the switches above and below
 * please update compat_sys_ioctl() too.
 *
 * do_vfs_ioctl() is not for drivers and not intended to be EXPORT_SYMBOL()'d.
 * It's just a simple helper for sys_ioctl and compat_sys_ioctl.
 */
int do_vfs_ioctl(struct file *filp, unsigned int fd, unsigned int cmd,
	     unsigned long arg)
{
	int error = 0;
	int __user *argp = (int __user *)arg;

	switch (cmd) {
	case FIOCLEX:
		set_close_on_exec(fd, 1);
		break;

	case FIONCLEX:
		set_close_on_exec(fd, 0);
		break;

	case FIONBIO:
		/* BKL needed to avoid races tweaking f_flags */
		lock_kernel();
		error = ioctl_fionbio(filp, argp);
		unlock_kernel();
		break;

	case FIOASYNC:
		/* BKL needed to avoid races tweaking f_flags */
		lock_kernel();
		error = ioctl_fioasync(fd, filp, argp);
		unlock_kernel();
		break;

	case FIOQSIZE:
		if (S_ISDIR(filp->f_path.dentry->d_inode->i_mode) ||
		    S_ISREG(filp->f_path.dentry->d_inode->i_mode) ||
		    S_ISLNK(filp->f_path.dentry->d_inode->i_mode)) {
			loff_t res =
				inode_get_bytes(filp->f_path.dentry->d_inode);
			error = copy_to_user((loff_t __user *)arg, &res,
					     sizeof(res)) ? -EFAULT : 0;
		} else
			error = -ENOTTY;
		break;

	case FIFREEZE:
		error = ioctl_fsfreeze(filp);
		break;

	case FITHAW:
		error = ioctl_fsthaw(filp);
		break;

	default:
		if (S_ISREG(filp->f_path.dentry->d_inode->i_mode))
			error = file_ioctl(filp, cmd, arg);
		else
			error = vfs_ioctl(filp, cmd, arg);
		break;
	}
	return error;
}

SYSCALL_DEFINE3(ioctl, unsigned int, fd, unsigned int, cmd, unsigned long, arg)
{
	struct file *filp;
	int error = -EBADF;
	int fput_needed;

	filp = fget_light(fd, &fput_needed);
	if (!filp)
		goto out;

	error = security_file_ioctl(filp, cmd, arg);
	if (error)
		goto out_fput;

	error = do_vfs_ioctl(filp, fd, cmd, arg);
 out_fput:
	fput_light(filp, fput_needed);
 out:
	return error;
}
