#pragma once

/* sxnwm: SUN windowing (docs/userland/SUN.md), the user32 of SUN.
 *
 * The reusable layer over sxn.h for windowed native programs: connect to
 * windowd (protocol v5, wm/wm_protocol.h), publish full or partial
 * frames, wait for composition, drain the native input ring, ask for a size,
 * follow resizes and closing, and publish the title and icon. Extracted from sdk/nativedemo, which
 * uses all of it -- one client proves the shape, widgets wait for a second.
 * Header-only like sxn.h: no tool or link change needed. Returns follow the
 * kernel convention (0 on success, negative -errno on failure), except
 * where noted.
 */

#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "wm/wm_protocol.h"
#include "wm/wm_shell_protocol.h"
#include "sxn/sxn.h"

struct sxnwm_client {
    struct savanxp_gpu_client_surface_header* header;
    struct savanxp_gpu_dirty_rect_batch* batches;
    uint32_t* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t batch_capacity;
    long submit_fd;
    long wake_fd;
    /* The event pipe: kept open, never read -- input comes on the ring. */
    long events_fd;
    /* Set after a request in the header (size, presentation); -1 when the WM
     * gave none, and then the submit event carries requests too. */
    long request_fd;
    struct savanxp_wm_input_ring* ring;
};

/* What a window says about itself: the title on its caption and its taskbar
 * button, the caption accent, its style and its 16x16 icon. A SUN program
 * cannot read its own .sxe (SUN has no files), so the build compiles its
 * .sxres into <program>.sunres.h, which defines `sun_resources` of this type:
 * the program's resources linked into it, as .rsrc is into a Windows .exe. */
struct sxnwm_presentation_source {
    const char* title;
    uint32_t flags;          /* SAVANXP_WM_PRESENTATION_* */
    uint32_t accent;         /* 0x00RRGGBB, with SAVANXP_WM_PRESENTATION_HAS_ACCENT */
    uint32_t window_style;   /* SAVANXP_WM_WINDOW_STYLE_* */
    uint32_t icon_extent;    /* 0 = no icon, else SAVANXP_WM_PRESENTATION_ICON_EXTENT */
    const uint32_t* icon_pixels; /* icon_extent squared, BGRA8888 */
};

/* Map the surface section and validate it, filling the context. */
static inline long sxnwm_attach(struct sxnwm_client* ctx, long section_fd) {
    long base;

    if (ctx == 0) {
        return -SAVANXP_EINVAL;
    }
    base = sxn_map_view(section_fd, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (base <= 0) {
        return base;
    }
    ctx->header = (struct savanxp_gpu_client_surface_header*)base;
    if (ctx->header->magic != SAVANXP_GPU_CLIENT_SURFACE_MAGIC ||
        ctx->header->version != SAVANXP_GPU_CLIENT_SURFACE_VERSION_5 ||
        ctx->header->pixels_offset < sizeof(*ctx->header) ||
        ctx->header->command_offset < sizeof(*ctx->header) ||
        ctx->header->batch_capacity == 0u ||
        ctx->header->batch_capacity > SAVANXP_GPU_CLIENT_BATCH_CAPACITY ||
        ctx->header->rect_capacity == 0u ||
        ctx->header->rect_capacity > SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS ||
        ctx->header->command_offset >= ctx->header->pixels_offset ||
        ctx->header->info.width == 0u || ctx->header->info.height == 0u ||
        ctx->header->info.pitch < ctx->header->info.width * (uint32_t)sizeof(uint32_t)) {
        return -SAVANXP_EINVAL;
    }
    ctx->width = ctx->header->info.width;
    ctx->height = ctx->header->info.height;
    ctx->stride = ctx->header->info.pitch / (uint32_t)sizeof(uint32_t);
    ctx->pixels = (uint32_t*)((unsigned char*)ctx->header + ctx->header->pixels_offset);
    ctx->batches = (struct savanxp_gpu_dirty_rect_batch*)((unsigned char*)ctx->header + ctx->header->command_offset);
    ctx->batch_capacity = ctx->header->batch_capacity;
    ctx->ring = 0;
    return 0;
}

/* Map the input ring and validate its magic and version. */
static inline long sxnwm_attach_input(struct sxnwm_client* ctx, long ring_fd) {
    long base;

    if (ctx == 0) {
        return -SAVANXP_EINVAL;
    }
    base = sxn_map_view(ring_fd, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (base <= 0) {
        return base;
    }
    ctx->ring = (struct savanxp_wm_input_ring*)base;
    if (ctx->ring->magic != SAVANXP_WM_INPUT_RING_MAGIC ||
        ctx->ring->version != SAVANXP_WM_INPUT_RING_VERSION_1) {
        return -SAVANXP_EINVAL;
    }
    return 0;
}

/* Connect to windowd and get a window: the hello asks for input on a ring
 * (a native client reads no pipe), the welcome brings the surface section,
 * the wake and submit events and the ring, which are mapped here. The read
 * of the welcome blocks: there is no native poll yet, and a WM that accepted
 * the connection answers within a pass of its loop. 0 or negative -errno. */
static inline long sxnwm_connect(struct sxnwm_client* ctx) {
    int32_t handles[2] = {-1, -1};
    long received[4] = {-1, -1, -1, -1};
    struct savanxp_wm_hello hello = {0};
    struct savanxp_wm_event record = {0};
    uint32_t index = 0;
    long result = 0;

    if (ctx == 0) {
        return -SAVANXP_EINVAL;
    }
    ctx->header = 0;
    ctx->ring = 0;
    ctx->events_fd = -1;
    ctx->request_fd = -1;
    result = sxn_port_connect(SAVANXP_WM_PORT_NAME, handles);
    if (result < 0) {
        return result;
    }
    hello.magic = SAVANXP_WM_HELLO_MAGIC;
    hello.version = SAVANXP_WM_PROTOCOL_VERSION;
    hello.flags = SAVANXP_WM_HELLO_INPUT_RING | SAVANXP_WM_HELLO_REQUEST_EVENT;
    if (sxn_write(handles[0], &hello, sizeof(hello)) != (long)sizeof(hello)) {
        result = -SAVANXP_EPIPE;
        goto fail;
    }
    for (;;) {
        result = sxn_read(handles[1], &record, sizeof(record));
        if (result != (long)sizeof(record)) {
            result = result < 0 ? result : -SAVANXP_EPIPE;
            goto fail;
        }
        if (record.kind == SAVANXP_WM_EVENT_WELCOME) {
            break;
        }
    }
    if (record.payload.welcome.status != 0) {
        result = record.payload.welcome.status;
        goto fail;
    }
    /* section, wake, submit, ring, and the request event when the welcome
     * says it is there */
    if (record.payload.welcome.handle_count !=
        ((record.payload.welcome.flags & SAVANXP_WM_WELCOME_REQUEST_EVENT) != 0u ? 5u : 4u)) {
        result = -SAVANXP_EIO;
        goto fail;
    }
    for (index = 0; index < 4u; index += 1) {
        received[index] = sxn_pipe_receive_handle(handles[1]);
        if (received[index] < 0) {
            result = received[index];
            goto fail;
        }
    }
    if ((record.payload.welcome.flags & SAVANXP_WM_WELCOME_REQUEST_EVENT) != 0u) {
        ctx->request_fd = sxn_pipe_receive_handle(handles[1]);
        if (ctx->request_fd < 0) {
            result = ctx->request_fd;
            ctx->request_fd = -1;
            goto fail;
        }
    }
    result = sxnwm_attach(ctx, received[0]);
    if (result == 0) {
        result = sxnwm_attach_input(ctx, received[3]);
    }
    if (result != 0) {
        goto fail;
    }
    /* The views keep the sections alive; only the channels stay open. */
    (void)sxn_close(received[0]);
    (void)sxn_close(received[3]);
    (void)sxn_close(handles[0]);
    ctx->wake_fd = received[1];
    ctx->submit_fd = received[2];
    ctx->events_fd = handles[1];
    return 0;

fail:
    for (index = 0; index < 4u; index += 1) {
        if (received[index] >= 0) {
            (void)sxn_close(received[index]);
        }
    }
    if (ctx->request_fd >= 0) {
        (void)sxn_close(ctx->request_fd);
        ctx->request_fd = -1;
    }
    (void)sxn_close(handles[0]);
    (void)sxn_close(handles[1]);
    ctx->header = 0;
    ctx->ring = 0;
    return result;
}

/* Publish one full-surface batch. Returns the new submit sequence (>0), or
 * negative -errno. -EAGAIN when every batch slot is still unretired: wait
 * for composed and retry, like the POSIX runtime does. */
static inline long sxnwm_submit_full(struct sxnwm_client* ctx) {
    struct savanxp_gpu_dirty_rect_batch* batch = 0;
    uint64_t sequence = 0;
    uint32_t index = 0;
    long result = 0;

    if (ctx == 0 || ctx->header == 0 || ctx->batches == 0) {
        return -SAVANXP_EINVAL;
    }
    if (ctx->header->submit_sequence - ctx->header->composed_sequence >= ctx->batch_capacity) {
        return -SAVANXP_EAGAIN;
    }
    sequence = ctx->header->submit_sequence + 1u;
    batch = &ctx->batches[(sequence - 1u) % ctx->batch_capacity];
    batch->rect_count = 1;
    batch->flags = SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE;
    batch->rects[0].x = 0;
    batch->rects[0].y = 0;
    batch->rects[0].width = ctx->width;
    batch->rects[0].height = ctx->height;
    for (index = 1; index < ctx->header->rect_capacity && index < SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS; index += 1) {
        batch->rects[index].x = 0;
        batch->rects[index].y = 0;
        batch->rects[index].width = 0;
        batch->rects[index].height = 0;
    }
    batch->submit_sequence = sequence;
    __asm__ volatile("" ::: "memory");
    ctx->header->submit_sequence = sequence;
    result = sxn_event_set(ctx->submit_fd);
    if (result != 0) {
        return result;
    }
    return (long)sequence;
}

/* Wait until windowd reports the given submit sequence composed: reset the
 * wake event, re-check, then wait, like the POSIX runtime. Returns 0 or
 * -ETIMEDOUT after max_rounds waits of wait_ms. */
static inline long sxnwm_wait_composed(struct sxnwm_client* ctx, uint64_t sequence, int max_rounds, long wait_ms) {
    int round = 0;

    if (ctx == 0 || ctx->header == 0) {
        return -SAVANXP_EINVAL;
    }
    for (round = 0; round < max_rounds; round += 1) {
        long result = sxn_event_reset(ctx->wake_fd);
        if (result != 0) {
            return result;
        }
        if (ctx->header->composed_sequence >= sequence) {
            return 0;
        }
        (void)sxn_wait_one(ctx->wake_fd, wait_ms);
    }
    return ctx->header->composed_sequence >= sequence ? 0 : -SAVANXP_ETIMEDOUT;
}

/* Drain one input record. 0 with *out filled, -EAGAIN when the ring is
 * empty, -EINVAL with no ring attached. Call in a loop until -EAGAIN. */
static inline long sxnwm_poll_input(struct sxnwm_client* ctx, struct savanxp_wm_event* out) {
    uint64_t tail = 0;

    if (ctx == 0 || out == 0) {
        return -SAVANXP_EINVAL;
    }
    if (ctx->ring == 0) {
        return -SAVANXP_EINVAL;
    }
    tail = ctx->ring->tail;
    if (tail >= ctx->ring->head) {
        return -SAVANXP_EAGAIN;
    }
    *out = ctx->ring->records[tail % SAVANXP_WM_INPUT_RING_CAPACITY];
    __asm__ volatile("" ::: "memory");
    ctx->ring->tail = tail + 1u;
    return 0;
}

/* Publish a batch of up to rect_capacity rectangles already painted on the
 * surface (InvalidateRect plus the paint). Rectangles are clipped to the
 * window; an empty one is skipped. Returns the submit sequence (>0), 0 when
 * nothing was left after clipping, or negative -errno (-EAGAIN with every
 * batch slot unretired, as sxnwm_submit_full). More rectangles than a batch
 * holds become one full-surface batch. */
static inline long sxnwm_submit_rects(
    struct sxnwm_client* ctx, const struct savanxp_gpu_dirty_rect* rects, uint32_t count) {
    struct savanxp_gpu_dirty_rect_batch* batch = 0;
    uint64_t sequence = 0;
    uint32_t used = 0;
    uint32_t index = 0;
    uint32_t capacity = 0;
    long result = 0;

    if (ctx == 0 || ctx->header == 0 || ctx->batches == 0 || (rects == 0 && count != 0)) {
        return -SAVANXP_EINVAL;
    }
    capacity = ctx->header->rect_capacity < SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS
        ? ctx->header->rect_capacity
        : SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    if (count > capacity) {
        return sxnwm_submit_full(ctx);
    }
    if (ctx->header->submit_sequence - ctx->header->composed_sequence >= ctx->batch_capacity) {
        return -SAVANXP_EAGAIN;
    }
    sequence = ctx->header->submit_sequence + 1u;
    batch = &ctx->batches[(sequence - 1u) % ctx->batch_capacity];
    for (index = 0; index < count; index += 1) {
        uint32_t x = rects[index].x;
        uint32_t y = rects[index].y;
        uint32_t width = rects[index].width;
        uint32_t height = rects[index].height;

        if (x >= ctx->width || y >= ctx->height || width == 0 || height == 0) {
            continue;
        }
        if (width > ctx->width - x) {
            width = ctx->width - x;
        }
        if (height > ctx->height - y) {
            height = ctx->height - y;
        }
        batch->rects[used].x = x;
        batch->rects[used].y = y;
        batch->rects[used].width = width;
        batch->rects[used].height = height;
        used += 1;
    }
    if (used == 0) {
        return 0;
    }
    for (index = used; index < capacity; index += 1) {
        batch->rects[index].x = 0;
        batch->rects[index].y = 0;
        batch->rects[index].width = 0;
        batch->rects[index].height = 0;
    }
    batch->rect_count = used;
    batch->flags = SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_NONE;
    batch->submit_sequence = sequence;
    __asm__ volatile("" ::: "memory");
    ctx->header->submit_sequence = sequence;
    result = sxn_event_set(ctx->submit_fd);
    if (result != 0) {
        return result;
    }
    return (long)sequence;
}

/* 1 once everything submitted has been composed: the surface may be painted
 * again without the compositor reading a half-drawn frame. */
static inline int sxnwm_idle(const struct sxnwm_client* ctx) {
    return ctx != 0 && ctx->header != 0 && ctx->header->composed_sequence >= ctx->header->submit_sequence;
}

/* Wait (up to timeout_ms, negative = forever) until sxnwm_idle. 0 or
 * -ETIMEDOUT. */
static inline long sxnwm_wait_idle(struct sxnwm_client* ctx, long timeout_ms) {
    uint64_t deadline = 0;

    if (ctx == 0 || ctx->header == 0) {
        return -SAVANXP_EINVAL;
    }
    deadline = timeout_ms < 0 ? 0 : sxn_clock_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        long wait_ms = 50;

        (void)sxn_event_reset(ctx->wake_fd);
        if (sxnwm_idle(ctx)) {
            return 0;
        }
        if (timeout_ms >= 0) {
            const uint64_t now = sxn_clock_ns();
            if (now >= deadline) {
                return -SAVANXP_ETIMEDOUT;
            }
            if ((deadline - now) / 1000000ull < (uint64_t)wait_ms) {
                wait_ms = (long)((deadline - now) / 1000000ull) + 1;
            }
        }
        (void)sxn_wait_one(ctx->wake_fd, wait_ms);
    }
}

/* Tell the WM the header carries a new request. */
static inline long sxnwm_notify(struct sxnwm_client* ctx) {
    if (ctx == 0) {
        return -SAVANXP_EINVAL;
    }
    return sxn_event_set(ctx->request_fd >= 0 ? ctx->request_fd : ctx->submit_fd);
}

/* Ask for a client area of width x height. A hint: the WM clips it, and the
 * answer arrives in the surface header (sxnwm_check_resize). */
static inline long sxnwm_request_size(struct sxnwm_client* ctx, uint32_t width, uint32_t height) {
    struct savanxp_wm_client_requests* requests = 0;

    if (ctx == 0 || ctx->header == 0 || width == 0 || height == 0) {
        return -SAVANXP_EINVAL;
    }
    /* Seqlock: odd while the pair is half written. */
    requests = &ctx->header->requests;
    requests->size_hint_sequence |= 1u;
    __asm__ volatile("" ::: "memory");
    requests->size_hint_width = width;
    requests->size_hint_height = height;
    __asm__ volatile("" ::: "memory");
    requests->size_hint_sequence += 1u;
    return sxnwm_notify(ctx);
}

/* Follow a resize: the WM writes the new client size into the surface header
 * (the POSIX runtime turns the same change into a RESIZED event). 1 if the
 * size changed and ctx->width/height now hold it, 0 if not. */
static inline long sxnwm_check_resize(struct sxnwm_client* ctx) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t max_height = 0;

    if (ctx == 0 || ctx->header == 0) {
        return 0;
    }
    width = ctx->header->info.width;
    height = ctx->header->info.height;
    max_height = ctx->header->info.pitch != 0u ? ctx->header->info.buffer_size / ctx->header->info.pitch : 0u;
    if (width == 0 || height == 0 || width > ctx->stride || height > max_height ||
        (width == ctx->width && height == ctx->height)) {
        return 0;
    }
    ctx->width = width;
    ctx->height = height;
    return 1;
}

/* 1 when the WM asks the window to close (its close button, Alt+F4). */
static inline int sxnwm_close_requested(const struct sxnwm_client* ctx) {
    return ctx != 0 && ctx->header != 0 &&
        (ctx->header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN) != 0u;
}

/* Copies `title` into a presentation title field, cut to fit. */
static inline void sxnwm_copy_title(char* out, const char* title) {
    uint32_t index = 0;

    for (index = 0; index < SAVANXP_WM_PRESENTATION_TITLE_CAPACITY; index += 1) {
        out[index] = 0;
    }
    for (index = 0; title != 0 && title[index] != '\0' &&
            index + 1u < SAVANXP_WM_PRESENTATION_TITLE_CAPACITY; index += 1) {
        out[index] = title[index];
    }
}

/* Publish what the window says about itself (see struct
 * sxnwm_presentation_source); the WM copies each new version once. */
static inline long sxnwm_set_presentation(struct sxnwm_client* ctx, const struct sxnwm_presentation_source* source) {
    struct savanxp_wm_presentation* out = 0;
    uint32_t sequence = 0;
    uint32_t index = 0;
    uint32_t pixels = 0;

    if (ctx == 0 || ctx->header == 0 || source == 0 ||
        source->icon_extent > SAVANXP_WM_PRESENTATION_ICON_EXTENT ||
        (source->icon_extent != 0u && source->icon_pixels == 0)) {
        return -SAVANXP_EINVAL;
    }
    out = &ctx->header->requests.presentation;
    /* Seqlock: odd while the fields are being written. */
    sequence = out->sequence | 1u;
    out->sequence = sequence;
    __asm__ volatile("" ::: "memory");
    out->flags = source->flags;
    out->accent = source->accent;
    out->window_style = source->window_style;
    out->icon_extent = source->icon_extent;
    sxnwm_copy_title(out->title, source->title);
    pixels = source->icon_extent * source->icon_extent;
    for (index = 0; index < SAVANXP_WM_PRESENTATION_ICON_EXTENT * SAVANXP_WM_PRESENTATION_ICON_EXTENT; index += 1) {
        out->icon_pixels[index] = index < pixels ? source->icon_pixels[index] : 0u;
    }
    __asm__ volatile("" ::: "memory");
    out->sequence = sequence + 1u;
    return sxnwm_notify(ctx);
}

/* Captures the pointer (on = 1) or lets it go (on = 0): while the window is
 * the active one, the WM hides the cursor and sends
 * SAVANXP_WM_EVENT_POINTER_MOTION records with how far the mouse moved,
 * unbounded by the screen -- what a first-person view needs. Losing the
 * focus (Alt+Tab, the Start menu, another window) suspends the capture
 * without the program doing anything, and it resumes when the window is
 * active again, so a captured window can never trap the user. ClipCursor
 * plus raw input, in Win32 terms. */
static inline long sxnwm_set_pointer_capture(struct sxnwm_client* ctx, int on) {
    if (ctx == 0 || ctx->header == 0) {
        return -SAVANXP_EINVAL;
    }
    ctx->header->requests.pointer_capture = on ? 1u : 0u;
    __asm__ volatile("" ::: "memory");
    return sxnwm_notify(ctx);
}

/* SetCursor: the shape the WM shows while the pointer is over this window
 * (enum savanxp_cursor_shape). State, not an event: the last one asked for
 * stays until the next. */
static inline long sxnwm_set_cursor_shape(struct sxnwm_client* ctx, uint32_t shape) {
    if (ctx == 0 || ctx->header == 0 || shape >= SAVANXP_CURSOR_SHAPE_COUNT) {
        return -SAVANXP_EINVAL;
    }
    ctx->header->requests.cursor_shape = shape;
    __asm__ volatile("" ::: "memory");
    return sxnwm_notify(ctx);
}

/* ---- owned windows (docs/desktop/OWNED_WINDOWS.md) ------------------------
 *
 * A dialog of the program, in a window of its own that the WM keeps above
 * its owner and disables the owner for. The request travels in the owner's
 * header; the WM answers by queueing the new surface on the owner's event
 * pipe. That pipe is the right channel for everything about owned windows:
 * a native client takes its main window's input from the ring and never
 * reads the pipe for it, so what arrives there is the dialogs' -- their
 * surface handles and their input, each record tagged with its window_id.
 *
 * The owned window's sxnwm_client shares the owner's submit, wake and
 * request events, so sxnwm_submit_full and sxnwm_set_cursor_shape work on it
 * unchanged. It has no ring (ring == 0): read its input with
 * sxnwm_poll_owned_input on the owner. */

#define SXNWM_WINDOW_OPEN_WAIT_MS 1000l
#define SXNWM_WINDOW_CLOSE_WAIT_MS 250l

/* Write an owned-window request into the owner's header and wait for its
 * reply, up to timeout_ms. The reply status, or -EAGAIN when it did not
 * come in time, -EPIPE when the owner is being closed. */
static inline long sxnwm_window_request(struct sxnwm_client* owner, uint32_t action, uint32_t window_id,
                                        uint32_t width, uint32_t height, const char* title, long timeout_ms) {
    struct savanxp_wm_client_requests* requests = 0;
    uint32_t sequence = 0;
    uint64_t deadline = 0;
    uint32_t index = 0;

    if (owner == 0 || owner->header == 0) {
        return -SAVANXP_EINVAL;
    }
    requests = &owner->header->requests;
    /* Seqlock: odd while the request is half written. */
    requests->window_sequence |= 1u;
    __asm__ volatile("" ::: "memory");
    {
        unsigned char* bytes = (unsigned char*)&requests->window;
        for (index = 0; index < (uint32_t)sizeof(requests->window); index += 1) {
            bytes[index] = 0;
        }
    }
    requests->window.action = action;
    requests->window.window_id = window_id;
    requests->window.width = width;
    requests->window.height = height;
    for (index = 0; title != 0 && title[index] != '\0' && index + 1u < SAVANXP_WM_WINDOW_TITLE_CAPACITY; index += 1) {
        requests->window.title[index] = title[index];
    }
    __asm__ volatile("" ::: "memory");
    requests->window_sequence += 1u;
    sequence = requests->window_sequence;
    (void)sxnwm_notify(owner);

    deadline = sxn_clock_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    for (;;) {
        if (requests->window_reply_sequence == sequence) {
            return requests->window_reply_status;
        }
        if (sxnwm_close_requested(owner)) {
            return -SAVANXP_EPIPE;
        }
        if (sxn_clock_ns() >= deadline) {
            return -SAVANXP_EAGAIN;
        }
        /* The wake event serves several conditions: reset, look again, and
         * only then sleep. */
        (void)sxn_event_reset(owner->wake_fd);
        if (requests->window_reply_sequence == sequence) {
            return requests->window_reply_status;
        }
        (void)sxn_wait_one(owner->wake_fd, 20);
    }
}

/* Open owned window `window_id` (1..SAVANXP_WM_MAX_OWNED_WINDOWS) of
 * width x height client pixels with `title`, filling `window`. 0, or -errno
 * when the WM refused (no room on screen, no free slot): the caller then
 * draws the dialog inside its own window, as the toolkit does. */
static inline long sxnwm_window_open(struct sxnwm_client* owner, struct sxnwm_client* window, uint32_t window_id,
                                     uint32_t width, uint32_t height, const char* title) {
    long section = -1;
    long result = 0;

    if (owner == 0 || owner->header == 0 || owner->events_fd < 0 || window == 0 || window_id == 0 ||
        window_id > SAVANXP_WM_MAX_OWNED_WINDOWS || width == 0 || height == 0) {
        return -SAVANXP_EINVAL;
    }
    /* Input for dialogs is read without blocking (sxnwm_poll_owned_input). */
    (void)sxn_set_nonblocking(owner->events_fd, 1);
    /* A handle already queued belongs to an earlier request that timed out on
     * this side: not this request's, and nobody will use it. */
    while ((section = sxn_pipe_receive_handle(owner->events_fd)) >= 0) {
        (void)sxn_close(section);
    }
    result = sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_OPEN, window_id, width, height, title,
                                  SXNWM_WINDOW_OPEN_WAIT_MS);
    if (result < 0) {
        if (result == -SAVANXP_EAGAIN) {
            /* Timed out: if the WM creates it anyway, it should destroy it. */
            (void)sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_CLOSE, window_id, 0, 0, 0, 0);
        }
        return result;
    }
    section = sxn_pipe_receive_handle(owner->events_fd);
    result = section < 0 ? section : sxnwm_attach(window, section);
    if (section >= 0) {
        /* The view keeps the section alive. */
        (void)sxn_close(section);
    }
    if (result != 0) {
        window->header = 0;
        (void)sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_CLOSE, window_id, 0, 0, 0,
                                   SXNWM_WINDOW_CLOSE_WAIT_MS);
        return result;
    }
    window->submit_fd = owner->submit_fd;
    window->wake_fd = owner->wake_fd;
    window->request_fd = owner->request_fd;
    window->events_fd = -1;
    return 0;
}

/* Close an owned window opened with sxnwm_window_open. Closing one that is
 * already gone is not an error. */
static inline long sxnwm_window_close(struct sxnwm_client* owner, struct sxnwm_client* window, uint32_t window_id) {
    struct savanxp_wm_event stale;

    if (owner == 0 || window == 0 || window->header == 0) {
        return -SAVANXP_EINVAL;
    }
    (void)sxn_unmap_view((unsigned long)window->header);
    window->header = 0;
    window->pixels = 0;
    (void)sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_CLOSE, window_id, 0, 0, 0,
                               SXNWM_WINDOW_CLOSE_WAIT_MS);
    /* After the request, not before: whatever the WM sent until it destroyed
     * the window is dropped here. */
    while (sxn_read(owner->events_fd, &stale, sizeof(stale)) == (long)sizeof(stale)) {
    }
    return 0;
}

/* ---- the task list (EnumWindows) ----
 *
 * The windows the taskbar shows, for a program that is not the shell: a Task
 * Manager's Applications tab. The WM gives this client a list of its own and
 * keeps it current; it is read under its seqlock with sxnwm_task_list_read.
 * Ids are task ids (the process's pid), not indices. */

/* Map the task list read-only into *out. 0, or -errno. */
static inline long sxnwm_task_list_open(struct sxnwm_client* owner, const struct savanxp_wm_window_list** out) {
    long section = -1;
    long result = 0;
    long base = 0;

    if (owner == 0 || owner->header == 0 || owner->events_fd < 0 || out == 0) {
        return -SAVANXP_EINVAL;
    }
    *out = 0;
    (void)sxn_set_nonblocking(owner->events_fd, 1);
    while ((section = sxn_pipe_receive_handle(owner->events_fd)) >= 0) {
        (void)sxn_close(section);
    }
    result = sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_TASK_LIST, 0, 0, 0, 0,
                                  SXNWM_WINDOW_OPEN_WAIT_MS);
    if (result < 0) {
        return result;
    }
    section = sxn_pipe_receive_handle(owner->events_fd);
    if (section < 0) {
        return section;
    }
    base = sxn_map_view(section, SXN_SECTION_READ);
    /* The view keeps the section alive. */
    (void)sxn_close(section);
    if (base <= 0) {
        return base < 0 ? base : -SAVANXP_ENOMEM;
    }
    *out = (const struct savanxp_wm_window_list*)base;
    return 0;
}

/* Copy the list whole into *out, retrying while the WM is writing it. 0, or
 * -EAGAIN if it never held still. */
static inline long sxnwm_task_list_read(const struct savanxp_wm_window_list* list,
                                        struct savanxp_wm_window_list* out) {
    int attempt = 0;

    if (list == 0 || out == 0) {
        return -SAVANXP_EINVAL;
    }
    for (attempt = 0; attempt < 64; attempt += 1) {
        const volatile uint64_t* sequence = &list->sequence;
        uint64_t before = *sequence;
        const unsigned char* from = (const unsigned char*)list;
        unsigned char* to = (unsigned char*)out;
        uint32_t index = 0;

        if ((before & 1u) != 0u) {
            continue;
        }
        __asm__ volatile("" ::: "memory");
        for (index = 0; index < (uint32_t)sizeof(*out); index += 1) {
            to[index] = from[index];
        }
        __asm__ volatile("" ::: "memory");
        if (*sequence == before) {
            if (out->count > SAVANXP_WM_MAX_WINDOWS) {
                out->count = SAVANXP_WM_MAX_WINDOWS;
            }
            return 0;
        }
    }
    return -SAVANXP_EAGAIN;
}

/* Switch To: bring task `id` to the front. 0, or -ENOENT when it is gone. */
static inline long sxnwm_task_activate(struct sxnwm_client* owner, uint32_t id) {
    return sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_TASK_ACTIVATE, id, 0, 0, 0,
                                SXNWM_WINDOW_OPEN_WAIT_MS);
}

/* End Task: ask task `id` to close, as its X button does. 0, -EPERM when it
 * is another user's, -ENOENT when it is gone. */
static inline long sxnwm_task_close(struct sxnwm_client* owner, uint32_t id) {
    return sxnwm_window_request(owner, SAVANXP_WM_WINDOW_ACTION_TASK_CLOSE, id, 0, 0, 0,
                                SXNWM_WINDOW_OPEN_WAIT_MS);
}

/* Drain one record of the owned windows' input from the owner's event pipe:
 * 0 with *out filled (out->window_id says whose), -EAGAIN when there is none.
 * Only after sxnwm_window_open, which makes the pipe non-blocking. */
static inline long sxnwm_poll_owned_input(struct sxnwm_client* owner, struct savanxp_wm_event* out) {
    long result;

    if (owner == 0 || out == 0 || owner->events_fd < 0) {
        return -SAVANXP_EINVAL;
    }
    result = sxn_read(owner->events_fd, out, sizeof(*out));
    if (result == (long)sizeof(*out)) {
        return 0;
    }
    return result < 0 ? result : -SAVANXP_EAGAIN;
}

/* SetWindowText: a new title, everything else as last published. Titles
 * longer than SAVANXP_WM_PRESENTATION_TITLE_CAPACITY - 1 are cut. */
static inline long sxnwm_set_title(struct sxnwm_client* ctx, const char* title) {
    struct savanxp_wm_presentation* out = 0;
    uint32_t sequence = 0;

    if (ctx == 0 || ctx->header == 0 || title == 0) {
        return -SAVANXP_EINVAL;
    }
    out = &ctx->header->requests.presentation;
    sequence = out->sequence | 1u;
    out->sequence = sequence;
    __asm__ volatile("" ::: "memory");
    sxnwm_copy_title(out->title, title);
    __asm__ volatile("" ::: "memory");
    out->sequence = sequence + 1u;
    return sxnwm_notify(ctx);
}
