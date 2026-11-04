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

#ifndef _BUFFER_H_
#define _BUFFER_H_

#include <stdbool.h>

#include <va/va_backend.h>

#include "object_heap.h"
#include "request.h"

#define BUFFER(data, id)                                                       \
	((struct object_buffer *)object_heap_lookup(&(data)->buffer_heap, id))
#define BUFFER_ID_OFFSET		0x08000000

struct object_buffer {
	struct object_base base;

	VABufferType type;
	unsigned int initial_count;
	unsigned int count;

	void *data;
	unsigned int size;

	/*
	 * Encoder frontend: VAImageBufferType buffers derived from an
	 * encode surface point their data straight at the surface's
	 * NV12 storage instead of owning a heap copy.  data_foreign
	 * suppresses the free() in RequestDestroyBuffer.
	 */
	bool data_foreign;

	VASurfaceID derived_surface_id;
	VABufferInfo info;
};

VAStatus RequestCreateBuffer(VADriverContextP context, VAContextID context_id,
			     VABufferType type, unsigned int size,
			     unsigned int count, void *data,
			     VABufferID *buffer_id);
/*
 * Create a buffer that borrows caller-owned memory (no copy, no free on
 * destroy).  Used by the encoder's vaDeriveImage() so a mapped derived
 * image writes directly into the VA surface.
 */
VAStatus RequestCreateBufferForeign(VADriverContextP context,
				    VABufferType type, void *data,
				    unsigned int size, VABufferID *buffer_id);
VAStatus RequestDestroyBuffer(VADriverContextP context, VABufferID buffer_id);
VAStatus RequestMapBuffer(VADriverContextP context, VABufferID buffer_id,
			  void **data_map);
VAStatus RequestUnmapBuffer(VADriverContextP context, VABufferID buffer_id);
VAStatus RequestBufferSetNumElements(VADriverContextP context,
				     VABufferID buffer_id, unsigned int count);
VAStatus RequestBufferInfo(VADriverContextP context, VABufferID buffer_id,
			   VABufferType *type, unsigned int *size,
			   unsigned int *count);
VAStatus RequestAcquireBufferHandle(VADriverContextP context,
				    VABufferID buffer_id,
				    VABufferInfo *buffer_info);
VAStatus RequestReleaseBufferHandle(VADriverContextP context,
	VABufferID buffer_id);

#endif
