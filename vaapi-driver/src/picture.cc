/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
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

#include "picture.h"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <system_error>
#include <cstdlib>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <va/va.h>
}

#include "context.h"
#include "driver.h"
#include "media.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"

using fourcc = uint32_t;

namespace {

uint8_t clamp8(int value)
{
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// Packed RGB32 to NV12, BT.709 limited range: the color a hardware session
// declares, so the encoder's stream needs no re-tagging. Chroma is point
// sampled, which screen content carries without visible loss.
void host_rgb_to_nv12(Surface& in, Surface& out)
{
    const unsigned iw = in.width, ih = in.height;
    const unsigned ow = out.width, oh = out.height;
    const bool rgba = in.host_fourcc == VA_FOURCC_RGBA;
    const uint8_t* src = in.host.data();

    out.host.resize(static_cast<size_t>(ow) * oh * 3 / 2);
    out.host_fourcc = VA_FOURCC_NV12;
    uint8_t* dst_y = out.host.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(ow) * oh;

    for (unsigned y = 0; y < oh; y++) {
        const unsigned sy = (iw == ow && ih == oh) ? y : static_cast<unsigned>(static_cast<uint64_t>(y) * ih / oh);
        const uint8_t* row = src + static_cast<size_t>(sy) * iw * 4;
        uint8_t* dy = dst_y + static_cast<size_t>(y) * ow;
        for (unsigned x = 0; x < ow; x++) {
            const unsigned sx = (iw == ow) ? x : static_cast<unsigned>(static_cast<uint64_t>(x) * iw / ow);
            const uint8_t* p = row + static_cast<size_t>(sx) * 4;
            const int b = rgba ? p[2] : p[0];
            const int g = p[1];
            const int r = rgba ? p[0] : p[2];
            dy[x] = clamp8(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
        }
    }

    for (unsigned y = 0; y < oh / 2; y++) {
        const unsigned sy = (iw == ow && ih == oh) ? 2 * y
                                                   : static_cast<unsigned>(static_cast<uint64_t>(2 * y) * ih / oh);
        const uint8_t* row = src + static_cast<size_t>(sy) * iw * 4;
        uint8_t* duv = dst_uv + static_cast<size_t>(y) * ow;
        for (unsigned x = 0; x < ow / 2; x++) {
            const unsigned sx = (iw == ow) ? 2 * x : static_cast<unsigned>(static_cast<uint64_t>(2 * x) * iw / ow);
            const uint8_t* p = row + static_cast<size_t>(sx) * 4;
            const int b = rgba ? p[2] : p[0];
            const int g = p[1];
            const int r = rgba ? p[0] : p[2];
            duv[2 * x] = clamp8(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            duv[2 * x + 1] = clamp8(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}

} // namespace

VAStatus beginPicture(VADriverContextP va_context, VAContextID context_id, VASurfaceID surface_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    if (auto vpp = driver_data->vpp_contexts.find(context_id); vpp != driver_data->vpp_contexts.end()) {
        if (!driver_data->surfaces.contains(surface_id)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        vpp->second.output = surface_id;
        vpp->second.input = VA_INVALID_ID;
        driver_data->surfaces.at(surface_id).status = VASurfaceRendering;
        return VA_STATUS_SUCCESS;
    }

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    auto& context = *driver_data->contexts.at(context_id);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    if (surface.status == VASurfaceRendering) {
        error_log(va_context, "begin: surface %u still rendering (capture_done=%d)\n",
            surface_id, surface.capture_done);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    if (context.is_encoder) {
        surface.status = VASurfaceRendering;
        context.render_surface_id = surface_id;
        return VA_STATUS_SUCCESS;
    }

    if (!context.surfaces_initialized) {
        std::vector<VASurfaceID> ids;
        for (auto& [id, s] : driver_data->surfaces) {
            if (s.width == static_cast<unsigned>(context.picture_width)
                && s.height == static_cast<unsigned>(context.picture_height))
                ids.push_back(id);
        }
        try {
            context.initialize_surfaces(ids);
            context.bind_surface(surface_id);
        } catch (std::exception& e) {
            error_log(va_context, "Failed to initialize surfaces: %s\n", e.what());
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    if (context.surfaces_initialized) {
        try {
            context.bind_surface(surface_id);
        } catch (std::exception& e) {
            error_log(va_context, "Failed to bind surface: %s\n", e.what());
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    surface.status = VASurfaceRendering;
    context.render_surface_id = surface_id;

    return VA_STATUS_SUCCESS;
}

VAStatus renderPicture(VADriverContextP va_context, VAContextID context_id, VABufferID* buffers_ids, int buffers_count)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);
    int rc;
    int i;

    if (auto vpp = driver_data->vpp_contexts.find(context_id); vpp != driver_data->vpp_contexts.end()) {
        for (i = 0; i < buffers_count; i++) {
            if (!driver_data->buffers.contains(buffers_ids[i])) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const auto& buffer = driver_data->buffers.at(buffers_ids[i]);
            if (buffer.type == VAProcPipelineParameterBufferType
                && buffer.size >= sizeof(VAProcPipelineParameterBuffer)) {
                const auto* params = reinterpret_cast<const VAProcPipelineParameterBuffer*>(buffer.data.get());
                vpp->second.input = params->surface;
            }
        }
        return VA_STATUS_SUCCESS;
    }

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    const auto& context = *driver_data->contexts.at(context_id);

    if (!driver_data->surfaces.contains(context.render_surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    for (i = 0; i < buffers_count; i++) {
        if (!driver_data->buffers.contains(buffers_ids[i])) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        rc = context.store_buffer(driver_data->buffers.at(buffers_ids[i]));
        if (rc != VA_STATUS_SUCCESS)
            return rc;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus endPicture(VADriverContextP va_context, VAContextID context_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);
    VAStatus status;

    if (auto vpp = driver_data->vpp_contexts.find(context_id); vpp != driver_data->vpp_contexts.end()) {
        if (!driver_data->surfaces.contains(vpp->second.output)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        auto& surface = driver_data->surfaces.at(vpp->second.output);
        if (vpp->second.input != VA_INVALID_ID && driver_data->surfaces.contains(vpp->second.input)) {
            host_rgb_to_nv12(driver_data->surfaces.at(vpp->second.input), surface);
        }
        surface.status = VASurfaceReady;
        vpp->second.output = VA_INVALID_ID;
        vpp->second.input = VA_INVALID_ID;
        return VA_STATUS_SUCCESS;
    }

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    auto& context = *driver_data->contexts.at(context_id);
    if (!driver_data->surfaces.contains(context.render_surface_id))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    auto& surface = driver_data->surfaces.at(context.render_surface_id);

    if (context.is_encoder) {
        VAStatus status = context.encode(context.render_surface_id);
        surface.status = VASurfaceDisplaying;
        context.render_surface_id = VA_INVALID_ID;
        return status;
    }

    gettimeofday(&surface.timestamp, NULL);

    if (context.device.media_fd >= 0) {
        if (surface.request_fd < 0) {
            surface.request_fd = media_request_alloc(context.device.media_fd);
        }

        status = context.set_controls();
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    try {
        surface.source_buffer->get().queue(surface.request_fd, &surface.timestamp, surface.source_size_used);
        surface.destination_buffer->get().queue(-1, nullptr,
            surface.destination_buffer->get().mapping()[0].size());
        surface.source_queued = true;
    } catch (std::system_error& e) {
        error_log(va_context, "Unable to queue buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (surface.request_fd >= 0) {
        try {
            media_request_queue(surface.request_fd);
            media_request_wait_completion(surface.request_fd);
        } catch (std::runtime_error& e) {
            close(surface.request_fd);
            surface.request_fd = -1;
            error_log(va_context, "Failed to process request: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    try {
        surface.destination_buffer->get().dequeue();
        if (surface.source_queued) {
            surface.source_buffer->get().dequeue();
            surface.source_queued = false;
        }
    } catch (std::system_error& e) {
        error_log(va_context, "Unable to dequeue buffer: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }


    if (surface.request_fd >= 0)
        media_request_reinit(surface.request_fd);

    surface.source_size_used = 0;
    surface.capture_done = false;
    surface.status = VASurfaceDisplaying;

    context.render_surface_id = VA_INVALID_ID;
    memset(&surface.params, 0, sizeof(surface.params));

    return VA_STATUS_SUCCESS;
}

VAStatus queryVideoProcPipelineCaps(VADriverContextP va_context, VAContextID context_id, VABufferID* filters,
    unsigned int num_filters, VAProcPipelineCaps* pipeline_caps)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    if (!driver_data->vpp_contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (pipeline_caps == nullptr) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    memset(pipeline_caps, 0, sizeof(*pipeline_caps));

    return VA_STATUS_SUCCESS;
}
