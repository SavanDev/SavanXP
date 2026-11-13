/*
 * services.c -- The gate's services. See services.h and sxw_abi.h.
 */
#include "services.h"

#include "sxw_abi.h"

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

static int read_words(const sxw_process *process, uint32_t address, uint32_t count, uint32_t *out) {
    if (!sxw_guest_range(process, address, 4u * count)) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
                 ((uint32_t)p[4 * i + 3] << 24);
    }
    return 1;
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

uint32_t sxw_service(sxw_services *s, uint32_t guest_esp) {
    const sxw_process *p = s->process;
    uint32_t frame[3];
    uint32_t a[3];
    if (!read_words(p, guest_esp, 3, frame)) {
        end_process(s, SXW_STATUS_ACCESS_VIOLATION);
        return 0;
    }
    switch (frame[1]) {
    case SXW_SVC_VERSION:
        return SXW_ABI_VERSION;
    case SXW_SVC_EXIT_PROCESS:
        end_process(s, read_words(p, frame[2], 1, a) ? a[0] : SXW_STATUS_ACCESS_VIOLATION);
        return 0;
    case SXW_SVC_STD_HANDLE:
        if (!read_words(p, frame[2], 1, a) || a[0] > 2u) {
            return 0;
        }
        return SXW_HANDLE_STDIN + 4u * a[0];
    case SXW_SVC_WRITE: {
        if (!read_words(p, frame[2], 3, a) || !sxw_guest_range(p, a[1], a[2])) {
            return SXW_FAILED;
        }
        int stream = stream_of(a[0]);
        if (stream < 0) {
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
        if (read_words(p, frame[2], 2, a) && sxw_guest_range(p, a[0], a[1])) {
            write_text(s, "win32: debug: ");
            (void)s->write(s->context, 2, (const void *)(uintptr_t)a[0], a[1]);
            write_text(s, "\n");
        }
        return 0;
    default:
        write_text(s, "win32: unknown service\n");
        end_process(s, SXW_STATUS_INVALID_SYSTEM_SERVICE);
        return 0;
    }
}
