/*
 * fs/gcdvdfs/inode.h
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
#ifndef __inode_fst__
#define __inode_fst__

#define ROOT_INO 1
#define APPLOADER_INO 2
#define BOOTDOL_INO 3
#define DATA_INO 4

#define PFE_TO_INO(fst,pfe) (((unsigned long)(pfe) - (unsigned long)(fst)->root)/sizeof(struct gc_dvdfs_file_entry)+DATA_INO)
#define INO_TO_PFE(fst,ino) ((fst)->root + ((ino)-DATA_INO))

struct root_dir_entry
{
  const char *name;
  unsigned int name_length;
  unsigned int filldir_type;
  unsigned int ino;
};

extern struct root_dir_entry root_dir_entries[];
extern unsigned int num_root_dir_entries;

extern struct address_space_operations gc_dvdfs_addr_operations;

#endif
