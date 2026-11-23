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

/* The handler libcss calls back to read and walk a libdom document. Its
 * private pointer (pw) is unused: pass NULL. */
extern css_select_handler webengine_select_handler;

/* Once before anything else, and once after the last document is gone.
 * webengine_init: 0, or -1 out of memory. */
int webengine_init(void);
void webengine_fini(void);

/* One stylesheet from CSS text: an author sheet (`url` resolves its relative
 * URLs), or the body of a style="" attribute when `inline_style`. NULL out of
 * memory. css_stylesheet_destroy it. */
css_stylesheet *webengine_stylesheet(const char *url, const uint8_t *data, size_t length,
                                     bool inline_style);

/* The styles of one document: its <style> elements as author sheets, in
 * document order, on top of the `ua` sheet when there is one. */
#define WEBENGINE_MAX_SHEETS 32
struct webengine_styles {
    css_select_ctx *select;
    css_stylesheet *sheets[WEBENGINE_MAX_SHEETS];
    unsigned sheet_count;
    /* Allocated: libcss' unit context has a const member, so it cannot be
     * assigned in place. */
    css_unit_ctx *units;
};

/* 0, or -1 out of memory or past WEBENGINE_MAX_SHEETS. `ua` stays the
 * caller's and must outlive the styles. */
int webengine_styles_open(struct webengine_styles *styles, dom_document *document,
                          const char *url, css_stylesheet *ua);
void webengine_styles_close(struct webengine_styles *styles);

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
