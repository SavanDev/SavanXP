#pragma once

/* Web Viewer's engine glue over NetSurf's MIT libraries
 * (docs/desktop/WEBVIEWER_ENGINE.md): HTML through libhubbub into a libdom
 * document, CSS through libcss, and the selection handler that lets libcss
 * walk that document. Neutral: it asks only for the C library, so the same
 * source builds into SUN programs and the host tests.
 *
 * Written from the libraries' own headers and documentation; NetSurf's GPL
 * core is not its model (docs/roadmaps/open/WEBVIEWER_ROADMAP.md, "The
 * clean-room rule"). E1 uses it to prove the libraries; E2 and E3 build Web
 * Viewer's document and styles on it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "webengine_html.h"
#include <libcss/libcss.h>

/* The handler libcss calls back to read and walk a libdom document. The
 * selection call supplies storage for the element's presentational hints. */
extern css_select_handler webengine_select_handler;

/* Once before anything else, and once after the last document is gone.
 * webengine_init: 0, or -1 out of memory. */
int webengine_init(void);
void webengine_fini(void);

/* Parse author CSS from `url`, or a style="" attribute when `inline_style`.
 * Returns NULL on a library failure. Destroy the result with css_stylesheet_destroy. */
css_stylesheet *webengine_stylesheet(const char *url, const uint8_t *data, size_t length,
                                     bool inline_style);

/* A stylesheet declaration in DOM order. For <link>, href is the URL as
 * written. For <style>, href is NULL. Both are borrowed during the callback. */
typedef int (*webengine_sheet_visitor)(void *user, dom_element *element, const char *href);
int webengine_visit_sheets(dom_document *document, webengine_sheet_visitor visit, void *user);

/* A fetched <link> sheet. The callback can leave data NULL to skip a sheet.
 * Its bytes and final URL need to live only through webengine_styles_open. */
struct webengine_sheet_source {
    const uint8_t *data;
    size_t length;
    const char *url;
};
typedef struct webengine_sheet_source (*webengine_link_sheet)(void *user, dom_element *element);

/* Owns the author sheets, selection context and CSS units for one document.
 * The caller's UA sheet must outlive the context. */
struct webengine_styles {
    css_select_ctx *select;
    css_stylesheet **sheets;
    size_t sheet_count;
    size_t sheet_capacity;
    css_stylesheet *empty_inline;
    css_unit_ctx *units;
};

int webengine_styles_open(struct webengine_styles *styles, dom_document *document,
                          const char *url, css_stylesheet *ua, webengine_link_sheet link, void *user);
void webengine_styles_close(struct webengine_styles *styles);

/* Select an element and compose its inherited values with its parent's.
 * With no parent this sets the root style for rem. Destroy the returned
 * style when leaving the element; keep the root alive through the walk. */
css_computed_style *webengine_style_element(struct webengine_styles *styles, dom_element *element,
                                            const css_computed_style *parent);

/* Called once per element, in document order, with its fully computed style
 * (inherited values composed from its parent's), which the visitor must not
 * keep. Returns 0 to go on, anything else to stop the walk with that value. */
typedef int (*webengine_visit)(void *user, dom_element *element, const css_computed_style *style,
                               unsigned depth);

/* Compute the style of every element of `document`, the style="" of each
 * included. 0 when every element was styled, the visitor's value when it
 * stopped, -1 on a library error. */
int webengine_style_document(struct webengine_styles *styles, dom_document *document,
                             webengine_visit visit, void *user);
