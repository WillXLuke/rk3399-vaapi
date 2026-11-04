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

#ifndef _CONTEXT_H_
#define _CONTEXT_H_

#include <stdbool.h>
#include <stdint.h>

#include <va/va_backend.h>

#include "object_heap.h"
#include "h264.h"

struct encoder_context;

#define CONTEXT(data, id)                                                      \
	((struct object_context *)object_heap_lookup(&(data)->context_heap, id))
#define CONTEXT_ID_OFFSET		0x02000000

struct object_context {
	struct object_base base;

	VAConfigID config_id;
	VASurfaceID render_surface_id;
	VASurfaceID *surfaces_ids;
	int surfaces_count;

	int picture_width;
	int picture_height;
	int flags;

	/*
	 * Encoder (VAEntrypointEncSlice) state.  Non-NULL only for encoder
	 * contexts; the decoder paths never look at it.
	 */
	struct encoder_context *encoder;

	/* H264 only */
	struct h264_dpb dpb;
	bool h264_start_code;

	/*
	 * iter9 α-7: monotonic per-context timestamp counter (us). Replaces
	 * gettimeofday in EndPicture so DPB.reference_ts / OUTPUT QBUF ts
	 * are small values matching ffmpeg-v4l2request's pattern. Placed
	 * here (object_context) not driver_data per Phase 5 IMP-1 to avoid
	 * cross-context collisions.
	 */
	uint64_t timestamp_counter;

	/* fresnel-fourier iter4: VP9 loop-filter delta state, persisted across
	 * frames per kernel UAPI <linux/v4l2-controls.h>:2578 ("If this syntax
	 * element is not present in the bitstream, users should pass its last
	 * value.") and VP9 spec defaults from FFmpeg vp9.c:666-671. Reset on
	 * keyframe / error-resilient / intra-only via vp9_lf.initialized=false. */
	struct {
		int8_t ref_deltas[4];
		int8_t mode_deltas[2];
		bool initialized;
	} vp9_lf;
};

VAStatus RequestCreateContext(VADriverContextP context, VAConfigID config_id,
			      int picture_width, int picture_height, int flags,
			      VASurfaceID *surfaces_ids, int surfaces_count,
			      VAContextID *context_id);
VAStatus RequestDestroyContext(VADriverContextP context,
			       VAContextID context_id);

#endif
