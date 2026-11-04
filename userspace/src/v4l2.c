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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include "utils.h"
#include "v4l2.h"

static bool v4l2_type_is_output(unsigned int type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return true;

	default:
		return false;
	}
}

static bool v4l2_type_is_mplane(unsigned int type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return true;

	default:
		return false;
	}
}

unsigned int v4l2_type_video_output(bool mplane)
{
	return mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE :
			V4L2_BUF_TYPE_VIDEO_OUTPUT;
}

unsigned int v4l2_type_video_capture(bool mplane)
{
	return mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
			V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

int v4l2_query_capabilities(int video_fd, unsigned int *capabilities)
{
	struct v4l2_capability capability;
	int rc;

	memset(&capability, 0, sizeof(capability));

	rc = ioctl(video_fd, VIDIOC_QUERYCAP, &capability);
	if (rc < 0)
		return -1;

	if (capabilities != NULL) {
		if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0)
			*capabilities = capability.device_caps;
		else
			*capabilities = capability.capabilities;
	}

	return 0;
}

static void v4l2_setup_format(struct v4l2_format *format, unsigned int type,
			      unsigned int width, unsigned int height,
			      unsigned int pixelformat)
{
	unsigned int sizeimage;

	memset(format, 0, sizeof(*format));
	format->type = type;

	sizeimage = v4l2_type_is_output(type) ? SOURCE_SIZE_MAX : 0;

	if (v4l2_type_is_mplane(type)) {
		format->fmt.pix_mp.width = width;
		format->fmt.pix_mp.height = height;
		format->fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
		format->fmt.pix_mp.pixelformat = pixelformat;
	} else {
		format->fmt.pix.width = width;
		format->fmt.pix.height = height;
		format->fmt.pix.sizeimage = sizeimage;
		format->fmt.pix.pixelformat = pixelformat;
	}
}

static void v4l2_setup_format_sizeimage(struct v4l2_format *format,
					unsigned int type,
					unsigned int width, unsigned int height,
					unsigned int pixelformat,
					unsigned int sizeimage)
{
	memset(format, 0, sizeof(*format));
	format->type = type;

	if (v4l2_type_is_mplane(type)) {
		format->fmt.pix_mp.width = width;
		format->fmt.pix_mp.height = height;
		format->fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
		format->fmt.pix_mp.pixelformat = pixelformat;
	} else {
		format->fmt.pix.width = width;
		format->fmt.pix.height = height;
		format->fmt.pix.sizeimage = sizeimage;
		format->fmt.pix.pixelformat = pixelformat;
	}
}

bool v4l2_find_format(int video_fd, unsigned int type, unsigned int pixelformat)
{
	struct v4l2_fmtdesc fmtdesc;
	int rc;

	memset(&fmtdesc, 0, sizeof(fmtdesc));
	fmtdesc.type = type;
	fmtdesc.index = 0;

	do {
		rc = ioctl(video_fd, VIDIOC_ENUM_FMT, &fmtdesc);
		if (rc < 0)
			break;

		if (fmtdesc.pixelformat == pixelformat)
			return true;

		fmtdesc.index++;
	} while (rc >= 0);

	return false;
}

int v4l2_try_format(int video_fd, unsigned int type, unsigned int width,
		    unsigned int height, unsigned int pixelformat)
{
	struct v4l2_format format;
	int rc;

	v4l2_setup_format(&format, type, width, height, pixelformat);

	rc = ioctl(video_fd, VIDIOC_TRY_FMT, &format);
	if (rc < 0) {
		request_log("Unable to try format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_set_format(int video_fd, unsigned int type, unsigned int pixelformat,
		    unsigned int width, unsigned int height)
{
	struct v4l2_format format;
	int rc;

	v4l2_setup_format(&format, type, width, height, pixelformat);

	rc = ioctl(video_fd, VIDIOC_S_FMT, &format);
	if (rc < 0) {
		request_log("Unable to set format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_set_format_sizeimage(int video_fd, unsigned int type,
			      unsigned int pixelformat,
			      unsigned int width, unsigned int height,
			      unsigned int sizeimage)
{
	struct v4l2_format format;
	int rc;

	if (sizeimage == 0)
		return v4l2_set_format(video_fd, type, pixelformat, width, height);

	v4l2_setup_format_sizeimage(&format, type, width, height, pixelformat,
				    sizeimage);

	rc = ioctl(video_fd, VIDIOC_S_FMT, &format);
	if (rc < 0) {
		request_log("Unable to set format (sizeimage=%u) for type %d: %s\n",
			    sizeimage, type, strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_get_format(int video_fd, unsigned int type, unsigned int *width,
		    unsigned int *height, unsigned int *bytesperline,
		    unsigned int *sizes, unsigned int *planes_count)
{
	struct v4l2_format format;
	unsigned int count;
	unsigned int i;
	int rc;

	memset(&format, 0, sizeof(format));
	format.type = type;

	rc = ioctl(video_fd, VIDIOC_G_FMT, &format);
	if (rc < 0) {
		request_log("Unable to get format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	if (v4l2_type_is_mplane(type)) {
		count = format.fmt.pix_mp.num_planes;

		if (width != NULL)
			*width = format.fmt.pix_mp.width;

		if (height != NULL)
			*height = format.fmt.pix_mp.height;

		if (planes_count != NULL)
			if (*planes_count > 0 && *planes_count < count)
				count = *planes_count;

		if (bytesperline != NULL)
			for (i = 0; i < count; i++)
				bytesperline[i] =
					format.fmt.pix_mp.plane_fmt[i].bytesperline;

		if (sizes != NULL)
			for (i = 0; i < count; i++)
				sizes[i] = format.fmt.pix_mp.plane_fmt[i].sizeimage;

		if (planes_count != NULL)
			*planes_count = count;
	} else {
		if (width != NULL)
			*width = format.fmt.pix.width;

		if (height != NULL)
			*height = format.fmt.pix.height;

		if (bytesperline != NULL)
			bytesperline[0] = format.fmt.pix.bytesperline;

		if (sizes != NULL)
			sizes[0] = format.fmt.pix.sizeimage;

		if (planes_count != NULL)
			*planes_count = 1;
	}

	return 0;
}

int v4l2_create_buffers(int video_fd, unsigned int type,
			unsigned int buffers_count, unsigned int *index_base)
{
	struct v4l2_create_buffers buffers;
	int rc;

	memset(&buffers, 0, sizeof(buffers));
	buffers.format.type = type;
	buffers.memory = V4L2_MEMORY_MMAP;
	buffers.count = buffers_count;

	rc = ioctl(video_fd, VIDIOC_G_FMT, &buffers.format);
	if (rc < 0) {
		request_log("Unable to get format for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	rc = ioctl(video_fd, VIDIOC_CREATE_BUFS, &buffers);
	if (rc < 0) {
		request_log("Unable to create buffer for type %d: %s\n", type,
			    strerror(errno));
		return -1;
	}

	if (index_base != NULL)
		*index_base = buffers.index;

	return 0;
}

int v4l2_query_buffer(int video_fd, unsigned int type, unsigned int index,
		      unsigned int *lengths, unsigned int *offsets,
		      unsigned int buffers_count)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	unsigned int i;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	rc = ioctl(video_fd, VIDIOC_QUERYBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to query buffer: %s\n", strerror(errno));
		return -1;
	}

	if (v4l2_type_is_mplane(type)) {
		if (lengths != NULL)
			for (i = 0; i < buffer.length; i++)
				lengths[i] = buffer.m.planes[i].length;

		if (offsets != NULL)
			for (i = 0; i < buffer.length; i++)
				offsets[i] = buffer.m.planes[i].m.mem_offset;
	} else {
		if (lengths != NULL)
			lengths[0] = buffer.length;

		if (offsets != NULL)
			offsets[0] = buffer.m.offset;
	}

	return 0;
}

int v4l2_request_buffers(int video_fd, unsigned int type,
			 unsigned int buffers_count)
{
	struct v4l2_requestbuffers buffers;
	int rc;

	memset(&buffers, 0, sizeof(buffers));
	buffers.type = type;
	buffers.memory = V4L2_MEMORY_MMAP;
	buffers.count = buffers_count;

	rc = ioctl(video_fd, VIDIOC_REQBUFS, &buffers);
	if (rc < 0) {
		request_log("Unable to request buffers: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_queue_buffer(int video_fd, int request_fd, unsigned int type,
		      struct timeval *timestamp, unsigned int index,
		      unsigned int size, unsigned int buffers_count)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	unsigned int i;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	for (i = 0; i < buffers_count; i++)
		if (v4l2_type_is_mplane(type))
			buffer.m.planes[i].bytesused = size;
		else
			buffer.bytesused = size;

	if (request_fd >= 0) {
		buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = request_fd;
	}

	if (timestamp != NULL)
		buffer.timestamp = *timestamp;

	rc = ioctl(video_fd, VIDIOC_QBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to queue buffer: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_dequeue_buffer(int video_fd, int request_fd, unsigned int type,
			unsigned int index, unsigned int buffers_count)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	if (request_fd >= 0) {
		buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = request_fd;
	}

	rc = ioctl(video_fd, VIDIOC_DQBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to dequeue buffer: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_queue_buffer_timestamped(int video_fd, int request_fd,
				  unsigned int type, unsigned int index,
				  unsigned int buffers_count,
				  const unsigned int *bytesused,
				  uint64_t timestamp_ns)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	unsigned int i;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	for (i = 0; i < buffers_count; i++) {
		unsigned int used = bytesused != NULL ? bytesused[i] : 0;

		if (v4l2_type_is_mplane(type))
			buffer.m.planes[i].bytesused = used;
		else
			buffer.bytesused = used;
	}

	/*
	 * V4L2 timestamps are nanoseconds in vb2_buffer.timestamp, which is
	 * the domain vb2_find_buffer() matches H.264 encode reference_ts
	 * against.  The hantro queues advertise V4L2_BUF_FLAG_TIMESTAMP_COPY,
	 * so the value handed in here is copied verbatim to the peer queue's
	 * buffer.
	 */
	buffer.timestamp.tv_sec = (time_t)(timestamp_ns / 1000000000ULL);
	buffer.timestamp.tv_usec =
		(suseconds_t)((timestamp_ns % 1000000000ULL) / 1000);

	if (request_fd >= 0) {
		buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
		buffer.request_fd = request_fd;
	}

	rc = ioctl(video_fd, VIDIOC_QBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to queue buffer (index %u): %s\n", index,
			    strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_dequeue_buffer_info(int video_fd, unsigned int type,
			     unsigned int index, unsigned int buffers_count,
			     unsigned int *bytesused, uint64_t *timestamp_ns)
{
	struct v4l2_plane planes[buffers_count];
	struct v4l2_buffer buffer;
	unsigned int i;
	int rc;

	memset(planes, 0, sizeof(planes));
	memset(&buffer, 0, sizeof(buffer));

	buffer.type = type;
	buffer.memory = V4L2_MEMORY_MMAP;
	buffer.index = index;
	buffer.length = buffers_count;
	buffer.m.planes = planes;

	rc = ioctl(video_fd, VIDIOC_DQBUF, &buffer);
	if (rc < 0) {
		request_log("Unable to dequeue buffer (index %u): %s\n", index,
			    strerror(errno));
		return -1;
	}

	if (bytesused != NULL) {
		if (v4l2_type_is_mplane(type)) {
			for (i = 0; i < buffers_count; i++)
				bytesused[i] = buffer.m.planes[i].bytesused;
		} else {
			bytesused[0] = buffer.bytesused;
		}
	}

	if (timestamp_ns != NULL)
		*timestamp_ns = (uint64_t)buffer.timestamp.tv_sec * 1000000000ULL +
				(uint64_t)buffer.timestamp.tv_usec * 1000;

	return 0;
}

int v4l2_export_buffer(int video_fd, unsigned int type, unsigned int index,
		       unsigned int flags, int *export_fds,
		       unsigned int export_fds_count)
{
	struct v4l2_exportbuffer exportbuffer;
	unsigned int i;
	int rc;

	for (i = 0; i < export_fds_count; i++) {
		memset(&exportbuffer, 0, sizeof(exportbuffer));
		exportbuffer.type = type;
		exportbuffer.index = index;
		exportbuffer.plane = i;
		exportbuffer.flags = flags;

		rc = ioctl(video_fd, VIDIOC_EXPBUF, &exportbuffer);
		if (rc < 0) {
			request_log("Unable to export buffer: %s\n",
				    strerror(errno));
			return -1;
		}

		export_fds[i] = exportbuffer.fd;
	}

	return 0;
}

static int v4l2_ioctl_controls(int video_fd, int request_fd, unsigned long ioc,
			       struct v4l2_ext_control *control_array,
			       unsigned int num_controls)
{
	struct v4l2_ext_controls controls;

	memset(&controls, 0, sizeof(controls));

	controls.controls = control_array;
	controls.count = num_controls;

	if (request_fd >= 0) {
		controls.which = V4L2_CTRL_WHICH_REQUEST_VAL;
		controls.request_fd = request_fd;
	}

	return ioctl(video_fd, ioc, &controls);
}

int v4l2_get_controls(int video_fd, int request_fd,
		      struct v4l2_ext_control *control_array,
		      unsigned int num_controls)
{
	int rc;

	rc = v4l2_ioctl_controls(video_fd, request_fd, VIDIOC_G_EXT_CTRLS,
				 control_array, num_controls);
	if (rc < 0) {
		/*
		 * EACCES on G_EXT_CTRLS for request fds is the normal case on
		 * this hantro rig — the kernel doesn't allow readback through
		 * the request_fd. Caller (h264.c) tracks this with a one-time
		 * "V4L2 readback unavailable" announcement. Suppress per-call
		 * noise to keep the log signal-to-noise high.
		 */
		if (errno != EACCES)
			request_log("Unable to get control(s): %s\n",
				    strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_set_controls(int video_fd, int request_fd,
		      struct v4l2_ext_control *control_array,
		      unsigned int num_controls)
{
	struct v4l2_ext_controls controls;
	int rc;

	memset(&controls, 0, sizeof(controls));
	controls.controls = control_array;
	controls.count = num_controls;
	if (request_fd >= 0) {
		controls.which = V4L2_CTRL_WHICH_REQUEST_VAL;
		controls.request_fd = request_fd;
	}

	rc = ioctl(video_fd, VIDIOC_S_EXT_CTRLS, &controls);
	if (rc < 0) {
		/* error_idx is the index of the first failing control;
		 * if it equals count, the ioctl itself failed (not a
		 * specific control payload).  Useful for triaging
		 * which V4L2_CID_STATELESS_* the kernel rejected. */
		if (controls.error_idx < num_controls)
			request_log("Unable to set control(s): %s "
				    "(error_idx=%u/%u failing_ctrl_id=0x%x size=%u)\n",
				    strerror(errno),
				    controls.error_idx, controls.count,
				    control_array[controls.error_idx].id,
				    control_array[controls.error_idx].size);
		else
			request_log("Unable to set control(s): %s "
				    "(error_idx=%u/%u ioctl-level)\n",
				    strerror(errno),
				    controls.error_idx, controls.count);
		return -1;
	}

	return 0;
}

int v4l2_set_control(int video_fd, int request_fd, unsigned int id, void *data,
		     unsigned int size)
{
	struct v4l2_ext_control control;

	memset(&control, 0, sizeof(control));

	control.id = id;
	control.ptr = data;
	control.size = size;

	return v4l2_set_controls(video_fd, request_fd, &control, 1);
}

int v4l2_set_stream(int video_fd, unsigned int type, bool enable)
{
	enum v4l2_buf_type buf_type = type;
	int rc;

	rc = ioctl(video_fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF,
		   &buf_type);
	if (rc < 0) {
		request_log("Unable to %sable stream: %s\n",
			    enable ? "en" : "dis", strerror(errno));
		return -1;
	}

	return 0;
}

int v4l2_query_ext_ctrl(int video_fd, unsigned int id,
			struct v4l2_query_ext_ctrl *qec)
{
	struct v4l2_query_ext_ctrl local;
	struct v4l2_query_ext_ctrl *target = qec ? qec : &local;
	int rc;

	memset(target, 0, sizeof(*target));
	target->id = id;

	rc = ioctl(video_fd, VIDIOC_QUERY_EXT_CTRL, target);
	if (rc < 0)
		return -1;

	return 0;
}

int v4l2_query_menu(int video_fd, unsigned int id, unsigned int index,
		    struct v4l2_querymenu *qm)
{
	int rc;

	if (qm == NULL)
		return -1;

	memset(qm, 0, sizeof(*qm));
	qm->id = id;
	qm->index = index;

	rc = ioctl(video_fd, VIDIOC_QUERYMENU, qm);
	if (rc < 0)
		return -1;

	return 0;
}

bool v4l2_ctrl_menu_has_value(int video_fd, unsigned int id,
			      unsigned int value)
{
	struct v4l2_query_ext_ctrl qec;
	struct v4l2_querymenu qm;
	long long i;

	if (v4l2_query_ext_ctrl(video_fd, id, &qec) < 0)
		return false;

	if (qec.type != V4L2_CTRL_TYPE_MENU &&
	    qec.type != V4L2_CTRL_TYPE_INTEGER_MENU)
		return false;

	for (i = qec.minimum; i <= qec.maximum; i += qec.step ? qec.step : 1) {
		if (v4l2_query_menu(video_fd, id, (unsigned int)i, &qm) < 0)
			continue;
		if ((unsigned int)i == value)
			return true;
	}

	return false;
}
