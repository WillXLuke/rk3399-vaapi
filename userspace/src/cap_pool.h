/*
 * Iteration 2 Fix 3: decoupled CAPTURE buffer pool with LRU recycling.
 *
 * Background — the bug this fixes:
 *
 *   Pre-iteration-2, each VAAPI surface was permanently 1:1 bound to a
 *   V4L2 CAPTURE buffer index at vaCreateSurfaces2 time. Each decode
 *   cycle re-QBUF'd that same physical buffer for the same surface ID.
 *   When mpv reused a surface for a new decode while the compositor
 *   still held an EXPBUF'd dma_buf fd to the prior frame's content,
 *   the kernel wrote new decode output into the SAME physical memory
 *   the compositor was reading from — visible as stutter / "back and
 *   forth" frame swap during mpv --hwdec=vaapi --vo=gpu playback.
 *
 *   V4L2 does not enforce the constraint (it lets QBUF re-queue a
 *   buffer regardless of dma_buf refcount on EXPBUF'd fds). userspace
 *   must coordinate.
 *
 * Architecture (Sonnet Phase 5 review for iter2):
 *
 *   Pool of N CAPTURE buffers (N >= max(surfaces_count, MIN_CAP_POOL)).
 *   Each slot has a state in {FREE, IN_DECODE, DECODED, EXPORTED}.
 *   Surfaces are no longer permanently bound; each vaBeginPicture
 *   acquires a FREE slot, binds it to the current decode, transitions
 *   it through IN_DECODE → DECODED → optionally EXPORTED.
 *
 *   The DECODED state captures the window between SyncSurface DQBUF
 *   and either ExportSurfaceHandle (DMA-BUF path) or DeriveImage
 *   (vaapi-copy path). LRU recycling considers ONLY FREE slots, so
 *   DECODED slots cannot be claimed by a concurrent decode while
 *   the consumer is still using the bound surface's content.
 *
 *   Concurrency: a pthread_mutex_t protects pool state. VAAPI is
 *   re-entrant for multi-threaded consumers (mpv may BeginPicture/
 *   SyncSurface from one thread and ExportSurfaceHandle from
 *   another).
 *
 * Limitations (deferred to iteration 3+):
 *
 *   - Option-A statistical mitigation, not a correct fix. The race
 *     window narrows from "constant" to "only when pool is exhausted
 *     and force-recycle of oldest EXPORTED slot fires." For typical
 *     mpv 16-surface playback with MIN_CAP_POOL=24, this never fires
 *     in practice (Sonnet review iter2 question 3). For pathological
 *     workloads (paused-with-video-still-visible, multi-stream),
 *     race windows still possible. Iteration 3 may revisit with
 *     V4L2_MEMORY_DMABUF + userspace allocation.
 *
 *   - LRU "force-recycle" still has the race in the worst case.
 *     Closing OUR EXPBUF fd does not close the consumer's dup — the
 *     consumer's fd keeps the dma_buf alive but the V4L2 layer will
 *     happily write new data into the underlying physical memory on
 *     re-QBUF. There is no public V4L2 API to query dma_buf refcount.
 *
 *   - Multi-context concurrent use (two libva contexts open
 *     simultaneously, e.g. Firefox playing two videos in different
 *     tabs through separate RDD instances): not addressed. Each
 *     context gets its own pool, but there's only one V4L2 device.
 */

#ifndef _CAP_POOL_H_
#define _CAP_POOL_H_

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <linux/videodev2.h>	/* for VIDEO_MAX_PLANES */

#define MIN_CAP_POOL 24

enum cap_slot_state {
	CAP_SLOT_FREE = 0,	/* available for a new decode acquisition */
	CAP_SLOT_IN_DECODE,	/* QBUF'd to V4L2, kernel owns */
	CAP_SLOT_DECODED,	/* DQBUF'd, valid pixel content; mapped by surface */
	CAP_SLOT_EXPORTED,	/* EXPBUF'd; consumer holds a dma_buf fd */
};

struct cap_pool_slot {
	unsigned int		v4l2_index;			/* V4L2 buffer index */
	void			*map[VIDEO_MAX_PLANES];		/* mmap pointers */
	unsigned int		map_lengths[VIDEO_MAX_PLANES];
	unsigned int		map_offsets[VIDEO_MAX_PLANES];
	unsigned int		buffers_count;			/* V4L2 buffers per logical NV12 (1 for single-plane MPLANE) */
	enum cap_slot_state	state;
	int			our_export_fd;			/* -1 if not exported; close on FREE transition */
	uint64_t		last_used_at_ns;		/* CLOCK_MONOTONIC when last touched (LRU) */
	int			bound_to_surface_id;		/* -1 if not bound; informational */
};

struct cap_pool {
	struct cap_pool_slot	*slots;
	unsigned int		count;		/* allocated slot count */
	pthread_mutex_t		lock;
	bool			initialized;
};

/*
 * cap_pool_init — allocate a pool of `count` CAPTURE buffers via
 * v4l2_create_buffers, mmap each buffer's planes, init slot states
 * to FREE. `count` is min'd against any reasonable hardware cap.
 *
 * Returns 0 on success, negative errno on failure.
 */
int cap_pool_init(struct cap_pool *pool, int video_fd, unsigned int capture_type,
		  unsigned int count, unsigned int v4l2_buffers_count_per_slot);

/*
 * cap_pool_destroy — close any outstanding our_export_fds, munmap all
 * planes, REQBUFS(0), free slots. Safe to call on a non-initialized
 * pool (no-op).
 *
 * Note: closing our_export_fd does not invalidate any consumer-held
 * dup'd fds — the kernel keeps the dma_buf alive while any fd refs
 * it. munmap on our side is independent of the consumer's mmap (each
 * mmap of a dma_buf is a distinct VMA).
 */
void cap_pool_destroy(struct cap_pool *pool, int video_fd, unsigned int capture_type);

/*
 * cap_pool_acquire — find a FREE slot with the oldest last_used_at_ns
 * (LRU). If no FREE slot is available, force-recycle the oldest
 * EXPORTED slot (close our_export_fd, demote to IN_DECODE for the
 * caller). Returns NULL only if no slots can be recycled at all
 * (catastrophic — pool too small).
 *
 * The returned slot is in IN_DECODE state. Caller QBUFs it and
 * transitions to DECODED via cap_pool_mark_decoded after DQBUF.
 */
struct cap_pool_slot *cap_pool_acquire(struct cap_pool *pool, int surface_id);

/*
 * cap_pool_mark_decoded — IN_DECODE → DECODED. Touches last_used_at_ns.
 * Called from RequestSyncSurface after successful DQBUF.
 */
void cap_pool_mark_decoded(struct cap_pool *pool, struct cap_pool_slot *slot);

/*
 * cap_pool_mark_exported — DECODED → EXPORTED. Stores `our_fd` so the
 * pool owns OUR copy of the EXPBUF'd fd; the consumer received a
 * dup'd / equivalent fd via the descriptor. last_used_at_ns is
 * touched again so EXPORTED slots are recycled in LRU order.
 *
 * Called from RequestExportSurfaceHandle after VIDIOC_EXPBUF.
 */
void cap_pool_mark_exported(struct cap_pool *pool, struct cap_pool_slot *slot, int our_fd);

/*
 * cap_pool_release — explicitly return a slot to FREE (close our
 * export fd if any). Called from RequestDestroySurfaces and from
 * RequestBeginPicture when re-acquiring (the surface's previous slot
 * is released first, then a new one acquired).
 */
void cap_pool_release(struct cap_pool *pool, struct cap_pool_slot *slot);

#endif /* _CAP_POOL_H_ */
