/* Web Viewer's layout (webengine_box.h): CSS 2.1 chapter 10's normal flow
 * and 9.5's floats over the box tree. Written from CSS 2.1 8.3.1 (collapsing
 * margins), 9.5 (floats and clearance), 10.3.3 and 10.3.5 (block and float
 * widths), 10.6.3 and 10.6.7 (heights) and 10.4/10.7 (minimum and maximum
 * sizes); NetSurf's layout is not its model (the clean-room rule).
 *
 * The walk keeps an explicit stack, so a deep document does not recurse. A
 * float is laid out with its margin box at the origin, where its size does
 * not depend on anything around it, and then moved to its place. */

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

/* The minimum and preferred widths of a block whose children's are known. */
static void intrinsic_block(struct webengine_flow *flow, struct webengine_box *box) {
    int min = 0;
    int max = 0;
    if (box->inline_content) {
        webengine_inline_widths(flow, box, &min, &max);
    } else {
        for (struct webengine_box *child = box->first; child != NULL; child = child->next) {
            if (child->type == WEBENGINE_BOX_BLOCK) {
                int edges = webengine_intrinsic_edges(child);
                min = child->min_content + edges > min ? child->min_content + edges : min;
                max = child->max_content + edges > max ? child->max_content + edges : max;
            }
        }
    }
    int width = intrinsic_size(box, &box->style->width);
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
    bool is_float;
    bool block_level;
    /* The outside marker that waited outside the float. */
    struct webengine_box *outer_marker;
    int outer_marker_left;
};

struct layout {
    struct webengine_flow flow;
    struct frame *frames;
    size_t depth;
    size_t capacity;
    bool failed;
};

static struct collapse *pending_of(struct layout *layout, size_t index) {
    return &layout->frames[layout->frames[index].pending].own;
}

/* A box that starts a block formatting context (9.4.1): its margins do not
 * collapse with its children's, and the floats inside it are its own. */
static bool new_context(const struct webengine_box *box) {
    return box->parent == NULL || box->style->overflow != CSS_OVERFLOW_VISIBLE || webengine_box_floats(box);
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
    memset(&layout->frames[layout->depth++], 0, sizeof(struct frame));
    return true;
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

/* The next float in the inline content of `block` after `after` (NULL for
 * the first), in the order of the content. */
static struct webengine_box *next_float(struct webengine_box *block, struct webengine_box *after) {
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
    bool auto_width = webengine_length_px(&style->width, cb, -1) < 0;
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
    frame->content_width = block_width(box, cb);
    box->x = parent->content_x + box->margin[WEBENGINE_LEFT];
    box->width = box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT] + frame->content_width +
        box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT];
    frame->content_x = box->x + box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
    int specified = webengine_length_px(&box->style->height, parent->percent_base, -1);
    frame->percent_base = specified >= 0 ? content_height(box, specified) : -1;
    bool context = new_context(box);
    frame->context = context ? layout->flow.float_count : parent->context;

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
    int width;
    if (specified >= 0) {
        width = content_width(box, specified);
    } else {
        intrinsic_widths(&layout->flow, box);
        int available = cb - box->margin[WEBENGINE_LEFT] - box->margin[WEBENGINE_RIGHT] - edges;
        width = box->max_content < available ? box->max_content : available;
        width = width > box->min_content ? width : box->min_content;
    }
    int max = webengine_length_px(&style->max_width, cb, -1);
    if (max >= 0 && width > content_width(box, max)) {
        width = content_width(box, max);
    }
    int min = webengine_length_px(&style->min_width, cb, 0);
    if (min > 0 && width < content_width(box, min)) {
        width = content_width(box, min);
    }
    frame->box = box;
    frame->child = box->first;
    frame->is_float = true;
    frame->block_level = block_level;
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

static void leave_block(struct layout *layout) {
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
            layout->depth--;
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
    int height = used_height(box, parent->percent_base, content_bottom - frame->content_top);
    box->height = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + height +
        box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    parent->cursor = box->y + box->height;
    if (pending != outer) {
        /* What collapses through the bottom joins the margins after it. */
        *outer = *pending;
    }
    collapse_add(outer, box->margin[WEBENGINE_BOTTOM]);
    layout->depth--;
}

static void leave_float(struct layout *layout) {
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
    int height = used_height(box, parent->percent_base, content_bottom - frame->content_top);
    box->height = box->border[WEBENGINE_TOP] + box->padding[WEBENGINE_TOP] + height +
        box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_BOTTOM];
    layout->flow.marker_item = frame->outer_marker;
    layout->flow.marker_left = frame->outer_marker_left;
    bool block_level = frame->block_level;
    layout->depth--;
    if (block_level) {
        /* A block's child goes where the next block would start. */
        layout->flow.context = parent->context;
        webengine_place_float(&layout->flow, box, parent->content_x, parent->content_x + parent->content_width,
                              parent->cursor + collapse_value(pending_of(layout, index - 1)));
    }
}

int webengine_layout(struct webengine_tree *tree, const struct webengine_fonts *fonts, int width) {
    struct layout layout = { .flow = { .fonts = fonts, .tree = tree } };
    tree->layout_width = width;
    tree->height = 0;
    tree->line_count = 0;
    tree->fragment_count = 0;
    tree->paint_count = 0;
    if (tree->root == NULL) {
        return 0;
    }
    /* The bottom of the stack is the initial containing block. */
    if (!push_frame(&layout)) {
        return -1;
    }
    layout.frames[0].content_width = width;
    layout.frames[0].percent_base = -1;
    layout.frames[0].resolved = true;
    enter_block(&layout, tree->root);
    while (layout.depth > 1 && !layout.failed && !layout.flow.failed) {
        struct frame *frame = &layout.frames[layout.depth - 1];
        if (frame->lines_pending) {
            struct webengine_box *next = next_float(frame->box, frame->scan);
            if (next != NULL) {
                frame->scan = next;
                enter_float(&layout, next, false);
            } else {
                frame->lines_pending = false;
                lay_out_inline(&layout);
            }
            continue;
        }
        struct webengine_box *child = frame->child;
        if (child == NULL) {
            if (frame->is_float) {
                leave_float(&layout);
            } else {
                leave_block(&layout);
            }
            continue;
        }
        frame->child = child->next;
        if (child->type == WEBENGINE_BOX_BLOCK) {
            if (webengine_box_floats(child)) {
                enter_float(&layout, child, true);
            } else {
                enter_block(&layout, child);
            }
        }
    }
    layout.failed = layout.failed || layout.flow.failed;
    if (!layout.failed) {
        tree->height = layout.frames[0].cursor + collapse_value(&layout.frames[0].own);
        layout.failed = webengine_paint_order(tree) != 0;
    }
    free(layout.frames);
    webengine_flow_free(&layout.flow);
    return layout.failed ? -1 : 0;
}
