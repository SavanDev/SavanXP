/* Web Viewer's layout (webengine_box.h): CSS 2.1 chapter 10's normal flow
 * over the box tree. Written from CSS 2.1 8.3.1 (collapsing margins), 10.3.3
 * (block widths), 10.6.3 (block heights) and 10.4/10.7 (minimum and maximum
 * sizes); NetSurf's layout is not its model (the clean-room rule).
 *
 * The walk keeps an explicit stack, so a deep document does not recurse. */

#include "webengine_box.h"
#include "webengine_layout.h"

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
};

struct layout {
    const struct webengine_fonts *fonts;
    struct frame *frames;
    size_t depth;
    size_t capacity;
    bool failed;
};

static struct collapse *pending_of(struct layout *layout, size_t index) {
    return &layout->frames[layout->frames[index].pending].own;
}

/* A box that starts a block formatting context: its margins do not collapse
 * with its children's. Floats and positioned boxes join in E5. */
static bool new_context(const struct webengine_box *box) {
    return box->parent == NULL || box->style->overflow != CSS_OVERFLOW_VISIBLE;
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

/* Lays out the inline content of the block at the top of the stack. The
 * height of its line boxes, 0 when it has none. */
static int lay_out_lines(struct layout *layout, struct frame *frame) {
    return webengine_layout_lines(layout->fonts, frame->box, frame->content_x, frame->cursor,
                                  frame->content_width);
}

static void enter_block(struct layout *layout, struct webengine_box *box) {
    size_t parent_index = layout->depth - 1;
    if (!push_frame(layout)) {
        return;
    }
    struct frame *parent = &layout->frames[parent_index];
    struct frame *frame = &layout->frames[layout->depth - 1];
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

    struct collapse *pending = pending_of(layout, parent_index);
    collapse_add(pending, box->margin[WEBENGINE_TOP]);
    if (box->border[WEBENGINE_TOP] > 0 || box->padding[WEBENGINE_TOP] > 0 || new_context(box)) {
        /* The margins above end at the top border, which is also that of
         * the enclosing boxes whose top margins collapsed with them. */
        int y = parent->cursor + collapse_value(pending);
        *pending = (struct collapse){ 0, 0 };
        frame->pending = layout->depth - 1;
        resolve_top(layout, layout->depth - 1, y);
    } else {
        /* The top margin collapses with the first child's. */
        frame->cursor = parent->cursor;
        frame->pending = parent->pending;
    }
    if (box->inline_content) {
        int start = frame->resolved ? frame->cursor : parent->cursor + collapse_value(pending);
        frame->cursor = start;
        int lines = lay_out_lines(layout, frame);
        if (lines > 0) {
            /* Line boxes separate the margins above from those below. */
            if (!frame->resolved) {
                *pending = (struct collapse){ 0, 0 };
                resolve_top(layout, layout->depth - 1, start);
            }
            frame->cursor = start + lines;
        } else if (!frame->resolved) {
            frame->cursor = parent->cursor;
        }
        frame->child = NULL;
    }
}

static void leave_block(struct layout *layout) {
    size_t index = layout->depth - 1;
    struct frame *frame = &layout->frames[index];
    struct frame *parent = &layout->frames[index - 1];
    struct webengine_box *box = frame->box;
    const struct webengine_box_style *style = box->style;
    int specified = webengine_length_px(&style->height, parent->percent_base, -1);
    int min = webengine_length_px(&style->min_height, parent->percent_base, 0);
    int max = webengine_length_px(&style->max_height, parent->percent_base, -1);
    bool auto_height = specified < 0;
    bool bottom_edge = box->border[WEBENGINE_BOTTOM] > 0 || box->padding[WEBENGINE_BOTTOM] > 0 ||
        new_context(box) || !auto_height;
    struct collapse *pending = pending_of(layout, index);
    struct collapse *outer = pending_of(layout, index - 1);

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
    int height = auto_height ? content_bottom - frame->content_top : content_height(box, specified);
    if (max >= 0 && height > content_height(box, max)) {
        height = content_height(box, max);
    }
    if (min > 0 && height < content_height(box, min)) {
        height = content_height(box, min);
    }
    if (height < 0) {
        height = 0;
    }
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

int webengine_layout(struct webengine_tree *tree, const struct webengine_fonts *fonts, int width) {
    struct layout layout = { .fonts = fonts };
    tree->layout_width = width;
    tree->height = 0;
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
    while (layout.depth > 1 && !layout.failed) {
        struct frame *frame = &layout.frames[layout.depth - 1];
        struct webengine_box *child = frame->child;
        if (child == NULL) {
            leave_block(&layout);
            continue;
        }
        frame->child = child->next;
        if (child->type == WEBENGINE_BOX_BLOCK) {
            enter_block(&layout, child);
        }
    }
    if (!layout.failed) {
        tree->height = layout.frames[0].cursor + collapse_value(&layout.frames[0].own);
    }
    free(layout.frames);
    return layout.failed ? -1 : 0;
}
