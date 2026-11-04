/*
 * Iteration 2 Fix 3: cap_pool implementation.
 *
 * Design rationale + limitations: see cap_pool.h docblock.
 *
 * Concurrency model:
 *   - All public functions take pool->lock at entry, release at exit.
 *   - cap_pool_acquire may sleep briefly while scanning slots; safe
 *     under lock since the scan is bounded by pool->count (<= 24
 *     typical).
 *   - The slot pointer returned by acquire / mark_decoded /
 *     mark_exported / release is stable across the call (lock is
 *     dropped before return) but the slot's state may change between
 *     calls. Callers MUST NOT cache slot pointers across sleep/I/O --
 *     they should treat slot pointers as opaque references valid only
 *     for the immediate operation.
 *
 *   In practice, our caller pattern is:
 *     surface_object->current_slot = cap_pool_acquire(...);
 *     v4l2_queue_buffer(slot->v4l2_index, ...);
 *     // later, in SyncSurface for the same surface:
 *     v4l2_dequeue_buffer(surface_object->current_slot->v4l2_index, ...);
 *     cap_pool_mark_decoded(surface_object->current_slot);
 *
 *   surface_object->current_slot is the persistent reference; the
 *   slot's V4L2 index is stable for the slot's lifetime. The state
 *   field IS read by other threads (acquire scans for FREE) — that
 *   reads are safe because:
 *     - acquire holds the lock during the scan
 *     - mark_decoded/mark_exported/release also hold the lock
 *   So state transitions are serialized.
 */

#include "cap_pool.h"
#include "v4l2.h"
#include "utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int cap_pool_init(struct cap_pool *pool, int video_fd, unsigned int capture_type,
		  unsigned int count, unsigned int v4l2_buffers_count_per_slot)
{
	unsigned int index_base;
	unsigned int i, j;
	int rc;

	if (pool == NULL || count == 0)
		return -EINVAL;

	memset(pool, 0, sizeof(*pool));

	rc = pthread_mutex_init(&pool->lock, NULL);
	if (rc != 0)
		return -rc;

	pool->slots = calloc(count, sizeof(*pool->slots));
	if (pool->slots == NULL) {
		pthread_mutex_destroy(&pool->lock);
		return -ENOMEM;
	}
	pool->count = count;

	rc = v4l2_create_buffers(video_fd, capture_type, count, &index_base);
	if (rc < 0) {
		free(pool->slots);
		pthread_mutex_destroy(&pool->lock);
		return rc;
	}

	for (i = 0; i < count; i++) {
		struct cap_pool_slot *slot = &pool->slots[i];

		slot->v4l2_index = index_base + i;
		slot->buffers_count = v4l2_buffers_count_per_slot;
		slot->state = CAP_SLOT_FREE;
		slot->our_export_fd = -1;
		slot->last_used_at_ns = 0;	/* never used → highest LRU priority */
		slot->bound_to_surface_id = -1;

		rc = v4l2_query_buffer(video_fd, capture_type, slot->v4l2_index,
				       slot->map_lengths, slot->map_offsets,
				       v4l2_buffers_count_per_slot);
		if (rc < 0) {
			request_log("cap_pool_init: query_buffer failed for "
				    "slot %u (v4l2_index=%u)\n",
				    i, slot->v4l2_index);
			goto error_cleanup;
		}

		for (j = 0; j < v4l2_buffers_count_per_slot; j++) {
			slot->map[j] = mmap(NULL, slot->map_lengths[j],
					    PROT_READ | PROT_WRITE, MAP_SHARED,
					    video_fd, slot->map_offsets[j]);
			if (slot->map[j] == MAP_FAILED) {
				request_log("cap_pool_init: mmap failed for "
					    "slot %u plane %u\n", i, j);
				slot->map[j] = NULL;
				goto error_cleanup;
			}
		}
	}

	pool->initialized = true;
	request_log("cap_pool_init: %u slots ready (v4l2_index=%u..%u, "
		    "%u plane(s) per slot)\n",
		    count, index_base, index_base + count - 1,
		    v4l2_buffers_count_per_slot);
	return 0;

error_cleanup:
	for (i = 0; i < count; i++) {
		struct cap_pool_slot *slot = &pool->slots[i];
		for (j = 0; j < v4l2_buffers_count_per_slot; j++) {
			if (slot->map[j] != NULL && slot->map[j] != MAP_FAILED)
				munmap(slot->map[j], slot->map_lengths[j]);
		}
	}
	(void)v4l2_request_buffers(video_fd, capture_type, 0);
	free(pool->slots);
	pthread_mutex_destroy(&pool->lock);
	memset(pool, 0, sizeof(*pool));
	return -EIO;
}

void cap_pool_destroy(struct cap_pool *pool, int video_fd, unsigned int capture_type)
{
	unsigned int i, j;

	if (pool == NULL || !pool->initialized)
		return;

	pthread_mutex_lock(&pool->lock);

	for (i = 0; i < pool->count; i++) {
		struct cap_pool_slot *slot = &pool->slots[i];

		if (slot->our_export_fd >= 0) {
			close(slot->our_export_fd);
			slot->our_export_fd = -1;
		}

		for (j = 0; j < slot->buffers_count; j++) {
			if (slot->map[j] != NULL && slot->map[j] != MAP_FAILED) {
				munmap(slot->map[j], slot->map_lengths[j]);
				slot->map[j] = NULL;
			}
		}
	}

	(void)v4l2_request_buffers(video_fd, capture_type, 0);

	pthread_mutex_unlock(&pool->lock);
	pthread_mutex_destroy(&pool->lock);

	free(pool->slots);
	pool->slots = NULL;
	pool->count = 0;
	pool->initialized = false;
}

struct cap_pool_slot *cap_pool_acquire(struct cap_pool *pool, int surface_id)
{
	struct cap_pool_slot *best = NULL;
	uint64_t best_ts = UINT64_MAX;
	unsigned int i;

	if (pool == NULL || !pool->initialized)
		return NULL;

	pthread_mutex_lock(&pool->lock);

	/* First pass: find the FREE slot with oldest last_used_at_ns. */
	for (i = 0; i < pool->count; i++) {
		struct cap_pool_slot *slot = &pool->slots[i];
		if (slot->state != CAP_SLOT_FREE)
			continue;
		if (slot->last_used_at_ns < best_ts) {
			best = slot;
			best_ts = slot->last_used_at_ns;
		}
	}

	/*
	 * Second pass (fallback): if no FREE slot, force-recycle the
	 * oldest EXPORTED slot. This is the documented Option A race
	 * window — the consumer may still hold a dup'd fd to this
	 * buffer's underlying physical memory, and the kernel will
	 * happily DMA new content into it. For typical mpv 16-surface
	 * playback with MIN_CAP_POOL=24, this fallback should never
	 * fire. If it does, the visual artifact is bounded to a few
	 * frames during recovery.
	 */
	if (best == NULL) {
		best_ts = UINT64_MAX;
		for (i = 0; i < pool->count; i++) {
			struct cap_pool_slot *slot = &pool->slots[i];
			if (slot->state != CAP_SLOT_EXPORTED)
				continue;
			if (slot->last_used_at_ns < best_ts) {
				best = slot;
				best_ts = slot->last_used_at_ns;
			}
		}
		if (best != NULL) {
			request_log("cap_pool_acquire: pool exhausted, "
				    "force-recycling EXPORTED slot v4l2_index=%u "
				    "(consumer race window may open)\n",
				    best->v4l2_index);
			if (best->our_export_fd >= 0) {
				close(best->our_export_fd);
				best->our_export_fd = -1;
			}
		}
	}

	if (best == NULL) {
		pthread_mutex_unlock(&pool->lock);
		request_log("cap_pool_acquire: no slot available "
			    "(pool->count=%u, all slots IN_DECODE/DECODED?)\n",
			    pool->count);
		return NULL;
	}

	/*
	 * Don't transition DECODED slots — they hold valid pixel content
	 * a consumer may still be reading via DeriveImage (vaapi-copy
	 * path). We never recycle DECODED. If a surface holds a DECODED
	 * slot for an extended period, it stays held; the surface's
	 * destruction (vaDestroySurfaces) is the only path that releases
	 * it. mpv typically progresses through DECODED → EXPORTED quickly
	 * for vaapi DMA-BUF; for vaapi-copy, DECODED → consumer reads
	 * via mmap → consumer is done after copy_surface_to_image returns.
	 * The vaapi-copy consumer has no explicit "I'm done" signal, so
	 * we rely on the next BeginPicture for the same surface to
	 * release the prior DECODED slot.
	 */

	best->state = CAP_SLOT_IN_DECODE;
	best->bound_to_surface_id = surface_id;
	best->last_used_at_ns = monotonic_ns();

	pthread_mutex_unlock(&pool->lock);
	return best;
}

void cap_pool_mark_decoded(struct cap_pool *pool, struct cap_pool_slot *slot)
{
	if (pool == NULL || slot == NULL)
		return;
	pthread_mutex_lock(&pool->lock);
	slot->state = CAP_SLOT_DECODED;
	slot->last_used_at_ns = monotonic_ns();
	pthread_mutex_unlock(&pool->lock);
}

void cap_pool_mark_exported(struct cap_pool *pool, struct cap_pool_slot *slot, int our_fd)
{
	if (pool == NULL || slot == NULL)
		return;
	pthread_mutex_lock(&pool->lock);
	if (slot->our_export_fd >= 0 && slot->our_export_fd != our_fd) {
		/*
		 * Double-Export: a previous EXPBUF'd fd existed. Close
		 * the old one. Consumer's old fd remains valid via
		 * dma_buf refcount. Documented in surface.c export path.
		 */
		close(slot->our_export_fd);
	}
	slot->our_export_fd = our_fd;
	slot->state = CAP_SLOT_EXPORTED;
	slot->last_used_at_ns = monotonic_ns();
	pthread_mutex_unlock(&pool->lock);
}

void cap_pool_release(struct cap_pool *pool, struct cap_pool_slot *slot)
{
	if (pool == NULL || slot == NULL)
		return;
	pthread_mutex_lock(&pool->lock);
	if (slot->our_export_fd >= 0) {
		close(slot->our_export_fd);
		slot->our_export_fd = -1;
	}
	slot->state = CAP_SLOT_FREE;
	slot->bound_to_surface_id = -1;
	slot->last_used_at_ns = monotonic_ns();
	pthread_mutex_unlock(&pool->lock);
}
