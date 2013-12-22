/*
 * fs/gcdvdfs/fst.h
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
#ifndef __fst__h
#define __fst__h

#define FST_OFFSET 0x0424

#define FST_FILE      0
#define FST_DIRECTORY 1

#pragma pack(1)
struct gc_dvdfs_disc_header
{
  u32 game_code;
  u16 maker_code;
  u8 disc_id;
  u8 version;
  u8 streaming;
  u8 streamBufSize;
  u8 padding1[22];
  u8 game_name[992];
  u32 offset_dh_bin;
  u32 addr_debug_monitor;
  u8 padding2[24];
  u32 offset_bootfile;
  u32 offset_fst;
  u32 fst_size;
  u32 max_fst_size;
  u32 user_position;
  u32 user_length;
  u8 padding[7];
};

struct gc_dvdfs_file_entry
{
  u8 type;
  u8 offset_filename[3];
  union
  {
    struct
    {
      u32 offset;
      u32 length;               /* if root, number of entries */
    } file;
    struct
    {
      u32 offset_parent;
      u32 offset_next;
    } dir;
  };
};

#define APPLOADER_OFFSET 0x2440
struct gc_dvdfs_apploader
{
  u8 version[10];
  u8 padding[6];
  u32 entry_point;
  u32 size;
};

struct gc_dvdfs_dol_header
{
  u32 text_file_pos[7];
  u32 data_file_pos[11];
  u32 text_mem_pos[7];
  u32 data_mem_pos[11];
  u32 text_section_size[7];
  u32 data_section_size[11];
  u32 bss_mem_address;
  u32 bss_size;
  u32 entry_point;
};

#pragma pack()

struct gc_dvdfs_fst;

struct gc_dvdfs_directory_info
{
  struct gc_dvdfs_fst *fst;
  struct gc_dvdfs_file_entry *pfe;

  u32 total_files;
  u32 total_directories;
  u32 total_file_size;
};

struct gc_dvdfs_fst
{
  struct gc_dvdfs_file_entry *root;
  const char *str_table;
  u32 size;
  u32 str_table_size;
  u32 dol_length;
  u32 dol_offset;
  u32 total_files;
  u32 total_directories;
  u32 total_file_size;
  struct gc_dvdfs_apploader apploader;
  struct gc_dvdfs_dol_header dol_header;
};

#define MAX_ENTRIES(fst) (fst)->root->dir.offset_next
#define FILENAME_OFFSET(pfe) (((pfe)->offset_filename[0] << 16) | \
                              ((pfe)->offset_filename[1] << 8) | \
                              (pfe)->offset_filename[2])

#define FILENAME(fst,pfe) ((fst)->str_table + FILENAME_OFFSET(pfe))

int gc_dvdfs_valid_file_entry(struct gc_dvdfs_fst *fst,struct gc_dvdfs_file_entry *pfe);
int gc_dvdfs_enumerate(struct gc_dvdfs_fst *fst,struct gc_dvdfs_file_entry *pfe,int (*callback)(struct gc_dvdfs_file_entry *pfe,void *param),void *param);

int gc_dvdfs_get_directory_info(struct gc_dvdfs_directory_info *di);

#endif
