#pragma once

/* The engine's layout internals, shared by its block flow
 * (webengine_layout.c) and its inline flow (webengine_lines.c). */

#include "webengine_box.h"

/* A length in pixels: a percentage of `base`, `automatic` for auto and none,
 * and for a percentage with no base (base < 0). */
int webengine_length_px(const struct webengine_len *length, int base, int automatic);

struct webengine_atom;
struct webengine_placed;

/* What the inline flow keeps between blocks. */
struct webengine_flow {
    const struct webengine_fonts *fonts;
    struct webengine_tree *tree;
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
