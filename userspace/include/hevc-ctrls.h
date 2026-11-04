/* SPDX-License-Identifier: GPL-2.0 */
/* Fourier-local override: HEVC controls are upstream since linux-media
 * 6.6+, so defer to the kernel's linux/v4l2-controls.h instead of
 * duplicating the struct definitions (duplication causes redefinition
 * errors on newer linux-api-headers). */
#ifndef _LIBVA_V4L2_REQUEST_HEVC_CTRLS_H
#define _LIBVA_V4L2_REQUEST_HEVC_CTRLS_H
#include <linux/v4l2-controls.h>
#endif
