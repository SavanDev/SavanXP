/* HTML parsing through libdom's hubbub binding. */

#include "webengine_html.h"

#include <dom/bindings/hubbub/parser.h>
#include <parserutils/charset/utf8.h>
#include <string.h>
#include <strings.h>

struct html_encoding {
    char name[64];
    dom_hubbub_encoding_source source;
};

/* One pass of the parser over the whole input. ENCODINGCHANGE when a <meta
 * charset> names another encoding than the one parsing started in; `found`
 * then holds it. */
static dom_hubbub_error parse_once(const uint8_t *data, size_t length, const char *charset,
                                   dom_document **document, struct html_encoding *found) {
    dom_hubbub_parser_params params = {
        .enc = charset,
        .fix_enc = charset != NULL,
        .enable_script = false,
    };
    dom_hubbub_parser *parser = NULL;
    *document = NULL;
    dom_hubbub_error error = dom_hubbub_parser_create(&params, &parser, document);
    if (error != DOM_HUBBUB_OK) {
        return error;
    }
    error = dom_hubbub_parser_parse_chunk(parser, data, length);
    if (error == DOM_HUBBUB_OK) {
        error = dom_hubbub_parser_completed(parser);
    }
    if (error == DOM_HUBBUB_OK || error == DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE) {
        const char *name = dom_hubbub_parser_get_encoding(parser, &found->source);
        size_t name_length = strlen(name);
        if (name_length >= sizeof found->name) {
            name_length = sizeof found->name - 1;
        }
        memmove(found->name, name, name_length);
        found->name[name_length] = '\0';
    }
    dom_hubbub_parser_destroy(parser);
    if (error != DOM_HUBBUB_OK) {
        dom_node_unref(*document);
        *document = NULL;
    }
    return error;
}

/* Keep the browser's UTF-8 display for unlabelled files while honouring
 * explicit encodings. ASCII is already identical in the fallback. */
static bool has_utf8_text(const uint8_t *data, size_t length) {
    bool non_ascii = false;

    for (size_t i = 0; i < length;) {
        uint32_t codepoint;
        size_t bytes;

        if (data[i] < 0x80) {
            ++i;
            continue;
        }
        if (parserutils_charset_utf8_to_ucs4(data + i, length - i, &codepoint, &bytes) != PARSERUTILS_OK ||
            bytes < 2 || codepoint < 0x80 || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        i += bytes;
        non_ascii = true;
    }
    return non_ascii;
}

dom_document *webengine_parse_html(const uint8_t *data, size_t length, const char *charset,
                                   char *encoding, size_t encoding_capacity) {
    struct html_encoding found;
    dom_document *document = NULL;
    dom_hubbub_error error = parse_once(data, length, charset, &document, &found);
    /* HTML's "change the encoding": start again, once, in the encoding the
     * <meta> named, and keep to it. */
    if (error == DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE && charset == NULL) {
        error = parse_once(data, length, found.name, &document, &found);
    } else if (error == DOM_HUBBUB_OK && charset == NULL && found.source == DOM_HUBBUB_ENCODING_SOURCE_DETECTED &&
               strcasecmp(found.name, "Windows-1252") == 0 &&
               (length < 3 || memcmp(data, "\xef\xbb\xbf", 3) != 0) && has_utf8_text(data, length)) {
        dom_node_unref(document);
        error = parse_once(data, length, "UTF-8", &document, &found);
    }
    if (error != DOM_HUBBUB_OK) {
        return NULL;
    }
    if (encoding != NULL && encoding_capacity > 0) {
        size_t found_length = strlen(found.name);
        if (found_length >= encoding_capacity) {
            found_length = encoding_capacity - 1;
        }
        memcpy(encoding, found.name, found_length);
        encoding[found_length] = '\0';
    }
    return document;
}
