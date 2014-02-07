/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/xattr.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/gfs2_ondisk.h>

#include "gfs2.h"
#include "incore.h"
#include "acl.h"
#include "xattr.h"
#include "glock.h"
#include "inode.h"
#include "meta_io.h"
#include "trans.h"
#include "util.h"

static int acl_get(struct gfs2_inode *ip, const char *name,
		   struct posix_acl **acl, struct gfs2_ea_location *el,
		   char **datap, unsigned int *lenp)
{
	char *data;
	unsigned int len;
	int error;

	el->el_bh = NULL;

	if (!ip->i_eattr)
		return 0;

	error = gfs2_ea_find(ip, GFS2_EATYPE_SYS, name, el);
	if (error)
		return error;
	if (!el->el_ea)
		return 0;
	if (!GFS2_EA_DATA_LEN(el->el_ea))
		goto out;

	len = GFS2_EA_DATA_LEN(el->el_ea);
	data = kmalloc(len, GFP_NOFS);
	error = -ENOMEM;
	if (!data)
		goto out;

	error = gfs2_ea_get_copy(ip, el, data, len);
	if (error < 0)
		goto out_kfree;
	error = 0;

	if (acl) {
		*acl = posix_acl_from_xattr(data, len);
		if (IS_ERR(*acl))
			error = PTR_ERR(*acl);
	}

out_kfree:
	if (error || !datap) {
		kfree(data);
	} else {
		*datap = data;
		*lenp = len;
	}
out:
	return error;
}

/**
 * gfs2_check_acl - Check an ACL to see if we're allowed to do something
 * @inode: the file we want to do something to
 * @mask: what we want to do
 *
 * Returns: errno
 */

int gfs2_check_acl(struct inode *inode, int mask)
{
	struct gfs2_ea_location el;
	struct posix_acl *acl = NULL;
	int error;

	error = acl_get(GFS2_I(inode), GFS2_POSIX_ACL_ACCESS, &acl, &el, NULL, NULL);
	brelse(el.el_bh);
	if (error)
		return error;

	if (acl) {
		error = posix_acl_permission(inode, acl, mask);
		posix_acl_release(acl);
		return error;
	}

	return -EAGAIN;
}

static int munge_mode(struct gfs2_inode *ip, mode_t mode)
{
	struct gfs2_sbd *sdp = GFS2_SB(&ip->i_inode);
	struct buffer_head *dibh;
	int error;

	error = gfs2_trans_begin(sdp, RES_DINODE, 0);
	if (error)
		return error;

	error = gfs2_meta_inode_buffer(ip, &dibh);
	if (!error) {
		gfs2_assert_withdraw(sdp,
				(ip->i_inode.i_mode & S_IFMT) == (mode & S_IFMT));
		ip->i_inode.i_mode = mode;
		gfs2_trans_add_bh(ip->i_gl, dibh, 1);
		gfs2_dinode_out(ip, dibh->b_data);
		brelse(dibh);
	}

	gfs2_trans_end(sdp);

	return 0;
}

int gfs2_acl_create(struct gfs2_inode *dip, struct gfs2_inode *ip)
{
	struct gfs2_ea_location el;
	struct gfs2_sbd *sdp = GFS2_SB(&dip->i_inode);
	struct posix_acl *acl = NULL, *clone;
	mode_t mode = ip->i_inode.i_mode;
	char *data = NULL;
	unsigned int len;
	int error;

	if (!sdp->sd_args.ar_posix_acl)
		return 0;
	if (S_ISLNK(ip->i_inode.i_mode))
		return 0;

	error = acl_get(dip, GFS2_POSIX_ACL_DEFAULT, &acl, &el, &data, &len);
	brelse(el.el_bh);
	if (error)
		return error;
	if (!acl) {
		mode &= ~current_umask();
		if (mode != ip->i_inode.i_mode)
			error = munge_mode(ip, mode);
		return error;
	}

	clone = posix_acl_clone(acl, GFP_NOFS);
	error = -ENOMEM;
	if (!clone)
		goto out;
	posix_acl_release(acl);
	acl = clone;

	if (S_ISDIR(ip->i_inode.i_mode)) {
		error = gfs2_xattr_set(&ip->i_inode, GFS2_EATYPE_SYS,
				       GFS2_POSIX_ACL_DEFAULT, data, len, 0);
		if (error)
			goto out;
	}

	error = posix_acl_create_masq(acl, &mode);
	if (error < 0)
		goto out;
	if (error == 0)
		goto munge;

	posix_acl_to_xattr(acl, data, len);
	error = gfs2_xattr_set(&ip->i_inode, GFS2_EATYPE_SYS,
			       GFS2_POSIX_ACL_ACCESS, data, len, 0);
	if (error)
		goto out;
munge:
	error = munge_mode(ip, mode);
out:
	posix_acl_release(acl);
	kfree(data);
	return error;
}

int gfs2_acl_chmod(struct gfs2_inode *ip, struct iattr *attr)
{
	struct posix_acl *acl = NULL, *clone;
	struct gfs2_ea_location el;
	char *data;
	unsigned int len;
	int error;

	error = acl_get(ip, GFS2_POSIX_ACL_ACCESS, &acl, &el, &data, &len);
	if (error)
		goto out_brelse;
	if (!acl)
		return gfs2_setattr_simple(ip, attr);

	clone = posix_acl_clone(acl, GFP_NOFS);
	error = -ENOMEM;
	if (!clone)
		goto out;
	posix_acl_release(acl);
	acl = clone;

	error = posix_acl_chmod_masq(acl, attr->ia_mode);
	if (!error) {
		posix_acl_to_xattr(acl, data, len);
		error = gfs2_ea_acl_chmod(ip, &el, attr, data);
	}

out:
	posix_acl_release(acl);
	kfree(data);
out_brelse:
	brelse(el.el_bh);
	return error;
}

static int gfs2_acl_type(const char *name)
{
	if (strcmp(name, GFS2_POSIX_ACL_ACCESS) == 0)
		return ACL_TYPE_ACCESS;
	if (strcmp(name, GFS2_POSIX_ACL_DEFAULT) == 0)
		return ACL_TYPE_DEFAULT;
	return -EINVAL;
}

static int gfs2_xattr_system_get(struct inode *inode, const char *name,
				 void *buffer, size_t size)
{
	int type;

	type = gfs2_acl_type(name);
	if (type < 0)
		return type;

	return gfs2_xattr_get(inode, GFS2_EATYPE_SYS, name, buffer, size);
}

static int gfs2_set_mode(struct inode *inode, mode_t mode)
{
	int error = 0;

	if (mode != inode->i_mode) {
		struct iattr iattr;

		iattr.ia_valid = ATTR_MODE;
		iattr.ia_mode = mode;

		error = gfs2_setattr_simple(GFS2_I(inode), &iattr);
	}

	return error;
}

static int gfs2_xattr_system_set(struct inode *inode, const char *name,
				 const void *value, size_t size, int flags)
{
	struct gfs2_sbd *sdp = GFS2_SB(inode);
	struct posix_acl *acl = NULL;
	int error = 0, type;

	if (!sdp->sd_args.ar_posix_acl)
		return -EOPNOTSUPP;

	type = gfs2_acl_type(name);
	if (type < 0)
		return type;
	if (flags & XATTR_CREATE)
		return -EINVAL;
	if (type == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode))
		return value ? -EACCES : 0;
	if ((current_fsuid() != inode->i_uid) && !capable(CAP_FOWNER))
		return -EPERM;
	if (S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;

	if (!value)
		goto set_acl;

	acl = posix_acl_from_xattr(value, size);
	if (!acl) {
		/*
		 * acl_set_file(3) may request that we set default ACLs with
		 * zero length -- defend (gracefully) against that here.
		 */
		goto out;
	}
	if (IS_ERR(acl)) {
		error = PTR_ERR(acl);
		goto out;
	}

	error = posix_acl_valid(acl);
	if (error)
		goto out_release;

	error = -EINVAL;
	if (acl->a_count > GFS2_ACL_MAX_ENTRIES)
		goto out_release;

	if (type == ACL_TYPE_ACCESS) {
		mode_t mode = inode->i_mode;
		error = posix_acl_equiv_mode(acl, &mode);

		if (error <= 0) {
			posix_acl_release(acl);
			acl = NULL;

			if (error < 0)
				return error;
		}

		error = gfs2_set_mode(inode, mode);
		if (error)
			goto out_release;
	}

set_acl:
	error = gfs2_xattr_set(inode, GFS2_EATYPE_SYS, name, value, size, 0);
out_release:
	posix_acl_release(acl);
out:
	return error;
}

struct xattr_handler gfs2_xattr_system_handler = {
	.prefix = XATTR_SYSTEM_PREFIX,
	.get    = gfs2_xattr_system_get,
	.set    = gfs2_xattr_system_set,
};

