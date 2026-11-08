/*
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2023 Max Schettler <max.schettler@posteo.de>
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

#include "format.h"

#include <algorithm>
#include <stdexcept>

extern "C" {
#include <linux/videodev2.h>

#include <libdrm/drm_fourcc.h>
#include <va/va.h>
}

#ifndef V4L2_PIX_FMT_NV15
#define V4L2_PIX_FMT_NV15 v4l2_fourcc('N', 'V', '1', '5')
#endif

namespace {

BufferLayout nv12_derive_layout(unsigned width, unsigned height)
{
    auto base_size = width * height;
    return BufferLayout {
        { 0, base_size, width, 0 },
        { 0, base_size / 2, width, base_size },
    };
}

// rkvdec writes packed NV15 (10 bits per component, four components in five
// bytes).  VA-API clients expect P010 (16-bit container, 10 significant high
// bits), so the logical layout is P010 even though the V4L2 buffer is NV15.
// copy_surface_to_image() performs the unpack.
BufferLayout p010_derive_layout(unsigned width, unsigned height)
{
    auto y_size = width * height * 2;
    return BufferLayout {
        { 0, y_size, width * 2, 0 },
        { 0, width * height, width * 2, y_size },
    };
}

} // namespace

const std::array<Format, 4> formats = {
    Format { { V4L2_PIX_FMT_NV12, &nv12_derive_layout }, { VA_FOURCC_NV12, VA_RT_FORMAT_YUV420 },
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR } },
    Format { { V4L2_PIX_FMT_NV12M, nullptr }, { VA_FOURCC_NV12, VA_RT_FORMAT_YUV420 },
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR } },
    Format { { V4L2_PIX_FMT_NV15, &p010_derive_layout }, { VA_FOURCC_P010, VA_RT_FORMAT_YUV420_10 },
        { DRM_FORMAT_P010, DRM_FORMAT_MOD_LINEAR } },
    Format { { V4L2_PIX_FMT_P010, &p010_derive_layout }, { VA_FOURCC_P010, VA_RT_FORMAT_YUV420_10 },
        { DRM_FORMAT_P010, DRM_FORMAT_MOD_LINEAR } },
};

const Format& lookup_format(fourcc v4l2_fourcc)
{
    auto it = std::ranges::find_if(formats, [&](auto&& f) { return f.v4l2.format == v4l2_fourcc; });
    if (it == formats.end()) {
        throw std::invalid_argument("Format not specified");
    }
    return *it;
}
