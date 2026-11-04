# Copyright (C) 2019 Bootlin
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sub license, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice (including the
# next paragraph) shall be included in all copies or substantial portions
# of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
# IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
# ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

autoconf_data = configuration_data()
autoconf_data.set('VA_DRIVER_INIT_FUNC', va_driver_init_func)
if get_option('daedalus_v4l2')
	autoconf_data.set('HAVE_DAEDALUS_V4L2', 1)
endif

autoconf = configure_file(
	output: 'autoconfig.h',
	configuration: autoconf_data)

sources = [
	'request.c',
	'object_heap.c',
	'config.c',
	'surface.c',
	'context.c',
	'buffer.c',
	'picture.c',
	'subpicture.c',
	'image.c',
	'encoder.c',
	'utils.c',
	'tiled_yuv.S',
	'video.c',
	'media.c',
	'v4l2.c',
	'mpeg2.c',
	'h264.c',
	'h264_slice_header.c',
	'request_pool.c',
	'cap_pool.c',
	'h265.c',
	'vp8.c',
	'vp9.c',
	'av1.c',
	'codec.c',
	'nv15.c',
	'nv12_col128.c',

	# Vendored GStreamer 1.28.2 H.265 parser + utilities (LGPL v2.1+,
	# see src/h265_parser/gst_compat.h for sourcing notes + per-iter2
	# adaptation strategy).
	'h265_parser/gst_compat.c',
	'h265_parser/gst/base/gstbitreader.c',
	'h265_parser/gst/base/gstbytereader.c',
	'h265_parser/gst/codecparsers/nalutils.c',
	'h265_parser/gst/codecparsers/gsth265parser.c'
]

headers = [
	'request.h',
	'object_heap.h',
	'config.h',
	'surface.h',
	'context.h',
	'buffer.h',
	'picture.h',
	'subpicture.h',
	'image.h',
	'encoder.h',
	'h264_enc_ctrls.h',
	'utils.h',
	'tiled_yuv.h',
	'video.h',
	'media.h',
	'v4l2.h',
	'mpeg2.h',
	'h264.h',
	'h264_slice_header.h',
	'request_pool.h',
	'cap_pool.h',
	'h265.h',
	'vp8.h',
	'vp9.h',
	'codec.h',
	'nv15.h',
	'nv12_col128.h',

	# Internal mirror of Linux 7.0 V4L2 HEVC EXT_SPS_*_RPS UAPI defs
	# (allows building against pre-7.0 linux-api-headers; redundant
	# once the host headers are 7.0+).
	'hevc-ctrls/v4l2-hevc-ext-controls.h',

	# Vendored GStreamer + project shim headers (see sources above).
	'h265_parser/gst_compat.h',
	'h265_parser/gst/gst.h',
	'h265_parser/gst/glib-compat-private.h',
	'h265_parser/gst/base/base-prelude.h',
	'h265_parser/gst/base/gstbitreader.h',
	'h265_parser/gst/base/gstbytereader.h',
	'h265_parser/gst/base/gstbitwriter.h',
	'h265_parser/gst/codecparsers/codecparsers-prelude.h',
	'h265_parser/gst/codecparsers/gsth265parser.h',
	'h265_parser/gst/codecparsers/nalutils.h'
]

includes = [
	include_directories('../include'),
	# Vendored GStreamer parser tree — the parser's #include <gst/base/...>
	# style references resolve here via stub headers that redirect to
	# gst_compat.h.
	include_directories('h265_parser')
]

cflags = [
	'-Wall',
	'-fvisibility=hidden'
]

deps = [
	kernel_headers_dep,
	libva_dep,
	libdrm_dep
]

v4l2_request_drv_video = shared_module('v4l2_request_drv_video',
	name_prefix: '',
	install: true,
	install_dir: join_paths(get_option('libdir'), 'dri'),
	c_args: cflags,
	sources: [ sources, headers, autoconf ],
	include_directories: includes,
	dependencies: deps)
