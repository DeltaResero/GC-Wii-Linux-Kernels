/*
 * drivers/block/gcn-dvd/request.c
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

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include "request.h"

struct data_item
{
	struct list_head list;
	struct gc_dvd_command data;
};

// define array of data items
static struct data_item data[MAX_ITEMS];
static spinlock_t lock;
static struct list_head lfree;

void gc_dvd_request_init(void)
{
	int i;
  
	INIT_LIST_HEAD(&lfree);
	spin_lock_init(&lock);
  
	for (i=0;i<MAX_ITEMS;++i) {
		list_add_tail(&data[i].list,&lfree);
	}
}

int gc_dvd_request_get_data(struct gc_dvd_command **ppcmd)
{
	unsigned long flags;
	struct data_item *pdi;
	// get the first object and remove from the free list
	spin_lock_irqsave(&lock,flags);
	if (list_empty(&lfree)) {
		spin_unlock_irqrestore(&lock,flags);
		return -ENOMEM;
	}
	pdi = (struct data_item*)lfree.next;
	list_del(&pdi->list);
	spin_unlock_irqrestore(&lock,flags);
	// return it
	*ppcmd = &pdi->data;
	return 0;
}

void gc_dvd_request_release_data(struct gc_dvd_command *pcmd)
{
	unsigned long flags;
	struct data_item *pdi;
	/* return the item */
	spin_lock_irqsave(&lock,flags);
	pdi = (struct data_item*)((u8*)pcmd - sizeof(struct list_head));
	list_add_tail(&pdi->list,&lfree);
	spin_unlock_irqrestore(&lock,flags);
}
