/*
 * Copyright (C) 2026 Markus Fritsche <fritsche.markus@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#ifndef _REQUEST_POOL_H_
#define _REQUEST_POOL_H_

#include <stdbool.h>

/*
 * OUTPUT (bitstream-input) buffer pool, decoupled from caller-allocated
 * VA surfaces. Sizing is driven by codec pipeline depth (typically 4
 * for H.264), not by the consumer's surface count.
 *
 * The pool owns the V4L2 buffer indices and their mmap pointers. A
 * decode request "borrows" a slot at vaBeginPicture, fills it across
 * vaRenderPicture calls, queues it at vaEndPicture, and releases it
 * after VIDIOC_DQBUF returns.
 *
 * This replaces the per-surface OUTPUT-buffer ownership model in the
 * pre-refactor code, where object_surface.source_* fields permanently
 * held a single OUTPUT buffer per surface — incorrect because OUTPUT
 * buffers are request-time resources, not picture-time resources, and
 * because the per-surface loop in RequestCreateContext only ran when
 * surfaces_count > 0 (breaking ffmpeg's vaapi-copy num_render_targets=0
 * convention).
 */

struct request_pool_slot {
	unsigned int	index;		/* V4L2 buffer index in OUTPUT queue */
	void		*data;		/* mmap pointer for this slot */
	unsigned int	size;		/* mmap size in bytes */
	bool		busy;		/* true while borrowed for a request */
	int		request_fd;	/* per-slot media-request fd, allocated
					 * once at pool init, REINIT'd between
					 * uses. iter6: replaces iter4 close+
					 * alloc-per-frame to eliminate cross-
					 * slot fd-reuse race that broke Firefox
					 * MediaSource's multi-surface decode. */
};

struct request_pool {
	struct request_pool_slot	*slots;
	unsigned int			 count;
	unsigned int			 next;	/* round-robin acquire cursor */
	int				 media_fd;	/* iter7: kept for
							 * force_release re-alloc */
	bool				 initialized;

	/*
	 * iter#15: cached S_FMT params from request_pool_init, so
	 * request_pool_resize can re-S_FMT the OUTPUT queue with a new
	 * sizeimage override on a mid-session resolution upshift overrun
	 * without the caller having to re-thread these through six call
	 * sites. video_fd is also cached so the resize is fully
	 * self-contained — request_pool_resize takes only the pool and
	 * the new sizeimage hint.
	 */
	int				 video_fd;
	unsigned int			 output_type;
	unsigned int			 pixelformat;
	unsigned int			 picture_width;
	unsigned int			 picture_height;
};

/*
 * Allocate count OUTPUT buffers via VIDIOC_CREATE_BUFS, query and mmap
 * each, populate pool->slots[]. Caller must have already done
 * VIDIOC_S_FMT on the OUTPUT queue. The S_FMT params (pixelformat,
 * picture_width, picture_height) are stashed on the pool so that
 * request_pool_resize can re-issue S_FMT with the same dimensions but
 * a larger sizeimage hint. Returns 0 on success, -1 on failure.
 */
int request_pool_init(struct request_pool *pool, int video_fd, int media_fd,
		      unsigned int output_type, unsigned int count,
		      unsigned int pixelformat,
		      unsigned int picture_width,
		      unsigned int picture_height);

/*
 * iter#15: grow the OUTPUT pool's per-slot sizeimage in place.
 *
 * Issued from codec_store_buffer when an Annex-B start code / VP8
 * header pad / slice payload won't fit in the current
 * surface->source_size — i.e. the stream's per-frame bitstream budget
 * has outgrown the OUTPUT pool slot's mmap (typical cause: SPS-driven
 * resolution upshift mid-session).
 *
 * Steps:
 *   1. STREAMOFF the OUTPUT queue.
 *   2. munmap every slot, close every per-slot media-request fd.
 *   3. VIDIOC_REQBUFS(count=0) to release the V4L2 buffer indices.
 *   4. S_FMT with the cached pixelformat / picture_width /
 *      picture_height but a sizeimage hint of new_sizeimage_min.
 *   5. CREATE_BUFS with the original slot count.
 *   6. Per-slot: query buffer length, mmap, alloc fresh request_fd.
 *   7. STREAMON.
 *
 * Returns 0 on success, -1 on failure (caller falls back to
 * VA_STATUS_ERROR_ALLOCATION_FAILED — the libva consumer recreates
 * the surface at the new resolution).
 *
 * Pre-condition: NO pool slot is currently borrowed (busy=false on
 * every slot) AND no buffer is in-flight on the OUTPUT queue. The
 * inline-Sync-in-EndPicture pattern (RequestEndPicture calls
 * RequestSyncSurface before returning) makes this trivially true at
 * codec_store_buffer time for the only-supported single-context
 * single-render-surface flow: the in-flight surface's slot is the
 * sole borrowed slot, and the resize caller temporarily releases it
 * before calling here.
 */
int request_pool_resize(struct request_pool *pool,
			unsigned int new_sizeimage_min);

/*
 * Munmap all slots and free the slots array. Idempotent.
 */
void request_pool_destroy(struct request_pool *pool);

/*
 * Claim the next free slot (round-robin). Returns the slot's V4L2
 * buffer index on success (slot in pool->slots[] is determined by
 * the returned index), or -1 if all slots are busy.
 */
int request_pool_acquire(struct request_pool *pool);

/*
 * Mark the slot at pool->slots[i] free for reuse. Caller must pass the
 * V4L2 buffer index returned earlier from request_pool_acquire().
 */
void request_pool_release(struct request_pool *pool, unsigned int index);

/*
 * iter7: error-recovery release. Called from RequestSyncSurface error
 * paths when media_request_reinit or VIDIOC_DQBUF failed mid-cycle and
 * the slot's request_fd is now in an undefined state. REINITs the fd;
 * if REINIT fails (kernel-side request object too far gone), close
 * the fd and re-alloc a fresh one. If realloc also fails, the slot
 * is left busy=true (effectively dead, count decremented by 1) — pool
 * survives but with reduced capacity until driver terminate. Other
 * slots are unaffected.
 *
 * Caller passes the V4L2 buffer index from request_pool_acquire().
 */
void request_pool_force_release(struct request_pool *pool,
				unsigned int index);

/*
 * Look up the pool slot owning a given V4L2 buffer index. Returns
 * pointer to the slot on success, NULL if the index is out of range.
 * The returned pointer is valid until pool destruction; do not free.
 */
struct request_pool_slot *request_pool_slot(struct request_pool *pool,
					    unsigned int index);

#endif
