/*
 * Copyright (C) 2026 Markus Fritsche <fritsche.markus@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#include "request_pool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "media.h"
#include "utils.h"
#include "v4l2.h"

int request_pool_init(struct request_pool *pool, int video_fd, int media_fd,
		      unsigned int output_type, unsigned int count,
		      unsigned int pixelformat,
		      unsigned int picture_width,
		      unsigned int picture_height)
{
	unsigned int index_base;
	unsigned int length;
	unsigned int offset;
	unsigned int i;
	int rc;

	if (pool == NULL || count == 0)
		return -1;

	if (pool->initialized)
		return 0;

	pool->slots = calloc(count, sizeof(*pool->slots));
	if (pool->slots == NULL)
		return -1;

	pool->count = count;
	pool->next = 0;
	pool->media_fd = media_fd;	/* iter7: kept for force_release re-alloc */

	/*
	 * iter#15: cache the S_FMT params so request_pool_resize can
	 * re-issue S_FMT with a sizeimage hint override on overrun.
	 */
	pool->video_fd = video_fd;
	pool->output_type = output_type;
	pool->pixelformat = pixelformat;
	pool->picture_width = picture_width;
	pool->picture_height = picture_height;

	for (i = 0; i < count; i++)
		pool->slots[i].request_fd = -1;

	rc = v4l2_create_buffers(video_fd, output_type, count, &index_base);
	if (rc < 0)
		goto error;

	for (i = 0; i < count; i++) {
		pool->slots[i].index = index_base + i;
		pool->slots[i].busy = false;

		rc = v4l2_query_buffer(video_fd, output_type,
				       pool->slots[i].index,
				       &length, &offset, 1);
		if (rc < 0)
			goto error;

		pool->slots[i].data = mmap(NULL, length,
					   PROT_READ | PROT_WRITE,
					   MAP_SHARED, video_fd, offset);
		if (pool->slots[i].data == MAP_FAILED) {
			pool->slots[i].data = NULL;
			goto error;
		}

		pool->slots[i].size = length;

		/*
		 * iter6: each pool slot owns a permanent media-request fd,
		 * allocated once here and REINIT'd between uses in
		 * RequestSyncSurface. Replaces the iter4 close+alloc-per-
		 * frame model, whose lowest-free fd reuse was racing with
		 * the kernel's per-buffer state-machine teardown when the
		 * consumer rotated through multiple OUTPUT pool slots
		 * faster than the kernel cleanup drained (Firefox's
		 * MediaSource pattern). 1:1 slot-to-fd binding eliminates
		 * cross-slot fd reuse.
		 */
		pool->slots[i].request_fd = media_request_alloc(media_fd);
		if (pool->slots[i].request_fd < 0)
			goto error;
	}

	pool->initialized = true;
	return 0;

error:
	request_pool_destroy(pool);
	return -1;
}

int request_pool_resize(struct request_pool *pool,
			unsigned int new_sizeimage_min)
{
	unsigned int index_base;
	unsigned int length;
	unsigned int offset;
	unsigned int saved_count;
	unsigned int i;
	int rc;

	if (pool == NULL || !pool->initialized || pool->count == 0)
		return -1;

	/*
	 * Pre-condition guard: no slot may be borrowed when we tear the
	 * pool down. The caller in codec_store_buffer temporarily releases
	 * the current in-flight surface's slot before invoking us; the
	 * inline-Sync-in-EndPicture pattern guarantees no other slot is
	 * borrowed elsewhere in the driver. Bail loudly if anyone breaks
	 * that invariant rather than corrupting in-flight V4L2 state.
	 */
	for (i = 0; i < pool->count; i++) {
		if (pool->slots[i].busy) {
			request_log("request_pool_resize: slot %u still busy — "
				    "caller must release before resize\n", i);
			return -1;
		}
	}

	saved_count = pool->count;

	/* STREAMOFF the OUTPUT queue so REQBUFS(0) is accepted. */
	rc = v4l2_set_stream(pool->video_fd, pool->output_type, false);
	if (rc < 0)
		return -1;

	/*
	 * Tear down every slot: munmap, close per-slot request_fd. Slot
	 * fields are zeroed in place so failure halfway is recoverable.
	 */
	for (i = 0; i < pool->count; i++) {
		if (pool->slots[i].data != NULL && pool->slots[i].size > 0) {
			munmap(pool->slots[i].data, pool->slots[i].size);
			pool->slots[i].data = NULL;
			pool->slots[i].size = 0;
		}
		if (pool->slots[i].request_fd >= 0) {
			close(pool->slots[i].request_fd);
			pool->slots[i].request_fd = -1;
		}
	}

	/*
	 * Release the V4L2 OUTPUT buffer indices. REQBUFS(0) is the only
	 * way to ask the kernel to free buffers so CREATE_BUFS can re-
	 * allocate with a new per-buffer sizeimage.
	 */
	rc = v4l2_request_buffers(pool->video_fd, pool->output_type, 0);
	if (rc < 0)
		return -1;

	/*
	 * Re-issue S_FMT with the cached dimensions but a larger
	 * sizeimage. The kernel may round up further (driver-specific
	 * page / alignment rules); we accept whatever it returns and
	 * pick that up from per-slot v4l2_query_buffer below.
	 */
	rc = v4l2_set_format_sizeimage(pool->video_fd, pool->output_type,
				       pool->pixelformat,
				       pool->picture_width,
				       pool->picture_height,
				       new_sizeimage_min);
	if (rc < 0)
		return -1;

	rc = v4l2_create_buffers(pool->video_fd, pool->output_type,
				 saved_count, &index_base);
	if (rc < 0)
		return -1;

	for (i = 0; i < saved_count; i++) {
		pool->slots[i].index = index_base + i;
		pool->slots[i].busy = false;

		rc = v4l2_query_buffer(pool->video_fd, pool->output_type,
				       pool->slots[i].index,
				       &length, &offset, 1);
		if (rc < 0)
			return -1;

		pool->slots[i].data = mmap(NULL, length,
					   PROT_READ | PROT_WRITE,
					   MAP_SHARED, pool->video_fd, offset);
		if (pool->slots[i].data == MAP_FAILED) {
			pool->slots[i].data = NULL;
			return -1;
		}
		pool->slots[i].size = length;

		pool->slots[i].request_fd = media_request_alloc(pool->media_fd);
		if (pool->slots[i].request_fd < 0)
			return -1;
	}

	rc = v4l2_set_stream(pool->video_fd, pool->output_type, true);
	if (rc < 0)
		return -1;

	pool->next = 0;
	return 0;
}

void request_pool_destroy(struct request_pool *pool)
{
	unsigned int i;

	if (pool == NULL || pool->slots == NULL)
		return;

	for (i = 0; i < pool->count; i++) {
		if (pool->slots[i].request_fd >= 0)
			close(pool->slots[i].request_fd);
		if (pool->slots[i].data != NULL && pool->slots[i].size > 0)
			munmap(pool->slots[i].data, pool->slots[i].size);
	}

	free(pool->slots);
	pool->slots = NULL;
	pool->count = 0;
	pool->next = 0;
	pool->initialized = false;
}

int request_pool_acquire(struct request_pool *pool)
{
	unsigned int start;
	unsigned int i;

	if (pool == NULL || !pool->initialized || pool->count == 0)
		return -1;

	start = pool->next;
	for (i = 0; i < pool->count; i++) {
		unsigned int slot = (start + i) % pool->count;

		if (!pool->slots[slot].busy) {
			pool->slots[slot].busy = true;
			pool->next = (slot + 1) % pool->count;
			return (int)pool->slots[slot].index;
		}
	}

	/* All slots busy; caller must wait for an in-flight DQBUF. */
	return -1;
}

void request_pool_release(struct request_pool *pool, unsigned int index)
{
	unsigned int i;

	if (pool == NULL || pool->slots == NULL)
		return;

	for (i = 0; i < pool->count; i++) {
		if (pool->slots[i].index == index) {
			pool->slots[i].busy = false;
			return;
		}
	}
}

void request_pool_force_release(struct request_pool *pool, unsigned int index)
{
	struct request_pool_slot *slot;
	unsigned int i;

	if (pool == NULL || pool->slots == NULL)
		return;

	slot = NULL;
	for (i = 0; i < pool->count; i++) {
		if (pool->slots[i].index == index) {
			slot = &pool->slots[i];
			break;
		}
	}
	if (slot == NULL)
		return;

	/*
	 * Try to recover the kernel-side request object via REINIT first.
	 * REINIT is the cheap path: kernel resets the request in place,
	 * fd stays valid, slot can be reused immediately.
	 */
	if (slot->request_fd >= 0 && media_request_reinit(slot->request_fd) == 0) {
		slot->busy = false;
		return;
	}

	/*
	 * REINIT failed (or slot's fd was already invalid). Close the fd
	 * and try to allocate a fresh one. This costs an extra ioctl pair
	 * relative to the REINIT happy path but keeps the slot usable.
	 *
	 * NOTE: alloc may return the same lowest-free fd number that was
	 * just closed. That's fine here because (a) this is a rare error-
	 * recovery path, not the per-frame happy path, and (b) the slot's
	 * V4L2 buffer has already been DQBUF'd by this point (or is in an
	 * indeterminate state we can't recover from regardless), so the
	 * iter6 race condition (cross-slot fd-reuse against a kernel
	 * buffer in mid-cleanup) does not apply.
	 */
	if (slot->request_fd >= 0)
		close(slot->request_fd);
	slot->request_fd = media_request_alloc(pool->media_fd);
	if (slot->request_fd < 0) {
		/*
		 * Realloc failed. Slot is now permanently dead — leave
		 * busy=true so acquire skips it. Pool capacity is
		 * effectively reduced by 1 until pool destroy.
		 */
		return;
	}

	slot->busy = false;
}

struct request_pool_slot *request_pool_slot(struct request_pool *pool,
					    unsigned int index)
{
	unsigned int i;

	if (pool == NULL || pool->slots == NULL)
		return NULL;

	for (i = 0; i < pool->count; i++) {
		if (pool->slots[i].index == index)
			return &pool->slots[i];
	}

	return NULL;
}
