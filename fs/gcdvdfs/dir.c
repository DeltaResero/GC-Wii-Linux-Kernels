/*
 * fs/gcdvdfs/dir.c
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
#include <linux/fs.h>
#include "fst.h"
#include "dir.h"
#include "inode.h"
#include "namei.h"

struct readdir_data
{
  struct gc_dvdfs_fst *fst;
  struct file *filp;
  filldir_t filldir;
  void *dirent;
  u32 idx;
};

static int gc_dvdfs_readdir_callback(struct gc_dvdfs_file_entry *pfe,void *param)
{
  struct readdir_data *rdd = (struct readdir_data*)param;
  int len;
  const char *pname;
  
  if ((gc_dvdfs_valid_file_entry(rdd->fst,pfe) == 0) &&
      (rdd->idx == rdd->filp->f_pos))
  {
    /* add this item to the list now */
    pname = FILENAME(rdd->fst,pfe);
    len = strlen(pname);
    
    if (rdd->filldir(rdd->dirent,pname,len,rdd->filp->f_pos,
		     PFE_TO_INO(rdd->fst,pfe),
		     (pfe->type == FST_DIRECTORY) ? DT_DIR : DT_REG) < 0)
    {
      /* failure, so stop enumerating */
      return -1;
    }
    rdd->filp->f_pos++;
  }
  /* increment count */
  rdd->idx++;
  return 0;
}

static int gc_dvdfs_readdir(struct file *filp,void *dirent,filldir_t filldir)
{
  struct inode *inode = filp->f_dentry->d_inode;
  struct gc_dvdfs_fst *fst = (struct gc_dvdfs_fst*)inode->i_sb->s_fs_info;
  struct gc_dvdfs_file_entry *pfe;
  struct readdir_data rdd;
  int i;
  
  /* first entry is . */
  switch ((u32)filp->f_pos)
  {
  case 0:
    if (filldir(dirent,".",1,0,inode->i_ino,DT_DIR) < 0)
      return 0;
    filp->f_pos = 1;
    /* fall through */
  case 1:
    if (filldir(dirent,"..",2,1,parent_ino(filp->f_dentry),DT_DIR) < 0)
      return 0;
    filp->f_pos = 2;
    /* fall through */
  default:
    if (inode->i_ino == ROOT_INO)
    {
      for (i=filp->f_pos-2;i<num_root_dir_entries;++i)
      {
	if (filldir(dirent,root_dir_entries[i].name,
		    root_dir_entries[i].name_length,filp->f_pos,
		    root_dir_entries[i].ino,
		    root_dir_entries[i].filldir_type) < 0)
	{
	  return 0;
	}
	
	filp->f_pos++;
      }
    }
    else if (inode->i_ino >= DATA_INO)
    {
      /* use enumerate, the function will check for the FST_DIRECTORY flag */
      pfe = INO_TO_PFE(fst,inode->i_ino);
      
      rdd.fst = fst;
      rdd.filp = filp;
      rdd.filldir = filldir;
      rdd.dirent = dirent;
      rdd.idx = 2;
      
      gc_dvdfs_enumerate(fst,pfe,gc_dvdfs_readdir_callback,&rdd);
    }
  }
  return 0;
}

struct file_operations gc_dvdfs_dir_operations = 
{
  .read = generic_read_dir,
  .readdir = gc_dvdfs_readdir,
};

struct inode_operations gc_dvdfs_dir_inode_operations = 
{
  .lookup = gc_dvdfs_lookup,
};

