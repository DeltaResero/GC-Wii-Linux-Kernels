/*
 * fs/gcdvdfs/dir.h
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

#ifndef __dir__gc__
#define __dir__gc__

extern struct file_operations gc_dvdfs_dir_operations;
extern struct inode_operations gc_dvdfs_dir_inode_operations;

#endif
