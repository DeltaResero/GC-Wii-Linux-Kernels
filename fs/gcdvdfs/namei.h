/*
 * fs/gcdvdfs/namei.h
 *
 * Nintendo GameCube Filesystem driver 
 * Copyright (C) 2006 The GameCube Linux Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */
#ifndef __gc_lookup__
#define __gc_lookup__

struct dentry *gc_dvdfs_lookup(struct inode *dir,struct dentry *dentry,struct nameidata *nid);

#endif
