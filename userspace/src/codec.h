/*
 * Copyright (C) 2026 claude-noether <claude-noether@reauktion.de>
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

#ifndef _CODEC_H_
#define _CODEC_H_

#include <va/va.h>

/**
 * pixelformat_for_profile - map a VA-API VAProfile to its V4L2 OUTPUT-side
 *                            pixel format FOURCC.
 *
 * @profile: VAProfile enum value as passed to vaCreateConfig.
 *
 * Returns the V4L2_PIX_FMT_* constant that the V4L2 device's OUTPUT_MPLANE
 * (bitstream-input) queue should be set to in order for the kernel
 * stateless decoder to dispatch to the right codec_mode. Used at
 * RequestCreateConfig to populate object_config->pixelformat, and read
 * from there at RequestCreateContext when committing the OUTPUT format
 * to the V4L2 device.
 *
 * Returns 0 for an unhandled profile; caller is expected to either
 * fall back to a safe default or refuse to proceed.
 */
unsigned int pixelformat_for_profile(VAProfile profile);

#endif /* _CODEC_H_ */
