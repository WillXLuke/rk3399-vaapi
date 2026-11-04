/* Stub for <gst/base/gstbitwriter.h>.
 *
 * The vendored nalutils.c uses GstBitWriter for NAL emulation-prevention
 * byte INSERTION during write-side (encoder) operations. The libva
 * backend never invokes those paths — we only PARSE NAL units, never
 * write them. The functions must still compile + link though, so we
 * stub them with abort() runtime guards: if any future code path
 * accidentally invokes a writer function, we fail-fast instead of
 * silently corrupting.
 *
 * Header surface mirrors upstream gstbitwriter.h minimally — enough
 * for nalutils.c to compile.
 */
#ifndef LIBVA_V4L2_REQUEST_FOURIER_GSTBITWRITER_STUB
#define LIBVA_V4L2_REQUEST_FOURIER_GSTBITWRITER_STUB

#include "gst_compat.h"

typedef struct {
    guint8 *data;
    guint  bit_size;
    guint  bit_capacity;
    gboolean auto_grow;
    gboolean owned;
} GstBitWriter;

static inline void
gst_bit_writer_init(GstBitWriter *bw) { (void)bw; abort(); }
static inline void
gst_bit_writer_init_with_size(GstBitWriter *bw, guint size, gboolean fixed) {
    (void)bw; (void)size; (void)fixed; abort();
}
static inline void
gst_bit_writer_reset(GstBitWriter *bw) { (void)bw; abort(); }
static inline gboolean
gst_bit_writer_put_bits_uint8(GstBitWriter *bw, guint8 value, guint nbits) {
    (void)bw; (void)value; (void)nbits; abort();
}
static inline gboolean
gst_bit_writer_align_bytes(GstBitWriter *bw, guint8 trailing_bit) {
    (void)bw; (void)trailing_bit; abort();
}
static inline guint8 *
gst_bit_writer_get_data(GstBitWriter *bw) { (void)bw; abort(); }
static inline guint
gst_bit_writer_get_size(const GstBitWriter *bw) { (void)bw; abort(); }
static inline guint
gst_bit_writer_reset_and_get_size(GstBitWriter *bw) { (void)bw; abort(); }
static inline guint8 *
gst_bit_writer_reset_and_get_data(GstBitWriter *bw) { (void)bw; abort(); }
static inline gboolean
gst_bit_writer_put_bits_uint16(GstBitWriter *bw, guint16 value, guint nbits) {
    (void)bw; (void)value; (void)nbits; abort();
}
static inline gboolean
gst_bit_writer_put_bits_uint32(GstBitWriter *bw, guint32 value, guint nbits) {
    (void)bw; (void)value; (void)nbits; abort();
}
static inline gboolean
gst_bit_writer_put_bytes(GstBitWriter *bw, const guint8 *data, guint nbytes) {
    (void)bw; (void)data; (void)nbytes; abort();
}

#define GST_BIT_WRITER_BIT_SIZE(bw)  ((bw)->bit_size)
#define GST_BIT_WRITER_DATA(bw)      ((bw)->data)

#endif
