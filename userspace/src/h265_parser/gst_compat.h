/*
 * gst_compat.h — minimal GLib/GStreamer compatibility shim for vendored
 * GStreamer 1.28.2 H.265 parser + bitreader + bytereader + nalutils.
 *
 * Strategy: provide #defines / typedefs for the GLib API surface those
 * 4 vendored files use, so they can compile against libc + libv4l2 only
 * (no glib2 / gst-base linkage). Vendored .c files are NOT modified
 * directly; instead this header is force-included via the Makefile's
 * `-include` flag on the vendored translation units.
 *
 * Coverage scoped to what gsth265parser.c + nalutils.c + gstbitreader.c
 * + gstbytereader.c actually call. Surveyed in
 * ampere-kernel-decoders phase4 step 2 prep — see
 * ~/src/ampere-kernel-decoders/phase4_plan_iter2.md and the survey
 * commit message for the empirical inventory.
 *
 * License: this shim is original work, MIT (matching the backend's
 * COPYING.MIT). The vendored .c files keep their LGPL v2.1+ headers
 * verbatim.
 */

#ifndef LIBVA_V4L2_REQUEST_FOURIER_GST_COMPAT_H
#define LIBVA_V4L2_REQUEST_FOURIER_GST_COMPAT_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== GLib type aliases ===== */

typedef bool          gboolean;
typedef char          gchar;
typedef unsigned char guchar;
typedef int           gint;
typedef int8_t        gint8;
typedef int16_t       gint16;
typedef int32_t       gint32;
typedef int64_t       gint64;
typedef unsigned int  guint;
typedef uint8_t       guint8;
typedef uint16_t      guint16;
typedef uint32_t      guint32;
typedef uint64_t      guint64;
typedef size_t        gsize;
typedef ptrdiff_t     gssize;
typedef void *        gpointer;
typedef const void *  gconstpointer;
typedef double        gdouble;
typedef float         gfloat;

/* GLib's gint64 / guint64 formatting is platform-conditional; for our
 * aarch64 ALARM target we don't need the full G_*_FORMAT machinery, but
 * gstbytereader uses G_GSIZE_FORMAT in a debug-only printf. */
#define G_GSIZE_FORMAT "zu"

#ifndef TRUE
# define TRUE  true
#endif
#ifndef FALSE
# define FALSE false
#endif

/* ===== memory ===== */

#define g_malloc(n)         malloc((size_t)(n))
#define g_malloc0(n)        calloc(1, (size_t)(n))
#define g_realloc(p, n)     realloc((p), (size_t)(n))
/* g_free needs to be addressable (passed as a function-pointer arg by
 * nalutils.c::gst_memory_new_wrapped — even though that call site is
 * dead code we don't invoke, it must compile). Plain `free` is
 * compatible: signature is `void (void *)` either way. */
#define g_free              free
#define g_new(type, n)      ((type *)malloc(sizeof(type) * (size_t)(n)))
#define g_new0(type, n)     ((type *)calloc((size_t)(n), sizeof(type)))
#define g_slice_new(type)   ((type *)malloc(sizeof(type)))
#define g_slice_new0(type)  ((type *)calloc(1, sizeof(type)))
#define g_slice_free(type, p)        free(p)
#define g_slice_free1(size, p)       free(p)
#define g_clear_pointer(pp, freefn) \
        do { freefn(*(pp)); *(pp) = NULL; } while (0)

/* g_memdup2 — GLib's 64-bit-safe memdup, used by gstbytereader. */
static inline gpointer
g_memdup2(gconstpointer mem, gsize byte_size)
{
    if (mem == NULL || byte_size == 0)
        return NULL;
    void *copy = malloc(byte_size);
    if (copy != NULL)
        memcpy(copy, mem, byte_size);
    return copy;
}

/* g_strcmp0 — NULL-safe strcmp. Used by gsth265parser in profile-name lookup. */
static inline int
g_strcmp0(const char *a, const char *b)
{
    if (a == b) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    return strcmp(a, b);
}

/* ===== asserts / return-guards =====
 *
 * Per ampere-kernel-decoders iter2 Phase 2 §"new failure modes" #5:
 * g_assert must NOT abort the process. It becomes a no-op here;
 * malformed bitstream is caught by the explicit parse-result returns
 * the parser already implements.
 *
 * g_return_if_fail / g_return_val_if_fail propagate as the original
 * GLib semantics (early return with optional value). */

#define g_assert(cond)                ((void)0)
#define g_assert_not_reached()        __builtin_unreachable()
#define g_return_if_fail(cond)        do { if (!(cond)) return; } while (0)
#define g_return_val_if_fail(cond, v) do { if (!(cond)) return (v); } while (0)

/* ===== GStreamer logging — no-ops =====
 *
 * The parser is heavy on debug logging. We compile all of it out;
 * the backend's own logging (request_log/error_log) wraps the parser
 * calls and reports parse-failure return codes from there. */

#define GST_DISABLE_GST_DEBUG 1

#define GST_DEBUG_CATEGORY_STATIC(name)
#define GST_DEBUG_CATEGORY_INIT(...) ((void)0)
#define GST_DEBUG_CATEGORY_GET(...)  ((void)0)
#define GST_DEBUG(...)               ((void)0)
#define GST_INFO(...)                ((void)0)
#define GST_WARNING(...)             ((void)0)
#define GST_ERROR(...)               ((void)0)
#define GST_LOG(...)                 ((void)0)
#define GST_FIXME(...)               ((void)0)
#define GST_MEMDUMP(...)             ((void)0)
#define GST_CAT_DEFAULT              (NULL)

/* ===== compiler / language helpers ===== */

#define G_LIKELY(x)        __builtin_expect(!!(x), 1)
#define G_UNLIKELY(x)      __builtin_expect(!!(x), 0)
#define G_GNUC_UNUSED      __attribute__((unused))
#define G_GNUC_INTERNAL
#define G_GNUC_MALLOC      __attribute__((malloc))
#define G_GNUC_NORETURN    __attribute__((noreturn))
#define G_GNUC_DEPRECATED
#define G_GNUC_DEPRECATED_FOR(x)
#define G_GNUC_PURE        __attribute__((pure))
#define G_GNUC_CONST       __attribute__((const))
#define G_GNUC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#define G_BEGIN_DECLS
#define G_END_DECLS
#define G_N_ELEMENTS(arr)  (sizeof(arr) / sizeof((arr)[0]))
#define G_STMT_START       do
#define G_STMT_END         while (0)
#define G_STRINGIFY(x)     G_STRINGIFY_(x)
#define G_STRINGIFY_(x)    #x

/* GStreamer ABI-padding slot count; upstream uses 4 reserved gpointers
 * at the end of public structs for future ABI extension. We replicate
 * the size so struct layout matches what gst_byte_reader_init / friends
 * write into. */
#define GST_PADDING        4
#define GST_PADDING_LARGE  20

/* Public-symbol visibility — backend's shared module uses
 * -fvisibility=hidden, so we don't need to mark anything public from
 * within the vendored parser. The original GST_*_API macros expand to
 * extern + dllimport on Windows; on Linux ELF builds where
 * fvisibility=hidden is active, they would mark public symbols. The
 * vendored functions are never called from outside h265_parser/, so
 * leaving these empty hides them automatically. */
#define GST_API
#define GST_API_EXPORT     extern
#define GST_API_IMPORT     extern

/* ===== Opaque GStreamer pipeline types =====
 *
 * GstBuffer + GstMemory are referenced by encoder-side dead-code
 * functions in gsth265parser.c (gst_h265_parser_insert_sei_hevc).
 * We never call those; declaring them as opaque structs lets the
 * function pointers / declarations compile, and the linker keeps the
 * dead-code .text section even though it's unreachable.
 *
 * If you ever need to actually USE GstBuffer in this tree, replace
 * these opaque decls with the project's own buffer abstraction; do not
 * try to vendor in libgst itself. */

typedef struct _GstBuffer GstBuffer;
typedef struct _GstMemory GstMemory;
typedef struct _GstMapInfo GstMapInfo;  /* opaque — dead-code in gsth265parser SEI insert */

/* GLib min/max constants — dead-code unsigned-overflow guards in
 * gsth265parser.c. */
#define G_MAXUINT8   ((guint8)0xFF)
#define G_MAXUINT16  ((guint16)0xFFFF)
#define G_MAXUINT32  ((guint32)0xFFFFFFFFU)
#define G_MAXUINT64  ((guint64)0xFFFFFFFFFFFFFFFFULL)
#define G_MAXINT8    ((gint8)0x7F)
#define G_MAXINT16   ((gint16)0x7FFF)
#define G_MAXINT32   ((gint32)0x7FFFFFFF)
#define G_MAXINT64   ((gint64)0x7FFFFFFFFFFFFFFFLL)
#define G_MININT8    ((gint8)(-0x80))
#define G_MININT16   ((gint16)(-0x8000))
#define G_MININT32   ((gint32)(-0x80000000))
#define G_MAXSIZE    ((gsize)-1)

/* GLib function-pointer typedefs used by g_list_* APIs (which our
 * gst_compat declares as abort-stubs). They show up in code paths
 * we never invoke but must compile. */
typedef void (*GDestroyNotify)(gpointer data);
typedef int  (*GCompareFunc)(gconstpointer a, gconstpointer b);
typedef int  (*GCompareDataFunc)(gconstpointer a, gconstpointer b, gpointer user_data);

/* GstMapFlags — passed to gst_memory_map / gst_buffer_map. Dead-code. */
#define GST_MAP_READ      (1 << 0)
#define GST_MAP_WRITE     (1 << 1)
#define GST_MAP_READWRITE (GST_MAP_READ | GST_MAP_WRITE)

/* Dead-code stubs for buffer / memory mapping (only referenced by
 * gst_h265_parser_insert_sei_hevc which we never call). The compile
 * needs declarations + addressable functions; abort on call. */
static inline gboolean
gst_memory_map(GstMemory *mem G_GNUC_UNUSED, GstMapInfo *info G_GNUC_UNUSED,
               int flags G_GNUC_UNUSED) { abort(); }
static inline void
gst_memory_unmap(GstMemory *mem G_GNUC_UNUSED, GstMapInfo *info G_GNUC_UNUSED) { abort(); }
static inline gboolean
gst_buffer_map(GstBuffer *buf G_GNUC_UNUSED, GstMapInfo *info G_GNUC_UNUSED,
               int flags G_GNUC_UNUSED) { abort(); }
static inline void
gst_buffer_unmap(GstBuffer *buf G_GNUC_UNUSED, GstMapInfo *info G_GNUC_UNUSED) { abort(); }
static inline GstBuffer *
gst_buffer_new(void) { abort(); }
static inline gboolean
gst_buffer_copy_into(GstBuffer *dst G_GNUC_UNUSED, GstBuffer *src G_GNUC_UNUSED,
                     int flags G_GNUC_UNUSED, gsize offset G_GNUC_UNUSED,
                     gssize size G_GNUC_UNUSED) { abort(); }
static inline void
gst_buffer_append_memory(GstBuffer *buf G_GNUC_UNUSED, GstMemory *mem G_GNUC_UNUSED) { abort(); }
static inline GstMemory *
gst_memory_ref(GstMemory *mem G_GNUC_UNUSED) { abort(); }
static inline void
gst_memory_unref(GstMemory *mem G_GNUC_UNUSED) { abort(); }
static inline GstMemory *
gst_memory_copy(GstMemory *mem G_GNUC_UNUSED, gssize offset G_GNUC_UNUSED, gssize size G_GNUC_UNUSED) { abort(); }
static inline void
gst_clear_buffer(GstBuffer **buf) { *buf = NULL; }
#define GST_IS_BUFFER(b) (false)

/* GstBufferCopyFlags — used only by gst_buffer_copy_into in dead code. */
#define GST_BUFFER_COPY_METADATA  (1 << 0)
#define GST_BUFFER_COPY_MEMORY    (1 << 1)
#define GST_BUFFER_COPY_DEEP      (1 << 2)

/* gst_util_ceil_log2(n) — ceil(log2(n)) for non-zero unsigned n.
 * Used by gsth265parser.c::gst_h265_slice_parse_ref_pic_list_modification.
 * That function is in the slice-header parser which the libva backend
 * does NOT invoke (we only call parse_sps) — but the linker still
 * needs a definition. Provide a real impl: cheaper to compute than to
 * justify a dead-code stub at every call site. */
static inline guint
gst_util_ceil_log2(guint32 n)
{
    if (n <= 1) return 0;
    /* __builtin_clz returns leading zeros for a 32-bit value;
     * 32 - clz(n-1) = bits needed = ceil(log2(n)). */
    return 32 - (guint)__builtin_clz(n - 1);
}

/* GstMapInfo's real definition is in <gst/gstmemory.h>; we need at
 * least enough to make `info->data` / `info->size` compile. */
struct _GstMapInfo {
    GstMemory *memory;
    int        flags;
    guint8    *data;
    gsize      size;
    gsize      maxsize;
    gpointer   user_data[4];
    gpointer   _gst_reserved[GST_PADDING];
};

/* gst_memory_new_wrapped — dead-code stub (nalutils.c calls it from
 * the SEI-insertion path the libva backend never invokes). */
static inline GstMemory *
gst_memory_new_wrapped(int flags, gpointer data, gsize maxsize,
                       gsize offset, gsize size, gpointer user_data,
                       void (*notify)(gpointer))
{
    (void)flags; (void)data; (void)maxsize; (void)offset; (void)size;
    (void)user_data; (void)notify;
    abort();
}

/* ===== byte-order read / write macros =====
 *
 * GStreamer provides these as static-inline functions in
 * <gst/gstutils.h>. We re-implement for aarch64 little-endian; the
 * parser is byte-stream input, so endian-conversion is mechanical.
 * The float / double variants are present in upstream but the parser
 * never invokes them — provide stubs so the address-taking sites in
 * gstbytereader.h's function table compile. */

#define GST_READ_UINT8(data)                                    \
    (*((const guint8 *)(data)))

#define GST_READ_UINT16_LE(data) (                              \
    ((guint16)((const guint8 *)(data))[0])           |          \
    ((guint16)((const guint8 *)(data))[1] << 8))

#define GST_READ_UINT16_BE(data) (                              \
    ((guint16)((const guint8 *)(data))[0] << 8)      |          \
    ((guint16)((const guint8 *)(data))[1]))

#define GST_READ_UINT24_LE(data) (                              \
    ((guint32)((const guint8 *)(data))[0])           |          \
    ((guint32)((const guint8 *)(data))[1] << 8)      |          \
    ((guint32)((const guint8 *)(data))[2] << 16))

#define GST_READ_UINT24_BE(data) (                              \
    ((guint32)((const guint8 *)(data))[0] << 16)     |          \
    ((guint32)((const guint8 *)(data))[1] << 8)      |          \
    ((guint32)((const guint8 *)(data))[2]))

#define GST_READ_UINT32_LE(data) (                              \
    ((guint32)((const guint8 *)(data))[0])           |          \
    ((guint32)((const guint8 *)(data))[1] << 8)      |          \
    ((guint32)((const guint8 *)(data))[2] << 16)     |          \
    ((guint32)((const guint8 *)(data))[3] << 24))

#define GST_READ_UINT32_BE(data) (                              \
    ((guint32)((const guint8 *)(data))[0] << 24)     |          \
    ((guint32)((const guint8 *)(data))[1] << 16)     |          \
    ((guint32)((const guint8 *)(data))[2] << 8)      |          \
    ((guint32)((const guint8 *)(data))[3]))

#define GST_READ_UINT64_LE(data) (                              \
    ((guint64)((const guint8 *)(data))[0])           |          \
    ((guint64)((const guint8 *)(data))[1] << 8)      |          \
    ((guint64)((const guint8 *)(data))[2] << 16)     |          \
    ((guint64)((const guint8 *)(data))[3] << 24)     |          \
    ((guint64)((const guint8 *)(data))[4] << 32)     |          \
    ((guint64)((const guint8 *)(data))[5] << 40)     |          \
    ((guint64)((const guint8 *)(data))[6] << 48)     |          \
    ((guint64)((const guint8 *)(data))[7] << 56))

#define GST_READ_UINT64_BE(data) (                              \
    ((guint64)((const guint8 *)(data))[0] << 56)     |          \
    ((guint64)((const guint8 *)(data))[1] << 48)     |          \
    ((guint64)((const guint8 *)(data))[2] << 40)     |          \
    ((guint64)((const guint8 *)(data))[3] << 32)     |          \
    ((guint64)((const guint8 *)(data))[4] << 24)     |          \
    ((guint64)((const guint8 *)(data))[5] << 16)     |          \
    ((guint64)((const guint8 *)(data))[6] << 8)      |          \
    ((guint64)((const guint8 *)(data))[7]))

/* Float / double readers — dead-code, abort if called. The function
 * table in gstbytereader.h takes the address of the underlying inline
 * which we don't need to be functional, only addressable. */
static inline gfloat
GST_READ_FLOAT_LE(const guint8 *data) { (void)data; abort(); }
static inline gfloat
GST_READ_FLOAT_BE(const guint8 *data) { (void)data; abort(); }
static inline gdouble
GST_READ_DOUBLE_LE(const guint8 *data) { (void)data; abort(); }
static inline gdouble
GST_READ_DOUBLE_BE(const guint8 *data) { (void)data; abort(); }

/* Write side — nalutils.c writes-out SEI bytes (dead path for us but
 * must compile). */
#define GST_WRITE_UINT8(data, val) do {                         \
    ((guint8 *)(data))[0] = (guint8)(val);                      \
} while (0)

#define GST_WRITE_UINT16_BE(data, val) do {                     \
    ((guint8 *)(data))[0] = (guint8)((val) >> 8);               \
    ((guint8 *)(data))[1] = (guint8)((val));                    \
} while (0)

#define GST_WRITE_UINT24_BE(data, val) do {                     \
    ((guint8 *)(data))[0] = (guint8)((val) >> 16);              \
    ((guint8 *)(data))[1] = (guint8)((val) >> 8);               \
    ((guint8 *)(data))[2] = (guint8)((val));                    \
} while (0)

#define GST_WRITE_UINT32_BE(data, val) do {                     \
    ((guint8 *)(data))[0] = (guint8)((val) >> 24);              \
    ((guint8 *)(data))[1] = (guint8)((val) >> 16);              \
    ((guint8 *)(data))[2] = (guint8)((val) >> 8);               \
    ((guint8 *)(data))[3] = (guint8)((val));                    \
} while (0)

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
# define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ===== GArray ===== */

typedef struct {
    char *data;                 /* exposed via g_array_index / GArray->data */
    guint len;                  /* element count */
    guint capacity;             /* allocated element slots */
    guint element_size;
    gboolean clear;             /* zero-fill on grow */
    void (*clear_func)(gpointer);
} GArray;

GArray *g_array_new(gboolean zero_terminated, gboolean clear, guint element_size);
GArray *g_array_sized_new(gboolean zero_terminated, gboolean clear,
                          guint element_size, guint reserved_size);
GArray *g_array_set_size(GArray *array, guint length);
GArray *g_array_append_vals(GArray *array, gconstpointer data, guint len);
void    g_array_set_clear_func(GArray *array, void (*clear_func)(gpointer));
gchar  *g_array_free(GArray *array, gboolean free_segment);
GArray *g_array_unref(GArray *array);

#define g_array_append_val(a, v) g_array_append_vals((a), &(v), 1)
#define g_array_index(a, t, i)   (((t *)(void *)(a)->data)[i])

/* ===== GList — stubs that abort if reached =====
 *
 * Surveyed call sites: gsth265parser.c uses g_list_prepend / g_list_sort /
 * g_list_free_full in code paths the libva backend does not invoke for
 * basic SPS parsing (likely SEI message accumulation). Stub to abort so
 * any future call surfaces immediately rather than silently corrupting. */

/* GList — full struct (not opaque) so callers can do `list->data`.
 * The functions still abort because we never construct a GList. */
typedef struct _GList GList;
struct _GList {
    gpointer data;
    GList   *next;
    GList   *prev;
};

static inline GList *g_list_prepend(GList *list G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) { abort(); }
static inline GList *g_list_sort(GList *list G_GNUC_UNUSED, int (*cmp)(gconstpointer, gconstpointer) G_GNUC_UNUSED) { abort(); }
static inline void g_list_free_full(GList *list G_GNUC_UNUSED, void (*free_func)(gpointer) G_GNUC_UNUSED) { abort(); }

/* ===== g_once_init_enter / g_once_init_leave =====
 *
 * GLib's lazy-init guards. The parser uses these for one-shot static
 * initialization (e.g. profile-name table). Our backend is single-
 * threaded at the parser-init site (driver_init), so we can simplify
 * to a plain run-once gate. */

#define g_once_init_enter(loc)        (*(loc) == 0)
#define g_once_init_leave(loc, val)   (*(loc) = (val))

/* ===== conversions ===== */

#define GINT_TO_POINTER(i) ((gpointer)(uintptr_t)(gint)(i))
#define GPOINTER_TO_INT(p) ((gint)(uintptr_t)(p))

#endif  /* LIBVA_V4L2_REQUEST_FOURIER_GST_COMPAT_H */
