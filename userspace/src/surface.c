/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "encoder.h"
#include "request.h"
#include "surface.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va_drmcommon.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include "media.h"
#include "utils.h"
#include "v4l2.h"
#include "video.h"

/*
 * iter5b-β: the OUTPUT-side V4L2 device-format lifecycle moved out
 * of this file. Pre-β CreateSurfaces2 owned the S_FMT(OUTPUT) +
 * CAPTURE-format probe + cap_pool_init + per-surface destination_*
 * fill; now that responsibility lives in context.c::RequestCreateContext
 * where the bound config (and therefore the active VAProfile) is
 * known via config_id. CreateSurfaces2 retains only surface object
 * ID allocation and per-surface bookkeeping. The previous
 * `surface_reset_format_cache` helper and `last_output_width/height`
 * fields are deleted (β doesn't gate re-S_FMT on
 * resolution — the lifecycle is CreateContext-centric and natural
 * setup/teardown happens at each context cycle).
 */

/*
 * Iter2 Fix 3 helpers — bind / unbind a cap_pool_slot to an
 * object_surface. Called from BeginPicture (acquire+bind) and
 * DestroySurfaces (unbind). Populates surface_object->destination_*
 * fields from the slot so existing code paths (the QBUF in
 * picture.c::EndPicture, the EXPBUF in ExportSurfaceHandle, the
 * mmap-read in copy_surface_to_image) continue to work unchanged.
 *
 * surface_bind_slot is called only from BeginPicture; the surface's
 * format-uniform fields (destination_planes_count, destination_sizes,
 * destination_offsets, destination_bytesperlines) are already set
 * by CreateSurfaces2 and stay constant.
 */
void surface_bind_slot(struct object_surface *surface_object,
		       struct cap_pool_slot *slot)
{
	unsigned int j;

	surface_object->current_slot = slot;
	surface_object->destination_index = slot->v4l2_index;
	surface_object->destination_buffers_count = slot->buffers_count;

	for (j = 0; j < slot->buffers_count; j++) {
		surface_object->destination_map[j] = slot->map[j];
		surface_object->destination_map_lengths[j] = slot->map_lengths[j];
		surface_object->destination_map_offsets[j] = slot->map_offsets[j];
	}

	/*
	 * destination_data[j] is the per-plane CPU pointer used by
	 * copy_surface_to_image. For single-buffer MPLANE NV12 (our
	 * common case), all planes live in slot->map[0] at varying
	 * offsets recorded in destination_offsets[].
	 */
	if (slot->buffers_count == 1) {
		for (j = 0; j < surface_object->destination_planes_count; j++)
			surface_object->destination_data[j] =
				(unsigned char *)slot->map[0] +
				surface_object->destination_offsets[j];
	} else {
		for (j = 0; j < surface_object->destination_planes_count; j++)
			surface_object->destination_data[j] = slot->map[j];
	}
}

void surface_unbind_slot(struct request_data *driver_data,
			 struct object_surface *surface_object)
{
	if (surface_object->current_slot == NULL)
		return;
	/*
	 * The pool may already be gone (vaDestroyContext tears it down while
	 * VA surfaces are still alive, because they are application-owned).
	 * Never touch slot storage after cap_pool_destroy.
	 */
	if (driver_data->capture_pool.initialized)
		cap_pool_release(&driver_data->capture_pool,
				 surface_object->current_slot);
	surface_object->current_slot = NULL;
}

/*
 * iter5b-β Commit D: fill format-uniform destination_* on a surface
 * from driver_data's CAPTURE-format cache. Idempotent: no-op if
 * destination_planes_count is non-zero already.
 */
void surface_fill_format_uniform(struct request_data *driver_data,
				 struct object_surface *surface_object)
{
	unsigned int j;

	if (!driver_data->fmt_valid)
		return;
	if (surface_object->destination_planes_count != 0)
		return;

	surface_object->destination_planes_count = driver_data->fmt_planes_count;
	surface_object->destination_buffers_count = driver_data->fmt_buffers_count;

	if (driver_data->fmt_buffers_count == 1) {
		for (j = 0; j < driver_data->fmt_planes_count; j++) {
			surface_object->destination_offsets[j] =
				j > 0 ? driver_data->fmt_sizes[j - 1] : 0;
			surface_object->destination_sizes[j] =
				driver_data->fmt_sizes[j];
			surface_object->destination_bytesperlines[j] =
				driver_data->fmt_bytesperlines[0];
		}
	} else if (driver_data->fmt_buffers_count == driver_data->fmt_planes_count) {
		for (j = 0; j < driver_data->fmt_planes_count; j++) {
			surface_object->destination_offsets[j] = 0;
			surface_object->destination_sizes[j] =
				driver_data->fmt_sizes[j];
			surface_object->destination_bytesperlines[j] =
				driver_data->fmt_bytesperlines[j];
		}
	}
}

VAStatus RequestCreateSurfaces2(VADriverContextP context, unsigned int format,
				unsigned int width, unsigned int height,
				VASurfaceID *surfaces_ids,
				unsigned int surfaces_count,
				VASurfaceAttrib *attributes,
				unsigned int attributes_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	unsigned int i;
	VASurfaceID id;

	/*
	 * iter5b-β: only RT-format-level validation here. All V4L2
	 * device state (OUTPUT format, CAPTURE format probe,
	 * cap_pool_init, per-surface destination_* fill) is deferred
	 * to RequestCreateContext where the bound VAConfigID
	 * (and therefore the active VAProfile) is known. CreateSurfaces2
	 * has no config_id parameter; the VA-API contract is
	 * CreateConfig → CreateSurfaces → CreateContext, and we
	 * can't know the OUTPUT pixel format until CreateContext binds.
	 *
	 * Surface objects allocated here hold only the requested
	 * width/height and per-surface lifecycle bookkeeping
	 * (current_slot, status, params, etc). The format-uniform
	 * destination_* fields are filled by CreateContext via
	 * surface_bind_format_uniform_fields(); the per-slot
	 * destination_* fields fill at BeginPicture via surface_bind_slot.
	 */
	/* iter39: allow YUV420_10 for Hi10P / Main10 surface allocation. */
	if (format != VA_RT_FORMAT_YUV420 &&
	    format != VA_RT_FORMAT_YUV420_10)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	for (i = 0; i < surfaces_count; i++) {
		id = object_heap_allocate(&driver_data->surface_heap);
		surface_object = SURFACE(driver_data, id);
		if (surface_object == NULL)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;

		surface_object->current_slot = NULL;	/* iter2 Fix 3 */
		surface_object->enc_data = NULL;	/* encoder storage */
		surface_object->export_fd = -1;
		surface_object->export_map = NULL;
		surface_object->export_size = 0;
		surface_object->enc_pitch = 0;
		surface_object->enc_width = 0;
		surface_object->enc_height = 0;
		surface_object->destination_index = 0;	/* set on bind */
		surface_object->destination_planes_count = 0;	/* set at CreateContext */
		surface_object->destination_buffers_count = 0;	/* set at CreateContext */

		surface_object->status = VASurfaceReady;
		surface_object->width = width;
		surface_object->height = height;

		surface_object->source_index = 0;
		surface_object->source_data = NULL;
		surface_object->source_size = 0;

		memset(&surface_object->params, 0,
		       sizeof(surface_object->params));
		surface_object->slices_count = 0;
		surface_object->slices_size = 0;

		surface_object->request_fd = -1;

		/*
		 * iter5b-β Commit D: if CreateContext has already populated
		 * the format-uniform cache (driver_data->fmt_valid), fill
		 * the new surface's destination_* immediately. This covers
		 * the case where a consumer creates more surfaces AFTER
		 * CreateContext. The first batch of surfaces (created before
		 * CreateContext) gets filled by CreateContext's surface_heap
		 * walk; this lazy-fill handles late arrivals.
		 */
		surface_fill_format_uniform(driver_data, surface_object);

		surfaces_ids[i] = id;
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestCreateSurfaces(VADriverContextP context, int width, int height,
			       int format, int surfaces_count,
			       VASurfaceID *surfaces_ids)
{
	return RequestCreateSurfaces2(context, format, width, height,
				      surfaces_ids, surfaces_count, NULL, 0);
}

VAStatus RequestDestroySurfaces(VADriverContextP context,
				VASurfaceID *surfaces_ids, int surfaces_count)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;
	unsigned int i;

	for (i = 0; i < surfaces_count; i++) {
		surface_object = SURFACE(driver_data, surfaces_ids[i]);
		if (surface_object == NULL)
			return VA_STATUS_ERROR_INVALID_SURFACE;

		/*
		 * source_* are now transient borrows from request_pool, not
		 * surface-owned mappings; the pool owns the underlying mmap.
		 * Nothing to free here.
		 *
		 * Iter2 Fix 3: destination_* mappings are owned by cap_pool;
		 * surface_unbind_slot returns the slot to FREE (closing OUR
		 * EXPBUF fd if any). Pool-owned mmaps are freed at
		 * cap_pool_destroy time (RequestDestroyContext).
		 */
		surface_unbind_slot(driver_data, surface_object);

		/* Encoder-owned NV12 storage (vaPutImage / derive upload). */
		encoder_surface_free_export(surface_object);
		encoder_surface_free_storage(surface_object);

		/*
		 * iter6: request_fd is owned by the OUTPUT pool slot, not by
		 * the surface. Do not close here. The pool closes all slot
		 * fds at request_pool_destroy time, which fires from
		 * RequestTerminate (driver unload) — the pool is driver-wide
		 * and survives context destroy/recreate cycles.
		 */

		object_heap_free(&driver_data->surface_heap,
				 (struct object_base *)surface_object);
	}

	return VA_STATUS_SUCCESS;
}

VAStatus RequestSyncSurface(VADriverContextP context, VASurfaceID surface_id)
{

	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object = NULL;
	VAStatus status;
	struct video_format *video_format;
	unsigned int output_type, capture_type;
	int request_fd = -1;
	int rc;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL) {
		status = VA_STATUS_ERROR_INVALID_SURFACE;
		goto error;
	}

	/*
	 * Encoder sessions: vaEndPicture() submits and waits for the hardware
	 * synchronously, so by the time the application calls vaSyncSurface
	 * the work is already complete.  This stays ahead of the decoder path
	 * (which has no V4L2 CAPTURE queue of its own in an encoder context)
	 * and ahead of the video_format NULL check, because encoder sessions
	 * never populate the decoder format cache.
	 *
	 * Gated on encode_active only, never on the presence of surface NV12
	 * storage: ffmpeg's hwframe-init vaDeriveImage probe runs before any
	 * context exists and - since derive aliases surface storage - may
	 * allocate that storage even on surfaces a decoder later uses.
	 */
	if (driver_data->encode_active &&
	    surface_object->current_slot == NULL) {
		surface_object->status = VASurfaceDisplaying;
		status = VA_STATUS_SUCCESS;
		goto complete;
	}

	/*
	 * Nothing is in flight: no V4L2/request state to wait on.  This is
	 * also the path ffmpeg's hwupload takes (vaSyncSurface on a freshly
	 * allocated input surface) before any encode/decode context exists.
	 */
	if (surface_object->status != VASurfaceRendering) {
		status = VA_STATUS_SUCCESS;
		goto complete;
	}

	video_format = driver_data->video_format;
	if (video_format == NULL) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	output_type = v4l2_type_video_output(video_format->v4l2_mplane);
	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	request_fd = surface_object->request_fd;
	if (request_fd < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = media_request_queue(request_fd);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	rc = media_request_wait_completion(request_fd);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*
	 * iter6: the request_fd belongs to the OUTPUT pool slot, not to the
	 * surface. REINIT to reset its state in place — close+alloc would
	 * reuse the lowest-free fd number against a kernel object whose
	 * teardown hasn't fully drained, racing with QBUF on a slot that
	 * was just released. The pool's 1:1 slot-to-fd binding eliminates
	 * cross-slot fd reuse, and REINIT here resets the request object
	 * for the next decode cycle on the same slot.
	 *
	 * Iter4's frame-11 EINVAL (which prompted the iter4 close+alloc
	 * model) was a control-payload bug — DPB carry-over with FFmpeg's
	 * V4L2_H264_FRAME_REF semantics not yet matched. That's been fixed
	 * since iter4 (`74d8dd1`), so REINIT is no longer compromised by
	 * the cluster-validation EINVAL pattern.
	 */
	rc = media_request_reinit(request_fd);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}
	surface_object->request_fd = -1;

	rc = v4l2_dequeue_buffer(driver_data->video_fd, -1, output_type,
				 surface_object->source_index, 1);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		/*
		 * iter7: OUTPUT DQBUF failed. The V4L2 buffer is in an
		 * indeterminate kernel state — it may still be QUEUED. Do
		 * NOT return the slot to acquire-rotation: the next QBUF
		 * on it would EINVAL. Leave source_data set so the error
		 * handler skips force_release and the slot stays dead-busy.
		 */
		goto error_buffer_indeterminate;
	}

	/*
	 * OUTPUT buffer is back from the kernel: return its pool slot
	 * for reuse and clear the surface's transient borrow handle.
	 */
	request_pool_release(&driver_data->output_pool,
			     surface_object->source_index);
	surface_object->source_data = NULL;
	surface_object->source_size = 0;

	rc = v4l2_dequeue_buffer(driver_data->video_fd, -1, capture_type,
				 surface_object->destination_index,
				 surface_object->destination_buffers_count);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*
	 * Iter2 Fix 3: CAPTURE buffer is back from the kernel with valid
	 * pixel content. Transition the slot IN_DECODE → DECODED. The slot
	 * stays bound to this surface until either ExportSurfaceHandle
	 * (→ EXPORTED), the next BeginPicture for this surface (slot is
	 * released first), or DestroySurfaces (release).
	 */
	if (surface_object->current_slot != NULL) {
		cap_pool_mark_decoded(&driver_data->capture_pool,
				      surface_object->current_slot);

		/*
		 * iter8 Phase 6 (γ): env-gated diagnostic dump of the CAPTURE
		 * buffer immediately after DQBUF + mark_decoded. Distinguishes
		 * "kernel didn't write" from "libva mis-reads" for Bug 4
		 * (H.264 partial-fill). Off by default; enable with
		 * LIBVA_V4L2_DUMP_CAPTURE=1. destination_data[] is valid here
		 * (surface_bind_slot populated it at BeginPicture).
		 */
		static const char *dump_env = NULL;
		static bool dump_env_checked = false;
		if (!dump_env_checked) {
			dump_env = getenv("LIBVA_V4L2_DUMP_CAPTURE");
			dump_env_checked = true;
		}
		if (dump_env != NULL && dump_env[0] == '1') {
			unsigned int p;
			char hexbuf[128];
			request_log("γ-dump: surface_id=%u v4l2_index=%u planes=%u\n",
				    (unsigned int)surface_id,
				    surface_object->destination_index,
				    surface_object->destination_planes_count);
			for (p = 0; p < surface_object->destination_planes_count; p++) {
				const unsigned char *d = surface_object->destination_data[p];
				size_t sz = surface_object->destination_sizes[p];
				size_t scan_lim;
				unsigned int nz = 0;
				size_t i;
				int pos;

				if (d == NULL) {
					request_log("γ-dump:  plane[%u] NULL ptr (size=%zu)\n",
						    p, sz);
					continue;
				}

				/*
				 * Phase 5 MIN-2: scan at least one Y-MB row
				 * (16 lines * bytesperline) for plane 0, else
				 * 1024 bytes for chroma plane.
				 */
				if (p == 0) {
					size_t mbrow =
					    surface_object->destination_bytesperlines[0] * 16;
					scan_lim = sz < mbrow ? sz : mbrow;
				} else {
					scan_lim = sz < 1024 ? sz : 1024;
				}
				for (i = 0; i < scan_lim; i++)
					if (d[i] != 0)
						nz++;

				request_log("γ-dump:  plane[%u] sz=%zu bpl=%u "
					    "scan=%zu non_zero=%u\n",
					    p, sz,
					    surface_object->destination_bytesperlines[p],
					    scan_lim, nz);

				pos = 0;
				for (i = 0; i < 32 && i < sz; i++)
					pos += snprintf(hexbuf + pos,
							sizeof(hexbuf) - pos,
							"%02x ", d[i]);
				request_log("γ-dump:  plane[%u] head[0..32]: %s\n",
					    p, hexbuf);

				if (sz >= 32) {
					pos = 0;
					for (i = 0; i < 32; i++)
						pos += snprintf(hexbuf + pos,
								sizeof(hexbuf) - pos,
								"%02x ", d[sz - 32 + i]);
					request_log("γ-dump:  plane[%u] tail[%zu..%zu]: %s\n",
						    p, sz - 32, sz - 1, hexbuf);
				}
			}
		}
	}

	surface_object->status = VASurfaceDisplaying;

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	/*
	 * iter7: error recovery for the OUTPUT pool slot. If the surface
	 * acquired a slot in BeginPicture (source_data != NULL indicates
	 * an active borrow), reset the slot's request_fd via
	 * request_pool_force_release so the slot returns to the
	 * acquire-rotation. force_release tries REINIT first; falls back
	 * to close+alloc if REINIT fails; leaves the slot dead-busy if
	 * even alloc fails (other slots unaffected). Replaces iter6's
	 * accepted bounded leak.
	 *
	 * Reachable from: media_request_queue / wait_completion / REINIT
	 * failures. NOT reachable for OUTPUT-DQBUF failure (separate label
	 * `error_buffer_indeterminate` below) because in that case the
	 * V4L2 buffer is in an indeterminate kernel state and reusing the
	 * slot would EINVAL on the next QBUF.
	 *
	 * If the surface never acquired a slot (source_data == NULL),
	 * there is no slot to release; nothing to do.
	 */
	if (surface_object != NULL) {
		if (surface_object->source_data != NULL) {
			request_pool_force_release(&driver_data->output_pool,
						   surface_object->source_index);
			surface_object->source_data = NULL;
			surface_object->source_size = 0;
		}
		surface_object->request_fd = -1;
	}
	goto complete;

error_buffer_indeterminate:
	/*
	 * iter7: OUTPUT DQBUF failed after a successful REINIT. The kernel
	 * V4L2 buffer is in an unknown state (possibly still QUEUED with
	 * pending decode result, possibly half-dequeued, possibly stuck
	 * in driver internals). The slot's request_fd has already been
	 * REINIT'd to a clean state, but reusing the slot for a new
	 * decode would QBUF on a buffer the kernel may still hold —
	 * triggering exactly the iter6 race we eliminated for the happy
	 * path.
	 *
	 * Leave the slot dead-busy: don't release, don't force_release.
	 * Other slots are unaffected. If this fires repeatedly, the pool
	 * leaks slots until starvation, at which point acquire returns -1
	 * and BeginPicture cleanly propagates ALLOCATION_FAILED. This is
	 * a strictly safer failure mode than reusing an indeterminate
	 * V4L2 buffer.
	 */
	if (surface_object != NULL) {
		surface_object->source_data = NULL;
		surface_object->source_size = 0;
		surface_object->request_fd = -1;
	}

complete:
	return status;
}

VAStatus RequestQuerySurfaceAttributes(VADriverContextP context,
				       VAConfigID config,
				       VASurfaceAttrib *attributes,
				       unsigned int *attributes_count)
{

	struct request_data *driver_data = context->pDriverData;
	VASurfaceAttrib *attributes_list;
	unsigned int attributes_list_size = V4L2_REQUEST_MAX_CONFIG_ATTRIBUTES *
					    sizeof(*attributes);
	int memory_types;
	unsigned int i = 0;

	attributes_list = malloc(attributes_list_size);
	memset(attributes_list, 0, attributes_list_size);

	attributes_list[i].type = VASurfaceAttribPixelFormat;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = VA_FOURCC_NV12;
	i++;

	attributes_list[i].type = VASurfaceAttribMinWidth;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 32;
	i++;

	attributes_list[i].type = VASurfaceAttribMaxWidth;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 2048;
	i++;

	attributes_list[i].type = VASurfaceAttribMinHeight;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 32;
	i++;

	attributes_list[i].type = VASurfaceAttribMaxHeight;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;
	attributes_list[i].value.value.i = 2048;
	i++;

	attributes_list[i].type = VASurfaceAttribMemoryType;
	attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE |
				   VA_SURFACE_ATTRIB_SETTABLE;
	attributes_list[i].value.type = VAGenericValueTypeInteger;

	memory_types = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
		VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

	/*
	 * First version of DRM prime export does not handle modifiers,
	 * that are required for supporting the tiled output format.
	 */

	if (video_format_is_linear(driver_data->video_format))
		memory_types |= VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

	attributes_list[i].value.value.i = memory_types;
	i++;

	attributes_list_size = i * sizeof(*attributes);

	if (attributes != NULL)
		memcpy(attributes, attributes_list, attributes_list_size);

	free(attributes_list);

	*attributes_count = i;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestQuerySurfaceStatus(VADriverContextP context,
				   VASurfaceID surface_id,
				   VASurfaceStatus *status)
{
	struct request_data *driver_data = context->pDriverData;
	struct object_surface *surface_object;


	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	*status = surface_object->status;

	return VA_STATUS_SUCCESS;
}

VAStatus RequestPutSurface(VADriverContextP context, VASurfaceID surface_id,
			   void *draw, short src_x, short src_y,
			   unsigned short src_width, unsigned short src_height,
			   short dst_x, short dst_y, unsigned short dst_width,
			   unsigned short dst_height, VARectangle *cliprects,
			   unsigned int cliprects_count, unsigned int flags)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestLockSurface(VADriverContextP context, VASurfaceID surface_id,
			    unsigned int *fourcc, unsigned int *luma_stride,
			    unsigned int *chroma_u_stride,
			    unsigned int *chroma_v_stride,
			    unsigned int *luma_offset,
			    unsigned int *chroma_u_offset,
			    unsigned int *chroma_v_offset,
			    unsigned int *buffer_name, void **buffer)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestUnlockSurface(VADriverContextP context, VASurfaceID surface_id)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus RequestExportSurfaceHandle(VADriverContextP context,
				    VASurfaceID surface_id, uint32_t mem_type,
				    uint32_t flags, void *descriptor)
{
	struct request_data *driver_data = context->pDriverData;
	VADRMPRIMESurfaceDescriptor *surface_descriptor = descriptor;
	struct object_surface *surface_object;
	struct video_format *video_format;
	int *export_fds = NULL;
	unsigned int export_fds_count;
	unsigned int planes_count;
	unsigned int capture_type;
	unsigned int size;
	unsigned int i;
	VAStatus status;
	int rc;

	video_format = driver_data->video_format;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

	surface_object = SURFACE(driver_data, surface_id);
	if (surface_object == NULL)
		return VA_STATUS_ERROR_INVALID_SURFACE;

	if (video_format == NULL) {
		/* Encoder input surface: export a linear NV12 DMA-BUF. */
		if (encoder_surface_export(surface_object, flags,
					   descriptor) < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		return VA_STATUS_SUCCESS;
	}

	export_fds_count = surface_object->destination_buffers_count;
	export_fds = malloc(export_fds_count * sizeof(*export_fds));

	capture_type = v4l2_type_video_capture(video_format->v4l2_mplane);

	rc = v4l2_export_buffer(driver_data->video_fd, capture_type,
				surface_object->destination_index, O_RDONLY,
				export_fds, export_fds_count);
	if (rc < 0) {
		status = VA_STATUS_ERROR_OPERATION_FAILED;
		goto error;
	}

	/*
	 * Iter2 Fix 3: pool now owns OUR copy of the EXPBUF'd fd. The
	 * consumer receives a dup'd / equivalent fd via the descriptor.
	 * Slot transitions DECODED → EXPORTED; it will be force-recyclable
	 * by LRU when the pool is exhausted, but FREE slots are always
	 * preferred.
	 */
	if (surface_object->current_slot != NULL && export_fds_count > 0)
		cap_pool_mark_exported(&driver_data->capture_pool,
				       surface_object->current_slot,
				       export_fds[0]);

	planes_count = surface_object->destination_planes_count;

	/* iter39: 10-bit session exports a DRM_FORMAT_NV15 buffer; advertise
	 * the matching fourcc so a PRIME consumer aware of NV15 (panfrost-
	 * Mesa et al.) can import correctly. PRIME consumers that only know
	 * NV12 / P010 should use the COPY (vaGetImage) path which unpacks
	 * NV15→P010 in image.c::copy_surface_to_image. */
	surface_descriptor->fourcc = driver_data->is_10bit
		? VA_FOURCC('N', 'V', '1', '5')
		: VA_FOURCC_NV12;
	surface_descriptor->width = surface_object->width;
	surface_descriptor->height = surface_object->height;
	surface_descriptor->num_objects = export_fds_count;

	size = 0;

	if (export_fds_count == 1)
		for (i = 0; i < planes_count; i++)
			size += surface_object->destination_sizes[i];

	/*
	 * Iteration 2 Fix 2: choose drm_format_modifier conditionally on
	 * pitch alignment. Mesa's WSI / Panfrost compositor path rejects
	 * DRM_FORMAT_MOD_NONE (= LINEAR explicit) buffers whose pitch isn't
	 * GPU-aligned (typically 64+ bytes for Mali). For 1920-wide content
	 * the pitch is 1920 (64-aligned, fine); for 864-wide content the
	 * pitch is 864 (only 16-aligned), Mesa rejects with "WSI pitch not
	 * properly aligned" and Firefox falls back to SW.
	 *
	 * Setting DRM_FORMAT_MOD_INVALID tells the importer "modifier
	 * unknown, treat as implicit / texture-only" — Firefox's
	 * DMABufSurface.cpp:1920 explicitly omits modifier attribs from
	 * eglCreateImage when the value is MOD_INVALID, bypassing Mesa's
	 * scanout-alignment check. The buffer is then texture-imported
	 * (small perf cost) instead of WSI scanout-imported, which is
	 * the correct behavior for a buffer that doesn't meet scanout
	 * alignment requirements.
	 *
	 * We branch on pitch alignment to preserve LINEAR semantics for
	 * already-aligned content (avoids unnecessary perf cost on the
	 * common 1920-wide case).
	 *
	 * Sonnet Phase 5 review (iter2 question 4) endorsed this
	 * conditional approach over a universal MOD_INVALID change.
	 */
	for (i = 0; i < export_fds_count; i++) {
		uint64_t modifier = video_format->drm_modifier;
		unsigned int bytesperline =
			surface_object->destination_bytesperlines[0];
		if (bytesperline & 63) /* not 64-byte aligned */
			modifier = DRM_FORMAT_MOD_INVALID;
		surface_descriptor->objects[i].drm_format_modifier = modifier;
		surface_descriptor->objects[i].fd = export_fds[i];
		surface_descriptor->objects[i].size = export_fds_count == 1 ?
						      size :
						      surface_object->destination_sizes[i];
	}

	/*
	 * Layer construction depends on the consumer's request flags
	 * (VA_EXPORT_SURFACE_*_LAYERS):
	 *
	 *   COMPOSED_LAYERS (default, mpv): one layer carrying both
	 *   Y and UV planes (drm_format=NV12, num_planes=2). Mesa
	 *   imports as a single NV12 EGLImage.
	 *
	 *   SEPARATE_LAYERS (Firefox 150 RDD): two layers, Y as a
	 *   single-plane R8 layer, UV as a single-plane GR88 layer.
	 *   Firefox's GetVAAPISurfaceDescriptor passes
	 *   VA_EXPORT_SURFACE_SEPARATE_LAYERS so its DMABufSurfaceYUV
	 *   import code can address Y and UV planes independently.
	 *   Without this branch, Firefox parsed our COMPOSED layout
	 *   as if it were SEPARATE, found bogus layer-1 data, and
	 *   silently fell back to FFmpeg(FFVPX) software decode.
	 *
	 * The earlier path 0001 mplane port assumed a single COMPOSED
	 * shape — fine for mpv but breaks any consumer requesting
	 * SEPARATE. Honor the flag.
	 */
	if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) && planes_count == 2) {
		surface_descriptor->num_layers = 2;

		/* Layer 0: Y plane as DRM_FORMAT_R8 (1 byte/pixel luma). */
		surface_descriptor->layers[0].drm_format = DRM_FORMAT_R8;
		surface_descriptor->layers[0].num_planes = 1;
		surface_descriptor->layers[0].object_index[0] =
			export_fds_count == 1 ? 0 : 0;
		surface_descriptor->layers[0].offset[0] =
			surface_object->destination_offsets[0];
		surface_descriptor->layers[0].pitch[0] =
			surface_object->destination_bytesperlines[0];

		/* Layer 1: UV plane as DRM_FORMAT_GR88 (interleaved
		 * U+V, 2 bytes/pixel chroma at half resolution). */
		surface_descriptor->layers[1].drm_format = DRM_FORMAT_GR88;
		surface_descriptor->layers[1].num_planes = 1;
		surface_descriptor->layers[1].object_index[0] =
			export_fds_count == 1 ? 0 : 1;
		surface_descriptor->layers[1].offset[0] =
			surface_object->destination_offsets[1];
		surface_descriptor->layers[1].pitch[0] =
			surface_object->destination_bytesperlines[1];
	} else {
		/* COMPOSED_LAYERS / default: one layer with all planes. */
		surface_descriptor->num_layers = 1;
		surface_descriptor->layers[0].drm_format = video_format->drm_format;
		surface_descriptor->layers[0].num_planes = planes_count;

		for (i = 0; i < planes_count; i++) {
			surface_descriptor->layers[0].object_index[i] =
				export_fds_count == 1 ? 0 : i;
			surface_descriptor->layers[0].offset[i] =
				surface_object->destination_offsets[i];
			surface_descriptor->layers[0].pitch[i] =
				surface_object->destination_bytesperlines[i];
		}
	}

	status = VA_STATUS_SUCCESS;
	goto complete;

error:
	for (i = 0; i < export_fds_count; i++)
		if (export_fds[i] >= 0)
			close(export_fds[i]);

complete:
	if (export_fds != NULL)
		free(export_fds);

	return status;
}
