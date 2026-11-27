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

/* The offsets of a position: relative box (9.4.3), percentages of a
 * containing block `width` wide and `height` high (or none, when < 0). */
void webengine_relative_offset(const struct webengine_box *box, int width, int height, int *dx, int *dy);

/* How an image box is laid out. */
enum webengine_image_mode {
    /* A box of its size, its picture painted in it when it is ready. */
    WEBENGINE_IMAGE_BOX,
    /* Its alt="" text, as text of its line: an inline image that is not
     * ready and has no size given. */
    WEBENGINE_IMAGE_ALT,
};

/* The content size of image box `box` (CSS 2.1 10.3.2, 10.6.2) in a
 * containing block `cb_width` wide and `cb_height` high (-1 unknown): the
 * width and height given, one of them and the picture's ratio, or the
 * picture's natural size. A picture not ready yet is as big as given;
 * with no size given, an inline one shows its alt text, and one that
 * failed and has no alt is 16 pixels square, as browsers show a broken
 * image. */
uint8_t webengine_image_size(const struct webengine_tree *tree, const struct webengine_box *box, int cb_width,
                             int cb_height, int *width, int *height);

/* --- Tables (webengine_table.c) --------------------------------------------- */

/* A cell in the grid: its first row and column and how many it spans, and
 * once laid out its height before the row stretches it and its baseline,
 * from its top border edge. */
struct webengine_table_cell {
    struct webengine_box *box;
    int row;
    int column;
    int rows;
    int columns;
    int height;
    int baseline;
};

/* A column: its minimum and preferred widths (border boxes of its cells),
 * a width in pixels (-1 none) or a percentage (-1 none), and once
 * distributed its width and its left edge from the table's content edge. */
struct webengine_table_column {
    int min;
    int max;
    int fixed;
    css_fixed percent;
    int width;
    int x;
};

struct webengine_table_row {
    struct webengine_box *box;
    struct webengine_box *group;
    int y;
    int height;
};

struct webengine_table {
    struct webengine_box *box;
    /* In grid order: row by row, left to right. */
    struct webengine_table_cell *cells;
    size_t cell_count;
    struct webengine_table_column *columns;
    size_t column_count;
    struct webengine_table_row *rows;
    size_t row_count;
    /* row_first[r]: the first cell starting in row r or later. */
    size_t *row_first;
    struct webengine_box **captions;
    size_t caption_count;
    /* Some cell spans more than one row. */
    bool spans;
    /* border-spacing across and down. */
    int spacing[2];
    /* The table's minimum and preferred content widths: the columns and
     * the spacing around them. */
    int min;
    int max;
    int *scratch;
};

/* The grid of the table `box` (17.5, HTML's table model): its captions, its
 * columns, its rows with the header group first and the footer group last,
 * and its cells. 0, or -1 when out of memory. */
int webengine_table_grid(struct webengine_table *table, struct webengine_box *box);

/* The columns' minimum and preferred widths and the table's, once the
 * cells' min_content and max_content are known. 0, or -1. */
int webengine_table_columns(struct webengine_table *table);

/* Distributes a table content `width` over the columns (17.5.2.2). */
void webengine_table_widths(struct webengine_table *table, int width);

void webengine_table_free(struct webengine_table *table);

/* The tree's painting order, from its layout. 0, or -1 when out of memory. */
int webengine_paint_order(struct webengine_tree *tree);
