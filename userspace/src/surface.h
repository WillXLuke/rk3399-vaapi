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

#ifndef _SURFACE_H_
#define _SURFACE_H_

#include <linux/videodev2.h>

#include <va/va_backend.h>

#include "object_heap.h"
#include "cap_pool.h"

#include "h265.h"

struct request_data;

#define SURFACE(data, id)                                                      \
	((struct object_surface *)object_heap_lookup(&(data)->surface_heap, id))
#define SURFACE_ID_OFFSET		0x04000000

struct object_surface {
	struct object_base base;

	VASurfaceStatus status;
	int width;
	int height;

	unsigned int source_index;
	void *source_data;
	unsigned int source_size;

	/*
	 * Iter2 Fix 3: destination_* fields below are now per-decode-cycle.
	 * They are populated from current_slot in RequestBeginPicture and
	 * remain valid through SyncSurface, ExportSurfaceHandle, and
	 * DeriveImage/copy_surface_to_image (vaapi-copy path). Subsequent
	 * BeginPicture for this surface releases the prior slot and
	 * acquires a new one.
	 *
	 * destination_planes_count, destination_sizes, destination_offsets,
	 * destination_bytesperlines are FORMAT-uniform across all CAPTURE
	 * buffers, so they're set once at CreateSurfaces2 time and stay.
	 *
	 * destination_index, destination_map[], destination_map_lengths,
	 * destination_map_offsets, destination_data[] are SLOT-specific
	 * and re-populated each BeginPicture from current_slot.
	 *
	 * destination_buffers_count is also format-uniform (V4L2 planes
	 * per buffer = 1 for single-plane MPLANE NV12).
	 */
	struct cap_pool_slot *current_slot;	/* iter2 Fix 3 */
	unsigned int destination_index;
	void *destination_map[VIDEO_MAX_PLANES];
	unsigned int destination_map_lengths[VIDEO_MAX_PLANES];
	unsigned int destination_map_offsets[VIDEO_MAX_PLANES];
	void *destination_data[VIDEO_MAX_PLANES];
	unsigned int destination_sizes[VIDEO_MAX_PLANES];
	unsigned int destination_offsets[VIDEO_MAX_PLANES];
	unsigned int destination_bytesperlines[VIDEO_MAX_PLANES];
	unsigned int destination_planes_count;
	unsigned int destination_buffers_count;

	unsigned int slices_size;
	unsigned int slices_count;

	struct timeval timestamp;

	/*
	 * Encoder-owned NV12 storage (tightly packed, enc_pitch = enc_width).
	 * Allocated lazily by the encoder frontend so vaPutImage()/the
	 * vaDeriveImage() upload path and the V4L2 OUTPUT copy in
	 * vaEndPicture() share one buffer per surface.  Unused (NULL) for
	 * decoder surfaces.
	 */
	void *enc_data;
	unsigned int enc_pitch;
	unsigned int enc_width;
	unsigned int enc_height;

	union {
		struct {
			VAPictureParameterBufferMPEG2 picture;
			VASliceParameterBufferMPEG2 slice;
			VAIQMatrixBufferMPEG2 iqmatrix;
			bool iqmatrix_set;
		} mpeg2;
		struct {
			VAIQMatrixBufferH264 matrix;
			bool matrix_set;
			VAPictureParameterBufferH264 picture;
			VASliceParameterBufferH264 slice;
		} h264;
		struct {
			VAPictureParameterBufferHEVC picture;
			VASliceParameterBufferHEVC slice;
			VASliceParameterBufferHEVC slices[HEVC_MAX_SLICES_PER_FRAME];
			unsigned int num_slices;
			VAIQMatrixBufferHEVC iqmatrix;
			bool iqmatrix_set;
		} h265;
		struct {
			VAPictureParameterBufferVP8 picture;
			VASliceParameterBufferVP8 slice;
			VAIQMatrixBufferVP8 iqmatrix;
			bool iqmatrix_set;
			VAProbabilityDataBufferVP8 probability;
			bool probability_set;
		} vp8;
		struct {
			VADecPictureParameterBufferVP9 picture;
			VASliceParameterBufferVP9 slice;
		} vp9;
		struct {
			/*
			 * AV1 picture parameter buffer.  Slice params are
			 * intentionally absent — the daedalus daemon track
			 * (issue #11) consumes the slice OBU bytes directly
			 * from the OUTPUT bitstream and synthesises only the
			 * sequence-header OBU from V4L2_CID_STATELESS_AV1_
			 * SEQUENCE.  No per-tile-group struct→OBU re-synthesis
			 * required from libva today.
			 */
			VADecPictureParameterBufferAV1 picture;
		} av1;
	} params;

	int request_fd;
};

VAStatus RequestCreateSurfaces2(VADriverContextP context, unsigned int format,
				unsigned int width, unsigned int height,
				VASurfaceID *surfaces_ids,
				unsigned int surfaces_count,
				VASurfaceAttrib *attributes,
				unsigned int attributes_count);
VAStatus RequestCreateSurfaces(VADriverContextP context, int width, int height,
			       int format, int surfaces_count,
			       VASurfaceID *surfaces_ids);
VAStatus RequestDestroySurfaces(VADriverContextP context,
				VASurfaceID *surfaces_ids, int surfaces_count);
VAStatus RequestSyncSurface(VADriverContextP context, VASurfaceID surface_id);
VAStatus RequestQuerySurfaceAttributes(VADriverContextP context,
				       VAConfigID config,
				       VASurfaceAttrib *attributes,
				       unsigned int *attributes_count);
VAStatus RequestQuerySurfaceStatus(VADriverContextP context,
				   VASurfaceID surface_id,
				   VASurfaceStatus *status);
VAStatus RequestPutSurface(VADriverContextP context, VASurfaceID surface_id,
			   void *draw, short src_x, short src_y,
			   unsigned short src_width, unsigned short src_height,
			   short dst_x, short dst_y, unsigned short dst_width,
			   unsigned short dst_height, VARectangle *cliprects,
			   unsigned int cliprects_count, unsigned int flags);
VAStatus RequestLockSurface(VADriverContextP context, VASurfaceID surface_id,
			    unsigned int *fourcc, unsigned int *luma_stride,
			    unsigned int *chroma_u_stride,
			    unsigned int *chroma_v_stride,
			    unsigned int *luma_offset,
			    unsigned int *chroma_u_offset,
			    unsigned int *chroma_v_offset,
			    unsigned int *buffer_name, void **buffer);
VAStatus RequestUnlockSurface(VADriverContextP context, VASurfaceID surface_id);
VAStatus RequestExportSurfaceHandle(VADriverContextP context,
				    VASurfaceID surface_id, uint32_t mem_type,
				    uint32_t flags, void *descriptor);

/*
 * iter5b-β Commit D: populate a surface's format-uniform destination_*
 * fields (planes_count, buffers_count, offsets, sizes, bytesperlines)
 * from driver_data's cached CAPTURE-side geometry. Idempotent: skip
 * if already filled (destination_planes_count != 0). Caller must
 * ensure driver_data->fmt_valid is true (CreateContext has run).
 *
 * Called by:
 *   - context.c::RequestCreateContext after v4l2_get_format(CAPTURE)
 *     populates the cache; walks the surface_heap and fills every
 *     existing surface (covers surfaces created before CreateContext,
 *     including the ffmpeg vaapi-copy case where surfaces_count=0 is
 *     passed but surfaces exist in the heap from earlier
 *     CreateSurfaces2 calls).
 *   - surface.c::RequestCreateSurfaces2 after surface allocation,
 *     covering the case where CreateContext fired before this
 *     CreateSurfaces2 call (fmt cache is valid, fill immediately).
 */
void surface_fill_format_uniform(struct request_data *driver_data,
				 struct object_surface *surface_object);

/*
 * Iter2 Fix 3: bind / unbind a CAPTURE-pool slot to an object_surface.
 * Called from picture.c::RequestBeginPicture (acquire+bind) and
 * surface.c::RequestDestroySurfaces (unbind). Mirrors slot's V4L2 index
 * and mmap pointers into surface_object->destination_* so existing
 * QBUF/DQBUF/EXPBUF code paths see no behavioral change.
 */
void surface_bind_slot(struct object_surface *surface_object,
		       struct cap_pool_slot *slot);
void surface_unbind_slot(struct request_data *driver_data,
			 struct object_surface *surface_object);

#endif
