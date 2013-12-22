/*
 * fs/gcdvdfs/main.c
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
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/genhd.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include "fst.h"
#include "inode.h"
#include "dir.h"
#include "dol.h"

#define GC_DVD_SECTOR_SIZE 2048
#define GC_DVD_MAX_SECTORS 712880

#define FST_MAGIC 0x78B39EA1
#define DEBUG

int gc_dvdfs_read_into_memory(struct super_block *s,u32 offset,u32 size,unsigned char *data)
{
  struct buffer_head *bh;
  u32 sector;
  u32 sector_end;
  u32 byte_start;
  u32 byte_len;
  unsigned char *start;

  sector       = offset          >> s->s_blocksize_bits;
  sector_end   = (offset + size - 1) >> s->s_blocksize_bits;
  while (sector <= sector_end)
  {
    if (!(bh = sb_bread(s,sector)))
    {
      return -EINVAL;
    }
    /* store data into the memory buffer */
    byte_start = sector << s->s_blocksize_bits;
    byte_len   = bh->b_size;
    start      = bh->b_data;
    /* check if not in the middle */
    if (byte_start < offset)
    {
      start      = bh->b_data + (offset - byte_start);
      byte_len  -= (offset - byte_start);
    }
    /* check if we will overflow */
    if (byte_len > size)
    {
      byte_len = size;
    }
    /* now copy the data */
    memcpy(data,start,byte_len);
    data += byte_len;
    size -= byte_len;
    /* move the sector along */
    sector += (bh->b_size >> s->s_blocksize_bits);
    brelse(bh);
  }
#ifdef DEBUG
  if (size != 0)
  {
    printk(KERN_INFO "WARNING - read_into_memory still has a size value %u\n",
	   size);
  }
#endif
  return 0;
}

static void gc_dvdfs_read_inode(struct inode *i)
{
  struct gc_dvdfs_directory_info di;
  unsigned int size;
  
  di.fst = (struct gc_dvdfs_fst*)i->i_sb->s_fs_info;
  /* load the defaults for all inodes */
  i->i_mtime.tv_sec = i->i_atime.tv_sec = i->i_ctime.tv_sec = 0;
  i->i_mtime.tv_nsec = i->i_atime.tv_nsec = i->i_ctime.tv_nsec = 0;
  i->i_uid = 0;
  i->i_gid = 0;
  /* load based on type */
  if (i->i_ino == ROOT_INO)
  {
    i->i_fop = &gc_dvdfs_dir_operations;
    i->i_op = &gc_dvdfs_dir_inode_operations;
    i->i_mode = S_IFDIR | 0111;
    i->i_nlink = 3;
    size = 0;
    }
  else
  {
    if ((i->i_ino >= DATA_INO) &&
	(di.pfe = INO_TO_PFE(di.fst,i->i_ino)) &&
	(di.pfe->type == FST_DIRECTORY))
    {
      i->i_fop = &gc_dvdfs_dir_operations;
      i->i_op  = &gc_dvdfs_dir_inode_operations;
      i->i_mode  = S_IFDIR | 0111;
      /* compute the number of links */
      gc_dvdfs_get_directory_info(&di);
      i->i_nlink = 2 + di.total_directories;
      
      size = sizeof(struct gc_dvdfs_file_entry);
    }
    else
    {
      i->i_fop = &generic_ro_fops;
      i->i_data.a_ops = &gc_dvdfs_addr_operations;
      i->i_mode = S_IFREG;
      i->i_nlink = 1;
      switch (i->i_ino)
      {
      case APPLOADER_INO:
	size = di.fst->apploader.size;
	break;
      case BOOTDOL_INO:
	size = di.fst->dol_length;
	break;
      default:
	size = di.pfe->file.length;
	break;
      }
    }     
  }
  
  i->i_mode |= 0444;
  i_size_write(i,size);
}

static int gc_dvdfs_statfs(struct dentry * dentry,struct kstatfs *sfs)
{
  struct super_block *sb = dentry->d_sb;
  struct gc_dvdfs_fst *fst = (struct gc_dvdfs_fst*)sb->s_fs_info;

  sfs->f_type   = sb->s_magic;
  sfs->f_bsize  = sb->s_blocksize;
  sfs->f_blocks = fst->total_file_size >> sb->s_blocksize_bits;
  sfs->f_bfree  = 0;
  sfs->f_bavail = 0;
  sfs->f_files  = fst->total_files;
  sfs->f_ffree  = 0;
  /* sfs->f_fsid   = 0; */
  sfs->f_namelen = 256;
  sfs->f_frsize = 0;
  return 0;
}

static void gc_dvdfs_free_fst(struct gc_dvdfs_fst *fst)
{
  if (fst)
  {
    vfree(fst);
  }
}

static void gc_dvdfs_put_super(struct super_block *sb)
{
  /* ok free my FST */
  if (sb->s_fs_info)
  {
    gc_dvdfs_free_fst((struct gc_dvdfs_fst*)sb->s_fs_info);
    sb->s_fs_info = NULL;
  }
}

static struct super_operations gcdvdfs_ops = {
  .read_inode = gc_dvdfs_read_inode,
  .put_super  = gc_dvdfs_put_super,
  .statfs     = gc_dvdfs_statfs 
};

static int gc_dvdfs_validate_fst(struct gc_dvdfs_fst *fst)
{
  unsigned int entries;
  unsigned int i;

  /* make sure the FST is completely valid */
  if (fst->root->type != FST_DIRECTORY)
  {
    printk(KERN_ERR "gcdvdfs: Root entry is not a directory!\n");
    return -EINVAL;
  }
  
  entries = be32_to_cpu(fst->root->dir.offset_next);
  if (entries >= (fst->size / sizeof(struct gc_dvdfs_file_entry)))
  {
    printk(KERN_ERR "gcdvdfs: Too many entries, will overflow the FST!\n");
    return -EINVAL;
  }
  /* ok let's convert all the data to native format and compute total size*/
  for (i=0;i<entries;++i)
  {
#ifdef __LITTLE_ENDIAN
    fst->root[i].file.length = be32_to_cpu(fst->root[i].file.length);
    fst->root[i].file.offset = be32_to_cpu(fst->root[i].file.offset);
#endif
    if (fst->root[i].type == FST_FILE)
    {
      fst->total_files++;
      fst->total_file_size += fst->root[i].file.length;
    }
    else if (fst->root[i].type == FST_DIRECTORY)
    {
      fst->total_directories++;
    }
  }

  return 0;
}

static int gc_dvdfs_fill_super(struct super_block *s,void *data,int silent)
{
  struct buffer_head *bh;
  struct gc_dvdfs_disc_header *dh;
  struct gc_dvdfs_fst *fst;
  unsigned int fst_offset;
  unsigned int fst_size;
  unsigned int dol_offset;
  
  sb_min_blocksize(s,GC_DVD_SECTOR_SIZE);
  /* s->s_maxbytes = 4*GC_DVD_SECTOR_SIZE; */
  
  /* read the header and check results */
  if (!(bh = sb_bread(s,0)))
  {
    return -EINVAL;
  }
  else if (bh->b_size < sizeof(struct gc_dvdfs_disc_header))
  {
    brelse(bh);
    return -EINVAL;
  }

  /* read the FST into memory */
  dh = (struct gc_dvdfs_disc_header*)bh->b_data;
  fst_offset = be32_to_cpu(dh->offset_fst);
  fst_size   = be32_to_cpu(dh->fst_size);
  dol_offset = be32_to_cpu(dh->offset_bootfile);
  brelse(bh);
  
  /* now allocate the fst */
  if (!(fst = vmalloc(sizeof(struct gc_dvdfs_fst) + fst_size)))
  {
    return -ENOMEM;
  }
  fst->size = fst_size;
  fst->root = (struct gc_dvdfs_file_entry*)((unsigned int)fst + sizeof(struct gc_dvdfs_fst));
  /* now try to read the fst */
  if (gc_dvdfs_read_into_memory(s,fst_offset,fst_size,(unsigned char*)fst->root))
  {
    printk(KERN_ERR "gcdvdfs: Unable to read FST into memory\n");
    goto fst_error;
  }  
  /* now try to read the apploader */
  if (gc_dvdfs_read_into_memory(s,APPLOADER_OFFSET,sizeof(struct gc_dvdfs_apploader),(unsigned char*)&fst->apploader))
  {
    printk(KERN_ERR "gcdvdfs: Unable to read apploader into memory\n");
    goto fst_error;
  }
  /* fix up the apploader */
  fst->apploader.entry_point = be32_to_cpu(fst->apploader.entry_point);
  fst->apploader.size        = be32_to_cpu(fst->apploader.size);
  /* now try to read the dol header */
  if (gc_dvdfs_read_into_memory(s,dol_offset,sizeof(struct gc_dvdfs_dol_header),(unsigned char*)&fst->dol_header))
  {
    printk(KERN_ERR "gcdvdfs: Unable to read DOL Header\n");
    goto fst_error;
  }
  gc_dvdfs_fix_raw_dol_header(&fst->dol_header);
  
  fst->dol_offset = dol_offset;
  fst->dol_length = gc_dvdfs_get_dol_file_size(&fst->dol_header);
  /* compute the location of the string table */
  fst_offset = be32_to_cpu(fst->root->dir.offset_next) * sizeof(struct gc_dvdfs_file_entry);
  fst->str_table = (unsigned char*)fst->root + fst_offset;
  fst->str_table_size = fst->size - fst_offset;
  fst->total_files = 0;
  fst->total_directories = 0;
  fst->total_file_size = 0;
  /* now validate the fst */
  if (gc_dvdfs_validate_fst(fst))
  {
    goto fst_error;
  }

  /* store my FST in s->s_fs_info */
  s->s_flags   |= MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOATIME | MS_NODIRATIME;
  s->s_fs_info  = fst;
  s->s_magic    = FST_MAGIC;
  s->s_op       = &gcdvdfs_ops;
  if (!(s->s_root = d_alloc_root(iget(s,ROOT_INO))))
  {
    goto fst_error;
  }
  return 0;

 fst_error:
  gc_dvdfs_free_fst(fst);
  return -EINVAL;
}

static int gc_dvdfs_get_sb(struct file_system_type *fs_type,int flags,const char *dev_name,void *data,struct vfsmount *mnt)
{
  return get_sb_bdev(fs_type,flags,dev_name,data,gc_dvdfs_fill_super,mnt);
}

static struct file_system_type gcdvdfs_type = {
  .owner    = THIS_MODULE,
  .name     = "gcdvdfs",
  .get_sb   = gc_dvdfs_get_sb, 
  /*.kill_sb  = gc_dvdfs_kill_sb, */
  .kill_sb  = kill_block_super,
  .fs_flags = FS_REQUIRES_DEV 
};

static int __init gc_dvdfs_init(void)
{
  printk(KERN_INFO "Gamecube DVD filesystem: by Todd Jeffreys\n");
  return register_filesystem(&gcdvdfs_type);
}

static void __exit gc_dvdfs_exit(void)
{
  unregister_filesystem(&gcdvdfs_type);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Todd Jeffreys");
MODULE_DESCRIPTION("Gamecube DVD filesystem");
  
module_init(gc_dvdfs_init);
module_exit(gc_dvdfs_exit);
