/*
 * gst_compat.c — GArray implementation for the vendored GStreamer parser.
 *
 * Scope: minimal subset of GArray API exercised by gsth265parser.c
 * (g_array_new, g_array_sized_new, g_array_append_vals + the
 * g_array_append_val macro, g_array_index macro, g_array_set_size,
 * g_array_set_clear_func, g_array_free, g_array_unref).
 *
 * Non-thread-safe (matches GArray's documented semantics — GArray is
 * not thread-safe in upstream GLib either, callers must serialize).
 *
 * License: MIT (matches backend's COPYING.MIT).
 */

#include "gst_compat.h"

/* ===== internal helpers ===== */

static gboolean
garray_grow(GArray *array, guint new_capacity)
{
    if (new_capacity <= array->capacity)
        return TRUE;

    /* round up to next power of two for amortized O(1) growth */
    guint cap = array->capacity > 0 ? array->capacity : 4;
    while (cap < new_capacity)
        cap *= 2;

    char *new_data = realloc(array->data, (size_t)cap * array->element_size);
    if (new_data == NULL)
        return FALSE;

    if (array->clear) {
        memset(new_data + (size_t)array->capacity * array->element_size, 0,
               (size_t)(cap - array->capacity) * array->element_size);
    }

    array->data = new_data;
    array->capacity = cap;
    return TRUE;
}

/* ===== public API ===== */

GArray *
g_array_sized_new(gboolean zero_terminated, gboolean clear,
                  guint element_size, guint reserved_size)
{
    /* zero_terminated is GLib-specific (appends a zero-element sentinel
     * for trailing-NULL semantics). The vendored parser does not use it;
     * we ignore the flag. */
    (void)zero_terminated;

    GArray *a = calloc(1, sizeof(GArray));
    if (a == NULL)
        return NULL;

    a->element_size = element_size;
    a->clear = clear;

    if (reserved_size > 0) {
        if (!garray_grow(a, reserved_size)) {
            free(a);
            return NULL;
        }
    }
    return a;
}

GArray *
g_array_new(gboolean zero_terminated, gboolean clear, guint element_size)
{
    return g_array_sized_new(zero_terminated, clear, element_size, 0);
}

GArray *
g_array_set_size(GArray *array, guint length)
{
    if (length > array->capacity) {
        if (!garray_grow(array, length))
            return array;
    }

    if (array->clear_func != NULL && length < array->len) {
        for (guint i = length; i < array->len; i++)
            array->clear_func(array->data + (size_t)i * array->element_size);
    }
    if (array->clear && length > array->len) {
        memset(array->data + (size_t)array->len * array->element_size, 0,
               (size_t)(length - array->len) * array->element_size);
    }
    array->len = length;
    return array;
}

GArray *
g_array_append_vals(GArray *array, gconstpointer data, guint len)
{
    if (len == 0)
        return array;

    if (!garray_grow(array, array->len + len))
        return array;

    memcpy(array->data + (size_t)array->len * array->element_size,
           data, (size_t)len * array->element_size);
    array->len += len;
    return array;
}

void
g_array_set_clear_func(GArray *array, void (*clear_func)(gpointer))
{
    array->clear_func = clear_func;
}

gchar *
g_array_free(GArray *array, gboolean free_segment)
{
    if (array == NULL)
        return NULL;

    if (array->clear_func != NULL) {
        for (guint i = 0; i < array->len; i++)
            array->clear_func(array->data + (size_t)i * array->element_size);
    }

    gchar *data = NULL;
    if (free_segment) {
        free(array->data);
    } else {
        data = array->data;
    }
    free(array);
    return data;
}

GArray *
g_array_unref(GArray *array)
{
    /* simplified to free; the backend never sub-references shared GArrays */
    g_array_free(array, TRUE);
    return NULL;
}
