/*
 * fs/gcdvdfs/fst.c
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
#include "fst.h"

int gc_dvdfs_valid_file_entry(struct gc_dvdfs_fst *fst,struct gc_dvdfs_file_entry *pfe)
{
  unsigned int foffset = FILENAME_OFFSET(pfe);

  if (((pfe->type != FST_FILE) && (pfe->type != FST_DIRECTORY)) ||
      (foffset >= fst->str_table_size))
  {
    return -EINVAL;
  }
  return 0;
}

int gc_dvdfs_enumerate(struct gc_dvdfs_fst *fst,struct gc_dvdfs_file_entry *pfe,int (*callback)(struct gc_dvdfs_file_entry *pfe,void *param),void *param)
{
  /* get the filename */
  unsigned int i;
  unsigned int entries;
  int r;

  /* only enumerate directories */
  if (pfe->type != FST_DIRECTORY)
  {
    return -EINVAL;
  }

  entries = pfe->dir.offset_next;
  i=((unsigned int)pfe - (unsigned int)fst->root)/sizeof(struct gc_dvdfs_file_entry) + 1;
  /* check if out of bounds */
  if (i >= MAX_ENTRIES(fst) || entries > MAX_ENTRIES(fst))
  {
    return -EINVAL;
  }

  /* loop through the files */
  while (i < entries)
  {
    /* do the callback */
    if ((r=callback(fst->root + i,param)) < 0)
    {
      return r;
    }
    
    if (fst->root[i].type == FST_DIRECTORY)
    {
      if (fst->root[i].dir.offset_next <= i)
      {
	/* we're going backwards or looping, abort */
	return -EINVAL;
      }
      i = fst->root[i].dir.offset_next;
    }
    else
    {
      ++i;
    }
  }
  return 0;
}

static int gc_dvdfs_get_directory_info_callback(struct gc_dvdfs_file_entry *pfe,void *param)
{
  struct gc_dvdfs_directory_info *di = (struct gc_dvdfs_directory_info*)param;
  
  if (gc_dvdfs_valid_file_entry(di->fst,pfe) == 0)
  {
    if (pfe->type == FST_FILE)
    {
      di->total_files++;
      di->total_file_size += pfe->file.length;
    }
    else if (pfe->type == FST_DIRECTORY)
    {
      di->total_directories++;
    }
  }
  return 0;
}

int gc_dvdfs_get_directory_info(struct gc_dvdfs_directory_info *di)
{
  di->total_files = 0;
  di->total_directories = 0;
  di->total_file_size = 0;

  return gc_dvdfs_enumerate(di->fst,di->pfe,gc_dvdfs_get_directory_info_callback,di);
}
