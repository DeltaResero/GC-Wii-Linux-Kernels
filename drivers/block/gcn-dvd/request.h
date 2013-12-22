/*
 * drivers/block/gcn-dvd/request.h
 *
 * Nintendo GameCube DVD driver 
 * Copyright (C) 2005 The GameCube Linux Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __request___
#define __request___

#define MAX_ITEMS 8

enum gc_dvd_interrupt_status { is_still_running,
			       is_transfer_complete,
			       is_error,
			       is_break,
			       is_cover_closed,
			       is_cover_opened};

struct gc_dvd_command
{
  struct list_head list;
  u32 flags;
  enum gc_dvd_interrupt_status int_status;
  u32 r_DI_DICMDBUF0;
  u32 r_DI_DICMDBUF1;
  u32 r_DI_DICMDBUF2;
  void *r_DI_DIMAR;
  u32 r_DI_DILENGTH;
  u32 r_DI_DICR;
  void *param;
  void (*completion_routine)(struct gc_dvd_command *cmd);
};

void gc_dvd_request_init(void);
int  gc_dvd_request_get_data(struct gc_dvd_command **ppcmd);
void gc_dvd_request_release_data(struct gc_dvd_command *pcmd);

#endif
