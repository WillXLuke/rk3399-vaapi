/*
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

#ifndef _V4L2_H_
#define _V4L2_H_

#include <stdbool.h>
#include <stdint.h>

#define SOURCE_SIZE_MAX						(1024 * 1024)

unsigned int v4l2_type_video_output(bool mplane);
unsigned int v4l2_type_video_capture(bool mplane);
int v4l2_query_capabilities(int video_fd, unsigned int *capabilities);
bool v4l2_find_format(int video_fd, unsigned int type,
		      unsigned int pixelformat);
int v4l2_set_format(int video_fd, unsigned int type, unsigned int pixelformat,
		    unsigned int width, unsigned int height);
/*
 * Same as v4l2_set_format but explicitly overrides the OUTPUT
 * sizeimage hint. Pass sizeimage=0 to get the v4l2_set_format default
 * (SOURCE_SIZE_MAX for OUTPUT, 0 for CAPTURE). Used by
 * request_pool_resize on a mid-session bitstream-budget overrun to
 * grow the OUTPUT pool slots past the SOURCE_SIZE_MAX floor.
 */
int v4l2_set_format_sizeimage(int video_fd, unsigned int type,
			      unsigned int pixelformat,
			      unsigned int width, unsigned int height,
			      unsigned int sizeimage);
int v4l2_get_format(int video_fd, unsigned int type, unsigned int *width,
		    unsigned int *height, unsigned int *bytesperline,
		    unsigned int *sizes, unsigned int *planes_count);
int v4l2_create_buffers(int video_fd, unsigned int type,
			unsigned int buffers_count, unsigned int *index_base);
int v4l2_query_buffer(int video_fd, unsigned int type, unsigned int index,
		      unsigned int *lengths, unsigned int *offsets,
		      unsigned int buffers_count);
int v4l2_request_buffers(int video_fd, unsigned int type,
			 unsigned int buffers_count);
int v4l2_queue_buffer(int video_fd, int request_fd, unsigned int type,
		      struct timeval *timestamp, unsigned int index,
		      unsigned int size, unsigned int buffers_count);
int v4l2_dequeue_buffer(int video_fd, int request_fd, unsigned int type,
			unsigned int index, unsigned int buffers_count);
int v4l2_export_buffer(int video_fd, unsigned int type, unsigned int index,
		       unsigned int flags, int *export_fds,
		       unsigned int export_fds_count);
/*
 * Encoder-oriented queue/dequeue helpers (RK3399 VEPU2 H.264 frontend).
 *
 * The decoder paths use v4l2_queue_buffer()/v4l2_dequeue_buffer(), which
 * don't carry per-plane bytesused or a 64-bit timestamp.  Stateless encode
 * needs both: OUTPUT planes have distinct payload sizes (NV12M Y/UV) and
 * the CAPTURE buffer timestamp is what the kernel resolves P-frame
 * references by, so it must survive QBUF/DQBUF verbatim.  Timestamps are
 * in nanoseconds (the vb2 timestamp domain, which is also the domain of
 * V4L2_CID_STATELESS_H264_ENCODE_PARAMS.reference_ts).
 */
int v4l2_queue_buffer_timestamped(int video_fd, int request_fd,
				  unsigned int type, unsigned int index,
				  unsigned int buffers_count,
				  const unsigned int *bytesused,
				  uint64_t timestamp_ns);
int v4l2_dequeue_buffer_info(int video_fd, unsigned int type,
			     unsigned int index, unsigned int buffers_count,
			     unsigned int *bytesused, uint64_t *timestamp_ns);

int v4l2_get_controls(int video_fd, int request_fd,
		      struct v4l2_ext_control *controls,
		      unsigned int num_controls);
int v4l2_set_controls(int video_fd, int request_fd,
		      struct v4l2_ext_control *controls,
		      unsigned int num_controls);
int v4l2_set_control(int video_fd, int request_fd, unsigned int id, void *data,
		     unsigned int size);
int v4l2_set_stream(int video_fd, unsigned int type, bool enable);

/*
 * Capability-probe helpers. These let calling code discover what the
 * backing kernel driver supports rather than hardcoding assumptions
 * about specific decoder hardware.
 */

/*
 * Query the metadata of an extended control by CID. Fills *qec on
 * success. Returns 0 if the control exists, -1 (errno=EINVAL) if the
 * driver does not expose this CID. Pass qec=NULL to test existence
 * only.
 */
struct v4l2_query_ext_ctrl;
int v4l2_query_ext_ctrl(int video_fd, unsigned int id,
			struct v4l2_query_ext_ctrl *qec);

/*
 * Query a single menu item of a menu/intmenu control at the given
 * index. Fills *qm on success. Returns 0 if the menu item exists at
 * this index, -1 otherwise.
 */
struct v4l2_querymenu;
int v4l2_query_menu(int video_fd, unsigned int id, unsigned int index,
		    struct v4l2_querymenu *qm);

/*
 * Convenience: for a menu-type control, return true iff `value` is a
 * valid menu entry (i.e. the driver accepts it). Walks all menu items
 * up to the control's maximum to check.
 */
bool v4l2_ctrl_menu_has_value(int video_fd, unsigned int id,
			      unsigned int value);

#endif
