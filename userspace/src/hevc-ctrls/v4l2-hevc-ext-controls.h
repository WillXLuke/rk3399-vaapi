/*
 * HEVC EXT_SPS_{ST,LT}_RPS UAPI definitions.
 *
 * These used to be vendored here because the host linux-api-headers could
 * be older than Linux 7.0. Mainline Linux 7.0+ (and the 7.3.1 headers this
 * driver is built against) define them in <linux/v4l2-controls.h>, so just
 * use the kernel definitions to avoid redefinition errors.
 */
#pragma once
#include <linux/v4l2-controls.h>
