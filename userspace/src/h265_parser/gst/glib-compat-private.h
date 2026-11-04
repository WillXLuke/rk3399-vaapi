/* Stub for <gst/glib-compat-private.h>.
 * In upstream GStreamer this provides backwards-compat shims for older
 * GLib versions (g_memdup2 polyfill being the load-bearing one).
 * Our gst_compat.h already defines g_memdup2 as a static inline, so
 * we just include the shim.
 */
#ifndef LIBVA_V4L2_REQUEST_FOURIER_GLIB_COMPAT_PRIVATE_STUB
#define LIBVA_V4L2_REQUEST_FOURIER_GLIB_COMPAT_PRIVATE_STUB
#include "gst_compat.h"
#endif
