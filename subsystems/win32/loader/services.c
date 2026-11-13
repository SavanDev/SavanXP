/*
 * services.c -- The gate's services. See services.h and sxw_abi.h.
 */
#include "services.h"

#include "sxw_abi.h"

#define SXW_MAX_STRING 256u   /* bytes of a name the guest passes, with its NUL */
#define SXW_MAX_ADDED 32u     /* modules one LoadLibrary may report */

int sxw_guest_range(const sxw_process *process, uint32_t address, uint32_t length) {
    for (uint32_t i = 0; i < process->region_count; ++i) {
        const sxw_region *r = &process->regions[i];
        if (address >= r->base && address - r->base <= r->size &&
            length <= r->size - (address - r->base)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int read_words(const sxw_process *process, uint32_t address, uint32_t count, uint32_t *out) {
    if (!sxw_guest_range(process, address, 4u * count)) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = get32(p + 4u * i);
    }
    return 1;
}

static int write_word(const sxw_process *process, uint32_t address, uint32_t value) {
    if (!sxw_guest_range(process, address, 4u)) {
        return 0;
    }
    set32((uint8_t *)(uintptr_t)address, value);
    return 1;
}

/* An 8-bit string of the guest, copied with its NUL. 0 when it is not guest
 * memory, empty, or longer than the buffer. */
static int read_ansi(const sxw_process *process, uint32_t address, char *out) {
    for (uint32_t i = 0; i < SXW_MAX_STRING; ++i) {
        if (!sxw_guest_range(process, address + i, 1u)) {
            return 0;
        }
        out[i] = *(const char *)(uintptr_t)(address + i);
        if (out[i] == 0) {
            return i != 0u;
        }
    }
    return 0;
}

/* A UTF-16 string of the guest, as UTF-8 with its NUL. Address 0 is the
 * empty string. 0 when it is not guest memory or does not fit. */
static int read_utf16(const sxw_process *process, uint32_t address, char *out) {
    uint32_t n = 0;
    out[0] = 0;
    if (address == 0u) {
        return 1;
    }
    for (uint32_t i = 0;; ++i) {
        if (!sxw_guest_range(process, address + 2u * i, 2u)) {
            return 0;
        }
        const uint8_t *p = (const uint8_t *)(uintptr_t)(address + 2u * i);
        uint32_t c = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        if (c == 0u) {
            out[n] = 0;
            return 1;
        }
        if (c >= 0xd800u && c <= 0xdfffu) {
            c = '?'; /* surrogates: the first profile has none */
        }
        uint32_t need = c < 0x80u ? 1u : c < 0x800u ? 2u : 3u;
        if (n + need + 1u > SXW_MAX_STRING) {
            return 0;
        }
        if (need == 1u) {
            out[n++] = (char)c;
        } else if (need == 2u) {
            out[n++] = (char)(0xc0u | (c >> 6));
            out[n++] = (char)(0x80u | (c & 0x3fu));
        } else {
            out[n++] = (char)(0xe0u | (c >> 12));
            out[n++] = (char)(0x80u | ((c >> 6) & 0x3fu));
            out[n++] = (char)(0x80u | (c & 0x3fu));
        }
    }
}

static void end_process(sxw_services *s, uint32_t status) {
    s->exit_status = status;
    s->exit_requested = 1;
}

static int stream_of(uint32_t handle) {
    switch (handle) {
    case SXW_HANDLE_STDIN: return 0;
    case SXW_HANDLE_STDOUT: return 1;
    case SXW_HANDLE_STDERR: return 2;
    default: return -1;
    }
}

static void write_text(sxw_services *s, const char *text) {
    uint32_t length = 0;
    while (text[length] != 0) {
        ++length;
    }
    (void)s->write(s->context, 2, text, length);
}

void sxw_services_start(sxw_services *s) {
    s->exit_requested = 0;
}

static uint32_t guest_time(sxw_services *s) {
    return s->fixed_time != 0u ? s->fixed_time : (uint32_t)s->now(s->context);
}

/* Writes the modules a load added, and their count in the low half of the
 * error word. */
static uint32_t report_added(const sxw_process *p, uint32_t buffer, uint32_t capacity, uint32_t error_out,
                             const sxw_start_module *added, uint32_t count) {
    if (count > capacity || !sxw_guest_range(p, buffer, 16u * count)) {
        return 0;
    }
    uint8_t *q = (uint8_t *)(uintptr_t)buffer;
    for (uint32_t i = 0; i < count; ++i) {
        set32(q + 16u * i + 0u, added[i].base);
        set32(q + 16u * i + 4u, added[i].entry);
        set32(q + 16u * i + 8u, added[i].tls_callbacks);
        set32(q + 16u * i + 12u, added[i].flags);
    }
    return write_word(p, error_out, count);
}

static uint32_t save_registry(sxw_services *s) {
    if (!s->registry->dirty || s->save_registry == 0) {
        return SXW_ERROR_SUCCESS;
    }
    size_t length = 0;
    if (sxw_reg_serialize(s->registry, s->registry_text, sizeof(s->registry_text), &length) != 0 ||
        s->save_registry(s->context, s->registry_text, length) != 0) {
        write_text(s, "win32: the registry could not be saved\n");
        return SXW_ERROR_REGISTRY_IO_FAILED;
    }
    s->registry->dirty = 0;
    return SXW_ERROR_SUCCESS;
}

static uint32_t registry_service(sxw_services *s, uint32_t service, const uint32_t *a) {
    sxw_process *p = s->process;
    char name[SXW_MAX_STRING];
    switch (service) {
    case SXW_SVC_REG_OPEN: {
        uint32_t handle = 0;
        uint32_t disposition = 0;
        if (!read_utf16(p, a[1], name) || !sxw_guest_range(p, a[3], 4u) ||
            (a[4] != 0u && !sxw_guest_range(p, a[4], 4u))) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        uint32_t error = sxw_reg_open(s->registry, a[0], name, a[2] != 0u, &handle, &disposition);
        if (error == SXW_ERROR_SUCCESS) {
            write_word(p, a[3], handle);
            if (a[4] != 0u) {
                write_word(p, a[4], disposition);
            }
            error = save_registry(s);
        }
        return error;
    }
    case SXW_SVC_REG_CLOSE:
        return sxw_reg_close(s->registry, a[0]);
    case SXW_SVC_REG_QUERY: {
        uint32_t type = 0;
        uint32_t size = 0;
        uint32_t capacity = 0;
        if (!read_utf16(p, a[1], name) || (a[2] != 0u && !sxw_guest_range(p, a[2], 4u))) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        if (a[4] == 0u) {
            if (a[3] != 0u) {
                return SXW_ERROR_INVALID_PARAMETER;
            }
        } else if (!read_words(p, a[4], 1, &capacity)) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        if (a[3] != 0u && !sxw_guest_range(p, a[3], capacity)) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        uint32_t error = sxw_reg_query(s->registry, a[0], name, &type,
                                       a[3] != 0u ? (uint8_t *)(uintptr_t)a[3] : 0, capacity, &size);
        if (error == SXW_ERROR_SUCCESS || error == SXW_ERROR_MORE_DATA) {
            if (a[2] != 0u) {
                write_word(p, a[2], type);
            }
            if (a[4] != 0u) {
                write_word(p, a[4], size);
            }
        }
        return error;
    }
    case SXW_SVC_REG_SET: {
        if (!read_utf16(p, a[1], name) || (a[4] != 0u && !sxw_guest_range(p, a[3], a[4]))) {
            return SXW_ERROR_INVALID_PARAMETER;
        }
        uint32_t error = sxw_reg_set(s->registry, a[0], name, a[2],
                                     a[4] != 0u ? (const uint8_t *)(uintptr_t)a[3] : 0, a[4]);
        return error == SXW_ERROR_SUCCESS ? save_registry(s) : error;
    }
    default:
        return SXW_ERROR_INVALID_PARAMETER;
    }
}

#define SXW_MAX_RECTS 64u

static uint32_t window_service(sxw_services *s, uint32_t service, const uint32_t *a) {
    sxw_process *p = s->process;
    const sxw_window_platform *w = s->windows;
    char title[SXW_MAX_STRING];
    switch (service) {
    case SXW_SVC_TICKS:
        return w != 0 ? w->ticks(s->context) : 0u;
    case SXW_SVC_DISPLAY: {
        uint32_t width = 0;
        uint32_t height = 0;
        if (w != 0) {
            w->display(s->context, &width, &height);
        }
        return write_word(p, a[0], width) && write_word(p, a[0] + 4u, height) ? 0u : SXW_FAILED;
    }
    case SXW_SVC_WINDOW_CREATE: {
        uint32_t width = a[1];
        uint32_t height = a[2];
        if (w == 0 || !read_utf16(p, a[0], title) || !sxw_guest_range(p, a[3], 8u) || width == 0u ||
            height == 0u || width > 0x4000u || height > 0x4000u || a[5] > SXW_WINDOW_POPUP ||
            (a[5] == SXW_WINDOW_MAIN) != (a[4] == 0u)) {
            return 0;
        }
        uint32_t id = w->create(s->context, title, &width, &height, a[4], a[5], (int32_t)a[6], (int32_t)a[7]);
        if (id != 0u) {
            write_word(p, a[3], width);
            write_word(p, a[3] + 4u, height);
        }
        return id;
    }
    case SXW_SVC_WINDOW_DESTROY:
        if (w != 0) {
            w->destroy(s->context, a[0]);
        }
        return 0;
    case SXW_SVC_WINDOW_TITLE:
        return w != 0 && read_utf16(p, a[1], title) && w->set_title(s->context, a[0], title) == 0 ? 0u
                                                                                                 : SXW_FAILED;
    case SXW_SVC_WINDOW_ICON: {
        uint32_t icon[SXW_ICON_EXTENT * SXW_ICON_EXTENT];
        return w != 0 && read_words(p, a[1], SXW_ICON_EXTENT * SXW_ICON_EXTENT, icon) &&
                       w->set_icon(s->context, a[0], icon) == 0
                   ? 0u
                   : SXW_FAILED;
    }
    case SXW_SVC_WINDOW_PRESENT: {
        uint32_t size[2];
        int32_t rects[4u * SXW_MAX_RECTS];
        const uint32_t count = a[5];
        if (w == 0 || !read_words(p, a[3], 2, size) || size[0] == 0u || size[1] == 0u || size[0] > 0x4000u ||
            size[1] > 0x4000u || (a[2] & 3u) != 0u || a[2] > 4u * 0x4000u || a[2] / 4u < size[0] || (a[1] & 3u) != 0u ||
            !sxw_guest_range(p, a[1], a[2] * (size[1] - 1u) + 4u * size[0]) || count > SXW_MAX_RECTS ||
            !read_words(p, a[4], 4u * count, (uint32_t *)rects)) {
            return SXW_FAILED;
        }
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            int32_t *r = &rects[4u * i];
            int32_t left = r[0] < 0 ? 0 : r[0];
            int32_t top = r[1] < 0 ? 0 : r[1];
            int32_t right = r[2] > (int32_t)size[0] ? (int32_t)size[0] : r[2];
            int32_t bottom = r[3] > (int32_t)size[1] ? (int32_t)size[1] : r[3];
            if (left < right && top < bottom) {
                int32_t *k = &rects[4u * kept++];
                k[0] = left;
                k[1] = top;
                k[2] = right;
                k[3] = bottom;
            }
        }
        if (kept == 0u) {
            return 0;
        }
        return w->present(s->context, a[0], (const uint32_t *)(uintptr_t)a[1], a[2] / 4u, size[0], size[1], rects,
                          kept) == 0
                   ? 0u
                   : SXW_FAILED;
    }
    case SXW_SVC_WAIT_EVENT: {
        sxw_event event;
        if (!sxw_guest_range(p, a[1], sizeof(event))) {
            return 0;
        }
        if (w == 0 || !w->wait_event(s->context, a[0], &event)) {
            return 0;
        }
        const uint32_t words[6] = {event.kind, event.window, event.a, event.b, event.c, event.time};
        for (uint32_t i = 0; i < 6u; ++i) {
            write_word(p, a[1] + 4u * i, words[i]);
        }
        return 1;
    }
    default:
        return SXW_FAILED;
    }
}

uint32_t sxw_service(sxw_services *s, uint32_t guest_esp) {
    sxw_process *p = s->process;
    uint32_t frame[3];
    uint32_t a[8];
    if (!read_words(p, guest_esp, 3, frame)) {
        end_process(s, SXW_STATUS_ACCESS_VIOLATION);
        return 0;
    }
    const uint32_t service = frame[1];
    static const uint8_t argument_count[SXW_SVC_COUNT] = {0, 1, 1, 3, 2, 0, 1, 1, 4, 5, 5,
                                                           1, 5, 5, 0, 1, 8, 1, 6, 2, 2, 2};
    if (service >= sizeof(argument_count)) {
        write_text(s, "win32: unknown service\n");
        end_process(s, SXW_STATUS_INVALID_SYSTEM_SERVICE);
        return 0;
    }
    if (argument_count[service] != 0u && !read_words(p, frame[2], argument_count[service], a)) {
        end_process(s, SXW_STATUS_ACCESS_VIOLATION);
        return 0;
    }
    switch (service) {
    case SXW_SVC_VERSION:
        return SXW_ABI_VERSION;
    case SXW_SVC_EXIT_PROCESS:
        end_process(s, a[0]);
        return 0;
    case SXW_SVC_STD_HANDLE:
        return a[0] > 2u ? 0u : SXW_HANDLE_STDIN + 4u * a[0];
    case SXW_SVC_WRITE: {
        int stream = stream_of(a[0]);
        if (stream < 0 || !sxw_guest_range(p, a[1], a[2])) {
            return SXW_FAILED;
        }
        uint32_t done = 0;
        while (done < a[2]) {
            long n = s->write(s->context, stream, (const uint8_t *)(uintptr_t)a[1] + done, a[2] - done);
            if (n <= 0) {
                return done != 0u ? done : SXW_FAILED;
            }
            done += (uint32_t)n;
        }
        return done;
    }
    case SXW_SVC_DEBUG_OUTPUT:
        if (sxw_guest_range(p, a[0], a[1])) {
            write_text(s, "win32: debug: ");
            (void)s->write(s->context, 2, (const void *)(uintptr_t)a[0], a[1]);
            write_text(s, "\n");
        }
        return 0;
    case SXW_SVC_TIME:
        return guest_time(s);
    case SXW_SVC_ALLOCATE: {
        uint32_t base = 0;
        return sxw_allocate_region(p, a[0], &base) == 0 ? base : 0u;
    }
    case SXW_SVC_FIND_MODULE: {
        char name[SXW_MAX_STRING];
        const sxw_module *m = read_ansi(p, a[0], name) ? sxw_find_module(p, name) : 0;
        return m != 0 ? m->base : 0u;
    }
    case SXW_SVC_LOAD_LIBRARY: {
        char name[SXW_MAX_STRING];
        sxw_start_module added[SXW_MAX_ADDED];
        uint32_t count = 0;
        uint32_t base = 0;
        uint32_t error = SXW_ERROR_INVALID_PARAMETER;
        if (!sxw_guest_range(p, a[3], 4u)) {
            return 0;
        }
        if (read_ansi(p, a[0], name) &&
            sxw_load_library(p, name, added, a[2] < SXW_MAX_ADDED ? a[2] : SXW_MAX_ADDED, &count, &base,
                             &error) == 0) {
            if (report_added(p, a[1], a[2], a[3], added, count)) {
                return base;
            }
            error = SXW_ERROR_INVALID_PARAMETER;
        }
        write_word(p, a[3], error);
        return 0;
    }
    case SXW_SVC_GET_PROC: {
        char name[SXW_MAX_STRING];
        sxw_start_module added[SXW_MAX_ADDED];
        uint32_t count = 0;
        uint32_t address = 0;
        uint32_t error = SXW_ERROR_INVALID_PARAMETER;
        if (!sxw_guest_range(p, a[4], 4u)) {
            return 0;
        }
        const int by_ordinal = a[1] < 0x10000u;
        if ((by_ordinal || read_ansi(p, a[1], name)) &&
            sxw_get_proc(p, a[0], by_ordinal ? 0 : name, (uint16_t)a[1], added,
                         a[3] < SXW_MAX_ADDED ? a[3] : SXW_MAX_ADDED, &count, &address, &error) == 0) {
            if (report_added(p, a[2], a[3], a[4], added, count)) {
                return address;
            }
            error = SXW_ERROR_INVALID_PARAMETER;
        }
        write_word(p, a[4], error);
        return 0;
    }
    case SXW_SVC_REG_OPEN:
    case SXW_SVC_REG_CLOSE:
    case SXW_SVC_REG_QUERY:
    case SXW_SVC_REG_SET:
        return registry_service(s, service, a);
    default:
        return window_service(s, service, a);
    }
}
