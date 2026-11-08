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
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <ctime>

extern "C" {
#include <linux/videodev2.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

// RK3399 is a 2xA72 + 4xA53 big.LITTLE part.  The scaling governor often
// leaves a conversion thread on an A53 (and the A72s at 600 MHz) even though
// this is the only CPU-heavy part of the encode.  For the duration of the
// conversion only, prefer the highest-capacity CPUs the caller's affinity
// already allows; restore the caller's mask afterwards.  If the caller pinned
// itself to little cores, CPU_COUNT(&target) is zero and nothing is changed.
class ScopedBigCoreAffinity {
public:
    ScopedBigCoreAffinity()
    {
#if defined(__aarch64__)
        if (sched_getaffinity(0, sizeof(old_), &old_) != 0)
            return;
        const cpu_set_t& big = big_cpus();
        if (CPU_COUNT(&big) == 0)
            return;

        CPU_ZERO(&target_);
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &old_) && CPU_ISSET(i, &big))
                CPU_SET(i, &target_);
        }
        if (CPU_COUNT(&target_) == 0 || CPU_COUNT(&target_) == CPU_COUNT(&old_))
            return;
        if (sched_setaffinity(0, sizeof(target_), &target_) == 0)
            changed_ = true;
#endif
    }

    ~ScopedBigCoreAffinity()
    {
#if defined(__aarch64__)
        if (changed_)
            sched_setaffinity(0, sizeof(old_), &old_);
#endif
    }

private:
#if defined(__aarch64__)
    static const cpu_set_t& big_cpus()
    {
        static const cpu_set_t set = [] {
            cpu_set_t result;
            CPU_ZERO(&result);
            const long cpus = sysconf(_SC_NPROCESSORS_CONF);
            int max_capacity = 0;
            for (long i = 0; i < cpus; i++) {
                char path[128];
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
                FILE* f = fopen(path, "r");
                if (!f)
                    continue;
                int capacity = 0;
                if (fscanf(f, "%d", &capacity) == 1 && capacity > max_capacity)
                    max_capacity = capacity;
                fclose(f);
            }
            if (max_capacity <= 0)
                return result;
            for (long i = 0; i < cpus; i++) {
                char path[128];
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
                FILE* f = fopen(path, "r");
                if (!f)
                    continue;
                int capacity = 0;
                if (fscanf(f, "%d", &capacity) == 1 && capacity == max_capacity)
                    CPU_SET(i, &result);
                fclose(f);
            }
            return result;
        }();
        return set;
    }

    cpu_set_t old_ {};
    cpu_set_t target_ {};
    bool changed_ = false;
#endif
};

uint8_t clamp8(int value)
{
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

#if defined(__aarch64__)
// Same-size RGB32 -> NV12.  Four pixels are enough to fill a NEON lane group
// and the arithmetic is exactly the scalar BT.709 limited-range formula below,
// so this path is bit-identical while doing 16 pixels per iteration.
inline uint8x8_t rgb_y8(uint8x8_t r8, uint8x8_t g8, uint8x8_t b8)
{
    uint16x8_t y = vmovl_u8(r8);
    y = vmulq_n_u16(y, 47);
    y = vmlaq_n_u16(y, vmovl_u8(g8), 157);
    y = vmlaq_n_u16(y, vmovl_u8(b8), 16);
    y = vaddq_u16(y, vdupq_n_u16(128));
    y = vshrq_n_u16(y, 8);
    y = vaddq_u16(y, vdupq_n_u16(16));
    return vmovn_u16(y);
}

inline void rgb_uv8(uint8x8_t r8, uint8x8_t g8, uint8x8_t b8, uint8x8_t& u8, uint8x8_t& v8)
{
    int16x8_t r = vreinterpretq_s16_u16(vmovl_u8(r8));
    int16x8_t g = vreinterpretq_s16_u16(vmovl_u8(g8));
    int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(b8));

    int16x8_t u = vmulq_n_s16(r, -26);
    u = vmlaq_n_s16(u, g, -87);
    u = vmlaq_n_s16(u, b, 112);
    u = vshrq_n_s16(vaddq_s16(u, vdupq_n_s16(128)), 8);
    u8 = vqmovun_s16(vaddq_s16(u, vdupq_n_s16(128)));

    int16x8_t v = vmulq_n_s16(r, 112);
    v = vmlaq_n_s16(v, g, -102);
    v = vmlaq_n_s16(v, b, -10);
    v = vshrq_n_s16(vaddq_s16(v, vdupq_n_s16(128)), 8);
    v8 = vqmovun_s16(vaddq_s16(v, vdupq_n_s16(128)));
}

void rgb_to_nv12_neon(const uint8_t* src, size_t src_pitch, uint8_t* dst_y, uint8_t* dst_uv, unsigned w,
    unsigned h, bool rgba)
{
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* row = src + static_cast<size_t>(y) * src_pitch;
        if (y + 2 < h)
            __builtin_prefetch(row + 2 * src_pitch, 0, 3);
        uint8_t* dy = dst_y + static_cast<size_t>(y) * w;
        unsigned x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x8x4_t a = vld4_u8(row + static_cast<size_t>(x) * 4);
            uint8x8x4_t b = vld4_u8(row + static_cast<size_t>(x + 8) * 4);
            uint8x8_t ra = rgba ? a.val[0] : a.val[2];
            uint8x8_t ba = rgba ? a.val[2] : a.val[0];
            uint8x8_t rb = rgba ? b.val[0] : b.val[2];
            uint8x8_t bb = rgba ? b.val[2] : b.val[0];
            vst1_u8(dy + x, rgb_y8(ra, a.val[1], ba));
            vst1_u8(dy + x + 8, rgb_y8(rb, b.val[1], bb));
        }
        for (; x < w; x++) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            const int r = rgba ? p[0] : p[2];
            const int g = p[1];
            const int b = rgba ? p[2] : p[0];
            dy[x] = clamp8(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
        }
    }

    for (unsigned y = 0; y < h / 2; y++) {
        const uint8_t* row = src + static_cast<size_t>(2 * y) * src_pitch;
        if (2 * y + 4 < h)
            __builtin_prefetch(row + 4 * src_pitch, 0, 3);
        uint8_t* duv = dst_uv + static_cast<size_t>(y) * w;
        unsigned x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x8x4_t a = vld4_u8(row + static_cast<size_t>(x) * 4);
            uint8x8x4_t b = vld4_u8(row + static_cast<size_t>(x + 8) * 4);
            uint8x8_t ua, va, ub, vb;
            rgb_uv8(rgba ? a.val[0] : a.val[2], a.val[1], rgba ? a.val[2] : a.val[0], ua, va);
            rgb_uv8(rgba ? b.val[0] : b.val[2], b.val[1], rgba ? b.val[2] : b.val[0], ub, vb);
            uint8x8x2_t ue = vuzp_u8(ua, ub);
            uint8x8x2_t ve = vuzp_u8(va, vb);
            uint8x8x2_t uv = { ue.val[0], ve.val[0] };
            // x counts luma pixels; 16 luma pixels are 8 chroma pairs, i.e.
            // 16 bytes in the NV12 chroma row.
            vst2_u8(duv + x, uv);
        }
        for (unsigned cx = x / 2; cx < w / 2; cx++) {
            const uint8_t* p = row + static_cast<size_t>(2 * cx) * 4;
            const int r = rgba ? p[0] : p[2];
            const int g = p[1];
            const int b = rgba ? p[2] : p[0];
            duv[2 * cx] = clamp8(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            duv[2 * cx + 1] = clamp8(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}
#endif

// Packed RGB32 to NV12, BT.709 limited range: the color a hardware session
// declares, so the encoder's stream needs no re-tagging. Chroma is point
// sampled, which screen content carries without visible loss.
void rgb_to_nv12_impl(const uint8_t* src, unsigned iw, unsigned ih, size_t src_pitch, bool rgba, uint8_t* dst,
    unsigned ow, unsigned oh)
{
    uint8_t* dst_y = dst;
    uint8_t* dst_uv = dst + static_cast<size_t>(ow) * oh;

    const bool same_size = (iw == ow && ih == oh);
#if defined(__aarch64__)
    if (same_size && (ow % 16) == 0 && (oh % 2) == 0) {
        rgb_to_nv12_neon(src, src_pitch, dst_y, dst_uv, ow, oh, rgba);
        return;
    }
#endif
    for (unsigned y = 0; y < oh; y++) {
        const unsigned sy = same_size ? y : static_cast<unsigned>(static_cast<uint64_t>(y) * ih / oh);
        const uint8_t* row = src + static_cast<size_t>(sy) * src_pitch;
        uint8_t* dy = dst_y + static_cast<size_t>(y) * ow;
        for (unsigned x = 0; x < ow; x++) {
            const unsigned sx = same_size ? x : static_cast<unsigned>(static_cast<uint64_t>(x) * iw / ow);
            const uint8_t* p = row + static_cast<size_t>(sx) * 4;
            const int b = rgba ? p[2] : p[0];
            const int g = p[1];
            const int r = rgba ? p[0] : p[2];
            dy[x] = clamp8(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
        }
    }

    for (unsigned y = 0; y < oh / 2; y++) {
        const unsigned sy = same_size ? 2 * y
                                      : static_cast<unsigned>(static_cast<uint64_t>(2 * y) * ih / oh);
        const uint8_t* row = src + static_cast<size_t>(sy) * src_pitch;
        uint8_t* duv = dst_uv + static_cast<size_t>(y) * ow;
        for (unsigned x = 0; x < ow / 2; x++) {
            const unsigned sx = same_size ? 2 * x : static_cast<unsigned>(static_cast<uint64_t>(2 * x) * iw / ow);
            const uint8_t* p = row + static_cast<size_t>(sx) * 4;
            const int b = rgba ? p[2] : p[0];
            const int g = p[1];
            const int r = rgba ? p[0] : p[2];
            duv[2 * x] = clamp8(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
            duv[2 * x + 1] = clamp8(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
        }
    }
}

void host_rgb_to_nv12(Surface& in, Surface& out)
{
    out.host.resize(static_cast<size_t>(out.width) * out.height * 3 / 2);
    out.host_fourcc = VA_FOURCC_NV12;
    rgb_to_nv12_impl(in.host.data(), in.width, in.height, static_cast<size_t>(in.width) * 4,
        in.host_fourcc == VA_FOURCC_RGBA, out.host.data(), out.width, out.height);
}

} // namespace

void rgb_to_nv12(const uint8_t* src, unsigned iw, unsigned ih, size_t src_pitch, bool rgba, uint8_t* dst,
    unsigned ow, unsigned oh)
{
    ScopedBigCoreAffinity affinity;
    rgb_to_nv12_impl(src, iw, ih, src_pitch, rgba, dst, ow, oh);
}

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
            auto& input = driver_data->surfaces.at(vpp->second.input);
            if (input.host_fourcc == VA_FOURCC_NV12) {
                surface.host = std::move(input.host);
                surface.host_fourcc = VA_FOURCC_NV12;
            } else {
                host_rgb_to_nv12(input, surface);
            }
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
