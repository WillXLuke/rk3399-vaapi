/* Stub for <gst/base/base-prelude.h> — GStreamer base-lib prelude.
 * In upstream GStreamer, this sets up the GstBaseExport macro + GObject
 * boilerplate. We bypass all of that and provide only what our four
 * vendored .c files actually need (gst_compat.h's typedefs).
 *
 * Crucially we also #define GST_BASE_API to nothing so the function
 * declarations in gstbitreader.h / gstbytereader.h drop the
 * dllimport / visibility attribute prefix.
 */
#ifndef LIBVA_V4L2_REQUEST_FOURIER_BASE_PRELUDE_STUB
#define LIBVA_V4L2_REQUEST_FOURIER_BASE_PRELUDE_STUB
#include "gst_compat.h"
#define GST_BASE_API
#endif
