#pragma once

/* sxnwm: native window-client substrate (toolkit seed, not a toolkit).
 *
 * The first reusable layer over sxn.h for windowed native programs: attach
 * a v4 client surface, publish full-surface frames, wait for composition,
 * and drain the native input ring. Extracted from sdk/nativedemo, which
 * uses all of it -- one client proves the shape, widgets wait for a second.
 * Header-only like sxn.h: no tool or link change needed. Returns follow the
 * kernel convention (0 on success, negative -errno on failure), except
 * where noted.
 */

#include <stdint.h>

#include "savanxp/syscall.h"
#include "savanxp/wm_protocol.h"
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
    /* Null until sxnwm_attach_input: output-only clients never set it. */
    struct savanxp_wm_input_ring* ring;
};

/* Map fd 3 and validate the v4 surface, filling the context. Fixed channel
 * numbers come from the protocol, like the fds themselves. */
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
        ctx->header->version != SAVANXP_GPU_CLIENT_SURFACE_VERSION_4 ||
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
    ctx->submit_fd = SAVANXP_WM_FD_SUBMIT_EVENT;
    ctx->wake_fd = SAVANXP_WM_FD_WAKE_EVENT;
    ctx->ring = 0;
    return 0;
}

/* Map the input ring (fd 7) and validate its magic and version. */
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
