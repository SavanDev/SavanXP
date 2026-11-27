#pragma once

/* The engine's layout internals, shared by its block flow
 * (webengine_layout.c), its inline flow (webengine_lines.c) and its painting
 * order (webengine_paint.c). */

#include "webengine_box.h"

/* A length in pixels: a percentage of `base`, `automatic` for auto and none,
 * and for a percentage with no base (base < 0). */
int webengine_length_px(const struct webengine_len *length, int base, int automatic);

struct webengine_atom;
struct webengine_placed;

/* A float's margin box, in canvas pixels, and its side (CSS_FLOAT_*). */
struct webengine_float {
    int left;
    int top;
    int right;
    int bottom;
    uint8_t side;
};

/* What the inline flow keeps between blocks. */
struct webengine_flow {
    const struct webengine_fonts *fonts;
    struct webengine_tree *tree;
    /* The floats placed so far, those of the block formatting context the
     * flow is in from floats[context] on: a context's floats end with it,
     * and one inside another starts after the outer one's. */
    struct webengine_float *floats;
    size_t float_count;
    size_t float_capacity;
    size_t context;
    /* Floats met on a line they did not fit on, placed under it. */
    struct webengine_box **deferred;
    size_t deferred_count;
    size_t deferred_capacity;
    /* A list item whose outside marker waits for the first line box inside
     * it, and the left edge of the item's content. */
    struct webengine_box *marker_item;
    int marker_left;
    /* Scratch, reused from block to block. */
    struct webengine_atom *atoms;
    size_t atom_capacity;
    struct webengine_placed *placed;
    size_t placed_capacity;
    struct webengine_box **open;
    size_t open_capacity;
    bool failed;
};

/* Lays out the inline content of `block` in line boxes from (x, y), `width`
 * wide, adding them and their fragments to the tree. The height of the line
 * boxes; 0 when there are none. */
int webengine_layout_lines(struct webengine_flow *flow, struct webengine_box *block, int x, int y, int width);

/* Places the waiting outside marker at the top of its item's content, `y`,
 * when no line box inside the item took it. */
void webengine_place_marker(struct webengine_flow *flow, int y);

void webengine_flow_free(struct webengine_flow *flow);

/* The room the context's floats leave between `left` and `right` over the
 * rows [top, top + height), narrowing the two. */
void webengine_float_space(const struct webengine_flow *flow, int top, int height, int *left, int *right);

/* The first bottom below `top` of a float in the rows [top, top + height):
 * where the room beside floats next changes. */
int webengine_float_below(const struct webengine_flow *flow, int top, int height);

/* The bottom of the context's lowest float that `clear` (CSS_CLEAR_*)
 * clears, or INT_MIN when there is none. */
int webengine_float_clearance(const struct webengine_flow *flow, uint8_t clear);

/* Places `float_box`, laid out with its margin box at the origin, in the
 * containing block between `left` and `right`, no higher than `top` (CSS 2.1
 * 9.5.1), and adds it to the context's floats. */
void webengine_place_float(struct webengine_flow *flow, struct webengine_box *float_box, int left, int right,
                           int top);

/* The horizontal margins, borders and padding of `box` that do not depend
 * on its containing block, for shrink-to-fit widths. */
int webengine_intrinsic_edges(const struct webengine_box *box);

/* The minimum and preferred widths of the inline content of `block`; its
 * floats' min_content and max_content are known. */
void webengine_inline_widths(struct webengine_flow *flow, struct webengine_box *block, int *min, int *max);

/* Moves a laid-out box, its line boxes and fragments and everything inside
 * it by (dx, dy). */
void webengine_translate(struct webengine_tree *tree, struct webengine_box *box, int dx, int dy);

/* The tree's painting order, from its layout. 0, or -1 when out of memory. */
int webengine_paint_order(struct webengine_tree *tree);
