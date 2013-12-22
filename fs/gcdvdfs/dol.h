/*
 * fs/gcdvdfs/dol.h
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
#ifndef __dol__h
#define __dol__h

void gc_dvdfs_fix_raw_dol_header(struct gc_dvdfs_dol_header *pdh);

u32 gc_dvdfs_get_dol_file_size(struct gc_dvdfs_dol_header *pdh);

#endif
