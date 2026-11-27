/* Web Viewer's inline flow (webengine_layout.h): CSS 2.1's inline formatting
 * context (9.4.2), line heights and vertical alignment (10.8), white space at
 * the ends of lines (16.6.1), text-align and text-indent (16.1, 16.2) and list
 * markers (12.5), with the line boxes beside floats (9.5). Written from the
 * specification; NetSurf's layout is not its model (the clean-room rule).
 *
 * A block's inline content becomes a row of atoms: words, runs of spaces,
 * the opening and closing edges of inline boxes, breaks, inside markers and
 * floats. Lines take atoms greedily and break at the last space that allows
 * it, in the room the floats leave them. */

#include "webengine_layout.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum atom_kind { ATOM_WORD, ATOM_SPACE, ATOM_OPEN, ATOM_CLOSE, ATOM_BREAK, ATOM_MARKER, ATOM_FLOAT, ATOM_ABSOLUTE };

struct webengine_atom {
    uint8_t kind;
    /* SPACE: a line may break here. WORD: its text wraps (a word wider
     * than the line may be cut). */
    bool wrap;
    /* SPACE: vanishes at the start and the end of a line; when not, at the
     * end it still hangs, unless white-space is pre. */
    bool collapsible;
    bool hangs;
    /* Left out of the line it ended up on. */
    bool dropped;
    /* FLOAT: placed already. */
    bool placed;
    /* ABSOLUTE: where the line put it, before text-align. */
    int x;
    struct webengine_box *box;
    size_t start;
    size_t length;
    int width;
};

/* An inline box on the line being built, or the block's own (the root). */
struct webengine_placed {
    struct webengine_box *box;
    int parent;
    int ascent;
    int descent;
    /* Above and below the baseline, half-leading included. */
    int above;
    int below;
    /* Its baseline below its anchor's, which is the nearest box aligned
     * top or bottom (itself included), or the root's group (-1). */
    int rel;
    int anchor;
    /* An anchor's group: the extent of what is aligned to it, around its
     * baseline. */
    int group_top;
    int group_bottom;
    int baseline;
    /* Its INLINE fragment, or -1 for the root. */
    long fragment;
};

static int fixed_round(css_fixed value) {
    return value >= 0 ? (int)((value + 512) >> 10) : -(int)((-value + 512) >> 10);
}

static bool grow(void **data, size_t *capacity, size_t count, size_t size) {
    if (count < *capacity) {
        return true;
    }
    size_t grown = *capacity == 0 ? 64 : *capacity * 2;
    while (grown <= count) {
        grown *= 2;
    }
    void *bigger = realloc(*data, grown * size);
    if (bigger == NULL) {
        return false;
    }
    *data = bigger;
    *capacity = grown;
    return true;
}

static long add_fragment(struct webengine_flow *flow, struct webengine_fragment fragment) {
    struct webengine_tree *tree = flow->tree;
    if (!grow((void **)&tree->fragments, &tree->fragment_capacity, tree->fragment_count,
              sizeof(*tree->fragments))) {
        flow->failed = true;
        return -1;
    }
    tree->fragments[tree->fragment_count] = fragment;
    return (long)tree->fragment_count++;
}

/* A face's content area and the box's line height (10.8.1): "normal" is
 * 1.15 times the content area. */
static void metrics(struct webengine_flow *flow, const struct webengine_box_style *style,
                    struct webengine_placed *placed) {
    int ascent = 0;
    int descent = 0;
    flow->fonts->metrics(flow->fonts->user, style->face, &ascent, &descent);
    int content = ascent + descent;
    int line = content + (content * 15 + 50) / 100;
    if (style->line_height.kind == WEBENGINE_LEN_NUMBER) {
        line = fixed_round((css_fixed)(((int64_t)style->line_height.value * style->font_size)));
    } else if (style->line_height.kind == WEBENGINE_LEN_PX) {
        line = fixed_round(style->line_height.value);
    }
    if (line < 0) {
        line = 0;
    }
    int leading = line - content;
    placed->ascent = ascent;
    placed->descent = descent;
    placed->above = ascent + (leading >= 0 ? leading / 2 : -((-leading + 1) / 2));
    placed->below = line - placed->above;
}

static int text_width(struct webengine_flow *flow, const struct webengine_box_style *style, const char *text,
                      size_t length) {
    return flow->fonts->width(flow->fonts->user, style->face, style->bold, text, length);
}

/* A drawn bullet's side: a third of the content area. */
static int bullet_size(struct webengine_flow *flow, const struct webengine_box_style *style) {
    int ascent = 0;
    int descent = 0;
    flow->fonts->metrics(flow->fonts->user, style->face, &ascent, &descent);
    int size = (ascent + descent) / 3;
    return size < 3 ? 3 : size;
}

static int marker_width(struct webengine_flow *flow, const struct webengine_box *marker) {
    return marker->text != NULL ? text_width(flow, marker->style, marker->text, marker->length)
                                : bullet_size(flow, marker->style);
}

/* The space between a marker and the item's text: one space's width. */
static int marker_gap(struct webengine_flow *flow, const struct webengine_box *marker) {
    return text_width(flow, marker->style, " ", 1);
}

/* --- Atoms ------------------------------------------------------------------------ */

static bool add_atom(struct webengine_flow *flow, size_t *count, struct webengine_atom atom) {
    if (!grow((void **)&flow->atoms, &flow->atom_capacity, *count, sizeof(*flow->atoms))) {
        flow->failed = true;
        return false;
    }
    flow->atoms[(*count)++] = atom;
    return true;
}

/* An inline box's used edges: horizontal margins and the padding take their
 * percentages from the block's width; auto margins are zero (10.3.1). */
static void inline_edges(struct webengine_box *box, int width) {
    const struct webengine_box_style *style = box->style;
    for (int side = 0; side < 4; side++) {
        box->border[side] = style->border_width[side];
        box->padding[side] = webengine_length_px(&style->padding[side], width, 0);
        if (box->padding[side] < 0) {
            box->padding[side] = 0;
        }
        box->margin[side] = webengine_length_px(&style->margin[side], width, 0);
    }
}

static void text_atoms(struct webengine_flow *flow, size_t *count, struct webengine_box *box) {
    uint8_t mode = box->style->white_space;
    bool wrap = mode == CSS_WHITE_SPACE_NORMAL || mode == CSS_WHITE_SPACE_PRE_WRAP ||
        mode == CSS_WHITE_SPACE_PRE_LINE;
    bool collapsible = mode == CSS_WHITE_SPACE_NORMAL || mode == CSS_WHITE_SPACE_NOWRAP ||
        mode == CSS_WHITE_SPACE_PRE_LINE;
    size_t i = 0;
    while (i < box->length && !flow->failed) {
        struct webengine_atom atom = { .box = box, .start = i, .wrap = wrap };
        if (box->text[i] == '\n') {
            atom.kind = ATOM_BREAK;
            atom.length = 1;
        } else if (box->text[i] == ' ') {
            atom.kind = ATOM_SPACE;
            atom.collapsible = collapsible;
            atom.hangs = mode != CSS_WHITE_SPACE_PRE;
            while (i + atom.length < box->length && box->text[i + atom.length] == ' ') {
                atom.length++;
            }
        } else {
            atom.kind = ATOM_WORD;
            while (i + atom.length < box->length && box->text[i + atom.length] != ' ' &&
                   box->text[i + atom.length] != '\n') {
                atom.length++;
            }
        }
        if (atom.kind != ATOM_BREAK) {
            atom.width = text_width(flow, box->style, box->text + i, atom.length);
        }
        i += atom.length;
        (void)add_atom(flow, count, atom);
    }
}

/* The atoms of the inline content of `block`, in order. */
static size_t collect_atoms(struct webengine_flow *flow, struct webengine_box *block, int width) {
    size_t count = 0;
    struct webengine_box *box = block->first;
    while (box != NULL && !flow->failed) {
        switch (box->type) {
        case WEBENGINE_BOX_TEXT:
            text_atoms(flow, &count, box);
            break;
        case WEBENGINE_BOX_BREAK:
            (void)add_atom(flow, &count, (struct webengine_atom){ .kind = ATOM_BREAK, .box = box });
            break;
        case WEBENGINE_BOX_MARKER:
            (void)add_atom(flow, &count, (struct webengine_atom){
                .kind = ATOM_MARKER, .box = box,
                .width = marker_width(flow, box) + marker_gap(flow, box) });
            break;
        case WEBENGINE_BOX_BLOCK:
            /* A float, laid out already, or an absolute box, which only
             * needs its static position (10.3.7): their content is their
             * own. */
            (void)add_atom(flow, &count, (struct webengine_atom){
                .kind = webengine_box_floats(box) ? ATOM_FLOAT : ATOM_ABSOLUTE, .box = box });
            break;
        case WEBENGINE_BOX_INLINE:
            inline_edges(box, width);
            (void)add_atom(flow, &count, (struct webengine_atom){
                .kind = ATOM_OPEN, .box = box,
                .width = box->first_piece ? box->margin[WEBENGINE_LEFT] + box->border[WEBENGINE_LEFT] +
                    box->padding[WEBENGINE_LEFT] : 0 });
            if (box->first != NULL) {
                box = box->first;
                continue;
            }
            break;
        default:
            break;
        }
        /* Next in order, closing the inline boxes that end here. */
        while (box != block && box->next == NULL) {
            box = box->parent;
            if (box != block) {
                (void)add_atom(flow, &count, (struct webengine_atom){
                    .kind = ATOM_CLOSE, .box = box,
                    .width = box->last_piece ? box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT] +
                        box->margin[WEBENGINE_RIGHT] : 0 });
            }
        }
        if (box == block) {
            break;
        }
        if (box->type == WEBENGINE_BOX_INLINE && box->first == NULL) {
            /* An empty inline closes at once. */
            (void)add_atom(flow, &count, (struct webengine_atom){
                .kind = ATOM_CLOSE, .box = box,
                .width = box->last_piece ? box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT] +
                    box->margin[WEBENGINE_RIGHT] : 0 });
        }
        box = box->next;
    }
    return count;
}

/* Cuts a word wider than the line after as many characters as fit in
 * `room`, one at least, inserting the rest as the next atom. */
static bool split_word(struct webengine_flow *flow, size_t *count, size_t index, int room) {
    struct webengine_atom *atom = &flow->atoms[index];
    const char *text = atom->box->text + atom->start;
    size_t keep = 0;
    for (size_t end = 1; end <= atom->length; end++) {
        if (end < atom->length && ((unsigned char)text[end] & 0xc0) == 0x80) {
            continue;
        }
        if (keep > 0 && text_width(flow, atom->box->style, text, end) > room) {
            break;
        }
        keep = end;
    }
    if (keep == 0 || keep >= atom->length) {
        return false;
    }
    if (!grow((void **)&flow->atoms, &flow->atom_capacity, *count, sizeof(*flow->atoms))) {
        flow->failed = true;
        return false;
    }
    atom = &flow->atoms[index];
    memmove(&flow->atoms[index + 2], &flow->atoms[index + 1], (*count - index - 1) * sizeof(*atom));
    (*count)++;
    struct webengine_atom rest = *atom;
    rest.start += keep;
    rest.length -= keep;
    rest.width = text_width(flow, rest.box->style, rest.box->text + rest.start, rest.length);
    atom->length = keep;
    atom->width = text_width(flow, atom->box->style, atom->box->text + atom->start, keep);
    flow->atoms[index + 1] = rest;
    return true;
}

/* --- Vertical alignment (10.8) ------------------------------------------------------ */

static long place(struct webengine_flow *flow, size_t *count, struct webengine_box *box, int parent) {
    if (!grow((void **)&flow->placed, &flow->placed_capacity, *count, sizeof(*flow->placed))) {
        flow->failed = true;
        return -1;
    }
    struct webengine_placed *placed = &flow->placed[*count];
    memset(placed, 0, sizeof(*placed));
    placed->box = box;
    placed->parent = parent;
    placed->fragment = -1;
    metrics(flow, box->style, placed);
    if (parent < 0) {
        placed->anchor = -1;
        return (long)(*count)++;
    }
    const struct webengine_placed *outer = &flow->placed[parent];
    const struct webengine_box_style *style = box->style;
    int shift = 0;
    switch (style->vertical_align) {
    case CSS_VERTICAL_ALIGN_TOP:
    case CSS_VERTICAL_ALIGN_BOTTOM:
        placed->anchor = (int)*count;
        placed->rel = 0;
        return (long)(*count)++;
    case CSS_VERTICAL_ALIGN_SUB:
        shift = outer->box->style->font_size / 5;
        break;
    case CSS_VERTICAL_ALIGN_SUPER:
        shift = -(outer->box->style->font_size / 3);
        break;
    case CSS_VERTICAL_ALIGN_TEXT_TOP:
        shift = placed->above - outer->ascent;
        break;
    case CSS_VERTICAL_ALIGN_TEXT_BOTTOM:
        shift = outer->descent - placed->below;
        break;
    case CSS_VERTICAL_ALIGN_MIDDLE:
        /* Its middle on the parent's baseline plus half an x-height, taken
         * as half the ascent. */
        shift = -(outer->ascent / 4) - (placed->below - placed->above) / 2;
        break;
    case CSS_VERTICAL_ALIGN_SET:
        shift = -webengine_length_px(&style->vertical_align_length, placed->above + placed->below, 0);
        break;
    default:
        break;
    }
    placed->anchor = outer->anchor;
    placed->rel = outer->rel + shift;
    return (long)(*count)++;
}

/* Sets every placed box's baseline on a line from `top`; the line's height
 * (10.8.1). The root's group, the boxes aligned on the block's baseline,
 * starts at the top; a box aligned top or bottom takes what is aligned to it
 * to the line's top or bottom. */
static int align_line(struct webengine_flow *flow, size_t count, int top) {
    int root_top = 0;
    int root_bottom = 0;
    bool any = false;
    for (size_t i = 0; i < count; i++) {
        const struct webengine_placed *placed = &flow->placed[i];
        if (placed->anchor < 0) {
            int above = placed->rel - placed->above;
            int below = placed->rel + placed->below;
            root_top = !any || above < root_top ? above : root_top;
            root_bottom = !any || below > root_bottom ? below : root_bottom;
            any = true;
        }
    }
    int height = root_bottom - root_top;
    for (size_t g = 0; g < count; g++) {
        struct webengine_placed *anchor = &flow->placed[g];
        if (anchor->anchor != (int)g) {
            continue;
        }
        anchor->group_top = 0;
        anchor->group_bottom = 0;
        for (size_t i = g; i < count; i++) {
            const struct webengine_placed *placed = &flow->placed[i];
            if (placed->anchor == (int)g) {
                int above = placed->rel - placed->above;
                int below = placed->rel + placed->below;
                anchor->group_top = i == g || above < anchor->group_top ? above : anchor->group_top;
                anchor->group_bottom = i == g || below > anchor->group_bottom ? below : anchor->group_bottom;
            }
        }
        if (anchor->group_bottom - anchor->group_top > height) {
            height = anchor->group_bottom - anchor->group_top;
        }
    }
    for (size_t i = 0; i < count; i++) {
        struct webengine_placed *placed = &flow->placed[i];
        int base = top - root_top;
        if (placed->anchor >= 0) {
            const struct webengine_placed *anchor = &flow->placed[placed->anchor];
            base = anchor->box->style->vertical_align == CSS_VERTICAL_ALIGN_TOP
                ? top - anchor->group_top : top + height - anchor->group_bottom;
        }
        placed->baseline = base + placed->rel;
    }
    return height;
}

/* --- Lines ------------------------------------------------------------------------- */

/* The index of the placed entry for `box`, searching back from the end. */
static int placed_index(struct webengine_flow *flow, size_t count, const struct webengine_box *box) {
    for (size_t i = count; i > 0; i--) {
        if (flow->placed[i - 1].box == box) {
            return (int)(i - 1);
        }
    }
    return 0;
}

/* Puts the waiting outside marker on a line whose root baseline is
 * `baseline`. */
static void hang_marker(struct webengine_flow *flow, int baseline) {
    struct webengine_box *marker = flow->marker_item->marker;
    struct webengine_placed placed;
    metrics(flow, marker->style, &placed);
    int width = marker_width(flow, marker);
    int right = flow->marker_left - marker_gap(flow, marker);
    marker->x = right - width;
    if (marker->text != NULL) {
        marker->y = baseline - placed.ascent;
        marker->height = placed.ascent + placed.descent;
    } else {
        /* A bullet sits on the x-height's middle, half the ascent up. */
        marker->height = width;
        marker->y = baseline - placed.ascent / 3 - width / 2 - 1;
    }
    marker->width = width;
    flow->marker_item = NULL;
}

void webengine_place_marker(struct webengine_flow *flow, int y) {
    if (flow->marker_item == NULL) {
        return;
    }
    struct webengine_placed placed;
    metrics(flow, flow->marker_item->style, &placed);
    hang_marker(flow, y + placed.above);
}

static int text_align_shift(uint8_t align, int room) {
    if (room <= 0) {
        return 0;
    }
    switch (align) {
    case CSS_TEXT_ALIGN_RIGHT:
    case CSS_TEXT_ALIGN_LIBCSS_RIGHT:
        return room;
    case CSS_TEXT_ALIGN_CENTER:
    case CSS_TEXT_ALIGN_LIBCSS_CENTER:
        return room / 2;
    default:
        /* left, and justify until it is done: its lines align left. */
        return 0;
    }
}

/* Builds the line box of atoms [start, end) at (x, y). Its height. */
static int build_line(struct webengine_flow *flow, struct webengine_box *block, size_t start, size_t end,
                      int x, int y, int width, int indent, size_t *open_count) {
    struct webengine_tree *tree = flow->tree;
    size_t first_fragment = tree->fragment_count;
    size_t count = 0;
    int cursor = x + indent;
    (void)place(flow, &count, block, -1);
    /* The inline boxes open from the line before continue here. */
    for (size_t i = 0; i < *open_count && !flow->failed; i++) {
        int parent = i == 0 ? 0 : (int)(count - 1);
        long index = place(flow, &count, flow->open[i], parent);
        if (index >= 0) {
            flow->placed[index].fragment = add_fragment(flow, (struct webengine_fragment){
                .type = WEBENGINE_FRAGMENT_INLINE, .box = flow->open[i], .x = cursor });
        }
    }
    for (size_t i = start; i < end && !flow->failed; i++) {
        struct webengine_atom *atom = &flow->atoms[i];
        if (atom->dropped) {
            continue;
        }
        struct webengine_box *box = atom->box;
        switch (atom->kind) {
        case ATOM_OPEN: {
            int parent = *open_count == 0 ? 0 : placed_index(flow, count, flow->open[*open_count - 1]);
            long index = place(flow, &count, box, parent);
            if (box->first_piece) {
                cursor += box->margin[WEBENGINE_LEFT];
            }
            if (index >= 0) {
                flow->placed[index].fragment = add_fragment(flow, (struct webengine_fragment){
                    .type = WEBENGINE_FRAGMENT_INLINE, .box = box, .x = cursor, .left_edge = box->first_piece });
            }
            if (box->first_piece) {
                cursor += box->border[WEBENGINE_LEFT] + box->padding[WEBENGINE_LEFT];
            }
            if (!grow((void **)&flow->open, &flow->open_capacity, *open_count, sizeof(*flow->open))) {
                flow->failed = true;
                break;
            }
            flow->open[(*open_count)++] = box;
            break;
        }
        case ATOM_CLOSE: {
            int index = placed_index(flow, count, box);
            if (box->last_piece) {
                cursor += box->padding[WEBENGINE_RIGHT] + box->border[WEBENGINE_RIGHT];
            }
            long fragment = flow->placed[index].fragment;
            if (fragment >= 0) {
                tree->fragments[fragment].width = cursor - tree->fragments[fragment].x;
                tree->fragments[fragment].right_edge = box->last_piece;
            }
            if (box->last_piece) {
                cursor += box->margin[WEBENGINE_RIGHT];
            }
            if (*open_count > 0) {
                (*open_count)--;
            }
            break;
        }
        case ATOM_WORD:
        case ATOM_SPACE: {
            struct webengine_fragment *last = tree->fragment_count > first_fragment
                ? &tree->fragments[tree->fragment_count - 1] : NULL;
            if (last != NULL && last->type == WEBENGINE_FRAGMENT_TEXT && last->box == box &&
                last->start + last->length == atom->start) {
                last->length += atom->length;
                last->width += atom->width;
            } else {
                (void)add_fragment(flow, (struct webengine_fragment){
                    .type = WEBENGINE_FRAGMENT_TEXT, .box = box, .x = cursor, .width = atom->width,
                    .start = atom->start, .length = atom->length });
            }
            cursor += atom->width;
            break;
        }
        case ATOM_ABSOLUTE:
            atom->x = cursor;
            break;
        case ATOM_MARKER:
            (void)add_fragment(flow, (struct webengine_fragment){
                .type = WEBENGINE_FRAGMENT_MARKER, .box = box, .x = cursor,
                .width = marker_width(flow, box), .length = box->length });
            cursor += atom->width;
            break;
        default:
            break;
        }
    }
    /* Inline boxes still open go on to the next line. */
    for (size_t i = 0; i < *open_count; i++) {
        long fragment = flow->placed[placed_index(flow, count, flow->open[i])].fragment;
        if (fragment >= 0) {
            tree->fragments[fragment].width = cursor - tree->fragments[fragment].x;
        }
    }
    if (flow->failed) {
        return 0;
    }
    int height = align_line(flow, count, y);
    int shift = text_align_shift(block->style->text_align, x + width - cursor);
    for (size_t i = start; i < end; i++) {
        if (flow->atoms[i].kind == ATOM_ABSOLUTE) {
            flow->atoms[i].box->x = flow->atoms[i].x + shift;
            flow->atoms[i].box->y = y;
        }
    }
    /* Each fragment's vertical place comes from the box it is in. */
    for (size_t f = first_fragment; f < tree->fragment_count; f++) {
        struct webengine_fragment *fragment = &tree->fragments[f];
        fragment->x += shift;
        const struct webengine_box *owner = fragment->type == WEBENGINE_FRAGMENT_INLINE ? fragment->box
            : fragment->box->parent;
        const struct webengine_placed *placed = &flow->placed[placed_index(flow, count, owner)];
        /* position: relative inlines move what is in them (9.4.3). */
        int moved = 0;
        for (const struct webengine_box *inline_box = owner; inline_box != block && inline_box != NULL;
             inline_box = inline_box->parent) {
            if (inline_box->style->position == CSS_POSITION_RELATIVE) {
                int dx = 0;
                int dy = 0;
                webengine_relative_offset(inline_box, width, -1, &dx, &dy);
                fragment->x += dx;
                moved += dy;
            }
        }
        if (fragment->type == WEBENGINE_FRAGMENT_INLINE) {
            const struct webengine_box *box = fragment->box;
            fragment->baseline = placed->baseline;
            fragment->y = placed->baseline - placed->ascent - box->padding[WEBENGINE_TOP] - box->border[WEBENGINE_TOP];
            fragment->height = placed->ascent + placed->descent + box->padding[WEBENGINE_TOP] +
                box->padding[WEBENGINE_BOTTOM] + box->border[WEBENGINE_TOP] + box->border[WEBENGINE_BOTTOM];
            fragment->y += moved;
            fragment->baseline += moved;
            continue;
        }
        struct webengine_placed own;
        metrics(flow, fragment->box->style, &own);
        fragment->baseline = placed->baseline + moved;
        if (fragment->type == WEBENGINE_FRAGMENT_MARKER && fragment->box->text == NULL) {
            int size = fragment->width;
            fragment->height = size;
            fragment->y = placed->baseline - own.ascent / 3 - size / 2 - 1 + moved;
        } else {
            fragment->y = placed->baseline - own.ascent + moved;
            fragment->height = own.ascent + own.descent;
        }
    }
    if (flow->marker_item != NULL) {
        hang_marker(flow, flow->placed[0].baseline);
    }
    if (!grow((void **)&tree->lines, &tree->line_capacity, tree->line_count, sizeof(*tree->lines))) {
        flow->failed = true;
        return 0;
    }
    tree->lines[tree->line_count++] = (struct webengine_line){
        .x = x, .y = y, .width = width, .height = height, .first_fragment = first_fragment,
        .fragment_count = tree->fragment_count - first_fragment };
    return height;
}

/* The width a float takes on a line: its margin box. */
static int float_outer_width(const struct webengine_box *box) {
    int width = box->margin[WEBENGINE_LEFT] + box->width + box->margin[WEBENGINE_RIGHT];
    return width > 0 ? width : 0;
}

static bool defer_float(struct webengine_flow *flow, struct webengine_box *box) {
    if (!grow((void **)&flow->deferred, &flow->deferred_capacity, flow->deferred_count,
              sizeof(*flow->deferred))) {
        flow->failed = true;
        return false;
    }
    flow->deferred[flow->deferred_count++] = box;
    return true;
}

int webengine_layout_lines(struct webengine_flow *flow, struct webengine_box *block, int x, int y, int width) {
    size_t count = collect_atoms(flow, block, width);
    size_t open_count = 0;
    int top = y;
    int indent = webengine_length_px(&block->style->text_indent, width, 0);
    /* The rows a line takes, to find the room floats leave it: the
     * block's own line height (its strut, 10.8.1). */
    struct webengine_placed strut;
    metrics(flow, block->style, &strut);
    int strut_height = strut.above + strut.below > 0 ? strut.above + strut.below : 1;
    block->first_line = flow->tree->line_count;
    block->line_count = 0;
    size_t i = 0;
    while (i < count && !flow->failed) {
        size_t start = i;
        size_t end = count;
        size_t brk = SIZE_MAX;
        int left = x;
        int right = x + width;
        webengine_float_space(flow, y, strut_height, &left, &right);
        int room = right - left;
        int line_indent = block->line_count == 0 ? indent : 0;
        int used = line_indent;
        bool content = false;
        bool lower = false;
        flow->deferred_count = 0;
        for (size_t j = start; j < count; j++) {
            struct webengine_atom *atom = &flow->atoms[j];
            atom->dropped = false;
            if (atom->kind == ATOM_FLOAT) {
                if (!atom->placed) {
                    atom->placed = true;
                    if (!content || used + float_outer_width(atom->box) <= room) {
                        /* It fits beside what the line has so far: it goes
                         * at the line's top, and the line shortens. */
                        webengine_place_float(flow, atom->box, x, x + width, y);
                        left = x;
                        right = x + width;
                        webengine_float_space(flow, y, strut_height, &left, &right);
                        room = right - left;
                    } else if (!defer_float(flow, atom->box)) {
                        break;
                    }
                }
                continue;
            }
            if (atom->kind == ATOM_ABSOLUTE) {
                continue;
            }
            if (atom->kind == ATOM_BREAK) {
                end = j + 1;
                content = true;
                break;
            }
            if (atom->kind == ATOM_SPACE) {
                if (!content && atom->collapsible) {
                    atom->dropped = true;
                    continue;
                }
                if (atom->wrap && content) {
                    brk = j;
                }
                used += atom->width;
                /* Preserved white space makes a line box (9.4.2). */
                content = content || !atom->collapsible;
                continue;
            }
            if (used + atom->width > room && content && brk != SIZE_MAX) {
                end = brk + 1;
                break;
            }
            if (used + atom->width > room && !content && room < width) {
                /* Nothing fits beside the floats: the line goes down to
                 * where the room next grows. */
                lower = true;
                break;
            }
            if (used + atom->width > room && !content && atom->kind == ATOM_WORD && atom->wrap &&
                split_word(flow, &count, j, room - used)) {
                atom = &flow->atoms[j];
                end = j + 1;
                content = true;
                break;
            }
            used += atom->width;
            if (atom->kind == ATOM_WORD || atom->kind == ATOM_MARKER) {
                content = true;
            }
        }
        if (flow->failed) {
            break;
        }
        if (lower) {
            y = webengine_float_below(flow, y, strut_height);
            continue;
        }
        /* White space at the end of the line: collapsible spaces go, and
         * preserved ones hang past the edge (16.6.1), unless pre keeps them. */
        for (size_t j = end; j > start; j--) {
            struct webengine_atom *atom = &flow->atoms[j - 1];
            if (atom->kind == ATOM_CLOSE || atom->kind == ATOM_OPEN || atom->kind == ATOM_BREAK ||
                atom->kind == ATOM_FLOAT || atom->kind == ATOM_ABSOLUTE || atom->dropped) {
                continue;
            }
            if (atom->kind != ATOM_SPACE || !(atom->collapsible || atom->hangs)) {
                break;
            }
            atom->dropped = true;
        }
        if (content) {
            int height = build_line(flow, block, start, end, left, y, room, line_indent, &open_count);
            y += height;
            block->line_count++;
        } else {
            /* No line box (9.4.2): only keep the inline boxes' nesting. An
             * absolute box's static position is where the line would be. */
            for (size_t j = start; j < end; j++) {
                if (flow->atoms[j].kind == ATOM_ABSOLUTE) {
                    flow->atoms[j].box->x = left;
                    flow->atoms[j].box->y = y;
                }
                if (flow->atoms[j].kind == ATOM_OPEN &&
                    grow((void **)&flow->open, &flow->open_capacity, open_count, sizeof(*flow->open))) {
                    flow->open[open_count++] = flow->atoms[j].box;
                } else if (flow->atoms[j].kind == ATOM_CLOSE && open_count > 0) {
                    open_count--;
                }
            }
        }
        /* The floats that did not fit go under the line. */
        for (size_t d = 0; d < flow->deferred_count; d++) {
            webengine_place_float(flow, flow->deferred[d], x, x + width, y);
        }
        flow->deferred_count = 0;
        i = end;
    }
    return y - top;
}

void webengine_inline_widths(struct webengine_flow *flow, struct webengine_box *block, int *min, int *max) {
    size_t count = collect_atoms(flow, block, 0);
    /* The widest piece nothing breaks, and the widest line nothing but a
     * forced break ends. */
    int word = 0;
    int line = webengine_length_px(&block->style->text_indent, 0, 0);
    int widest_word = 0;
    int widest_line = 0;
    bool line_start = true;
    for (size_t i = 0; i < count; i++) {
        const struct webengine_atom *atom = &flow->atoms[i];
        switch (atom->kind) {
        case ATOM_SPACE:
            if (atom->wrap) {
                widest_word = word > widest_word ? word : widest_word;
                word = 0;
            }
            if (!(line_start && atom->collapsible)) {
                line += atom->width;
            }
            break;
        case ATOM_BREAK:
            widest_word = word > widest_word ? word : widest_word;
            widest_line = line > widest_line ? line : widest_line;
            word = 0;
            line = 0;
            line_start = true;
            break;
        case ATOM_ABSOLUTE:
            break;
        case ATOM_FLOAT: {
            int edges = webengine_intrinsic_edges(atom->box);
            int narrow = atom->box->min_content + edges;
            widest_word = narrow > widest_word ? narrow : widest_word;
            line += atom->box->max_content + edges;
            break;
        }
        default:
            word += atom->width;
            line += atom->width;
            line_start = line_start && (atom->kind == ATOM_OPEN || atom->kind == ATOM_CLOSE);
            break;
        }
    }
    widest_word = word > widest_word ? word : widest_word;
    widest_line = line > widest_line ? line : widest_line;
    *min = widest_word;
    *max = widest_line;
}

void webengine_flow_free(struct webengine_flow *flow) {
    free(flow->atoms);
    free(flow->placed);
    free(flow->open);
    free(flow->floats);
    free(flow->deferred);
    memset(flow, 0, sizeof(*flow));
}
