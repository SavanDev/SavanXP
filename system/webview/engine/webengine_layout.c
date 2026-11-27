/* Web Viewer's layout (webengine_box.h): CSS 2.1 chapter 10's normal flow,
 * 9.5's floats and 9.6's positioning over the box tree. Written from CSS 2.1
 * 8.3.1 (collapsing margins), 9.4.3 (relative positioning), 9.5 (floats and
 * clearance), 10.3.3, 10.3.5 and 10.3.7 (block, float and absolute widths),
 * 10.6.3, 10.6.4 and 10.6.7 (heights) and 10.4/10.7 (minimum and maximum
 * sizes); NetSurf's layout is not its model (the clean-room rule).
 *
 * The walk keeps an explicit stack, so a deep document does not recurse. A
 * float is laid out with its margin box at the origin, where its size does
 * not depend on anything around it, and then moved to its place. An
 * absolute box waits for its containing block to end, and a box ends only
 * once the absolute boxes it contains are laid out. */

#include "webengine_box.h"
#include "webengine_layout.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* --- Lengths -------------------------------------------------------------------- */

static int fixed_round(css_fixed value) {
    return value >= 0 ? (int)((value + 512) >> 10) : -(int)((-value + 512) >> 10);
}

/* A length in pixels: a percentage of `base`, `automatic` for auto, none,
 * or a percentage with no base (base < 0). */
int webengine_length_px(const struct webengine_len *length, int base, int automatic) {
    switch (length->kind) {
    case WEBENGINE_LEN_PX:
        return fixed_round(length->value);
    case WEBENGINE_LEN_PERCENT:
        if (base < 0) {
            return automatic;
        }
        return fixed_round((css_fixed)(((int64_t)length->value * base) / 100));
    default:
        return automatic;
    }
}

/* --- Collapsing margins (CSS 2.1 8.3.1) ------------------------------------------ */

/* Adjoining margins collapse into the largest positive one plus the most
 * negative one. */
struct collapse {
    int positive;
    int negative;
};

static void collapse_add(struct collapse *collapse, int margin) {
    if (margin > collapse->positive) {
        collapse->positive = margin;
    }
    if (margin < collapse->negative) {
        collapse->negative = margin;
    }
}

static int collapse_value(const struct collapse *collapse) {
    return collapse->positive + collapse->negative;
}

/* --- Block widths (CSS 2.1 10.3.3, 10.4) ------------------------------------------- */

static bool is_auto(const struct webengine_len *length) {
    return length->kind == WEBENGINE_LEN_AUTO;
}

/* The used width and horizontal margins of `box` for a content width
 * `width` (or -1 for auto) in a containing block `cb` wide. */
static int solve_width(struct webengine_box *box, int cb, int width) {
    const struct webengine_box_style *style = box->style;
    bool left_auto = is_auto(&style->margin[WEBENGINE_LEFT]);
    bool right_auto = is_auto(&style->margin[WEBENGINE_RIGHT]);
    int left = webengine_length_px(&style->margin[WEBENGINE_LEFT], cb, 0);
    int right = webengine_length_px(&style->margin[WEBENGINE_RIGHT], cb, 0);
    int edges = box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] +
        box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT];
    if (width < 0) {
        /* Auto margins are zero; the width takes what is left. */
        width = cb - left - edges - right;
        if (width < 0) {
            width = 0;
            right = cb - left - edges;
        }
    } else {
        if (left + edges + width + right > cb) {
            left_auto = right_auto = false;
        }
        /* HTML's align="" and <center> align blocks too, through libcss'
         * -libcss- values of the parent's text-align, when neither margin
         * is auto. */
        uint8_t align = box->parent != NULL ? box->parent->style->text_align : CSS_TEXT_ALIGN_DEFAULT;
        if (!left_auto && !right_auto && left + edges + width + right <= cb) {
            if (align == CSS_TEXT_ALIGN_LIBCSS_CENTER) {
                left_auto = right_auto = true;
                left = right = 0;
            } else if (align == CSS_TEXT_ALIGN_LIBCSS_RIGHT) {
                left_auto = true;
            }
        }
        int rest = cb - edges - width;
        if (left_auto && right_auto) {
            left = rest / 2;
            right = rest - left;
        } else if (left_auto) {
            left = rest - right;
        } else {
            /* Over-constrained, or only the right margin auto: the right
             * margin gives way (left to right text). */
            right = rest - left;
        }
    }
    box->margin[WEBENGINE_LEFT] = left;
    box->margin[WEBENGINE_RIGHT] = right;
    return width;
}

/* A specified width as a content width: box-sizing: border-box counts the
 * padding and borders in it. */
static int content_width(const struct webengine_box *box, int specified) {
    if (box->style->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
        specified -= box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] +
            box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT];
    }
    return specified < 0 ? 0 : specified;
}

static int content_height(const struct webengine_box *box, int specified) {
    if (box->style->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
        specified -= box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] +
            box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    }
    return specified < 0 ? 0 : specified;
}

static int block_width(struct webengine_box *box, int cb) {
    const struct webengine_box_style *style = box->style;
    int specified = webengine_length_px(&style->width, cb, -1);
    int width = solve_width(box, cb, specified < 0 ? -1 : content_width(box, specified));
    int max = webengine_length_px(&style->max_width, cb, -1);
    if (max >= 0 && width > content_width(box, max)) {
        width = solve_width(box, cb, content_width(box, max));
    }
    int min = webengine_length_px(&style->min_width, cb, 0);
    if (min > 0 && width < content_width(box, min)) {
        width = solve_width(box, cb, content_width(box, min));
    }
    return width;
}

/* The edges of `box`: borders, and padding and vertical margins, which take
 * their percentages from the containing block's width. */
static void block_edges(struct webengine_box *box, int cb) {
    const struct webengine_box_style *style = box->style;
    for (int side = 0; side < 4; side++) {
        box->border[side] = style->border_width[side];
        box->padding[side] = webengine_length_px(&style->padding[side], cb, 0);
        if (box->padding[side] < 0) {
            box->padding[side] = 0;
        }
    }
    box->margin[WEBENGINE_TOP] = webengine_length_px(&style->margin[WEBENGINE_TOP], cb, 0);
    box->margin[WEBENGINE_BOTTOM] = webengine_length_px(&style->margin[WEBENGINE_BOTTOM], cb, 0);
}

/* --- Floats (CSS 2.1 9.5) -------------------------------------------------------- */

static bool in_rows(const struct webengine_float *area, int top, int height) {
    return area->top < top + height && area->bottom > top;
}

void webengine_float_space(const struct webengine_flow *flow, int top, int height, int *left, int *right) {
    if (height < 1) {
        height = 1;
    }
    for (size_t i = flow->context; i < flow->float_count; i++) {
        const struct webengine_float *area = &flow->floats[i];
        if (!in_rows(area, top, height)) {
            continue;
        }
        if (area->side == CSS_FLOAT_LEFT && area->right > *left) {
            *left = area->right;
        } else if (area->side == CSS_FLOAT_RIGHT && area->left < *right) {
            *right = area->left;
        }
    }
}

int webengine_float_below(const struct webengine_flow *flow, int top, int height) {
    if (height < 1) {
        height = 1;
    }
    int below = INT_MAX;
    for (size_t i = flow->context; i < flow->float_count; i++) {
        const struct webengine_float *area = &flow->floats[i];
        if (in_rows(area, top, height) && area->bottom < below) {
            below = area->bottom;
        }
    }
    return below == INT_MAX ? top + 1 : below;
}

int webengine_float_clearance(const struct webengine_flow *flow, uint8_t clear) {
    int bottom = INT_MIN;
    if (clear != CSS_CLEAR_LEFT && clear != CSS_CLEAR_RIGHT && clear != CSS_CLEAR_BOTH) {
        return bottom;
    }
    for (size_t i = flow->context; i < flow->float_count; i++) {
        const struct webengine_float *area = &flow->floats[i];
        bool cleared = clear == CSS_CLEAR_BOTH || (clear == CSS_CLEAR_LEFT && area->side == CSS_FLOAT_LEFT) ||
            (clear == CSS_CLEAR_RIGHT && area->side == CSS_FLOAT_RIGHT);
        if (cleared && area->bottom > bottom) {
            bottom = area->bottom;
        }
    }
    return bottom;
}

void webengine_place_float(struct webengine_flow *flow, struct webengine_box *float_box, int left, int right,
                           int top) {
    int width = float_box->margin[WEBENGINE_LEFT] + float_box->width + float_box->margin[WEBENGINE_RIGHT];
    int height = float_box->margin[WEBENGINE_TOP] + float_box->height + float_box->margin[WEBENGINE_BOTTOM];
    uint8_t side = float_box->style->float_side;
    width = width > 0 ? width : 0;
    height = height > 0 ? height : 0;
    /* No higher than an earlier float (rules 5 and 6), nor than what it
     * clears. */
    for (size_t i = flow->context; i < flow->float_count; i++) {
        if (flow->floats[i].top > top) {
            top = flow->floats[i].top;
        }
    }
    int clear = webengine_float_clearance(flow, float_box->style->clear);
    if (clear > top) {
        top = clear;
    }
    /* As high as it fits beside the floats there, then as far out. */
    int x = left;
    for (;;) {
        int room_left = left;
        int room_right = right;
        webengine_float_space(flow, top, height, &room_left, &room_right);
        if (room_right - room_left >= width || (room_left == left && room_right == right)) {
            x = side == CSS_FLOAT_LEFT ? room_left : room_right - width;
            break;
        }
        top = webengine_float_below(flow, top, height);
    }
    webengine_translate(flow->tree, float_box, x, top);
    if (flow->float_count == flow->float_capacity) {
        size_t capacity = flow->float_capacity == 0 ? 16 : flow->float_capacity * 2;
        struct webengine_float *floats = realloc(flow->floats, capacity * sizeof(*floats));
        if (floats == NULL) {
            flow->failed = true;
            return;
        }
        flow->floats = floats;
        flow->float_capacity = capacity;
    }
    flow->floats[flow->float_count++] = (struct webengine_float){ x, top, x + width, top + height, side };
}

/* The bottom of the lowest float from floats[first] on, or INT_MIN. */
static int floats_bottom(const struct webengine_flow *flow, size_t first) {
    int bottom = INT_MIN;
    for (size_t i = first; i < flow->float_count; i++) {
        if (flow->floats[i].bottom > bottom) {
            bottom = flow->floats[i].bottom;
        }
    }
    return bottom;
}

void webengine_translate(struct webengine_tree *tree, struct webengine_box *top, int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return;
    }
    struct webengine_box *box = top;
    for (;;) {
        if (box->type == WEBENGINE_BOX_BLOCK) {
            box->x += dx;
            box->y += dy;
            if (box->marker != NULL) {
                box->marker->x += dx;
                box->marker->y += dy;
            }
            for (size_t l = box->first_line; box->inline_content && l < box->first_line + box->line_count; l++) {
                struct webengine_line *line = &tree->lines[l];
                line->x += dx;
                line->y += dy;
                line->baseline += dy;
                for (size_t f = line->first_fragment; f < line->first_fragment + line->fragment_count; f++) {
                    tree->fragments[f].x += dx;
                    tree->fragments[f].y += dy;
                    tree->fragments[f].baseline += dy;
                }
            }
        }
        if (box->first != NULL) {
            box = box->first;
            continue;
        }
        while (box != top && box->next == NULL) {
            box = box->parent;
        }
        if (box == top) {
            return;
        }
        box = box->next;
    }
}

/* --- Shrink-to-fit widths (CSS 2.1 10.3.5) ----------------------------------------- */

static int px_or_zero(const struct webengine_len *length) {
    return length->kind == WEBENGINE_LEN_PX ? fixed_round(length->value) : 0;
}

int webengine_intrinsic_edges(const struct webengine_box *box) {
    const struct webengine_box_style *style = box->style;
    int left = px_or_zero(&style->padding[WEBENGINE_LEFT]);
    int right = px_or_zero(&style->padding[WEBENGINE_RIGHT]);
    return px_or_zero(&style->margin[WEBENGINE_LEFT]) + px_or_zero(&style->margin[WEBENGINE_RIGHT]) +
        style->border_width[WEBENGINE_LEFT] + style->border_width[WEBENGINE_RIGHT] +
        (left > 0 ? left : 0) + (right > 0 ? right : 0);
}

/* A width in pixels as a content width, -1 when it is not in pixels. */
static int intrinsic_size(const struct webengine_box *box, const struct webengine_len *length) {
    if (length->kind != WEBENGINE_LEN_PX) {
        return -1;
    }
    int size = fixed_round(length->value);
    if (box->style->box_sizing == CSS_BOX_SIZING_BORDER_BOX) {
        const struct webengine_box_style *style = box->style;
        size -= style->border_width[WEBENGINE_LEFT] + style->border_width[WEBENGINE_RIGHT] +
            px_or_zero(&style->padding[WEBENGINE_LEFT]) + px_or_zero(&style->padding[WEBENGINE_RIGHT]);
    }
    return size > 0 ? size : 0;
}

/* A table's minimum and preferred content widths, from its grid (17.5.2.2);
 * a width in pixels, which counts its borders and padding, as a floor. */
static void intrinsic_table(struct webengine_flow *flow, struct webengine_box *box) {
    struct webengine_table table;
    if (webengine_table_grid(&table, box) != 0 || webengine_table_columns(&table) != 0) {
        webengine_table_free(&table);
        flow->failed = true;
        box->min_content = box->max_content = 0;
        return;
    }
    int min = table.min;
    int max = table.max;
    webengine_table_free(&table);
    const struct webengine_box_style *style = box->style;
    if (style->width.kind == WEBENGINE_LEN_PX) {
        int edges = style->border_width[WEBENGINE_LEFT] + style->border_width[WEBENGINE_RIGHT] +
            px_or_zero(&style->padding[WEBENGINE_LEFT]) + px_or_zero(&style->padding[WEBENGINE_RIGHT]);
        int width = fixed_round(style->width.value) - edges;
        min = width > min ? width : min;
        max = min;
    }
    box->min_content = min;
    box->max_content = max > min ? max : min;
}

/* The minimum and preferred widths of a block whose children's are known. */
static void intrinsic_block(struct webengine_flow *flow, struct webengine_box *box) {
    int min = 0;
    int max = 0;
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        intrinsic_table(flow, box);
        return;
    }
    if (box->inline_content) {
        webengine_inline_widths(flow, box, &min, &max);
    } else {
        for (struct webengine_box *child = box->first; child != NULL; child = child->next) {
            if (child->type == WEBENGINE_BOX_BLOCK && !webengine_box_absolute(child)) {
                int edges = webengine_intrinsic_edges(child);
                min = child->min_content + edges > min ? child->min_content + edges : min;
                max = child->max_content + edges > max ? child->max_content + edges : max;
            }
        }
    }
    /* A cell's width is its column's (webengine_table_columns). */
    int width = box->table_role == WEBENGINE_TABLE_CELL ? -1 : intrinsic_size(box, &box->style->width);
    if (width >= 0) {
        min = max = width;
    }
    int cap = intrinsic_size(box, &box->style->max_width);
    if (cap >= 0) {
        min = min < cap ? min : cap;
        max = max < cap ? max : cap;
    }
    int floor = intrinsic_size(box, &box->style->min_width);
    if (floor > 0) {
        min = min > floor ? min : floor;
        max = max > floor ? max : floor;
    }
    box->min_content = min;
    box->max_content = max > min ? max : min;
}

/* The minimum and preferred widths of `top` and the blocks inside it,
 * innermost first, without recursing. */
static void intrinsic_widths(struct webengine_flow *flow, struct webengine_box *top) {
    struct webengine_box *box = top;
    for (;;) {
        while (box->first != NULL) {
            box = box->first;
        }
        for (;;) {
            if (box->type == WEBENGINE_BOX_BLOCK) {
                intrinsic_block(flow, box);
            }
            if (box == top) {
                return;
            }
            if (box->next != NULL) {
                box = box->next;
                break;
            }
            box = box->parent;
        }
    }
}

/* --- The walk ------------------------------------------------------------------------ */

struct frame {
    struct webengine_box *box;
    /* The next child to lay out. */
    struct webengine_box *child;
    int content_x;
    int content_width;
    /* The top of the content box, once known. */
    int content_top;
    /* The content height percentages of children resolve against, or -1. */
    int percent_base;
    /* Where the next child's margin box starts, before collapsing. */
    int cursor;
    /* The margins adjoining that point, not yet resolved, kept by this frame
     * or by the ancestor whose top margin this box's collapses with. */
    struct collapse own;
    size_t pending;
    /* The box's top border edge is known: something resolved the margins
     * above it. */
    bool resolved;
    /* The first float of the block formatting context the box's content
     * is in (webengine_flow.floats). */
    size_t context;
    /* Inline content whose floats are being laid out, `scan` the last one
     * so far; its line boxes come after them. */
    bool lines_pending;
    struct webengine_box *scan;
    /* A float, laid out at the origin: placed when it ends if it is a
     * block's child, by the line it is on if it is in inline content. */
    uint8_t kind;
    bool block_level;
    /* Its own box is done; what is left is the absolute boxes for which it
     * is the containing block. */
    bool finished;
    /* The frame of the nearest positioned box, itself included, 0 for the
     * initial containing block; and the first absolute box met inside. */
    size_t positioned;
    size_t absolute_base;
    /* ABSOLUTE: the containing block's height, the content height when
     * top and bottom stretch it (or -1), and where its bottom margin edge
     * goes when only bottom places it (or INT_MIN). */
    int cb_height;
    int stretch;
    int anchor_bottom;
    /* The outside marker that waited outside the float. */
    struct webengine_box *outer_marker;
    int outer_marker_left;
    /* A table: its state, layout.tables[table - 1], and whether its parts
     * are all laid out. A cell: grid.cells[cell - 1] of the table below. */
    size_t table;
    bool table_done;
    size_t cell;
};

enum frame_kind { FRAME_BLOCK, FRAME_FLOAT, FRAME_ABSOLUTE, FRAME_CELL };

/* A table being laid out (CSS 2.1 17.5): its parts in order, top captions,
 * the rows and their cells, then bottom captions. */
enum table_phase { TABLE_TOP_CAPTIONS, TABLE_ROWS, TABLE_BOTTOM_CAPTIONS };

struct table_layout {
    struct webengine_table grid;
    uint8_t phase;
    /* The next caption or cell, and the row being laid out. */
    size_t next;
    size_t row;
    /* The table box's top border edge, and its content box's left edge,
     * width and top. */
    int table_y;
    int content_x;
    int content_width;
    int content_top;
};

/* An absolute box waiting for the frame of its containing block to end. */
struct absolute {
    struct webengine_box *box;
    size_t containing;
};

struct layout {
    struct webengine_flow flow;
    struct frame *frames;
    size_t depth;
    size_t capacity;
    int viewport_height;
    struct absolute *absolutes;
    size_t absolute_count;
    size_t absolute_capacity;
    /* The lowest bottom of an absolute box, for the page's height. */
    int extent;
    /* The tables open, innermost last. */
    struct table_layout *tables;
    size_t table_count;
    size_t table_capacity;
    bool failed;
};

static struct collapse *pending_of(struct layout *layout, size_t index) {
    return &layout->frames[layout->frames[index].pending].own;
}

/* A box that starts a block formatting context (9.4.1): its margins do not
 * collapse with its children's, and the floats inside it are its own. */
static bool new_context(const struct webengine_box *box) {
    return box->parent == NULL || box->style->overflow != CSS_OVERFLOW_VISIBLE || webengine_box_floats(box) ||
        webengine_box_absolute(box) || box->table_role == WEBENGINE_TABLE_TABLE ||
        box->table_role == WEBENGINE_TABLE_CELL || box->table_role == WEBENGINE_TABLE_CAPTION;
}

void webengine_relative_offset(const struct webengine_box *box, int width, int height, int *dx, int *dy) {
    *dx = webengine_length_px(&box->style->offset[WEBENGINE_LEFT], width, 0);
    *dy = webengine_length_px(&box->style->offset[WEBENGINE_TOP], height, 0);
}

/* Moves a position: relative box that has ended by its offsets (9.4.3). */
static void offset_relative(struct layout *layout, struct webengine_box *box, int width, int height) {
    if (box->style->position == CSS_POSITION_RELATIVE && box->parent != NULL) {
        int dx = 0;
        int dy = 0;
        webengine_relative_offset(box, width, height, &dx, &dy);
        webengine_translate(layout->flow.tree, box, dx, dy);
    }
}

/* The box's top border edge is at `y`; so is that of every enclosing box
 * whose top margin collapsed with it. */
static void resolve_top(struct layout *layout, size_t index, int y) {
    for (size_t i = index; i > 0 && !layout->frames[i].resolved; i--) {
        struct frame *frame = &layout->frames[i];
        frame->resolved = true;
        frame->box->y = y;
        frame->content_top = y + frame->box->border[WEBENGINE_TOP] + frame->box->padding[WEBENGINE_TOP];
        frame->cursor = frame->content_top;
    }
}

static bool push_frame(struct layout *layout) {
    if (layout->depth == layout->capacity) {
        size_t capacity = layout->capacity == 0 ? 64 : layout->capacity * 2;
        struct frame *frames = realloc(layout->frames, capacity * sizeof(*frames));
        if (frames == NULL) {
            layout->failed = true;
            return false;
        }
        layout->frames = frames;
        layout->capacity = capacity;
    }
    struct frame *frame = &layout->frames[layout->depth++];
    memset(frame, 0, sizeof(*frame));
    frame->absolute_base = layout->absolute_count;
    return true;
}

/* Puts an absolute box met at the top of the stack on its containing
 * block's list: the nearest positioned box's, the viewport's for fixed. */
static void meet_absolute(struct layout *layout, struct webengine_box *box) {
    if (layout->absolute_count == layout->absolute_capacity) {
        size_t capacity = layout->absolute_capacity == 0 ? 16 : layout->absolute_capacity * 2;
        struct absolute *absolutes = realloc(layout->absolutes, capacity * sizeof(*absolutes));
        if (absolutes == NULL) {
            layout->failed = true;
            return;
        }
        layout->absolutes = absolutes;
        layout->absolute_capacity = capacity;
    }
    size_t containing = box->style->position == CSS_POSITION_FIXED ? 0
        : layout->frames[layout->depth - 1].positioned;
    layout->absolutes[layout->absolute_count++] = (struct absolute){ box, containing };
}

/* What every block does once its box is known: an outside marker waits for
 * the item's first line box (12.5), and inline content waits for its floats. */
static void start_content(struct layout *layout, struct frame *frame) {
    struct webengine_box *box = frame->box;
    if (box->marker != NULL) {
        layout->flow.marker_item = box;
        layout->flow.marker_left = frame->content_x;
    }
    if (box->inline_content) {
        frame->lines_pending = true;
        frame->child = NULL;
    }
}

/* The next float or absolute box in the inline content of `block` after
 * `after` (NULL for the first), in the order of the content. */
static struct webengine_box *next_out_of_flow(struct webengine_box *block, struct webengine_box *after) {
    struct webengine_box *box = after != NULL ? after : block->first;
    bool skip = after != NULL;
    while (box != NULL) {
        if (!skip) {
            if (box->type == WEBENGINE_BOX_BLOCK) {
                return box;
            }
            if (box->type == WEBENGINE_BOX_INLINE && box->first != NULL) {
                box = box->first;
                continue;
            }
        }
        skip = false;
        while (box != block && box->next == NULL) {
            box = box->parent;
        }
        box = box == block ? NULL : box->next;
    }
    return NULL;
}

/* A block formatting context's root next to floats in the one around it
 * (9.5): its border box goes beside them, narrower when its width is auto,
 * else lower down where it fits. */
static void avoid_floats(struct layout *layout, size_t index) {
    struct frame *frame = &layout->frames[index];
    struct frame *parent = &layout->frames[index - 1];
    struct webengine_box *box = frame->box;
    const struct webengine_box_style *style = box->style;
    int cb = parent->content_width;
    int left = parent->content_x;
    int right = parent->content_x + cb;
    int margin_left = webengine_length_px(&style->margin[WEBENGINE_LEFT], cb, 0);
    int margin_right = webengine_length_px(&style->margin[WEBENGINE_RIGHT], cb, 0);
    int edges = box->width - frame->content_width;
    /* A table keeps the width its columns gave it. */
    bool auto_width = webengine_length_px(&style->width, cb, -1) < 0 && box->table_role != WEBENGINE_TABLE_TABLE;
    int specified = webengine_length_px(&style->height, parent->percent_base, -1);
    int rows = specified >= 0 ? content_height(box, specified) + box->border[WEBENGINE_TOP] +
        box->padding[WEBENGINE_TOP] + box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM] : 1;
    int y = box->y;
    layout->flow.context = parent->context;
    for (;;) {
        int room_left = left;
        int room_right = right;
        webengine_float_space(&layout->flow, y, rows, &room_left, &room_right);
        if (room_left == left && room_right == right) {
            break;
        }
        int border_left = left + margin_left > room_left ? left + margin_left : room_left;
        int border_right = right - margin_right < room_right ? right - margin_right : room_right;
        if (auto_width || border_right - border_left >= box->width) {
            if (auto_width) {
                int width = border_right - border_left - edges;
                frame->content_width = width > 0 ? width : 0;
                box->width = frame->content_width + edges;
            }
            /* Auto margins centre it, or push it right, in the room. */
            bool auto_left = is_auto(&style->margin[WEBENGINE_LEFT]);
            bool auto_right = is_auto(&style->margin[WEBENGINE_RIGHT]);
            if (!auto_width && auto_left && auto_right) {
                border_left += (border_right - border_left - box->width) / 2;
            } else if (!auto_width && auto_left) {
                border_left = border_right - box->width;
            }
            int shift = border_left - box->x;
            box->x += shift;
            frame->content_x += shift;
            break;
        }
        y = webengine_float_below(&layout->flow, y, rows);
    }
    if (y != box->y) {
        frame->content_top += y - box->y;
        frame->cursor = frame->content_top;
        box->y = y;
    }
}

static int table_open(struct layout *layout, struct webengine_box *box, int cb, int available);
static void table_start(struct layout *layout, size_t index);

/* The margins of `box` in pixels, auto counting as nothing. */
static int horizontal_margins(const struct webengine_box *box, int cb) {
    return webengine_length_px(&box->style->margin[WEBENGINE_LEFT], cb, 0) +
        webengine_length_px(&box->style->margin[WEBENGINE_RIGHT], cb, 0);
}

static int horizontal_edges(const struct webengine_box *box) {
    return box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] + box->padding[WEBENGINE_RIGHT] +
        box->border[WEBENGINE_RIGHT];
}

static void enter_block(struct layout *layout, struct webengine_box *box) {
    size_t parent_index = layout->depth - 1;
    if (!push_frame(layout)) {
        return;
    }
    size_t index = layout->depth - 1;
    struct frame *parent = &layout->frames[parent_index];
    struct frame *frame = &layout->frames[index];
    int cb = parent->content_width;
    block_edges(box, cb);
    frame->box = box;
    frame->child = box->first;
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        /* A table's width comes from its columns (17.5.2.2); its margins
         * are solved around it. */
        int width = table_open(layout, box, cb, cb - horizontal_margins(box, cb)) - horizontal_edges(box);
        frame->content_width = solve_width(box, cb, width > 0 ? width : 0);
    } else {
        frame->content_width = block_width(box, cb);
    }
    box->x = parent->content_x + box->margin[WEBENGINE_LEFT];
    box->width = box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] + frame->content_width +
        box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT];
    frame->content_x = box->x + box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
    int specified = webengine_length_px(&box->style->height, parent->percent_base, -1);
    frame->percent_base = specified >= 0 ? content_height(box, specified) : -1;
    bool context = new_context(box);
    frame->context = context ? layout->flow.float_count : parent->context;
    frame->positioned = webengine_box_positioned(box) ? index : parent->positioned;

    struct collapse *pending = pending_of(layout, parent_index);
    struct collapse above = *pending;
    collapse_add(pending, box->margin[WEBENGINE_TOP]);
    /* Clearance (9.5.2): a box that would start above the floats it clears
     * starts at their bottom instead, and its top margin no longer collapses
     * with the margins above it, which end where they would without it. */
    layout->flow.context = parent->context;
    int clear = webengine_float_clearance(&layout->flow, box->style->clear);
    bool cleared = clear != INT_MIN && parent->cursor + collapse_value(pending) < clear;
    if (cleared) {
        resolve_top(layout, parent_index, parent->cursor + collapse_value(&above));
        *pending = (struct collapse){ 0, 0 };
    }
    if (box->marker != NULL) {
        /* A marker still waiting for a line goes at the top of this item. */
        webengine_place_marker(&layout->flow, parent->cursor + collapse_value(pending));
    }
    if (cleared || box->border[WEBENGINE_TOP] > 0 || box->padding[WEBENGINE_TOP] > 0 || context) {
        /* The margins above end at the top border, which is also that of
         * the enclosing boxes whose top margins collapsed with them. */
        int y = cleared ? clear : parent->cursor + collapse_value(pending);
        *pending = (struct collapse){ 0, 0 };
        frame->pending = index;
        resolve_top(layout, index, y);
    } else {
        /* The top margin collapses with the first child's. */
        frame->cursor = parent->cursor;
        frame->pending = parent->pending;
    }
    if (context && parent_index > 0) {
        avoid_floats(layout, index);
    }
    start_content(layout, frame);
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        table_start(layout, index);
    }
}

/* An auto width that shrinks to fit the content (10.3.5) in `available`. */
static int shrink_to_fit(struct layout *layout, struct webengine_box *box, int available) {
    intrinsic_widths(&layout->flow, box);
    int width = box->max_content < available ? box->max_content : available;
    return width > box->min_content ? width : box->min_content;
}

/* A width within min-width and max-width. */
static int clamp_width(const struct webengine_box *box, int cb, int width) {
    int max = webengine_length_px(&box->style->max_width, cb, -1);
    if (max >= 0 && width > content_width(box, max)) {
        width = content_width(box, max);
    }
    int min = webengine_length_px(&box->style->min_width, cb, 0);
    if (min > 0 && width < content_width(box, min)) {
        width = content_width(box, min);
    }
    return width;
}

/* A float (10.3.5): auto margins are zero, and an auto width shrinks to fit
 * its content. Its margin box starts at the origin. */
static void enter_float(struct layout *layout, struct webengine_box *box, bool block_level) {
    size_t parent_index = layout->depth - 1;
    if (!push_frame(layout)) {
        return;
    }
    size_t index = layout->depth - 1;
    struct frame *parent = &layout->frames[parent_index];
    struct frame *frame = &layout->frames[index];
    const struct webengine_box_style *style = box->style;
    int cb = parent->content_width;
    block_edges(box, cb);
    box->margin[WEBENGINE_LEFT] = webengine_length_px(&style->margin[WEBENGINE_LEFT], cb, 0);
    box->margin[WEBENGINE_RIGHT] = webengine_length_px(&style->margin[WEBENGINE_RIGHT], cb, 0);
    int edges = box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] + box->padding[WEBENGINE_RIGHT] +
        box->border[WEBENGINE_RIGHT];
    int specified = webengine_length_px(&style->width, cb, -1);
    int width = 0;
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        width = table_open(layout, box, cb, cb - box->margin[WEBENGINE_LEFT] - box->margin[WEBENGINE_RIGHT]) - edges;
        width = width > 0 ? width : 0;
    } else {
        width = specified >= 0 ? content_width(box, specified)
            : shrink_to_fit(layout, box, cb - box->margin[WEBENGINE_LEFT] - box->margin[WEBENGINE_RIGHT] - edges);
        width = clamp_width(box, cb, width);
    }
    frame->box = box;
    frame->child = box->first;
    frame->kind = FRAME_FLOAT;
    frame->block_level = block_level;
    frame->positioned = webengine_box_positioned(box) ? index : parent->positioned;
    frame->content_width = width;
    box->x = box->margin[WEBENGINE_LEFT];
    box->y = box->margin[WEBENGINE_TOP];
    box->width = edges + width;
    frame->content_x = box->x + box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
    int height = webengine_length_px(&style->height, parent->percent_base, -1);
    frame->percent_base = height >= 0 ? content_height(box, height) : -1;
    frame->pending = index;
    frame->resolved = true;
    frame->content_top = box->y + box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP];
    frame->cursor = frame->content_top;
    frame->context = layout->flow.float_count;
    frame->outer_marker = layout->flow.marker_item;
    frame->outer_marker_left = layout->flow.marker_left;
    layout->flow.marker_item = NULL;
    start_content(layout, frame);
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        table_start(layout, index);
    }
}

/* Lays out the line boxes of the block at the top of the stack, once its
 * floats are laid out. */
static void lay_out_inline(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct frame *parent = &layout->frames[index - 1];
    struct collapse *pending = pending_of(layout, index - 1);
    int start = frame->resolved ? frame->cursor : parent->cursor + collapse_value(pending);
    frame->cursor = start;
    layout->flow.context = frame->context;
    int lines = webengine_layout_lines(&layout->flow, frame->box, frame->content_x, start, frame->content_width);
    if (lines > 0) {
        /* Line boxes separate the margins above from those below. */
        if (!frame->resolved) {
            *pending = (struct collapse){ 0, 0 };
            resolve_top(layout, index, start);
        }
        frame->cursor = start + lines;
    } else if (!frame->resolved) {
        frame->cursor = parent->cursor;
    }
}

/* The used height of `box`'s content: `content` when auto, within its
 * minimum and maximum (10.6.3, 10.7). */
static int used_height(const struct webengine_box *box, int percent_base, int content) {
    const struct webengine_box_style *style = box->style;
    int specified = webengine_length_px(&style->height, percent_base, -1);
    int min = webengine_length_px(&style->min_height, percent_base, 0);
    int max = webengine_length_px(&style->max_height, percent_base, -1);
    int height = specified < 0 ? content : content_height(box, specified);
    if (max >= 0 && height > content_height(box, max)) {
        height = content_height(box, max);
    }
    if (min > 0 && height < content_height(box, min)) {
        height = content_height(box, min);
    }
    return height > 0 ? height : 0;
}

/* A block's own box ends: its height, then the margins and cursor of the
 * block around it, then its relative offset. */
static void finish_block(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct frame *parent = &layout->frames[index - 1];
    struct webengine_box *box = frame->box;
    const struct webengine_box_style *style = box->style;
    bool auto_height = webengine_length_px(&style->height, parent->percent_base, -1) < 0;
    int min = webengine_length_px(&style->min_height, parent->percent_base, 0);
    bool context = new_context(box);
    bool bottom_edge = box->border[WEBENGINE_BOTTOM] > 0 || box->padding[WEBENGINE_BOTTOM] > 0 || context ||
        !auto_height;
    struct collapse *pending = pending_of(layout, index);
    struct collapse *outer = pending_of(layout, index - 1);

    if (layout->flow.marker_item == box) {
        /* No line box inside the item: its marker goes at its top. */
        webengine_place_marker(&layout->flow, frame->resolved ? frame->content_top
                                                              : parent->cursor + collapse_value(outer));
    }

    if (!frame->resolved) {
        if (bottom_edge || min > 0) {
            /* Not empty after all: its top border ends the margins above,
             * those of the empty children inside included. */
            resolve_top(layout, index, parent->cursor + collapse_value(outer));
            *outer = (struct collapse){ 0, 0 };
        } else {
            /* Empty: its margins collapse through it (8.3.1), and its top
             * border edge is where it would be with a bottom border. */
            box->y = parent->cursor + collapse_value(outer);
            box->height = 0;
            collapse_add(outer, box->margin[WEBENGINE_BOTTOM]);
            offset_relative(layout, box, parent->content_width, parent->percent_base);
            return;
        }
    }
    int content_bottom = frame->cursor;
    if (bottom_edge) {
        /* The last child's bottom margin stays inside. */
        content_bottom += collapse_value(pending);
        *pending = (struct collapse){ 0, 0 };
    }
    if (context) {
        /* A context's auto height takes in its floats (10.6.7), which end
         * with it. */
        int floats = floats_bottom(&layout->flow, frame->context);
        if (floats > content_bottom) {
            content_bottom = floats;
        }
        layout->flow.float_count = frame->context;
    }
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        /* The wrapper: the table box and its captions. */
        box->height = content_bottom - box->y;
    } else {
        int height = used_height(box, parent->percent_base, content_bottom - frame->content_top);
        box->height = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + height +
            box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    }
    parent->cursor = box->y + box->height;
    if (pending != outer) {
        /* What collapses through the bottom joins the margins after it. */
        *outer = *pending;
    }
    collapse_add(outer, box->margin[WEBENGINE_BOTTOM]);
    offset_relative(layout, box, parent->content_width, parent->percent_base);
}

static void finish_float(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct frame *parent = &layout->frames[index - 1];
    struct webengine_box *box = frame->box;
    if (layout->flow.marker_item == box) {
        webengine_place_marker(&layout->flow, frame->content_top);
    }
    int content_bottom = frame->cursor + collapse_value(&frame->own);
    int floats = floats_bottom(&layout->flow, frame->context);
    if (floats > content_bottom) {
        content_bottom = floats;
    }
    layout->flow.float_count = frame->context;
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        box->height = content_bottom - box->y;
    } else {
        int height = used_height(box, parent->percent_base, content_bottom - frame->content_top);
        box->height = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + height +
            box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    }
    layout->flow.marker_item = frame->outer_marker;
    layout->flow.marker_left = frame->outer_marker_left;
    if (frame->block_level) {
        /* A block's child goes where the next block would start. */
        layout->flow.context = parent->context;
        webengine_place_float(&layout->flow, box, parent->content_x, parent->content_x + parent->content_width,
                              parent->cursor + collapse_value(pending_of(layout, index - 1)));
    }
    offset_relative(layout, box, parent->content_width, parent->percent_base);
}

/* An absolute box (10.3.7, 10.6.4), when its containing block, the frame
 * at the top of the stack, has ended: its padding box, or the viewport.
 * Its box records its static position, where it would have been in the
 * flow, for the offsets that are auto. */
static void enter_absolute(struct layout *layout, struct webengine_box *box) {
    size_t containing = layout->depth - 1;
    if (!push_frame(layout)) {
        return;
    }
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    const struct webengine_box_style *style = box->style;
    int cb_x = 0;
    int cb_y = 0;
    int cb_width = layout->frames[0].content_width;
    int cb_height = layout->viewport_height;
    box->containing = NULL;
    if (containing > 0) {
        const struct webengine_box *block = layout->frames[containing].box;
        cb_x = block->x + block->border[WEBENGINE_LEFT];
        cb_y = block->y + block->border[WEBENGINE_TOP];
        cb_width = block->width - block->border[WEBENGINE_LEFT] - block->border[WEBENGINE_RIGHT];
        cb_height = block->height - block->border[WEBENGINE_TOP] - block->border[WEBENGINE_BOTTOM];
        box->containing = block;
    }
    int static_left = box->x - cb_x;
    int static_top = box->y - cb_y;
    block_edges(box, cb_width);

    /* Horizontally: left + margins + borders + padding + width + right is
     * the containing block's width; what is auto gives way. */
    bool left_auto = is_auto(&style->offset[WEBENGINE_LEFT]);
    bool right_auto = is_auto(&style->offset[WEBENGINE_RIGHT]);
    int specified = webengine_length_px(&style->width, cb_width, -1);
    bool width_auto = specified < 0;
    int left = webengine_length_px(&style->offset[WEBENGINE_LEFT], cb_width, 0);
    int right = webengine_length_px(&style->offset[WEBENGINE_RIGHT], cb_width, 0);
    int margin_left = webengine_length_px(&style->margin[WEBENGINE_LEFT], cb_width, 0);
    int margin_right = webengine_length_px(&style->margin[WEBENGINE_RIGHT], cb_width, 0);
    int edges = box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] + box->padding[WEBENGINE_RIGHT] +
        box->border[WEBENGINE_RIGHT];
    int width = width_auto ? 0 : content_width(box, specified);
    if (left_auto && right_auto) {
        left = static_left;
        if (width_auto) {
            width = shrink_to_fit(layout, box, cb_width - left - margin_left - margin_right - edges);
        }
    } else if (width_auto && (left_auto || right_auto)) {
        width = shrink_to_fit(layout, box, cb_width - (left_auto ? right : left) - margin_left - margin_right - edges);
    } else if (width_auto) {
        width = cb_width - left - right - margin_left - margin_right - edges;
        width = width > 0 ? width : 0;
    } else if (!left_auto && !right_auto) {
        int rest = cb_width - left - right - edges - width;
        bool auto_left_margin = is_auto(&style->margin[WEBENGINE_LEFT]);
        bool auto_right_margin = is_auto(&style->margin[WEBENGINE_RIGHT]);
        if (auto_left_margin && auto_right_margin) {
            margin_left = rest > 0 ? rest / 2 : 0;
        } else if (auto_left_margin) {
            margin_left = rest - margin_right;
        }
    }
    width = clamp_width(box, cb_width, width);
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        int available = cb_width - margin_left - margin_right -
            (left_auto && right_auto ? static_left : (left_auto ? 0 : left) + (right_auto ? 0 : right));
        width = table_open(layout, box, cb_width, available) - edges;
        width = width > 0 ? width : 0;
    }
    if (left_auto && !right_auto) {
        left = cb_width - right - margin_right - edges - width - margin_left;
    }
    box->margin[WEBENGINE_LEFT] = margin_left;
    box->margin[WEBENGINE_RIGHT] = margin_right;
    box->x = cb_x + left + margin_left;
    box->width = edges + width;

    /* Vertically the same, once the content's height is known; until
     * then the box starts at its top offset or its static position. */
    bool top_auto = is_auto(&style->offset[WEBENGINE_TOP]);
    bool bottom_auto = is_auto(&style->offset[WEBENGINE_BOTTOM]);
    int top = webengine_length_px(&style->offset[WEBENGINE_TOP], cb_height, 0);
    int bottom = webengine_length_px(&style->offset[WEBENGINE_BOTTOM], cb_height, 0);
    int edges_v = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + box->padding[WEBENGINE_BOTTOM] +
        box->border[WEBENGINE_BOTTOM];
    int height = webengine_length_px(&style->height, cb_height, -1);
    frame->cb_height = cb_height;
    frame->stretch = -1;
    frame->anchor_bottom = INT_MIN;
    if (!top_auto && !bottom_auto && height < 0) {
        int stretch = cb_height - top - bottom - box->margin[WEBENGINE_TOP] - box->margin[WEBENGINE_BOTTOM] - edges_v;
        frame->stretch = stretch > 0 ? stretch : 0;
    } else if (top_auto && !bottom_auto) {
        frame->anchor_bottom = cb_y + cb_height - bottom - box->margin[WEBENGINE_BOTTOM];
    }
    box->y = cb_y + (top_auto ? static_top : top) + box->margin[WEBENGINE_TOP];

    frame->box = box;
    frame->child = box->first;
    frame->kind = FRAME_ABSOLUTE;
    frame->positioned = index;
    frame->content_width = width;
    frame->content_x = box->x + box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
    frame->percent_base = height >= 0 ? content_height(box, height) : frame->stretch;
    frame->pending = index;
    frame->resolved = true;
    frame->content_top = box->y + box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP];
    frame->cursor = frame->content_top;
    frame->context = layout->flow.float_count;
    frame->outer_marker = layout->flow.marker_item;
    frame->outer_marker_left = layout->flow.marker_left;
    layout->flow.marker_item = NULL;
    start_content(layout, frame);
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        table_start(layout, index);
    }
}

static void finish_absolute(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct webengine_box *box = frame->box;
    if (layout->flow.marker_item == box) {
        webengine_place_marker(&layout->flow, frame->content_top);
    }
    int content_bottom = frame->cursor + collapse_value(&frame->own);
    int floats = floats_bottom(&layout->flow, frame->context);
    if (floats > content_bottom) {
        content_bottom = floats;
    }
    layout->flow.float_count = frame->context;
    if (box->table_role == WEBENGINE_TABLE_TABLE) {
        box->height = content_bottom - box->y;
    } else {
        int content = frame->stretch >= 0 ? frame->stretch : content_bottom - frame->content_top;
        int height = used_height(box, frame->cb_height, content);
        box->height = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + height +
            box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    }
    layout->flow.marker_item = frame->outer_marker;
    layout->flow.marker_left = frame->outer_marker_left;
    if (frame->anchor_bottom != INT_MIN) {
        webengine_translate(layout->flow.tree, box, 0, frame->anchor_bottom - box->height - box->y);
    }
    if (box->style->position != CSS_POSITION_FIXED) {
        int bottom = box->y + box->height + box->margin[WEBENGINE_BOTTOM];
        layout->extent = bottom > layout->extent ? bottom : layout->extent;
    }
}

/* Lays out the next absolute box whose containing block is the frame at
 * the top of the stack, in the order they were met. False when none is
 * left. */
static bool take_absolute(struct layout *layout) {
    size_t top = layout->depth - 1;
    for (size_t i = layout->frames[top].absolute_base; i < layout->absolute_count; i++) {
        if (layout->absolutes[i].containing == top) {
            struct webengine_box *box = layout->absolutes[i].box;
            memmove(&layout->absolutes[i], &layout->absolutes[i + 1],
                    (layout->absolute_count - i - 1) * sizeof(*layout->absolutes));
            layout->absolute_count--;
            enter_absolute(layout, box);
            return true;
        }
    }
    return false;
}

/* --- Tables (CSS 2.1 17.5) ------------------------------------------------------------ */

/* Opens the table `box`, whose edges are known: its grid and its columns'
 * widths. Its border box's width: a width in pixels counts borders and
 * padding, as browsers have it, and no table is narrower than its columns'
 * minimum; an auto width takes `available` up to the preferred width. */
static int table_open(struct layout *layout, struct webengine_box *box, int cb, int available) {
    if (layout->table_count == layout->table_capacity) {
        size_t capacity = layout->table_capacity == 0 ? 8 : layout->table_capacity * 2;
        struct table_layout *tables = realloc(layout->tables, capacity * sizeof(*tables));
        if (tables == NULL) {
            layout->failed = true;
            return 0;
        }
        layout->tables = tables;
        layout->table_capacity = capacity;
    }
    struct table_layout *table = &layout->tables[layout->table_count];
    memset(table, 0, sizeof(*table));
    intrinsic_widths(&layout->flow, box);
    if (webengine_table_grid(&table->grid, box) != 0 || webengine_table_columns(&table->grid) != 0) {
        webengine_table_free(&table->grid);
        layout->failed = true;
        return 0;
    }
    layout->table_count++;
    int edges = horizontal_edges(box);
    int specified = webengine_length_px(&box->style->width, cb, -1);
    int min = table->grid.min + edges;
    int max = table->grid.max + edges;
    int width = specified >= 0 ? specified : available < max ? available : max;
    width = width > min ? width : min;
    /* Nor narrower than its captions' minimum (17.4). */
    for (size_t i = 0; i < table->grid.caption_count; i++) {
        const struct webengine_box *caption = table->grid.captions[i];
        int wide = caption->min_content + webengine_intrinsic_edges(caption);
        width = wide > width ? wide : width;
    }
    table->content_width = width - edges;
    webengine_table_widths(&table->grid, table->content_width);
    return width;
}

/* The table just opened belongs to the frame `index`, whose box is placed:
 * its parts are laid out from here, the captions across the whole wrapper. */
static void table_start(struct layout *layout, size_t index) {
    if (layout->failed || layout->table_count == 0) {
        return;
    }
    struct frame *frame = &layout->frames[index];
    struct webengine_box *box = frame->box;
    struct table_layout *table = &layout->tables[layout->table_count - 1];
    frame->table = layout->table_count;
    frame->child = NULL;
    frame->content_x = box->x;
    frame->content_width = box->width;
    frame->content_top = box->y;
    frame->cursor = box->y;
    frame->pending = index;
    frame->own = (struct collapse){ 0, 0 };
    frame->percent_base = -1;
    table->content_x = box->x + box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
}

/* Moves what is inside `box`, not the box. */
static void move_content(struct webengine_tree *tree, struct webengine_box *box, int dy) {
    if (dy != 0) {
        webengine_translate(tree, box, 0, dy);
        box->y -= dy;
    }
}

/* Lays out cell `index` of the table below the top of the stack in its
 * slot: a block formatting context the width of its columns, at its row's
 * top. Cells have no margins. */
static void enter_cell(struct layout *layout, size_t index) {
    size_t parent_index = layout->depth - 1;
    struct table_layout *table = &layout->tables[layout->frames[parent_index].table - 1];
    struct webengine_table_cell *cell = &table->grid.cells[index];
    struct webengine_box *box = cell->box;
    const struct webengine_table_column *first = &table->grid.columns[cell->column];
    const struct webengine_table_column *last = &table->grid.columns[cell->column + cell->columns - 1];
    int x = table->content_x + first->x;
    int y = table->grid.rows[cell->row].y;
    int width = last->x + last->width - first->x;
    if (!push_frame(layout)) {
        return;
    }
    size_t frame_index = layout->depth - 1;
    struct frame *parent = &layout->frames[parent_index];
    struct frame *frame = &layout->frames[frame_index];
    block_edges(box, table->content_width);
    for (int side = 0; side < 4; side++) {
        box->margin[side] = 0;
    }
    box->x = x;
    box->y = y;
    box->width = width;
    int content = width - horizontal_edges(box);
    frame->box = box;
    frame->child = box->first;
    frame->kind = FRAME_CELL;
    frame->cell = index + 1;
    frame->positioned = webengine_box_positioned(box) ? frame_index : parent->positioned;
    frame->content_width = content > 0 ? content : 0;
    frame->content_x = x + box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
    frame->percent_base = -1;
    frame->pending = frame_index;
    frame->resolved = true;
    frame->content_top = y + box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP];
    frame->cursor = frame->content_top;
    frame->context = layout->flow.float_count;
    frame->outer_marker = layout->flow.marker_item;
    frame->outer_marker_left = layout->flow.marker_left;
    layout->flow.marker_item = NULL;
    start_content(layout, frame);
}

/* A cell's baseline (17.5.3): its first line box in the flow, else the
 * bottom of its content box. From its top border edge. */
static int cell_baseline(const struct webengine_tree *tree, const struct webengine_box *cell) {
    const struct webengine_box *box = cell->first;
    while (box != NULL) {
        bool out = webengine_box_floats(box) || webengine_box_absolute(box);
        if (!out && box->type == WEBENGINE_BOX_BLOCK && box->inline_content && box->line_count > 0) {
            return tree->lines[box->first_line].baseline - cell->y;
        }
        if (!out && box->first != NULL && box->type == WEBENGINE_BOX_BLOCK) {
            box = box->first;
            continue;
        }
        while (box != cell && box->next == NULL) {
            box = box->parent;
        }
        box = box == cell ? NULL : box->next;
    }
    if (cell->inline_content && cell->line_count > 0) {
        return tree->lines[cell->first_line].baseline - cell->y;
    }
    return cell->height - cell->border[WEBENGINE_BOTTOM] - cell->padding[WEBENGINE_BOTTOM];
}

static void finish_cell(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct table_layout *table = &layout->tables[layout->frames[index - 1].table - 1];
    struct webengine_table_cell *cell = &table->grid.cells[frame->cell - 1];
    struct webengine_box *box = frame->box;
    if (layout->flow.marker_item == box) {
        webengine_place_marker(&layout->flow, frame->content_top);
    }
    int content_bottom = frame->cursor + collapse_value(&frame->own);
    int floats = floats_bottom(&layout->flow, frame->context);
    if (floats > content_bottom) {
        content_bottom = floats;
    }
    layout->flow.float_count = frame->context;
    /* A cell's height is a minimum: the row grows to its content. */
    int content = content_bottom - frame->content_top;
    int height = used_height(box, -1, content);
    height = height > content ? height : content;
    box->height = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + height +
        box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    layout->flow.marker_item = frame->outer_marker;
    layout->flow.marker_left = frame->outer_marker_left;
    cell->height = box->height;
    cell->baseline = cell_baseline(layout->flow.tree, box);
}

/* Moves row `row` down to `y`, with the cells that start in it. */
static void move_row(struct layout *layout, struct table_layout *table, size_t row, int y) {
    struct webengine_table_row *line = &table->grid.rows[row];
    int dy = y - line->y;
    if (dy == 0) {
        return;
    }
    line->y = y;
    for (size_t c = table->grid.row_first[row]; c < table->grid.row_first[row + 1]; c++) {
        webengine_translate(layout->flow.tree, table->grid.cells[c].box, 0, dy);
    }
}

/* Spreads `extra` height over rows [first, last], laid out already, in
 * proportion to their heights, and moves the rows after the first. */
static void grow_rows(struct layout *layout, struct table_layout *table, size_t first, size_t last, int extra) {
    struct webengine_table_row *rows = table->grid.rows;
    int64_t total = 0;
    for (size_t r = first; r <= last; r++) {
        total += rows[r].height;
    }
    int64_t sum = 0;
    int given = 0;
    for (size_t r = first; r <= last; r++) {
        sum += rows[r].height;
        int upto = r == last ? extra : total > 0 ? (int)(((int64_t)extra * sum) / total) : 0;
        rows[r].height += upto - given;
        given = upto;
        if (r > first) {
            move_row(layout, table, r, rows[r - 1].y + rows[r - 1].height + table->grid.spacing[1]);
        }
    }
}

static bool baseline_aligned(const struct webengine_box *cell) {
    uint8_t align = cell->style->vertical_align;
    return align != CSS_VERTICAL_ALIGN_TOP && align != CSS_VERTICAL_ALIGN_MIDDLE && align != CSS_VERTICAL_ALIGN_BOTTOM;
}

/* Row `row` has its cells: their baselines line up, it takes the height of
 * the tallest cell in it alone, and the cells spanning down to it make the
 * rows they span tall enough (17.5.3). */
static void finish_row(struct layout *layout, struct table_layout *table, size_t row) {
    struct webengine_table_row *line = &table->grid.rows[row];
    struct webengine_table_cell *cells = table->grid.cells;
    size_t first = table->grid.row_first[row];
    size_t end = table->grid.row_first[row + 1];
    int specified = webengine_length_px(&line->box->style->height, -1, -1);
    line->height = specified > 0 ? specified : 0;
    int baseline = INT_MIN;
    for (size_t c = first; c < end; c++) {
        if (baseline_aligned(cells[c].box) && cells[c].baseline > baseline) {
            baseline = cells[c].baseline;
        }
    }
    for (size_t c = first; c < end; c++) {
        if (baseline_aligned(cells[c].box) && cells[c].baseline < baseline) {
            int shift = baseline - cells[c].baseline;
            move_content(layout->flow.tree, cells[c].box, shift);
            cells[c].height += shift;
            cells[c].baseline = baseline;
        }
        if (cells[c].rows == 1 && cells[c].height > line->height) {
            line->height = cells[c].height;
        }
    }
    for (size_t c = 0; c < end && table->grid.spans; c++) {
        if (cells[c].rows > 1 && (size_t)(cells[c].row + cells[c].rows - 1) == row) {
            const struct webengine_table_row *top = &table->grid.rows[cells[c].row];
            int have = line->y + line->height - top->y;
            if (cells[c].height > have) {
                grow_rows(layout, table, (size_t)cells[c].row, row, cells[c].height - have);
            }
        }
    }
}

/* Every row is laid out: the table's height, then each cell stretched to
 * the rows it spans with its content aligned in it, and the boxes of the
 * rows and row groups. */
static void finish_grid(struct layout *layout, size_t index) {
    struct frame *frame = &layout->frames[index];
    struct table_layout *table = &layout->tables[frame->table - 1];
    struct webengine_box *box = frame->box;
    struct webengine_table *grid = &table->grid;
    size_t rows = grid->row_count;
    int bottom = rows > 0 ? grid->rows[rows - 1].y + grid->rows[rows - 1].height + grid->spacing[1]
                          : table->content_top;
    int content = bottom - table->content_top;
    int edges = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + box->padding[WEBENGINE_BOTTOM] +
        box->border[WEBENGINE_BOTTOM];
    /* A height counts borders and padding, as the width does. */
    int specified = webengine_length_px(&box->style->height, layout->frames[index - 1].percent_base, -1) - edges;
    if (specified > content && rows > 0) {
        grow_rows(layout, table, 0, rows - 1, specified - content);
    }
    content = specified > content ? specified : content;
    for (size_t c = 0; c < grid->cell_count; c++) {
        struct webengine_table_cell *cell = &grid->cells[c];
        const struct webengine_table_row *top = &grid->rows[cell->row];
        const struct webengine_table_row *last = &grid->rows[cell->row + cell->rows - 1];
        int height = last->y + last->height - top->y;
        uint8_t align = cell->box->style->vertical_align;
        int room = height - cell->height;
        int dy = align == CSS_VERTICAL_ALIGN_BOTTOM ? room : align == CSS_VERTICAL_ALIGN_MIDDLE ? room / 2 : 0;
        move_content(layout->flow.tree, cell->box, dy);
        cell->box->height = height;
    }
    int left = table->content_x;
    int width = 0;
    if (grid->column_count > 0) {
        const struct webengine_table_column *last = &grid->columns[grid->column_count - 1];
        left += grid->columns[0].x;
        width = last->x + last->width - grid->columns[0].x;
    }
    for (struct webengine_box *child = box->first; child != NULL; child = child->next) {
        if (child->table_role != WEBENGINE_TABLE_CAPTION) {
            child->x = left;
            child->y = table->content_top;
            child->width = child->table_role == WEBENGINE_TABLE_COLUMN ||
                child->table_role == WEBENGINE_TABLE_COLUMN_GROUP ? 0 : width;
            child->height = 0;
        }
    }
    for (size_t r = 0; r < rows; r++) {
        struct webengine_table_row *row = &grid->rows[r];
        row->box->x = left;
        row->box->y = row->y;
        row->box->width = width;
        row->box->height = row->height;
        struct webengine_box *group = row->group;
        if (group != NULL) {
            if (group->height == 0 && group->y == table->content_top) {
                group->y = row->y;
            }
            group->height = row->y + row->height - group->y;
        }
    }
    box->table_y = table->table_y;
    box->table_height = table->content_top - table->table_y + content + box->padding[WEBENGINE_BOTTOM] +
        box->border[WEBENGINE_BOTTOM];
    frame->cursor = box->table_y + box->table_height;
}

/* The next step of the table at the top of the stack. */
static void table_step(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct table_layout *table = &layout->tables[frame->table - 1];
    struct webengine_table *grid = &table->grid;
    struct webengine_box *box = frame->box;
    switch (table->phase) {
    case TABLE_TOP_CAPTIONS:
        while (table->next < grid->caption_count) {
            struct webengine_box *caption = grid->captions[table->next++];
            if (caption->style->caption_side != CSS_CAPTION_SIDE_BOTTOM) {
                enter_block(layout, caption);
                return;
            }
        }
        table->table_y = frame->cursor + collapse_value(&frame->own);
        frame->own = (struct collapse){ 0, 0 };
        table->content_top = table->table_y + box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP];
        if (grid->row_count > 0) {
            grid->rows[0].y = table->content_top + grid->spacing[1];
        }
        table->phase = TABLE_ROWS;
        table->next = 0;
        return;
    case TABLE_ROWS:
        if (table->row < grid->row_count) {
            if (table->next < grid->cell_count && grid->cells[table->next].row == (int)table->row) {
                enter_cell(layout, table->next++);
                return;
            }
            finish_row(layout, table, table->row);
            table->row++;
            if (table->row < grid->row_count) {
                const struct webengine_table_row *above = &grid->rows[table->row - 1];
                grid->rows[table->row].y = above->y + above->height + grid->spacing[1];
            }
            return;
        }
        finish_grid(layout, index);
        table->phase = TABLE_BOTTOM_CAPTIONS;
        table->next = 0;
        return;
    default:
        while (table->next < grid->caption_count) {
            struct webengine_box *caption = grid->captions[table->next++];
            if (caption->style->caption_side == CSS_CAPTION_SIDE_BOTTOM) {
                enter_block(layout, caption);
                return;
            }
        }
        frame->table_done = true;
        webengine_table_free(grid);
        layout->table_count--;
        return;
    }
}

/* Forgets the line boxes of the last layout, so that moving a box that is
 * not laid out yet moves none. */
static void forget_lines(struct webengine_box *root) {
    struct webengine_box *box = root;
    while (box != NULL) {
        box->line_count = 0;
        if (box->first != NULL) {
            box = box->first;
            continue;
        }
        while (box != NULL && box->next == NULL) {
            box = box->parent;
        }
        if (box != NULL) {
            box = box->next;
        }
    }
}

int webengine_layout(struct webengine_tree *tree, const struct webengine_fonts *fonts, int width, int height) {
    struct layout layout = { .flow = { .fonts = fonts, .tree = tree }, .viewport_height = height };
    tree->layout_width = width;
    tree->height = 0;
    tree->line_count = 0;
    tree->fragment_count = 0;
    tree->paint_count = 0;
    if (tree->root == NULL) {
        return 0;
    }
    forget_lines(tree->root);
    /* The bottom of the stack is the initial containing block. */
    if (!push_frame(&layout)) {
        return -1;
    }
    layout.frames[0].content_width = width;
    layout.frames[0].percent_base = -1;
    layout.frames[0].resolved = true;
    enter_block(&layout, tree->root);
    while (!layout.failed && !layout.flow.failed) {
        if (layout.depth == 1) {
            /* The root has ended: the viewport's absolute boxes. */
            if (!take_absolute(&layout)) {
                break;
            }
            continue;
        }
        struct frame *frame = &layout.frames[layout.depth - 1];
        if (frame->lines_pending) {
            struct webengine_box *next = next_out_of_flow(frame->box, frame->scan);
            if (next == NULL) {
                frame->lines_pending = false;
                lay_out_inline(&layout);
            } else {
                frame->scan = next;
                if (webengine_box_absolute(next)) {
                    meet_absolute(&layout, next);
                } else {
                    enter_float(&layout, next, false);
                }
            }
            continue;
        }
        if (frame->table != 0 && !frame->table_done) {
            table_step(&layout);
            continue;
        }
        struct webengine_box *child = frame->child;
        if (child == NULL) {
            if (!frame->finished) {
                frame->finished = true;
                if (frame->kind == FRAME_FLOAT) {
                    finish_float(&layout);
                } else if (frame->kind == FRAME_ABSOLUTE) {
                    finish_absolute(&layout);
                } else if (frame->kind == FRAME_CELL) {
                    finish_cell(&layout);
                } else {
                    finish_block(&layout);
                }
            } else if (!take_absolute(&layout)) {
                layout.depth--;
            }
            continue;
        }
        frame->child = child->next;
        if (child->type != WEBENGINE_BOX_BLOCK) {
            continue;
        }
        if (webengine_box_absolute(child)) {
            /* Its static position: where the next block would start. */
            child->x = frame->content_x;
            child->y = frame->cursor + collapse_value(pending_of(&layout, layout.depth - 1));
            meet_absolute(&layout, child);
        } else if (webengine_box_floats(child)) {
            enter_float(&layout, child, true);
        } else {
            enter_block(&layout, child);
        }
    }
    layout.failed = layout.failed || layout.flow.failed;
    if (!layout.failed) {
        tree->height = layout.frames[0].cursor + collapse_value(&layout.frames[0].own);
        tree->height = layout.extent > tree->height ? layout.extent : tree->height;
        layout.failed = webengine_paint_order(tree) != 0;
    }
    for (size_t i = 0; i < layout.table_count; i++) {
        webengine_table_free(&layout.tables[i].grid);
    }
    free(layout.tables);
    free(layout.frames);
    free(layout.absolutes);
    webengine_flow_free(&layout.flow);
    return layout.failed ? -1 : 0;
}
