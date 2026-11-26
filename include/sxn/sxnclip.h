#pragma once

/* sxnclip: the clipboard's formats (docs/desktop/CLIPBOARD.md) as typed calls
 * over /dev/clipboard, for a SUN program that keeps a set of formats rather
 * than text alone. The first consumer is winhost, which publishes and reads
 * Win32's clipboard formats through them (docs/userland/WIN32.md,
 * "The clipboard").
 *
 * The same calls serve /dev/dragdata, the store a drag carries its formats
 * in (docs/desktop/DRAG_AND_DROP.md): open it with sxn_dragdata_open.
 *
 * Header-only like sxn.h. The handle is a device handle: open it, make the
 * calls, close it, as a copy or a paste does; one kept open can be waited on
 * to follow changes (the device's readable state). Every call returns 0 or
 * the count it says, or a negative status.
 */

#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "sxn/sxn.h"

/* A handle to the clipboard; writing needs `write`. */
static inline long sxn_clipboard_open(int write) {
    return sxn_open_device("clipboard", SXN_DEVICE_READ | (write ? SXN_DEVICE_WRITE : 0u));
}

/* A handle to the drag's store, /dev/dragdata; a source writes, a target
 * reads. Every call below works on it as on the clipboard. */
static inline long sxn_dragdata_open(int write) {
    return sxn_open_device("dragdata", SXN_DEVICE_READ | (write ? SXN_DEVICE_WRITE : 0u));
}

/* The set's sequence, number of formats and text length. Asking also marks
 * the change as seen for this handle. */
static inline long sxn_clipboard_info(long handle, struct savanxp_clipboard_info* info) {
    return sxn_device_control(handle, CLIP_IOC_GET_INFO, (unsigned long)info);
}

/* Replaces the whole set with `count` formats (1..SAVANXP_CLIPBOARD_MAX_FORMATS),
 * or fails leaving the old one; count 0 empties it. */
static inline long sxn_clipboard_set(long handle, const struct savanxp_clipboard_entry* entries, uint32_t count) {
    if (count == 0) {
        return sxn_device_control(handle, CLIP_IOC_CLEAR, 0);
    }
    struct savanxp_clipboard_set request;
    request.entries = (uint64_t)(uintptr_t)entries;
    request.count = count;
    request.reserved0 = 0;
    return sxn_device_control(handle, CLIP_IOC_SET_FORMATS, (unsigned long)&request);
}

/* Up to `capacity` bytes of the format called `name` into `buffer`; its whole
 * length in *length. The bytes copied, or -ENOENT when the set lacks it. */
static inline long sxn_clipboard_get(long handle, const char* name, void* buffer, uint32_t capacity,
                                     uint32_t* length) {
    struct savanxp_clipboard_get request;
    unsigned i = 0;
    for (; name[i] != 0 && i + 1 < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++i) {
        request.name[i] = name[i];
    }
    for (; i < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++i) {
        request.name[i] = 0;
    }
    request.buffer = (uint64_t)(uintptr_t)buffer;
    request.capacity = capacity;
    request.length = 0;
    const long copied = sxn_device_control(handle, CLIP_IOC_GET_FORMAT, (unsigned long)&request);
    if (copied >= 0 && length != 0) {
        *length = request.length;
    }
    return copied;
}

/* The name (SAVANXP_CLIPBOARD_NAME_CAPACITY bytes, NUL-terminated) and
 * length of the format at `index`, in the writer's order; -ENOENT past the
 * last. */
static inline long sxn_clipboard_format(long handle, uint32_t index, char* name, uint32_t* length) {
    struct savanxp_clipboard_format_info request;
    request.index = index;
    request.length = 0;
    const long status = sxn_device_control(handle, CLIP_IOC_ENUM_FORMAT, (unsigned long)&request);
    if (status == 0) {
        for (unsigned i = 0; i < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++i) {
            name[i] = request.name[i];
        }
        if (length != 0) {
            *length = request.length;
        }
    }
    return status;
}
