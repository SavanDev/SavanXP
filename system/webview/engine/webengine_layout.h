#pragma once

/* The engine's layout internals, shared by its block flow
 * (webengine_layout.c) and its inline flow (webengine_lines.c). */

#include "webengine_box.h"

/* A length in pixels: a percentage of `base`, `automatic` for auto and none,
 * and for a percentage with no base (base < 0). */
int webengine_length_px(const struct webengine_len *length, int base, int automatic);

/* Lays out the inline content of `block` in line boxes from (x, y), `width`
 * wide. The height of the line boxes; 0 when there are none. */
int webengine_layout_lines(const struct webengine_fonts *fonts, struct webengine_box *block, int x, int y,
                           int width);
