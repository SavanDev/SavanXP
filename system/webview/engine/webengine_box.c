/* Web Viewer's box tree (webengine_box.h): CSS 2.1 chapter 9's boxes from a
 * styled libdom document. Written from CSS 2.1 (9.2 box generation, 12.5
 * lists, 16.6 white space) and libcss' headers. */

#include "webengine_box.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* CSS 2.1 Appendix D, with the sizes and margins of HTML's rendering section
 * where the two differ (as browsers do), and lists indented by padding. */
const char webengine_ua_css[] =
    "html, address, blockquote, body, center, dd, dir, div, dl, dt, fieldset, form, h1, h2, h3,"
    " h4, h5, h6, hr, menu, ol, p, pre, ul, article, aside, details, figcaption, figure, footer,"
    " header, hgroup, main, nav, section, summary {display:block}"
    "head, script, style, title, link, meta, base, template, noscript {display:none}"
    "li {display:list-item}"
    "table {display:table} tr {display:table-row} thead {display:table-header-group}"
    "tbody {display:table-row-group} tfoot {display:table-footer-group}"
    "col {display:table-column} colgroup {display:table-column-group}"
    "td, th {display:table-cell} caption {display:table-caption; text-align:center}"
    "th {font-weight:bolder; text-align:center} td, th {padding:1px}"
    "body {margin:8px}"
    "h1 {font-size:2em; margin:0.67em 0} h2 {font-size:1.5em; margin:0.83em 0}"
    "h3 {font-size:1.17em; margin:1em 0} h4 {margin:1.33em 0}"
    "h5 {font-size:0.83em; margin:1.67em 0} h6 {font-size:0.67em; margin:2.33em 0}"
    "p, dl, pre {margin:1em 0} blockquote, figure {margin:1em 40px}"
    "ul, ol, dir, menu {margin:1em 0; padding-left:40px} dd {margin-left:40px}"
    "ol {list-style-type:decimal}"
    "ul ul, ul ol, ol ul, ol ol {margin-top:0; margin-bottom:0}"
    "ul ul, ol ul {list-style-type:circle} ul ul ul, ol ul ul, ul ol ul, ol ol ul {list-style-type:square}"
    "h1, h2, h3, h4, h5, h6, b, strong {font-weight:bolder}"
    "i, cite, em, var, address, dfn {font-style:italic}"
    "pre, tt, code, kbd, samp, listing, xmp, plaintext {font-family:monospace}"
    "pre, listing, xmp, plaintext {white-space:pre}"
    "big {font-size:1.17em} small, sub, sup {font-size:0.83em}"
    "sub {vertical-align:sub} sup {vertical-align:super}"
    "center {text-align:center}"
    "u, ins {text-decoration:underline} s, strike, del {text-decoration:line-through}"
    "hr {color:gray; border-style:inset; border-width:1px; margin:0.5em auto}"
    "a:link {color:blue; text-decoration:underline}";
const size_t webengine_ua_css_length = sizeof(webengine_ua_css) - 1;

/* --- Tree storage --------------------------------------------------------------- */

struct frame {
    dom_node *node;
    css_computed_style *style;
    /* The element's box, or its latest piece when a block split it. NULL
     * for display: none. */
    struct webengine_box *box;
    const struct webengine_box_style *box_style;
    int link;
    /* The ordinal of the last list item among the element's children. */
    int counter;
    /* An inline whose box a block cut; the next inline content goes into a
     * new piece (CSS 2.1 9.2.1.1). */
    bool split;
    /* A float: the white space state of the content around it, which the
     * float does not take part in, to restore when it ends. */
    bool out_of_flow;
    bool outer_space;
    unsigned outer_column;
};

struct builder {
    struct webengine_tree *tree;
    struct webengine_styles *styles;
    const struct webengine_fonts *fonts;
    bool failed;
    /* The inline formatting context so far ends in a collapsible space, or
     * has nothing yet: the next collapsible space is dropped. */
    bool space;
    /* The column in preserved text, for tab stops. */
    unsigned column;
    struct frame *stack;
    size_t depth;
    size_t capacity;
    char *text;
    size_t text_capacity;
};

static struct webengine_box_style *new_style(struct webengine_tree *tree) {
    if (tree->style_count == tree->style_capacity) {
        size_t capacity = tree->style_capacity == 0 ? 32 : tree->style_capacity * 2;
        struct webengine_box_style **styles = realloc(tree->styles, capacity * sizeof(*styles));
        if (styles == NULL) {
            return NULL;
        }
        tree->styles = styles;
        tree->style_capacity = capacity;
    }
    struct webengine_box_style *style = calloc(1, sizeof(*style));
    if (style != NULL) {
        tree->styles[tree->style_count++] = style;
    }
    return style;
}

static struct webengine_box *new_box(struct builder *b, enum webengine_box_type type,
                                     const struct webengine_box_style *style, const char *tag) {
    struct webengine_box *box = calloc(1, sizeof(*box));
    if (box == NULL) {
        b->failed = true;
        return NULL;
    }
    box->type = type;
    box->style = style;
    box->link = -1;
    box->first_piece = true;
    box->last_piece = true;
    snprintf(box->tag, sizeof(box->tag), "%s", tag);
    return box;
}

static void append(struct webengine_box *parent, struct webengine_box *child) {
    child->parent = parent;
    if (parent->last != NULL) {
        parent->last->next = child;
    } else {
        parent->first = child;
    }
    parent->last = child;
}

static void free_box(struct webengine_box *box) {
    /* Iterative, so a deep document does not recurse. */
    while (box != NULL) {
        if (box->first != NULL) {
            struct webengine_box *child = box->first;
            box->first = NULL;
            box = child;
            continue;
        }
        if (box->marker != NULL) {
            free(box->marker->text);
            free(box->marker);
            box->marker = NULL;
        }
        struct webengine_box *next = box->next != NULL ? box->next : box->parent;
        free(box->text);
        free(box);
        box = next;
    }
}

void webengine_tree_free(struct webengine_tree *tree) {
    if (tree == NULL) {
        return;
    }
    if (tree->root != NULL) {
        tree->root->next = NULL;
        tree->root->parent = NULL;
        free_box(tree->root);
    }
    for (size_t i = 0; i < tree->link_count; i++) {
        free(tree->links[i]);
    }
    for (size_t i = 0; i < tree->style_count; i++) {
        free(tree->styles[i]);
    }
    free(tree->links);
    free(tree->styles);
    free(tree->lines);
    free(tree->fragments);
    free(tree->paint);
    free(tree);
}

/* --- Computed values -------------------------------------------------------------- */

static int fixed_round(css_fixed value) {
    return value >= 0 ? (int)((value + 512) >> 10) : -(int)((-value + 512) >> 10);
}

static struct webengine_len length(const css_computed_style *style, const css_unit_ctx *units,
                                   css_fixed value, css_unit unit) {
    if (unit == CSS_UNIT_PCT) {
        return (struct webengine_len){ WEBENGINE_LEN_PERCENT, value };
    }
    return (struct webengine_len){ WEBENGINE_LEN_PX, css_unit_len2css_px(style, units, value, unit) };
}

/* A property that is a length, a percentage or `keyword`. */
#define LENGTH_OR(getter, set, keyword)                                                    \
    do {                                                                                   \
        css_fixed value_ = 0;                                                              \
        css_unit unit_ = CSS_UNIT_PX;                                                      \
        uint8_t type_ = getter(style, &value_, &unit_);                                    \
        *out = type_ == (set) ? length(style, units, value_, unit_)                       \
                              : (struct webengine_len){ (keyword), 0 };                    \
    } while (0)

static void margin_lengths(const css_computed_style *style, const css_unit_ctx *units,
                           struct webengine_box_style *box) {
    struct webengine_len *out;
    out = &box->margin[WEBENGINE_TOP];
    LENGTH_OR(css_computed_margin_top, CSS_MARGIN_SET, WEBENGINE_LEN_AUTO);
    out = &box->margin[WEBENGINE_RIGHT];
    LENGTH_OR(css_computed_margin_right, CSS_MARGIN_SET, WEBENGINE_LEN_AUTO);
    out = &box->margin[WEBENGINE_BOTTOM];
    LENGTH_OR(css_computed_margin_bottom, CSS_MARGIN_SET, WEBENGINE_LEN_AUTO);
    out = &box->margin[WEBENGINE_LEFT];
    LENGTH_OR(css_computed_margin_left, CSS_MARGIN_SET, WEBENGINE_LEN_AUTO);
    out = &box->padding[WEBENGINE_TOP];
    LENGTH_OR(css_computed_padding_top, CSS_PADDING_SET, WEBENGINE_LEN_PX);
    out = &box->padding[WEBENGINE_RIGHT];
    LENGTH_OR(css_computed_padding_right, CSS_PADDING_SET, WEBENGINE_LEN_PX);
    out = &box->padding[WEBENGINE_BOTTOM];
    LENGTH_OR(css_computed_padding_bottom, CSS_PADDING_SET, WEBENGINE_LEN_PX);
    out = &box->padding[WEBENGINE_LEFT];
    LENGTH_OR(css_computed_padding_left, CSS_PADDING_SET, WEBENGINE_LEN_PX);
    out = &box->width;
    LENGTH_OR(css_computed_width, CSS_WIDTH_SET, WEBENGINE_LEN_AUTO);
    out = &box->height;
    LENGTH_OR(css_computed_height, CSS_HEIGHT_SET, WEBENGINE_LEN_AUTO);
    out = &box->min_width;
    LENGTH_OR(css_computed_min_width, CSS_MIN_WIDTH_SET, WEBENGINE_LEN_AUTO);
    out = &box->max_width;
    LENGTH_OR(css_computed_max_width, CSS_MAX_WIDTH_SET, WEBENGINE_LEN_NONE);
    out = &box->min_height;
    LENGTH_OR(css_computed_min_height, CSS_MIN_HEIGHT_SET, WEBENGINE_LEN_AUTO);
    out = &box->max_height;
    LENGTH_OR(css_computed_max_height, CSS_MAX_HEIGHT_SET, WEBENGINE_LEN_NONE);
    out = &box->text_indent;
    LENGTH_OR(css_computed_text_indent, CSS_TEXT_INDENT_SET, WEBENGINE_LEN_PX);
}

static void border_side(const css_computed_style *style, const css_unit_ctx *units,
                        struct webengine_box_style *box, int side) {
    typedef uint8_t (*width_getter)(const css_computed_style *, css_fixed *, css_unit *);
    typedef uint8_t (*style_getter)(const css_computed_style *);
    typedef uint8_t (*color_getter)(const css_computed_style *, css_color *);
    static const width_getter widths[4] = {
        css_computed_border_top_width, css_computed_border_right_width,
        css_computed_border_bottom_width, css_computed_border_left_width,
    };
    static const style_getter styles[4] = {
        css_computed_border_top_style, css_computed_border_right_style,
        css_computed_border_bottom_style, css_computed_border_left_style,
    };
    static const color_getter colors[4] = {
        css_computed_border_top_color, css_computed_border_right_color,
        css_computed_border_bottom_color, css_computed_border_left_color,
    };
    css_fixed value = 0;
    css_unit unit = CSS_UNIT_PX;
    uint8_t kind = widths[side](style, &value, &unit);
    uint8_t border_style = styles[side](style);
    int width = kind == CSS_BORDER_WIDTH_THIN ? 1 : kind == CSS_BORDER_WIDTH_MEDIUM ? 3
              : kind == CSS_BORDER_WIDTH_THICK ? 5
              : fixed_round(css_unit_len2css_px(style, units, value, unit));
    css_color color = 0;
    /* A side with no style has no width (CSS 2.1 8.5.1). */
    box->border_style[side] = border_style;
    box->border_width[side] = border_style == CSS_BORDER_STYLE_NONE ||
            border_style == CSS_BORDER_STYLE_HIDDEN || width < 0 ? 0 : width;
    if (colors[side](style, &color) == CSS_BORDER_COLOR_CURRENT_COLOR) {
        (void)css_computed_color(style, &color);
    }
    box->border_color[side] = color;
}

static bool monospace_family(const css_computed_style *style) {
    lwc_string **names = NULL;
    uint8_t family = css_computed_font_family(style, &names);
    if (family == CSS_FONT_FAMILY_MONOSPACE) {
        return true;
    }
    static const char *const fixed[] = { "monospace", "courier", "courier new", "consolas",
                                         "monaco", "menlo", "lucida console" };
    for (size_t i = 0; names != NULL && names[i] != NULL; i++) {
        for (size_t j = 0; j < sizeof(fixed) / sizeof(fixed[0]); j++) {
            size_t n = strlen(fixed[j]);
            if (lwc_string_length(names[i]) == n && strncasecmp(lwc_string_data(names[i]), fixed[j], n) == 0) {
                return true;
            }
        }
    }
    return false;
}

static const struct webengine_box_style *element_style(struct builder *b, const css_computed_style *style,
                                                       const struct webengine_box_style *parent) {
    const css_unit_ctx *units = b->styles->units;
    struct webengine_box_style *box = new_style(b->tree);
    if (box == NULL) {
        b->failed = true;
        return NULL;
    }
    css_fixed value = 0;
    css_unit unit = CSS_UNIT_PX;
    (void)css_computed_color(style, &box->color);
    (void)css_computed_background_color(style, &box->background);
    (void)css_computed_font_size(style, &value, &unit);
    box->font_size = fixed_round(css_unit_len2css_px(style, units, value, unit));
    uint8_t weight = css_computed_font_weight(style);
    box->bold = weight == CSS_FONT_WEIGHT_BOLD || weight == CSS_FONT_WEIGHT_BOLDER ||
        (weight >= CSS_FONT_WEIGHT_600 && weight <= CSS_FONT_WEIGHT_900);
    box->monospace = monospace_family(style);
    box->face = b->fonts->face(b->fonts->user, box->font_size, box->bold, box->monospace);
    uint8_t decoration = css_computed_text_decoration(style);
    box->decoration = (uint8_t)((parent != NULL ? parent->decoration : 0) |
        (decoration & (CSS_TEXT_DECORATION_UNDERLINE | CSS_TEXT_DECORATION_OVERLINE |
                       CSS_TEXT_DECORATION_LINE_THROUGH)));
    box->white_space = css_computed_white_space(style);
    box->text_align = css_computed_text_align(style);
    box->list_style_type = css_computed_list_style_type(style);
    box->list_style_position = css_computed_list_style_position(style);
    box->visibility = css_computed_visibility(style);
    box->box_sizing = css_computed_box_sizing(style);
    box->overflow = css_computed_overflow_y(style);
    box->float_side = css_computed_float(style);
    box->clear = css_computed_clear(style);
    box->vertical_align = css_computed_vertical_align(style, &value, &unit);
    box->vertical_align_length = box->vertical_align == CSS_VERTICAL_ALIGN_SET
        ? length(style, units, value, unit) : (struct webengine_len){ WEBENGINE_LEN_PX, 0 };
    switch (css_computed_line_height(style, &value, &unit)) {
    case CSS_LINE_HEIGHT_NUMBER:
        box->line_height = (struct webengine_len){ WEBENGINE_LEN_NUMBER, value };
        break;
    case CSS_LINE_HEIGHT_DIMENSION:
        box->line_height = unit == CSS_UNIT_PCT
            ? (struct webengine_len){ WEBENGINE_LEN_NUMBER, value / 100 }
            : length(style, units, value, unit);
        break;
    default:
        box->line_height = (struct webengine_len){ WEBENGINE_LEN_NORMAL, 0 };
        break;
    }
    margin_lengths(style, units, box);
    for (int side = 0; side < 4; side++) {
        border_side(style, units, box, side);
    }
    return box;
}

/* An anonymous box's values: the inherited ones from `parent`, the initial
 * ones for the rest (CSS 2.1 9.2.1.1). */
static const struct webengine_box_style *anonymous_style(struct builder *b,
                                                         const struct webengine_box_style *parent) {
    struct webengine_box_style *box = new_style(b->tree);
    if (box == NULL) {
        b->failed = true;
        return NULL;
    }
    box->color = parent->color;
    box->font_size = parent->font_size;
    box->face = parent->face;
    box->bold = parent->bold;
    box->monospace = parent->monospace;
    box->decoration = parent->decoration;
    box->white_space = parent->white_space;
    box->text_align = parent->text_align;
    box->list_style_type = parent->list_style_type;
    box->list_style_position = parent->list_style_position;
    box->visibility = parent->visibility;
    box->line_height = parent->line_height;
    box->text_indent = parent->text_indent;
    box->box_sizing = CSS_BOX_SIZING_CONTENT_BOX;
    box->overflow = CSS_OVERFLOW_VISIBLE;
    box->float_side = CSS_FLOAT_NONE;
    box->clear = CSS_CLEAR_NONE;
    box->vertical_align = CSS_VERTICAL_ALIGN_BASELINE;
    box->width = box->height = box->min_width = box->min_height =
        (struct webengine_len){ WEBENGINE_LEN_AUTO, 0 };
    box->max_width = box->max_height = (struct webengine_len){ WEBENGINE_LEN_NONE, 0 };
    for (int side = 0; side < 4; side++) {
        box->border_style[side] = CSS_BORDER_STYLE_NONE;
    }
    return box;
}

/* --- Placing boxes ------------------------------------------------------------------ */

/* Turns a block whose children are inline into one whose children are
 * blocks: its inline content moves into an anonymous block. */
static void wrap_inline_content(struct builder *b, struct webengine_box *block) {
    if (!block->inline_content) {
        return;
    }
    block->inline_content = false;
    if (block->first == NULL) {
        return;
    }
    struct webengine_box *anonymous = new_box(b, WEBENGINE_BOX_BLOCK, anonymous_style(b, block->style), "");
    if (anonymous == NULL) {
        return;
    }
    anonymous->inline_content = true;
    anonymous->first = block->first;
    anonymous->last = block->last;
    for (struct webengine_box *child = anonymous->first; child != NULL; child = child->next) {
        child->parent = anonymous;
    }
    block->first = block->last = NULL;
    append(block, anonymous);
}

/* Where inline content of `box` goes: `box` itself when it is an inline or
 * a block holding inline content, else the anonymous block after its last
 * child block. White space was already reset where that block ended. */
static struct webengine_box *inline_container(struct builder *b, struct webengine_box *box) {
    if (box->type != WEBENGINE_BOX_BLOCK || box->inline_content) {
        return box;
    }
    if (box->first == NULL) {
        box->inline_content = true;
        return box;
    }
    if (box->last->type == WEBENGINE_BOX_BLOCK && box->last->tag[0] == '\0' && box->last->inline_content) {
        return box->last;
    }
    struct webengine_box *anonymous = new_box(b, WEBENGINE_BOX_BLOCK, anonymous_style(b, box->style), "");
    if (anonymous == NULL) {
        return NULL;
    }
    anonymous->inline_content = true;
    append(box, anonymous);
    return anonymous;
}

/* The box the next inline content of the innermost open element goes into.
 * An inline a block split gets its next piece here, its open ancestors up
 * to the block too. */
static struct webengine_box *inline_target(struct builder *b) {
    size_t top = b->depth;
    while (top > 0 && b->stack[top - 1].split) {
        top--;
    }
    if (top < b->depth) {
        struct webengine_box *parent = inline_container(b, b->stack[top - 1].box);
        for (size_t i = top; i < b->depth && parent != NULL; i++) {
            struct frame *frame = &b->stack[i];
            struct webengine_box *piece = new_box(b, WEBENGINE_BOX_INLINE, frame->box->style, frame->box->tag);
            if (piece == NULL) {
                return NULL;
            }
            piece->link = frame->box->link;
            piece->first_piece = false;
            append(parent, piece);
            frame->box = piece;
            frame->split = false;
            parent = piece;
        }
    }
    return inline_container(b, b->stack[b->depth - 1].box);
}

/* The block a new block-level box goes into, splitting the inlines between
 * it and the innermost open element. */
static struct webengine_box *block_target(struct builder *b) {
    size_t block = b->depth;
    while (block > 0 && b->stack[block - 1].box->type != WEBENGINE_BOX_BLOCK) {
        block--;
    }
    struct webengine_box *container = b->stack[block - 1].box;
    for (size_t i = block; i < b->depth; i++) {
        if (!b->stack[i].split) {
            b->stack[i].box->last_piece = false;
            b->stack[i].split = true;
        }
    }
    wrap_inline_content(b, container);
    b->space = true;
    b->column = 0;
    return container;
}

static void append_inline(struct builder *b, struct webengine_box *box) {
    struct webengine_box *target = inline_target(b);
    if (target == NULL) {
        b->failed = true;
        return;
    }
    append(target, box);
}

/* --- Text --------------------------------------------------------------------------- */

static bool reserve_text(struct builder *b, size_t length) {
    if (length <= b->text_capacity) {
        return true;
    }
    size_t capacity = b->text_capacity == 0 ? 256 : b->text_capacity;
    while (capacity < length) {
        capacity *= 2;
    }
    char *text = realloc(b->text, capacity);
    if (text == NULL) {
        b->failed = true;
        return false;
    }
    b->text = text;
    b->text_capacity = capacity;
    return true;
}

static bool html_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* CSS 2.1 16.6.1: white space collapses across the inline formatting
 * context, newlines and tabs as white-space says. */
static size_t process_white_space(struct builder *b, uint8_t mode, const char *text, size_t length) {
    bool preserve = mode == CSS_WHITE_SPACE_PRE || mode == CSS_WHITE_SPACE_PRE_WRAP;
    size_t used = 0;
    if (!reserve_text(b, length * 8 + 1)) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (preserve) {
            if (c == '\r') {
                if (i + 1 < length && text[i + 1] == '\n') {
                    continue;
                }
                c = '\n';
            }
            if (c == '\n') {
                b->text[used++] = '\n';
                b->column = 0;
            } else if (c == '\t') {
                do {
                    b->text[used++] = ' ';
                } while (++b->column % 8 != 0);
            } else if ((unsigned char)c >= 32 && c != 127) {
                b->text[used++] = c;
                if (((unsigned char)c & 0xc0) != 0x80) {
                    b->column++;
                }
            }
            b->space = false;
            continue;
        }
        if (mode == CSS_WHITE_SPACE_PRE_LINE && (c == '\n' || c == '\r')) {
            if (c == '\r' && i + 1 < length && text[i + 1] == '\n') {
                continue;
            }
            if (used > 0 && b->text[used - 1] == ' ') {
                used--;
            }
            b->text[used++] = '\n';
            b->space = true;
            continue;
        }
        if (html_space(c)) {
            if (!b->space) {
                b->text[used++] = ' ';
                b->space = true;
            }
            continue;
        }
        if ((unsigned char)c >= 32 && c != 127) {
            b->text[used++] = c;
            b->space = false;
        }
    }
    return used;
}

static void add_text_box(struct builder *b, const char *text, size_t length, bool replaced) {
    struct frame *frame = &b->stack[b->depth - 1];
    struct webengine_box *box = new_box(b, WEBENGINE_BOX_TEXT, frame->box_style, "");
    if (box == NULL) {
        return;
    }
    box->text = malloc(length + 1);
    if (box->text == NULL) {
        free(box);
        b->failed = true;
        return;
    }
    memcpy(box->text, text, length);
    box->text[length] = '\0';
    box->length = length;
    box->link = frame->link;
    box->replaced = replaced;
    append_inline(b, box);
    if (b->failed) {
        return;
    }
}

static void add_text(struct builder *b, const char *text, size_t length) {
    size_t used = process_white_space(b, b->stack[b->depth - 1].box_style->white_space, text, length);
    if (used > 0) {
        add_text_box(b, b->text, used, false);
    }
}

/* --- Elements ------------------------------------------------------------------------ */

static dom_string *attribute(dom_node *node, const char *name) {
    dom_string *key = NULL;
    dom_string *value = NULL;
    if (dom_string_create_interned((const uint8_t *)name, strlen(name), &key) == DOM_NO_ERR) {
        (void)dom_element_get_attribute(node, key, &value);
        dom_string_unref(key);
    }
    return value;
}

static bool integer_attribute(dom_node *node, const char *name, int *out) {
    dom_string *value = attribute(node, name);
    bool found = false;
    if (value != NULL) {
        char digits[16];
        size_t n = dom_string_byte_length(value);
        if (n > 0 && n < sizeof(digits)) {
            memcpy(digits, dom_string_data(value), n);
            digits[n] = '\0';
            char *end = NULL;
            long parsed = strtol(digits, &end, 10);
            found = end != digits && parsed > -1000000 && parsed < 1000000;
            if (found) {
                *out = (int)parsed;
            }
        }
        dom_string_unref(value);
    }
    return found;
}

static int add_link(struct builder *b, const char *href, size_t length) {
    char **links = realloc(b->tree->links, (b->tree->link_count + 1) * sizeof(*links));
    if (links == NULL) {
        b->failed = true;
        return -1;
    }
    b->tree->links = links;
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        b->failed = true;
        return -1;
    }
    memcpy(copy, href, length);
    copy[length] = '\0';
    links[b->tree->link_count] = copy;
    return (int)b->tree->link_count++;
}

static void add_marker(struct builder *b, struct webengine_box *item, const css_computed_style *style,
                       int ordinal) {
    const struct webengine_box_style *item_style = item->style;
    if (item_style->list_style_type == CSS_LIST_STYLE_TYPE_NONE) {
        return;
    }
    struct webengine_box *marker = new_box(b, WEBENGINE_BOX_MARKER, item_style, "");
    if (marker == NULL) {
        return;
    }
    marker->ordinal = ordinal;
    marker->link = -1;
    uint8_t type = item_style->list_style_type;
    /* Disc, circle and square are drawn, not written: the faces may not
     * have the glyphs. */
    if (type != CSS_LIST_STYLE_TYPE_DISC && type != CSS_LIST_STYLE_TYPE_CIRCLE &&
        type != CSS_LIST_STYLE_TYPE_SQUARE) {
        char text[64];
        size_t length = 0;
        if (css_computed_format_list_style(style, ordinal, text, sizeof(text), &length) == CSS_OK &&
            length < sizeof(text)) {
            marker->text = malloc(length + 1);
            if (marker->text == NULL) {
                free(marker);
                b->failed = true;
                return;
            }
            memcpy(marker->text, text, length);
            marker->text[length] = '\0';
            marker->length = length;
        }
    }
    if (item_style->list_style_position == CSS_LIST_STYLE_POSITION_INSIDE) {
        append(inline_container(b, item), marker);
        b->space = true;
    } else {
        marker->parent = item;
        item->marker = marker;
    }
}

static void lower_name(dom_node *node, char *name, size_t capacity) {
    dom_string *value = NULL;
    name[0] = '\0';
    if (dom_node_get_node_name(node, &value) != DOM_NO_ERR || value == NULL) {
        return;
    }
    size_t length = dom_string_byte_length(value);
    if (length < capacity) {
        const char *text = dom_string_data(value);
        for (size_t i = 0; i < length; i++) {
            name[i] = text[i] >= 'A' && text[i] <= 'Z' ? (char)(text[i] - 'A' + 'a') : text[i];
        }
        name[length] = '\0';
    }
    dom_string_unref(value);
}

/* Makes the element's boxes. True when its children are to be walked. */
static bool enter_element(struct builder *b, struct frame *frame) {
    char name[12];
    struct frame *parent = b->depth > 1 ? &b->stack[b->depth - 2] : NULL;
    lower_name(frame->node, name, sizeof(name));
    frame->link = parent != NULL ? parent->link : -1;
    uint8_t display = css_computed_display(frame->style, parent == NULL);
    if (display == CSS_DISPLAY_NONE) {
        return false;
    }
    frame->box_style = element_style(b, frame->style, parent != NULL ? parent->box_style : NULL);
    if (frame->box_style == NULL) {
        return false;
    }
    if (strcmp(name, "a") == 0) {
        dom_string *href = attribute(frame->node, "href");
        if (href != NULL) {
            if (dom_string_byte_length(href) > 0) {
                frame->link = add_link(b, dom_string_data(href), dom_string_byte_length(href));
            }
            dom_string_unref(href);
        }
    }
    if (strcmp(name, "ol") == 0) {
        int start = 1;
        (void)integer_attribute(frame->node, "start", &start);
        frame->counter = start - 1;
    }
    if (parent == NULL) {
        /* The root is a block whatever its display. */
        frame->box = new_box(b, WEBENGINE_BOX_BLOCK, frame->box_style, name);
        b->tree->root = frame->box;
        return frame->box != NULL;
    }
    /* <br> and <img> have no box of their own to hold content: what they
     * make goes into their parent's, from the parent's frame. */
    if (strcmp(name, "br") == 0) {
        struct webengine_box *box = new_box(b, WEBENGINE_BOX_BREAK, frame->box_style, "br");
        if (box != NULL) {
            b->depth--;
            append_inline(b, box);
            b->depth++;
            b->space = true;
        }
        return false;
    }
    if (strcmp(name, "img") == 0) {
        b->depth--;
        add_text_box(b, "[image]", 7, true);
        b->depth++;
        b->space = false;
        return false;
    }
    bool block = display != CSS_DISPLAY_INLINE && display != CSS_DISPLAY_INLINE_BLOCK &&
        display != CSS_DISPLAY_INLINE_TABLE && display != CSS_DISPLAY_INLINE_FLEX &&
        display != CSS_DISPLAY_INLINE_GRID;
    if (!block) {
        struct webengine_box *box = new_box(b, WEBENGINE_BOX_INLINE, frame->box_style, name);
        if (box != NULL) {
            box->link = frame->link;
            frame->box = box;
            /* The frame is already on the stack: put the box in its parent's. */
            b->depth--;
            append_inline(b, box);
            b->depth++;
        }
        return box != NULL;
    }
    struct webengine_box *box = new_box(b, WEBENGINE_BOX_BLOCK, frame->box_style, name);
    if (box == NULL) {
        return false;
    }
    if (frame->box_style->float_side == CSS_FLOAT_LEFT || frame->box_style->float_side == CSS_FLOAT_RIGHT) {
        /* A float is out of the flow: it splits no inline and stays where
         * it is in the inline content, and white space collapses across
         * it as if it were not there (CSS 2.1 9.5, 16.6.1). */
        b->depth--;
        struct webengine_box *container = inline_target(b);
        b->depth++;
        if (container == NULL) {
            free(box);
            b->failed = true;
            return false;
        }
        append(container, box);
        frame->out_of_flow = true;
        frame->outer_space = b->space;
        frame->outer_column = b->column;
        b->space = true;
        b->column = 0;
    } else {
        b->depth--;
        struct webengine_box *container = block_target(b);
        b->depth++;
        append(container, box);
    }
    frame->box = box;
    if (display == CSS_DISPLAY_LIST_ITEM) {
        int ordinal = parent->counter + 1;
        (void)integer_attribute(frame->node, "value", &ordinal);
        parent->counter = ordinal;
        add_marker(b, box, frame->style, ordinal);
    }
    return true;
}

static void leave_element(struct builder *b, struct frame *frame) {
    if (frame->out_of_flow) {
        b->space = frame->outer_space;
        b->column = frame->outer_column;
    } else if (frame->box != NULL && frame->box->type == WEBENGINE_BOX_BLOCK) {
        b->space = true;
        b->column = 0;
    }
}

static bool push(struct builder *b, dom_node *node) {
    if (b->depth == b->capacity) {
        size_t capacity = b->capacity == 0 ? 32 : b->capacity * 2;
        struct frame *stack = realloc(b->stack, capacity * sizeof(*stack));
        if (stack == NULL) {
            b->failed = true;
            return false;
        }
        b->stack = stack;
        b->capacity = capacity;
    }
    b->stack[b->depth++] = (struct frame){ .node = node, .link = -1 };
    return true;
}

static void pop(struct builder *b) {
    struct frame *frame = &b->stack[--b->depth];
    if (frame->box != NULL) {
        leave_element(b, frame);
    }
    if (frame->style != NULL) {
        css_computed_style_destroy(frame->style);
    }
    dom_node_unref(frame->node);
}

/* The canvas takes the root's background, or body's (CSS 2.1 14.2). */
static void find_canvas(struct webengine_tree *tree) {
    const struct webengine_box *root = tree->root;
    if (root == NULL) {
        return;
    }
    if (root->style->background >> 24) {
        tree->canvas = root->style->background;
        tree->canvas_source = root;
        return;
    }
    for (const struct webengine_box *child = root->first; child != NULL; child = child->next) {
        if (strcmp(child->tag, "body") == 0) {
            tree->canvas = child->style->background;
            tree->canvas_source = child->style->background >> 24 ? child : NULL;
            return;
        }
    }
}

struct webengine_tree *webengine_tree_build(dom_document *document, struct webengine_styles *styles,
                                            const struct webengine_fonts *fonts) {
    struct builder b = { .styles = styles, .fonts = fonts, .space = true };
    b.tree = calloc(1, sizeof(*b.tree));
    if (b.tree == NULL) {
        return NULL;
    }
    dom_element *root = NULL;
    if (dom_document_get_document_element(document, &root) != DOM_NO_ERR || root == NULL) {
        return b.tree;
    }
    dom_node *node = (dom_node *)root;
    while (node != NULL && !b.failed) {
        dom_node_type type;
        dom_node *next = NULL;
        bool descend = false;
        if (dom_node_get_node_type(node, &type) != DOM_NO_ERR) {
            b.failed = true;
            dom_node_unref(node);
            break;
        }
        if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
            dom_string *value = NULL;
            if (b.depth > 0 && b.stack[b.depth - 1].box != NULL &&
                dom_node_get_node_value(node, &value) == DOM_NO_ERR && value != NULL) {
                add_text(&b, dom_string_data(value), dom_string_byte_length(value));
            }
            dom_string_unref(value);
        } else if (type == DOM_ELEMENT_NODE && push(&b, node)) {
            struct frame *frame = &b.stack[b.depth - 1];
            const css_computed_style *parent = b.depth > 1 ? b.stack[b.depth - 2].style : NULL;
            frame->style = webengine_style_element(styles, (dom_element *)node, parent);
            node = NULL; /* the frame owns the reference now */
            if (frame->style == NULL) {
                b.failed = true;
            } else if (enter_element(&b, frame) &&
                       dom_node_get_first_child(frame->node, &next) != DOM_NO_ERR) {
                b.failed = true;
            }
            descend = next != NULL;
            if (!descend) {
                /* No children: leave it at once. */
                dom_node *element = frame->node;
                dom_node_ref(element);
                pop(&b);
                node = element;
            }
        }
        if (descend) {
            node = next;
            continue;
        }
        /* The next sibling, or the parent's, climbing out of finished elements. */
        while (node != NULL && !b.failed) {
            if (dom_node_get_next_sibling(node, &next) != DOM_NO_ERR) {
                b.failed = true;
                next = NULL;
            }
            dom_node_unref(node);
            node = NULL;
            if (next != NULL || b.depth == 0) {
                node = next;
                break;
            }
            /* The parent is the innermost open element. */
            node = b.stack[b.depth - 1].node;
            dom_node_ref(node);
            pop(&b);
            if (b.depth == 0) {
                dom_node_unref(node);
                node = NULL;
                break;
            }
        }
        if (node != NULL && b.failed) {
            dom_node_unref(node);
            node = NULL;
        }
    }
    if (node != NULL) {
        dom_node_unref(node);
    }
    while (b.depth > 0) {
        pop(&b);
    }
    styles->units->root_style = NULL;
    free(b.stack);
    free(b.text);
    if (b.failed) {
        webengine_tree_free(b.tree);
        return NULL;
    }
    find_canvas(b.tree);
    return b.tree;
}

struct webengine_tree *webengine_tree_text(const char *text, size_t length, int font_size,
                                           const struct webengine_fonts *fonts) {
    struct builder b = { .fonts = fonts, .space = true };
    b.tree = calloc(1, sizeof(*b.tree));
    if (b.tree == NULL) {
        return NULL;
    }
    struct webengine_box_style *style = new_style(b.tree);
    if (style != NULL) {
        style->color = 0xff000000;
        style->font_size = font_size;
        style->face = fonts->face(fonts->user, font_size, false, false);
        style->white_space = CSS_WHITE_SPACE_PRE_WRAP;
        style->text_align = CSS_TEXT_ALIGN_LEFT;
        style->visibility = CSS_VISIBILITY_VISIBLE;
        style->list_style_type = CSS_LIST_STYLE_TYPE_NONE;
        style->line_height = (struct webengine_len){ WEBENGINE_LEN_NORMAL, 0 };
        style->vertical_align = CSS_VERTICAL_ALIGN_BASELINE;
        style->box_sizing = CSS_BOX_SIZING_CONTENT_BOX;
        style->overflow = CSS_OVERFLOW_VISIBLE;
        style->float_side = CSS_FLOAT_NONE;
        style->clear = CSS_CLEAR_NONE;
        style->width = style->height = style->min_width = style->min_height =
            (struct webengine_len){ WEBENGINE_LEN_AUTO, 0 };
        style->max_width = style->max_height = (struct webengine_len){ WEBENGINE_LEN_NONE, 0 };
        for (int side = 0; side < 4; side++) {
            style->border_style[side] = CSS_BORDER_STYLE_NONE;
            /* The page's 8px body margin, so text does not touch the frame. */
            style->padding[side] = (struct webengine_len){ WEBENGINE_LEN_PX, INTTOFIX(8) };
        }
        b.tree->root = new_box(&b, WEBENGINE_BOX_BLOCK, style, "");
    }
    if (b.tree->root == NULL || !push(&b, NULL)) {
        webengine_tree_free(b.tree);
        free(b.stack);
        return NULL;
    }
    b.stack[0].box = b.tree->root;
    b.stack[0].box_style = style;
    add_text(&b, text, length);
    free(b.stack);
    free(b.text);
    if (b.failed) {
        webengine_tree_free(b.tree);
        return NULL;
    }
    return b.tree;
}

/* --- Dump ----------------------------------------------------------------------------- */

static void quote(char *out, size_t capacity, const char *text, size_t length) {
    size_t used = 0;
    for (size_t i = 0; i < length && used + 3 < capacity; i++) {
        if (text[i] == '\n') {
            out[used++] = '\\';
            out[used++] = 'n';
        } else if (text[i] == '"' || text[i] == '\\') {
            out[used++] = '\\';
            out[used++] = text[i];
        } else {
            out[used++] = text[i];
        }
    }
    out[used] = '\0';
}

static void dump_box(const struct webengine_box *box, unsigned depth, bool laid_out, webengine_dump_line line,
                     void *user) {
    char text[200];
    char out[256];
    unsigned indent = depth * 2 < 64 ? depth * 2 : 64;
    memset(out, ' ', indent);
    switch (box->type) {
    case WEBENGINE_BOX_BLOCK: {
        const char *side = !webengine_box_floats(box) ? ""
            : box->style->float_side == CSS_FLOAT_LEFT ? " float left" : " float right";
        int used = box->tag[0] == '\0'
            ? snprintf(out + indent, sizeof(out) - indent, "anonymous")
            : snprintf(out + indent, sizeof(out) - indent, "block %s%s", box->tag, side);
        if (laid_out && used > 0) {
            snprintf(out + indent + used, sizeof(out) - indent - (size_t)used, " %d %d %d %d",
                     box->x, box->y, box->width, box->height);
        }
        break;
    }
    case WEBENGINE_BOX_INLINE:
        snprintf(out + indent, sizeof(out) - indent, "inline %s%s%s", box->tag,
                 box->first_piece ? "" : " (continued)", box->last_piece ? "" : " (split)");
        break;
    case WEBENGINE_BOX_TEXT:
        quote(text, sizeof(text), box->text, box->length);
        snprintf(out + indent, sizeof(out) - indent, "text \"%s\"%s%s", text,
                 box->link >= 0 ? " link" : "", box->replaced ? " image" : "");
        break;
    case WEBENGINE_BOX_BREAK:
        snprintf(out + indent, sizeof(out) - indent, "break");
        break;
    case WEBENGINE_BOX_MARKER: {
        bool outside = box->parent != NULL && box->parent->marker == box;
        quote(text, sizeof(text), box->text != NULL ? box->text : "", box->length);
        int used = snprintf(out + indent, sizeof(out) - indent, "marker %d \"%s\"%s", box->ordinal, text,
                            outside ? " outside" : "");
        if (laid_out && outside && used > 0) {
            snprintf(out + indent + used, sizeof(out) - indent - (size_t)used, " %d %d %d %d",
                     box->x, box->y, box->width, box->height);
        }
        break;
    }
    }
    line(user, out);
}

/* A laid-out block's line boxes and their fragments, in place of its
 * children. Outside markers are dumped with their item. */
static void dump_lines(const struct webengine_tree *tree, const struct webengine_box *block, unsigned depth,
                       webengine_dump_line line, void *user) {
    char text[200];
    char out[256];
    for (size_t l = block->first_line; l < block->first_line + block->line_count; l++) {
        const struct webengine_line *box = &tree->lines[l];
        unsigned indent = depth * 2 < 64 ? depth * 2 : 64;
        memset(out, ' ', indent);
        snprintf(out + indent, sizeof(out) - indent, "line %d %d %d %d", box->x, box->y, box->width, box->height);
        line(user, out);
        indent = indent + 2;
        memset(out, ' ', indent);
        for (size_t f = box->first_fragment; f < box->first_fragment + box->fragment_count; f++) {
            const struct webengine_fragment *fragment = &tree->fragments[f];
            switch (fragment->type) {
            case WEBENGINE_FRAGMENT_INLINE:
                snprintf(out + indent, sizeof(out) - indent, "inline %s %d %d %d %d%s%s", fragment->box->tag,
                         fragment->x, fragment->y, fragment->width, fragment->height,
                         fragment->left_edge ? "" : " (open left)", fragment->right_edge ? "" : " (open right)");
                break;
            case WEBENGINE_FRAGMENT_TEXT:
                quote(text, sizeof(text), fragment->box->text + fragment->start, fragment->length);
                snprintf(out + indent, sizeof(out) - indent, "text \"%s\" %d %d %d %d", text, fragment->x,
                         fragment->y, fragment->width, fragment->height);
                break;
            case WEBENGINE_FRAGMENT_MARKER:
                if (fragment->box->parent != NULL && fragment->box->parent->marker == fragment->box) {
                    continue;
                }
                quote(text, sizeof(text), fragment->box->text != NULL ? fragment->box->text : "",
                      fragment->box->length);
                snprintf(out + indent, sizeof(out) - indent, "marker %d \"%s\" %d %d %d %d", fragment->box->ordinal,
                         text, fragment->x, fragment->y, fragment->width, fragment->height);
                break;
            }
            line(user, out);
        }
    }
}

void webengine_tree_dump(const struct webengine_tree *tree, webengine_dump_line line, void *user) {
    bool laid_out = tree->layout_width > 0;
    const struct webengine_box *box = tree->root;
    /* Before layout every box is a level; after it only blocks are, the
     * floats in inline content one level inside the block they are in. */
    unsigned depth = 0;
    while (box != NULL) {
        bool shown = !laid_out || box->type == WEBENGINE_BOX_BLOCK;
        if (shown) {
            dump_box(box, depth, laid_out, line, user);
            if (box->marker != NULL) {
                dump_box(box->marker, depth + 1, laid_out, line, user);
            }
            if (laid_out && box->inline_content) {
                dump_lines(tree, box, depth + 1, line, user);
            }
        }
        if (box->first != NULL) {
            depth += shown;
            box = box->first;
            continue;
        }
        while (box != NULL && box->next == NULL) {
            box = box->parent;
            if (box != NULL) {
                depth -= !laid_out || box->type == WEBENGINE_BOX_BLOCK;
            }
        }
        if (box != NULL) {
            box = box->next;
        }
    }
}
