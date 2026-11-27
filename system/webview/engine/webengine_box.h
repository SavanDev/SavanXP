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
    /* Block-level: an element's block, list item or table part (tables
     * stack until E6), or an anonymous block around inline content. */
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
    /* TEXT and MARKER: UTF-8, '\n' only where white-space preserves it. */
    char *text;
    size_t length;
    /* The link the box is in (webengine_tree.links), or -1. */
    int link;
    /* MARKER: the list item's ordinal. */
    int ordinal;
    /* TEXT: an image's placeholder, until E6. */
    bool replaced;
    /* BLOCK: its children are inline-level (an inline formatting context). */
    bool inline_content;
    /* INLINE: which pieces of a split inline this is; only the first draws
     * its left edge and only the last its right one. */
    bool first_piece;
    bool last_piece;
    /* BLOCK of a list item with an outside marker. */
    struct webengine_box *marker;

    /* Set by webengine_layout. BLOCK: the border box in canvas pixels and
     * the used widths of its edges. */
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
};

/* A line box (CSS 2.1 9.4.2) and the fragments laid out on it. */
struct webengine_line {
    int x;
    int y;
    int width;
    int height;
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

struct webengine_tree {
    struct webengine_box *root;
    /* Set by webengine_layout: the width it laid out for, and the height of
     * everything, the root's margins included. 0 before. */
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
    char **links;
    size_t link_count;
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
 * in the face for `font_size`. */
struct webengine_tree *webengine_tree_text(const char *text, size_t length, int font_size,
                                           const struct webengine_fonts *fonts);

void webengine_tree_free(struct webengine_tree *tree);

/* Lays the tree out in a viewport `width` CSS pixels wide (CSS 2.1 chapter
 * 10's normal flow): every block's position and size, and the line boxes
 * and fragments of inline content. 0, or -1 when out of memory. Laying out
 * again at another width replaces the last layout. */
int webengine_layout(struct webengine_tree *tree, const struct webengine_fonts *fonts, int width);

/* The tree as text, a line per box, indented two spaces a level, for the
 * tests: "block p", "anonymous", "inline b", "text \"...\"", "break",
 * "marker 1". Once laid out, a block's line ends with its border box,
 * "x y width height", and a block with inline content lists its line boxes
 * and their fragments in place of its children. `line` gets each one
 * without its newline. */
typedef void (*webengine_dump_line)(void *user, const char *line);
void webengine_tree_dump(const struct webengine_tree *tree, webengine_dump_line line, void *user);
