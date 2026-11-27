#pragma once

/* Web Viewer's box tree (docs/desktop/WEBVIEWER_ENGINE.md#the-box-tree), E4 of
 * docs/roadmaps/open/WEBVIEWER_ROADMAP.md: the boxes CSS 2.1's visual
 * formatting model (chapter 9) makes from a styled libdom document. Each box
 * carries the computed values layout needs, in pixels where they are known
 * and as percentages where they wait for a containing block.
 *
 * Written from CSS 2.1 and libcss' headers; NetSurf's box construction is not
 * its model (the clean-room rule). Neutral like webengine.h: the same source
 * builds into Web Viewer and the host tests. */

#include "webengine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the caller's fonts answer. Faces are the caller's numbers. */
struct webengine_fonts {
    void *user;
    /* The face for text of `size` CSS pixels. */
    int (*face)(void *user, int size, bool bold, bool monospace);
    /* The advance of `length` bytes of UTF-8 `text`, bold included. */
    int (*width)(void *user, int face, bool bold, const char *text, size_t length);
    /* Above and below the baseline, in pixels. */
    void (*metrics)(void *user, int face, int *ascent, int *descent);
};

enum webengine_len_kind {
    WEBENGINE_LEN_PX,      /* value: CSS pixels, css_fixed */
    WEBENGINE_LEN_PERCENT, /* value: percent, css_fixed */
    WEBENGINE_LEN_AUTO,
    WEBENGINE_LEN_NONE,    /* max-width and max-height: none */
    WEBENGINE_LEN_NORMAL,  /* line-height: normal */
    WEBENGINE_LEN_NUMBER,  /* line-height: a multiple of the font size, css_fixed */
};

struct webengine_len {
    uint8_t kind;
    css_fixed value;
};

enum webengine_side { WEBENGINE_TOP, WEBENGINE_RIGHT, WEBENGINE_BOTTOM, WEBENGINE_LEFT };

/* The values of one element that layout and paint read. Anonymous boxes get
 * their parent's inherited values and the initial ones for the rest. */
struct webengine_box_style {
    css_color color;
    css_color background;
    int font_size;
    int face;
    bool bold;
    bool monospace;
    /* text-decoration lines (CSS_TEXT_DECORATION_*), the element's own and
     * every ancestor's: they propagate to the text inside. */
    uint8_t decoration;
    uint8_t white_space;
    uint8_t text_align;
    uint8_t vertical_align;
    uint8_t list_style_type;
    uint8_t list_style_position;
    uint8_t visibility;
    uint8_t box_sizing;
    uint8_t overflow;
    /* float (CSS_FLOAT_*) and clear (CSS_CLEAR_*). */
    uint8_t float_side;
    uint8_t clear;
    /* position (CSS_POSITION_*; sticky counts as static) and z-index. */
    uint8_t position;
    bool z_auto;
    int32_t z_index;
    /* top, right, bottom and left. For position: relative, libcss already
     * resolved them (9.4.3): top and left are the offsets. */
    struct webengine_len offset[4];
    /* Tables (CSS 2.1 17): border-spacing across and down, in pixels,
     * caption-side and empty-cells (CSS_CAPTION_SIDE_*, CSS_EMPTY_CELLS_*). */
    int border_spacing[2];
    uint8_t caption_side;
    uint8_t empty_cells;
    struct webengine_len vertical_align_length;
    struct webengine_len line_height;
    struct webengine_len text_indent;
    struct webengine_len margin[4];
    struct webengine_len padding[4];
    struct webengine_len width;
    struct webengine_len height;
    struct webengine_len min_width;
    struct webengine_len max_width;
    struct webengine_len min_height;
    struct webengine_len max_height;
    /* Zero when the side's style is none or hidden. */
    int border_width[4];
    uint8_t border_style[4];
    css_color border_color[4];
};

enum webengine_box_type {
    /* Block-level: an element's block, list item or table part (its
     * `table_role`), or an anonymous block around inline content. A
     * float or an absolutely positioned box is a block too, out of the
     * flow: it stays where it appears, among inline content when that is
     * where it is (CSS 2.1 9.5, 9.6). */
    WEBENGINE_BOX_BLOCK,
    /* An inline element; a block inside it splits it into pieces. */
    WEBENGINE_BOX_INLINE,
    /* Text after white-space processing (CSS 2.1 16.6.1). */
    WEBENGINE_BOX_TEXT,
    /* A forced line break, <br>. */
    WEBENGINE_BOX_BREAK,
    /* A list item's marker: in the item's inline content when
     * list-style-position is inside, otherwise its `marker`. */
    WEBENGINE_BOX_MARKER,
    /* An inline-level image, a replaced element (CSS 2.1 10.3.2, 10.6.2):
     * an atom of its line. A block-level one (display: block, a float, an
     * absolute box) is a BLOCK with an `image`. */
    WEBENGINE_BOX_IMAGE,
};

/* An image of the page (webengine_tree.images): its source as written in
 * src="", for the caller to resolve, fetch and decode, and once it has, its
 * natural size. The caller sets `state`, `width` and `height` and lays the
 * tree out again. */
enum webengine_image_state {
    WEBENGINE_IMAGE_LOADING,
    WEBENGINE_IMAGE_READY,
    WEBENGINE_IMAGE_FAILED,
};

struct webengine_image {
    char *url;
    uint8_t state;
    int width;
    int height;
};

/* What a block is in a table (CSS 2.1 17.2), from its display or, for an
 * anonymous box, from what it wraps. */
enum webengine_table_role {
    WEBENGINE_TABLE_NONE,
    /* The table wrapper box and the table box in one: x, y, width and
     * height are the wrapper's, captions included; table_y and
     * table_height are the table box's. */
    WEBENGINE_TABLE_TABLE,
    WEBENGINE_TABLE_CAPTION,
    WEBENGINE_TABLE_ROW_GROUP,
    WEBENGINE_TABLE_HEADER_GROUP,
    WEBENGINE_TABLE_FOOTER_GROUP,
    WEBENGINE_TABLE_ROW,
    WEBENGINE_TABLE_CELL,
    /* A column or a column group: no boxes inside, only widths. */
    WEBENGINE_TABLE_COLUMN,
    WEBENGINE_TABLE_COLUMN_GROUP,
};

/* A clip rectangle in canvas pixels, when `on`. */
struct webengine_clip {
    bool on;
    int x;
    int y;
    int width;
    int height;
};

struct webengine_box {
    enum webengine_box_type type;
    struct webengine_box *parent;
    struct webengine_box *first;
    struct webengine_box *last;
    struct webengine_box *next;
    const struct webengine_box_style *style;
    /* The element's name in lower case, "" for an anonymous box. */
    char tag[12];
    /* TEXT and MARKER: UTF-8, '\n' only where white-space preserves it.
     * An image: its alt="" text, NULL without one. */
    char *text;
    size_t length;
    /* An image (IMAGE, or a block-level BLOCK): webengine_tree.images[image];
     * -1 for every other box. */
    int image;
    /* The link the box is in (webengine_tree.links), or -1. */
    int link;
    /* MARKER: the list item's ordinal. */
    int ordinal;
    /* BLOCK: its children are inline-level (an inline formatting context). */
    bool inline_content;
    /* INLINE: which pieces of a split inline this is; only the first draws
     * its left edge and only the last its right one. */
    bool first_piece;
    bool last_piece;
    /* BLOCK of a list item with an outside marker. */
    struct webengine_box *marker;
    /* BLOCK: its role in a table, and for a cell the columns and rows it
     * spans (colspan, rowspan; 0 rows is to the end of its row group), for a
     * column its span. */
    uint8_t table_role;
    int col_span;
    int row_span;

    /* Set by webengine_layout. BLOCK: the border box in canvas pixels and
     * the used widths of its edges. IMAGE: its content width and height,
     * and its used edges. */
    int x;
    int y;
    int width;
    int height;
    int margin[4];
    int border[4];
    int padding[4];
    /* BLOCK with inline content: its line boxes, tree->lines[first_line]
     * on. INLINE: the used edges are in margin, border and padding too. */
    size_t first_line;
    size_t line_count;
    /* BLOCK, while layout works out a shrink-to-fit width (CSS 2.1
     * 10.3.5): its content's minimum and preferred widths. */
    int min_content;
    int max_content;
    /* TABLE: the table box's top border edge and height, below its top
     * captions and above its bottom ones. */
    int table_y;
    int table_height;
    /* An absolutely positioned BLOCK: the box whose padding box is its
     * containing block, NULL for the viewport. */
    const struct webengine_box *containing;
    /* Set with the painting order: what clips the box's content, its own
     * padding box when its overflow is not visible (CSS 2.1 11.1.1)
     * within what clips the box itself. */
    struct webengine_clip clip;
};

static inline bool webengine_box_floats(const struct webengine_box *box) {
    return box->type == WEBENGINE_BOX_BLOCK && box->parent != NULL &&
        (box->style->float_side == CSS_FLOAT_LEFT || box->style->float_side == CSS_FLOAT_RIGHT);
}

/* position: absolute or fixed, out of the flow (9.6). */
static inline bool webengine_box_absolute(const struct webengine_box *box) {
    return box->type == WEBENGINE_BOX_BLOCK && box->parent != NULL &&
        (box->style->position == CSS_POSITION_ABSOLUTE || box->style->position == CSS_POSITION_FIXED);
}

/* Positioned: relative, absolute or fixed. The root is never moved. */
static inline bool webengine_box_positioned(const struct webengine_box *box) {
    return box->parent != NULL && (box->style->position == CSS_POSITION_RELATIVE ||
                                   box->style->position == CSS_POSITION_ABSOLUTE ||
                                   box->style->position == CSS_POSITION_FIXED);
}

/* A line box (CSS 2.1 9.4.2) and the fragments laid out on it. */
struct webengine_line {
    int x;
    int y;
    int width;
    int height;
    /* The block's own baseline on the line. */
    int baseline;
    size_t first_fragment;
    size_t fragment_count;
};

enum webengine_fragment_type {
    /* A piece of an inline box on one line: its border box. */
    WEBENGINE_FRAGMENT_INLINE,
    /* A piece of a text box: the glyphs' box, ascent to descent. */
    WEBENGINE_FRAGMENT_TEXT,
    /* A list item's marker: the text's box, or the shape's for disc,
     * circle and square. */
    WEBENGINE_FRAGMENT_MARKER,
    /* An image: its border box. */
    WEBENGINE_FRAGMENT_IMAGE,
};

struct webengine_fragment {
    uint8_t type;
    /* INLINE: whether this piece draws the box's left and right edges. */
    bool left_edge;
    bool right_edge;
    const struct webengine_box *box;
    int x;
    int y;
    int width;
    int height;
    /* TEXT and MARKER: where the glyphs sit. */
    int baseline;
    /* TEXT and MARKER: the bytes of box->text to draw. */
    size_t start;
    size_t length;
};

/* What to paint, in order (CSS 2.1 Appendix E): a block's background and
 * borders, a block's line boxes, or an outside marker. */
enum webengine_paint_kind {
    WEBENGINE_PAINT_BLOCK,
    WEBENGINE_PAINT_LINES,
    WEBENGINE_PAINT_MARKER,
};

struct webengine_paint {
    uint8_t kind;
    /* Inside a position: fixed box: it stays put on the screen while the
     * page scrolls. */
    bool fixed;
    /* What overflow clips the item to. */
    struct webengine_clip clip;
    /* The block; for MARKER, the marker box, laid out in its x, y, width
     * and height. */
    const struct webengine_box *box;
};

struct webengine_tree {
    struct webengine_box *root;
    /* Set by webengine_layout: the width it laid out for, and the height of
     * everything, the root's margins and absolute boxes included. 0 before. */
    int layout_width;
    int height;
    struct webengine_line *lines;
    size_t line_count;
    size_t line_capacity;
    struct webengine_fragment *fragments;
    size_t fragment_count;
    size_t fragment_capacity;
    /* The canvas: the root's background, or body's when the root has none
     * (CSS 2.1 14.2); the box it came from does not paint it again. */
    css_color canvas;
    const struct webengine_box *canvas_source;
    /* The viewport's overflow (CSS_OVERFLOW_*): the root's, or body's when
     * the root's is visible (11.1.1). The box it came from does not clip. */
    uint8_t viewport_overflow;
    /* Set by webengine_layout: the painting order. */
    struct webengine_paint *paint;
    size_t paint_count;
    size_t paint_capacity;
    char **links;
    size_t link_count;
    struct webengine_image *images;
    size_t image_count;
    /* Owned by the tree. */
    struct webengine_box_style **styles;
    size_t style_count;
    size_t style_capacity;
};

/* The user-agent stylesheet (CSS 2.1 Appendix D and HTML's rendering
 * section), as text for webengine_stylesheet. */
extern const char webengine_ua_css[];
extern const size_t webengine_ua_css_length;

/* The boxes of `document`, styled with `styles`. NULL when out of memory or
 * the libraries fail. */
struct webengine_tree *webengine_tree_build(dom_document *document, struct webengine_styles *styles,
                                            const struct webengine_fonts *fonts);

/* A tree of plain text, for the source view and text/plain: one block whose
 * text keeps its line breaks and wraps at spaces (pre-wrap), black on white,
 * in the face for `font_size`, with 8 pixels of padding around it. */
struct webengine_tree *webengine_tree_text(const char *text, size_t length, int font_size,
                                           const struct webengine_fonts *fonts);

void webengine_tree_free(struct webengine_tree *tree);

/* Lays the tree out in a viewport `width` by `height` CSS pixels (CSS 2.1
 * chapter 10's normal flow, 9.5's floats and 9.6's positioning): every
 * block's position and size, the line boxes and fragments of inline
 * content, and the painting order. The height is the containing block of
 * fixed boxes and of absolute ones outside any positioned box. 0, or -1
 * when out of memory. Laying out again replaces the last layout. */
int webengine_layout(struct webengine_tree *tree, const struct webengine_fonts *fonts, int width, int height);

/* The tree as text, a line per box, indented two spaces a level, for the
 * tests: "block p", "anonymous", "inline b", "text \"...\"", "break",
 * "marker 1", "image 3" (its index in images), with "float left" or "float right" after a float's name and
 * "relative", "absolute" or "fixed" after a positioned box's. An anonymous
 * table part names its role: "anonymous table", "anonymous row",
 * "anonymous cell".
 * Once laid out, a block's line ends with its border box, "x y width
 * height", and a block with inline content lists its line boxes and their
 * fragments in place of its children, then the floats among them, a level
 * further in. `line` gets each one without its newline. */
typedef void (*webengine_dump_line)(void *user, const char *line);
void webengine_tree_dump(const struct webengine_tree *tree, webengine_dump_line line, void *user);
