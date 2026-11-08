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

#include "context.h"
#include "format.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va.h>
}

#include "config.h"
#include "driver.h"
#include "h264.h"
#include "h265.h"
#include "encoder.h"
#include "mpeg2.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"
#include "vp8.h"
#ifdef ENABLE_VP9
#include "vp9.h"
#endif

std::set<VAProfile> Context::supported_profiles(const std::vector<V4L2M2MDevice>& devices)
{
    std::set<VAProfile> result;
    for (auto&& device : devices) {
        for (auto&& profile : MPEG2Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : H265Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : H264Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : VP8Context::supported_profiles(device)) {
            result.insert(profile);
        }
#ifdef ENABLE_VP9
        for (auto&& profile : VP9Context::supported_profiles(device)) {
            result.insert(profile);
        }
#endif
    }
    return result;
}

Context* Context::create(DriverData* driver_data, VAProfile profile, VAEntrypoint entrypoint, int picture_width,
    int picture_height, std::span<VASurfaceID> surface_ids)
{
    if (entrypoint == VAEntrypointEncSlice) {
        for (auto&& device : driver_data->devices) {
            if (H264EncContext::supported_profiles(device).contains(profile))
                return new H264EncContext(driver_data, device, profile, picture_width, picture_height);
        }
        throw std::invalid_argument("Unimplemented encode profile");
    }

    for (auto&& device : driver_data->devices) {
        if (MPEG2Context::supported_profiles(device).contains(profile)) {
            return new MPEG2Context(driver_data, device, picture_width, picture_height, surface_ids);
        }
        if (H265Context::supported_profiles(device).contains(profile)) {
            return new H265Context(driver_data, device, profile, picture_width, picture_height, surface_ids);
        }
        if (H264Context::supported_profiles(device).contains(profile)) {
            return new H264Context(driver_data, device, profile, picture_width, picture_height, surface_ids);
        }
        if (VP8Context::supported_profiles(device).contains(profile)) {
            return new VP8Context(driver_data, device, picture_width, picture_height, surface_ids);
        }
#ifdef ENABLE_VP9
        if (VP9Context::supported_profiles(device).contains(profile)) {
            return new VP9Context(driver_data, device, picture_width, picture_height, surface_ids);
        }
#endif
    }

    throw std::invalid_argument("Unimplemented profile");
}

Context::Context(DriverData* driver_data, V4L2M2MDevice& dev, fourcc pixelformat, int picture_width, int picture_height,
    std::span<VASurfaceID> surface_ids, bool defer_surfaces)
    : render_surface_id(VA_INVALID_ID)
    , picture_width(picture_width)
    , picture_height(picture_height)
    , driver_data(driver_data)
    , device(dev)
{
    device.set_format(device.output_buf_type, pixelformat, picture_width, picture_height);

    // Applications such as FFmpeg create the VA context before the surfaces.
    // Defer the capture setup until the first picture in that case.  HEVC also
    // defers so it can prime the SPS at the right bit depth first.
    if (!defer_surfaces && !surface_ids.empty())
        initialize_surfaces(surface_ids);
}

void Context::initialize_surfaces(std::span<VASurfaceID> surface_ids)
{
    if (surfaces_initialized)
        return;
    if (surface_ids.empty())
        throw std::invalid_argument("No surfaces to be created");

    // Applications create surfaces lazily, so the initial span can be smaller
    // than the pool they eventually bind.  FFmpeg's HEVC decoder allocates 20
    // surfaces for a 4:2:0 stream; 20 buffers covers it and cuts 4K10 setup
    // memory by about 90 MB.
    num_buffers = std::max<unsigned>(surface_ids.size(), 20);

    /* The Rockchip driver initializes its internal image format from the
     * current SPS control and refuses to change it once buffers are
     * allocated. Set a matching SPS before requesting the capture buffers,
     * so that per-request SPS controls are accepted. */
    prime_capture(driver_data->surfaces.at(surface_ids[0]));

    unsigned capture_count = createSurfacesDeferred(driver_data, *this, surface_ids, num_buffers);
    unsigned output_count = device.request_buffers(device.output_buf_type, num_buffers);
    num_buffers = std::min(capture_count, output_count);

    for (auto id : surface_ids)
        bind_surface(id);

    device.set_streaming(true);
    surfaces_initialized = true;
}

void Context::prime_capture(const Surface& surface)
{
    v4l2_ctrl_h264_sps sps = {};
    sps.chroma_format_idc = 1;
    sps.pic_width_in_mbs_minus1 = (surface.width + 15) / 16 - 1;
    sps.pic_height_in_map_units_minus1 = (surface.height + 15) / 16 - 1;
    sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
    v4l2_ext_control ctrl = {
        .id = V4L2_CID_STATELESS_H264_SPS,
        .size = sizeof(sps),
        .ptr = &sps,
    };
    device.set_ext_controls(-1, std::span(&ctrl, 1));
}

void Context::bind_surface(VASurfaceID surface_id)
{
    auto& surface = driver_data->surfaces.at(surface_id);
    if (surface.source_buffer.has_value())
        return;
    if (next_buffer_index >= num_buffers)
        throw std::runtime_error("No free V4L2 buffer for surface");

    if (surface.logical_destination_layout.empty()) {
        const auto& driver_format = device.capture_format.fmt.pix_mp;
        const auto& fmt = lookup_format(driver_format.pixelformat);
        if (fmt.v4l2.derive_layout) {
            surface.logical_destination_layout
                = fmt.v4l2.derive_layout(driver_format.width, driver_format.height);
        } else {
            for (unsigned j = 0; j < driver_format.num_planes; j++) {
                surface.logical_destination_layout.push_back({
                    j,
                    driver_format.plane_fmt[j].sizeimage,
                    driver_format.plane_fmt[j].bytesperline,
                    (j > 0) ? (surface.logical_destination_layout[j - 1].offset
                                  + surface.logical_destination_layout[j - 1].size)
                            : 0,
                });
            }
        }
    }

    surface.destination_buffer = std::cref(device.buffer(device.capture_buf_type, next_buffer_index));
    surface.source_buffer = std::cref(device.buffer(device.output_buf_type, next_buffer_index));
    surface.source_size_used = 0;
    next_buffer_index++;
}

Context::~Context()
{
    device.set_streaming(false);
    device.request_buffers(device.capture_buf_type, 0);
}

VAStatus createContext(VADriverContextP va_context, VAConfigID config_id, int picture_width, int picture_height,
    int flags, VASurfaceID* surface_ids, int surfaces_count, VAContextID* context_id)
{
    // FIXME: Should create own V4L2M2MDevice to localize settings?
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    if (!driver_data->configs.contains(config_id)) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    const auto& config = driver_data->configs.at(config_id);

    auto surfaces = std::span(surface_ids, surfaces_count);
    for (auto&& surface : surfaces) {
        if (!driver_data->surfaces.contains(surface)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (config.entrypoint == VAEntrypointVideoProc) {
        // Both maps answer libva with the same opaque ID space, so an ID has to
        // be free in both of them.
        *context_id = smallest_free_key(driver_data->vpp_contexts);
        while (driver_data->contexts.contains(*context_id))
            (*context_id)++;
        driver_data->vpp_contexts.emplace(*context_id, VppContext {});
        return VA_STATUS_SUCCESS;
    }
    try {
        *context_id = smallest_free_key(driver_data->contexts);
        while (driver_data->vpp_contexts.contains(*context_id))
            (*context_id)++;
        auto [context, inserted] = driver_data->contexts.emplace(std::make_pair(
            *context_id, Context::create(driver_data, config.profile, config.entrypoint, picture_width,
                picture_height, surfaces)));
        if (!inserted) {
            error_log(va_context, "Failed to create context\n");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    } catch (std::exception& e) {
        error_log(va_context, "Failed to create context: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus destroyContext(VADriverContextP va_context, VAContextID context_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (auto vpp = driver_data->vpp_contexts.find(context_id); vpp != driver_data->vpp_contexts.end()) {
        driver_data->vpp_contexts.erase(vpp);
        return VA_STATUS_SUCCESS;
    }
    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    driver_data->contexts.erase(context_id);

    return VA_STATUS_SUCCESS;
}
