/*
 * fs/gcdvdfs/namei.c
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

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/genhd.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include "fst.h"
#include "inode.h"

/*
  i is the parent directory
  dentry I must complete
  nid is lookup nameidata

  Complete by calling d_add(dentry,inode)
  
  If name is not found, inode must be set to null.

  Return an error only when the lookup fails due to hardware/other error

*/

struct gc_dvdfs_lookup_data
{
  struct gc_dvdfs_fst *fst;
  const char *pname;
  unsigned long ino;
};

static int gc_dvdfs_lookup_callback(struct gc_dvdfs_file_entry *pfe,void *param)
{
  struct gc_dvdfs_lookup_data *data = (struct gc_dvdfs_lookup_data*)param;

  if (gc_dvdfs_valid_file_entry(data->fst,pfe) == 0)
  {
    if (strcmp(FILENAME(data->fst,pfe),data->pname) == 0)
    {
      data->ino = PFE_TO_INO(data->fst,pfe);
      /* this will stop enumeration */
      return -1;
    }
  }
  return 0;
}

struct dentry *gc_dvdfs_lookup(struct inode *dir,struct dentry *dentry,struct nameidata *nid)
{
  struct inode *inode = NULL;
  struct gc_dvdfs_lookup_data data;
  unsigned int i;
  
  data.ino = 0;
  data.pname = dentry->d_name.name;
  /* handle special case of the root directory */
  if (dir->i_ino == ROOT_INO)
  {
    for (i=0;i<num_root_dir_entries;++i)
    {
      if (strcmp(data.pname,root_dir_entries[i].name) == 0)
      {
	data.ino = root_dir_entries[i].ino;
	break;
      }
    }
  }
  else if (dir->i_ino >= DATA_INO)
  {
    data.fst = (struct gc_dvdfs_fst*)dir->i_sb->s_fs_info;
    
    gc_dvdfs_enumerate(data.fst,INO_TO_PFE(data.fst,dir->i_ino),
		       gc_dvdfs_lookup_callback,&data);
  }

  /* now convert the inode number into the structure */
  if (data.ino)
  {
    if (!(inode = iget(dir->i_sb,data.ino)))
    {
      return ERR_PTR(-EACCES);
    }
  }  
  
  d_add(dentry,inode);
  return NULL;
}
