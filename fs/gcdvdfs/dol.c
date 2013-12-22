/*
 * fs/gcdvdfs/dol.c
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
#include "fst.h"
#include "dol.h"

void gc_dvdfs_fix_raw_dol_header(struct gc_dvdfs_dol_header *pdh)
{
#ifdef __LITTLE_ENDIAN
#define SWAP(a) (a) = be32_to_cpu(a)
  unsigned int i;
  
  for (i=0;i<7;++i)
  {
    SWAP(pdh->text_file_pos[i]);
    SWAP(pdh->text_mem_pos[i]);
    SWAP(pdh->text_section_size[i]);
  }
  
  for (i=0;i<11;++i)
  {
    SWAP(pdh->data_file_pos[i]);
    SWAP(pdh->data_mem_pos[i]);
    SWAP(pdh->data_section_size[i]);
  }
  
  SWAP(pdh->bss_mem_address);
  SWAP(pdh->bss_size);
  SWAP(pdh->entry_point);
#endif
}

u32 gc_dvdfs_get_dol_file_size(struct gc_dvdfs_dol_header *pdh)
{
  unsigned int tmp;
  unsigned int i;
  unsigned int max = 0;

  for (i=0;i<7;++i)
  {
    tmp = pdh->text_file_pos[i] + pdh->text_section_size[i];
    if (tmp > max)
      max = tmp;
  }
  for (i=0;i<11;++i)
  {
    tmp = pdh->data_file_pos[i] + pdh->data_section_size[i];
    if (tmp > max)
      max = tmp;
  }
  return max;
}
