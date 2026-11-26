/*
 * clipboard.c -- The clipboard services (sxw_abi.h, SXW_SVC_CLIPBOARD_SET to
 * _GET): user32's set of named formats copied between guest memory and the
 * platform's clipboard (docs/desktop/CLIPBOARD.md); and the drag's
 * (SXW_SVC_DRAG_REPLY to _GET, docs/desktop/DRAG_AND_DROP.md), whose formats
 * are read the same way from the platform's drag store. Every guest pointer
 * is checked before it is read or written; the formats a set may hold, their
 * names and their size are the device's limits.
 */
#include <string.h>

#include "guest.h"
#include "services.h"

/* Guest memory is mapped at its own addresses in the host. */
static void *at(uint32_t address) {
    return (void *)(uintptr_t)address;
}

/* A name is printable ASCII, 1 to SXW_CLIPBOARD_NAME - 1 bytes, as the
 * device takes it. */
static int read_name(const sxw_process *p, uint32_t address, char *out) {
    for (uint32_t i = 0; i < SXW_CLIPBOARD_NAME; ++i) {
        if (!sxw_guest_range(p, address + i, 1u)) {
            return 0;
        }
        const char c = *(const char *)at(address + i);
        out[i] = c;
        if (c == 0) {
            return i != 0;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
            return 0;
        }
    }
    return 0;
}

uint32_t sxw_clipboard_service(sxw_services *s, uint32_t service, const uint32_t *a) {
    const sxw_process *p = s->process;
    const sxw_clipboard_platform *c = s->clipboard;
    /* The drag's formats are read as the clipboard's, from its own store. */
    if (service == SXW_SVC_DRAG_FORMAT || service == SXW_SVC_DRAG_GET) {
        c = s->dragdata;
        service = service == SXW_SVC_DRAG_FORMAT ? SXW_SVC_CLIPBOARD_FORMAT : SXW_SVC_CLIPBOARD_GET;
    }
    switch (service) {
    case SXW_SVC_DRAG_REPLY:
        if (s->windows == NULL || s->windows->drag_reply == NULL ||
            (a[1] & ~(uint32_t)(SXW_DRAG_EFFECT_COPY | SXW_DRAG_EFFECT_MOVE | SXW_DRAG_EFFECT_LINK)) != 0) {
            return SXW_FAILED;
        }
        return s->windows->drag_reply(s->context, a[0], a[1]) == 0 ? 0u : SXW_FAILED;
    case SXW_SVC_CLIPBOARD_SET: {
        const uint32_t count = a[1];
        if (c == NULL || count > SXW_CLIPBOARD_FORMATS ||
            (count != 0 && !sxw_guest_range(p, a[0], count * (uint32_t)sizeof(sxw_clip_entry)))) {
            return SXW_FAILED;
        }
        static char names[SXW_CLIPBOARD_FORMATS][SXW_CLIPBOARD_NAME];
        sxw_clip_format formats[SXW_CLIPBOARD_FORMATS];
        uint64_t total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            sxw_clip_entry e;
            memcpy(&e, at(a[0] + i * (uint32_t)sizeof(e)), sizeof(e));
            total += e.length;
            if (!read_name(p, e.name, names[i]) || total > SXW_CLIPBOARD_CAPACITY ||
                (e.length != 0 && !sxw_guest_range(p, e.data, e.length))) {
                return SXW_FAILED;
            }
            formats[i].name = names[i];
            formats[i].data = e.length != 0 ? at(e.data) : NULL;
            formats[i].length = e.length;
        }
        return c->set(s->context, formats, count) == 0 ? 0u : SXW_FAILED;
    }
    case SXW_SVC_CLIPBOARD_INFO: {
        uint64_t sequence = 0;
        uint32_t count = 0;
        if (!sxw_guest_range(p, a[0], 12u) || (c != NULL && c->info(s->context, &sequence, &count) != 0)) {
            return SXW_FAILED;
        }
        uint8_t *out = at(a[0]);
        sxw_set32(out, (uint32_t)sequence);
        sxw_set32(out + 4, (uint32_t)(sequence >> 32));
        sxw_set32(out + 8, count);
        return 0;
    }
    case SXW_SVC_CLIPBOARD_FORMAT: {
        char name[SXW_CLIPBOARD_NAME];
        uint32_t length = 0;
        if (c == NULL || !sxw_guest_range(p, a[1], SXW_CLIPBOARD_NAME) || !sxw_guest_range(p, a[2], 4u) ||
            c->format(s->context, a[0], name, &length) != 0) {
            return SXW_FAILED;
        }
        memcpy(at(a[1]), name, SXW_CLIPBOARD_NAME);
        sxw_set32(at(a[2]), length);
        return 0;
    }
    case SXW_SVC_CLIPBOARD_GET: {
        char name[SXW_CLIPBOARD_NAME];
        if (c == NULL || !read_name(p, a[0], name) || !sxw_guest_range(p, a[3], 4u) ||
            (a[2] != 0 && !sxw_guest_range(p, a[1], a[2]))) {
            return SXW_FAILED;
        }
        uint32_t length = 0;
        const long copied = c->get(s->context, name, a[2] != 0 ? at(a[1]) : NULL, a[2], &length);
        if (copied < 0) {
            return SXW_FAILED;
        }
        sxw_set32(at(a[3]), length);
        return (uint32_t)copied;
    }
    default:
        return SXW_FAILED;
    }
}
