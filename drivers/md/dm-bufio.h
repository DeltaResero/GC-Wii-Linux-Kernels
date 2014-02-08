/*
 * Copyright (C) 2009 Red Hat Czech, s.r.o.
 *
 * Mikulas Patocka <mpatocka@redhat.com>
 *
 * This file is released under the GPL.
 */

#ifndef DM_BUFIO_H
#define DM_BUFIO_H

struct dm_bufio_client;
struct dm_buffer;

void *dm_bufio_read(struct dm_bufio_client *c, sector_t block, struct dm_buffer **bp);
void *dm_bufio_new(struct dm_bufio_client *c, sector_t block, struct dm_buffer **bp);
void dm_bufio_release(struct dm_buffer *b);

void dm_bufio_mark_buffer_dirty(struct dm_buffer *b);
int dm_bufio_write_dirty_buffers(struct dm_bufio_client *c);
int dm_bufio_issue_flush(struct dm_bufio_client *c);

void dm_bufio_release_move(struct dm_buffer *b, sector_t new_block);

struct dm_bufio_client *dm_bufio_client_create(struct block_device *bdev, unsigned block_size, unsigned flags, __u64 cache_threshold, __u64 cache_limit);
void dm_bufio_client_destroy(struct dm_bufio_client *c);
void dm_bufio_drop_buffers(struct dm_bufio_client *c);

#endif
