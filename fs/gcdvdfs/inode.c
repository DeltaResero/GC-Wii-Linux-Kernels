/*
 * fs/gcdvdfs/inode.c
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
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/vfs.h>
#include <linux/highmem.h>
#include <linux/blkdev.h>

#include "fst.h"
#include "inode.h"
#include "dir.h"
#include "namei.h"

extern int gc_dvdfs_read_into_memory(struct super_block *s,u32 offset,
				     u32 size,unsigned char *data);

static int gc_dvdfs_readpage(struct file *file,struct page *page)
{
  struct inode *inode = (struct inode*)page->mapping->host;
  struct gc_dvdfs_fst *fst = (struct gc_dvdfs_fst*)inode->i_sb->s_fs_info;
  struct gc_dvdfs_file_entry *pfe;
  void *buf;
  loff_t offset;
  loff_t block_base;
  loff_t len;
  int ret = 0;
  
  switch (inode->i_ino)
  {
  case ROOT_INO:
    return -EIO;
  case APPLOADER_INO:
    block_base = APPLOADER_OFFSET;
    break;
  case BOOTDOL_INO:
    block_base = fst->dol_offset;
    break;
  default:
    pfe = INO_TO_PFE(fst,inode->i_ino);
    block_base = pfe->file.offset;
    break;
  }
  
  get_page(page);
  ClearPageUptodate(page);
  ClearPageError(page);
  /* do a lock here  */
  
  offset = (page->index << PAGE_CACHE_SHIFT);
  block_base += offset;
  /* now map into kernel space and fill the page */
  kmap(page);
  buf = page_address(page);
  if (offset < inode->i_size) 
  {
    len = min((loff_t)PAGE_SIZE,inode->i_size - offset);
    if (gc_dvdfs_read_into_memory(inode->i_sb,block_base,len,buf))
    {
      ret = -EIO;
    }
  }
  flush_dcache_page(page);
  kunmap(page);
  
  /* unlock */
  
  if (ret)
  {
    SetPageError(page);
  }
  else
  {
    SetPageUptodate(page);
  }
  page_cache_release(page);
  
  unlock_page(page);
  return ret;
}

struct address_space_operations gc_dvdfs_addr_operations =
{
  .readpage = gc_dvdfs_readpage,
};

struct root_dir_entry root_dir_entries[] = {
  { "apploader", 9, DT_REG, APPLOADER_INO },
  { "boot.dol" , 8, DT_REG, BOOTDOL_INO },
  { "data"     , 4, DT_DIR, DATA_INO }
};

unsigned int num_root_dir_entries = sizeof(root_dir_entries) / sizeof(root_dir_entries[0]);
