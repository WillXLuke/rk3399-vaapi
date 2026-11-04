/* Stub for <gst/gst.h> — redirects to the project's gst_compat shim.
 * The vendored GStreamer 1.28.2 H.265 parser was originally built against
 * full GStreamer; we only need the GLib type aliases + memory helpers +
 * macro stubs, all provided by gst_compat.h. Original gst.h would pull
 * in GObject + GstObject + the entire framework, which we don't link.
 */
#ifndef LIBVA_V4L2_REQUEST_FOURIER_GST_H_STUB
#define LIBVA_V4L2_REQUEST_FOURIER_GST_H_STUB
#include "gst_compat.h"
#endif
