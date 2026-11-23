#pragma once

#include <stddef.h>
#include <stdint.h>

#include <dom/dom.h>

/* Parse a whole document. `charset` is the transport's (Content-Type), or
 * NULL to let hubbub detect it; the encoding it settled on is copied into
 * `encoding` when that is not NULL. The caller owns the document:
 * dom_node_unref it. NULL out of memory or on a parser error. */
dom_document *webengine_parse_html(const uint8_t *data, size_t length, const char *charset,
                                   char *encoding, size_t encoding_capacity);
