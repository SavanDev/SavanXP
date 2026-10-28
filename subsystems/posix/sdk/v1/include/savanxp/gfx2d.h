#pragma once

/* The platform painter (sxgfx/gfx2d.h) plus what only a POSIX program has:
 * the scaled presenter, which presents onto a POSIX window. */

#include "sxgfx/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-resolution pixel-art presenter: integer nearest-neighbour scaling,
 * centred on the client surface, presenting only the smallest band of source
 * rows that changed. The SDK keeps the previous copy and the expanded row; the
 * app only hands in its original frame. */
struct savanxp_gfx_context;
struct sx_scaled_presenter {
    struct savanxp_gfx_context* gfx;
    struct sx_bitmap source;
    struct sx_bitmap target;
    struct sx_rect destination;
    uint32_t scale;
    uint32_t* expanded_row;
    uint32_t* previous;
    size_t expanded_row_capacity;
    size_t source_pixel_count;
    int previous_valid;
    int force_surface_present;
};

/* Initialises a 320x200-style presenter. `source_pixels` is a contiguous BGRX
 * frame of `source_width * source_height` pixels. Returns 0 or a negative
 * error. The presenter neither acquires nor releases the gfx context. */
int sx_scaled_presenter_init(
    struct sx_scaled_presenter* presenter,
    struct savanxp_gfx_context* gfx,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t* source_pixels,
    uint32_t background);
/* Reapplies the layout after a RESIZED, clears the background and forces the
 * next frame to be complete. The target buffer the gfx runtime keeps may be
 * larger than the new width/height, which is why the visible rect is reported,
 * not its capacity. */
int sx_scaled_presenter_retarget(
    struct sx_scaled_presenter* presenter,
    uint32_t background);
/* 1 if there was damage and a present, 0 if the frame did not change,
 * negative if the present failed. The previous copy is committed only after a
 * successful present. */
long sx_scaled_presenter_present(
    struct sx_scaled_presenter* presenter,
    const uint32_t* source_pixels);
void sx_scaled_presenter_destroy(struct sx_scaled_presenter* presenter);

#ifdef __cplusplus
}
#endif
