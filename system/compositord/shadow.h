#pragma once
#include "cursor_cur.h"
#include "sxgfx/gfx2d.h"

#define WINDOW_SHADOW_RADIUS 12
#define WINDOW_SHADOW_ALPHA 36
#define WINDOW_SHADOW_OFFSET_X 1
#define WINDOW_SHADOW_OFFSET_Y 2

#define CURSOR_SHADOW_MARGIN 3
#define CURSOR_SHADOW_MAX (CUR_MAX_DIMENSION + 8)
struct cursor_shadow {
    int width, height;
    uint32_t pixels[CURSOR_SHADOW_MAX * CURSOR_SHADOW_MAX];
};
void cursor_shadow_build(const struct cursor_image *image, struct cursor_shadow *shadow);
/* Packs straight-alpha pixels for the 64x64 hardware plane; -1 if it cannot fit. */
int cursor_shadow_pack(const struct cursor_image *image, const struct cursor_shadow *shadow, uint32_t pixels[64 * 64]);
struct sx_rect window_shadow_bounds(struct sx_rect frame);
unsigned window_shadow_alpha(struct sx_rect frame, int x, int y);
