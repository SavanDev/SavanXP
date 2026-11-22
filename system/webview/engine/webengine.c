/* Web Viewer's engine glue: parsing, stylesheets and the style walk
 * (webengine.h). Written from libdom's, libhubbub's and libcss' headers and
 * libcss' docs/API. */

#include "webengine.h"

#include <dom/bindings/hubbub/parser.h>
#include <stdlib.h>
#include <string.h>

int webengine_select_init(void);
void webengine_select_fini(void);

int webengine_init(void) {
    return webengine_select_init();
}

void webengine_fini(void) {
    webengine_select_fini();
}

/* --- Parsing ------------------------------------------------------------------ */

/* One pass of the parser over the whole input. ENCODINGCHANGE when a <meta
 * charset> names another encoding than the one parsing started in; `found`
 * then holds it. */
static dom_hubbub_error parse_once(const uint8_t *data, size_t length, const char *charset,
                                   dom_document **document, char *found, size_t found_capacity) {
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
    if ((error == DOM_HUBBUB_OK || error == DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE) && found != NULL &&
        found_capacity > 0) {
        dom_hubbub_encoding_source source;
        const char *name = dom_hubbub_parser_get_encoding(parser, &source);
        size_t name_length = name != NULL ? strlen(name) : 0;
        if (name_length >= found_capacity) {
            name_length = found_capacity - 1;
        }
        memcpy(found, name != NULL ? name : "", name_length);
        found[name_length] = '\0';
    }
    dom_hubbub_parser_destroy(parser);
    if (error != DOM_HUBBUB_OK) {
        dom_node_unref(*document);
        *document = NULL;
    }
    return error;
}

dom_document *webengine_parse_html(const uint8_t *data, size_t length, const char *charset,
                                   char *encoding, size_t encoding_capacity) {
    char found[64];
    dom_document *document = NULL;
    dom_hubbub_error error = parse_once(data, length, charset, &document, found, sizeof found);
    /* HTML's "change the encoding": start again, once, in the encoding the
     * <meta> named, and keep to it. */
    if (error == DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE && charset == NULL) {
        error = parse_once(data, length, found, &document, found, sizeof found);
    }
    if (error != DOM_HUBBUB_OK) {
        return NULL;
    }
    if (encoding != NULL && encoding_capacity > 0) {
        size_t found_length = strlen(found);
        if (found_length >= encoding_capacity) {
            found_length = encoding_capacity - 1;
        }
        memcpy(encoding, found, found_length);
        encoding[found_length] = '\0';
    }
    return document;
}

/* --- Stylesheets ----------------------------------------------------------- */

/* Relative URLs in a sheet stay as written until E3 resolves them against
 * the page with sxnhttp's resolution. */
static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs) {
    (void)pw;
    (void)base;
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

css_stylesheet *webengine_stylesheet(const char *url, const uint8_t *data, size_t length,
                                     bool inline_style) {
    css_stylesheet_params params = {
        .params_version = CSS_STYLESHEET_PARAMS_VERSION_1,
        .level = CSS_LEVEL_DEFAULT,
        .charset = "UTF-8",
        .url = url != NULL ? url : "",
        .allow_quirks = false,
        .inline_style = inline_style,
        .resolve = resolve_url,
    };
    css_stylesheet *sheet = NULL;
    if (css_stylesheet_create(&params, &sheet) != CSS_OK) {
        return NULL;
    }
    css_error error = css_stylesheet_append_data(sheet, data, length);
    if (error == CSS_OK || error == CSS_NEEDDATA) {
        error = css_stylesheet_data_done(sheet);
    }
    /* @import is not followed yet: the sheet stands without what it imports. */
    if (error != CSS_OK && error != CSS_IMPORTS_PENDING) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }
    return sheet;
}

/* --- Walking elements ---------------------------------------------------------- */

static bool is_element(dom_node *node) {
    dom_node_type type;
    return dom_node_get_node_type(node, &type) == DOM_NO_ERR && type == DOM_ELEMENT_NODE;
}

/* The first element child, or the next element sibling; borrowed. */
static dom_node *next_element(dom_node *node, bool child) {
    dom_node *current = NULL;
    dom_exception error = child ? dom_node_get_first_child(node, &current)
                                : dom_node_get_next_sibling(node, &current);
    while (error == DOM_NO_ERR && current != NULL && !is_element(current)) {
        dom_node *next = NULL;
        error = dom_node_get_next_sibling(current, &next);
        dom_node_unref(current);
        current = next;
    }
    if (current != NULL) {
        dom_node_unref(current);
    }
    return error == DOM_NO_ERR ? current : NULL;
}

/* The next element in document order after `node`'s subtree, within `root`;
 * borrowed. */
static dom_node *after_subtree(dom_node *node, dom_node *root) {
    while (node != NULL && node != root) {
        dom_node *sibling = next_element(node, false);
        if (sibling != NULL) {
            return sibling;
        }
        dom_node *parent = NULL;
        if (dom_node_get_parent_node(node, &parent) != DOM_NO_ERR || parent == NULL) {
            return NULL;
        }
        dom_node_unref(parent);
        node = parent;
    }
    return NULL;
}

/* Whether the element is called `name`, written in lower case. */
static bool named(dom_node *node, const char *name) {
    dom_string *local = NULL;
    if (dom_node_get_local_name(node, &local) != DOM_NO_ERR || local == NULL) {
        return false;
    }
    /* libdom keeps HTML element names in upper case. */
    size_t length = strlen(name);
    bool match = dom_string_byte_length(local) == length;
    for (size_t i = 0; match && i < length; i++) {
        char c = dom_string_data(local)[i];
        match = (c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) == name[i];
    }
    dom_string_unref(local);
    return match;
}

/* --- Styles ----------------------------------------------------------------- */

int webengine_styles_open(struct webengine_styles *styles, dom_document *document,
                          const char *url, css_stylesheet *ua) {
    memset(styles, 0, sizeof *styles);
    /* Until E4 has a viewport: 800x600 at 96 dpi, 16px text. */
    const css_unit_ctx units = {
        .viewport_width = INTTOFIX(800),
        .viewport_height = INTTOFIX(600),
        .font_size_default = INTTOFIX(16),
        .font_size_minimum = INTTOFIX(6),
        .device_dpi = INTTOFIX(96),
    };
    styles->units = malloc(sizeof *styles->units);
    if (styles->units == NULL) {
        return -1;
    }
    memcpy(styles->units, &units, sizeof units);
    if (css_select_ctx_create(&styles->select) != CSS_OK) {
        webengine_styles_close(styles);
        return -1;
    }
    if (ua != NULL && css_select_ctx_append_sheet(styles->select, ua, CSS_ORIGIN_UA, NULL) != CSS_OK) {
        webengine_styles_close(styles);
        return -1;
    }
    dom_element *root = NULL;
    if (dom_document_get_document_element(document, &root) != DOM_NO_ERR || root == NULL) {
        return 0;
    }
    int result = 0;
    for (dom_node *node = (dom_node *)root; node != NULL && result == 0;) {
        if (!named(node, "style")) {
            dom_node *child = next_element(node, true);
            node = child != NULL ? child : after_subtree(node, (dom_node *)root);
            continue;
        }
        dom_string *text = NULL;
        if (dom_node_get_text_content(node, &text) != DOM_NO_ERR) {
            result = -1;
            break;
        }
        if (styles->sheet_count == WEBENGINE_MAX_SHEETS) {
            result = -1;
        } else {
            css_stylesheet *sheet = webengine_stylesheet(
                url, text != NULL ? (const uint8_t *)dom_string_data(text) : (const uint8_t *)"",
                text != NULL ? dom_string_byte_length(text) : 0, false);
            if (sheet == NULL || css_select_ctx_append_sheet(styles->select, sheet, CSS_ORIGIN_AUTHOR,
                                                             NULL) != CSS_OK) {
                if (sheet != NULL) {
                    css_stylesheet_destroy(sheet);
                }
                result = -1;
            } else {
                styles->sheets[styles->sheet_count++] = sheet;
            }
        }
        if (text != NULL) {
            dom_string_unref(text);
        }
        node = after_subtree(node, (dom_node *)root);
    }
    dom_node_unref(root);
    if (result != 0) {
        webengine_styles_close(styles);
    }
    return result;
}

void webengine_styles_close(struct webengine_styles *styles) {
    if (styles->select != NULL) {
        css_select_ctx_destroy(styles->select);
    }
    for (unsigned i = 0; i < styles->sheet_count; i++) {
        css_stylesheet_destroy(styles->sheets[i]);
    }
    free(styles->units);
    memset(styles, 0, sizeof *styles);
}

/* One open element of the walk: its selection results and its composed
 * style (the root's is the result's own, already complete). */
struct frame {
    dom_node *node;
    css_select_results *results;
    css_computed_style *composed;
};

static void frame_release(struct frame *frame) {
    if (frame->composed != NULL && frame->composed != frame->results->styles[CSS_PSEUDO_ELEMENT_NONE]) {
        css_computed_style_destroy(frame->composed);
    }
    if (frame->results != NULL) {
        css_select_results_destroy(frame->results);
    }
}

static int select_element(struct webengine_styles *styles, dom_node *node,
                          const css_computed_style *parent, struct frame *frame) {
    static const css_media media = {.type = CSS_MEDIA_SCREEN};
    css_stylesheet *inline_sheet = NULL;
    dom_string *key = NULL;
    dom_string *value = NULL;
    if (dom_string_create_interned((const uint8_t *)"style", 5, &key) != DOM_NO_ERR) {
        return -1;
    }
    if (dom_element_get_attribute(node, key, &value) == DOM_NO_ERR && value != NULL) {
        inline_sheet = webengine_stylesheet("", (const uint8_t *)dom_string_data(value),
                                            dom_string_byte_length(value), true);
        dom_string_unref(value);
    }
    dom_string_unref(key);

    frame->node = node;
    frame->results = NULL;
    frame->composed = NULL;
    css_error error = css_select_style(styles->select, node, styles->units, &media, inline_sheet,
                                       &webengine_select_handler, NULL, &frame->results);
    if (inline_sheet != NULL) {
        css_stylesheet_destroy(inline_sheet);
    }
    if (error != CSS_OK || frame->results == NULL ||
        frame->results->styles[CSS_PSEUDO_ELEMENT_NONE] == NULL) {
        if (frame->results != NULL) {
            css_select_results_destroy(frame->results);
            frame->results = NULL;
        }
        return -1;
    }
    css_computed_style *own = frame->results->styles[CSS_PSEUDO_ELEMENT_NONE];
    if (parent == NULL) {
        frame->composed = own;
        return 0;
    }
    if (css_computed_style_compose(parent, own, styles->units, &frame->composed) != CSS_OK) {
        css_select_results_destroy(frame->results);
        frame->results = NULL;
        frame->composed = NULL;
        return -1;
    }
    return 0;
}

int webengine_style_document(struct webengine_styles *styles, dom_document *document,
                             webengine_visit visit, void *user) {
    dom_element *root = NULL;
    if (dom_document_get_document_element(document, &root) != DOM_NO_ERR) {
        return -1;
    }
    if (root == NULL) {
        return 0;
    }
    struct frame *stack = NULL;
    size_t depth = 0;
    size_t capacity = 0;
    int result = 0;
    dom_node *node = (dom_node *)root;
    while (node != NULL) {
        if (depth == capacity) {
            size_t grown = capacity == 0 ? 32 : capacity * 2;
            struct frame *bigger = realloc(stack, grown * sizeof *stack);
            if (bigger == NULL) {
                result = -1;
                break;
            }
            stack = bigger;
            capacity = grown;
        }
        const css_computed_style *parent = depth > 0 ? stack[depth - 1].composed : NULL;
        if (select_element(styles, node, parent, &stack[depth]) != 0) {
            result = -1;
            break;
        }
        if (depth == 0) {
            styles->units->root_style = stack[0].composed;
        }
        result = visit(user, (dom_element *)node, stack[depth].composed, (unsigned)depth);
        depth++;
        if (result != 0) {
            break;
        }
        dom_node *child = next_element(node, true);
        if (child != NULL) {
            node = child;
            continue;
        }
        /* Leave finished elements until one has a next sibling. */
        node = NULL;
        while (depth > 0 && node == NULL) {
            struct frame *top = &stack[--depth];
            node = next_element(top->node, false);
            frame_release(top);
        }
    }
    while (depth > 0) {
        frame_release(&stack[--depth]);
    }
    styles->units->root_style = NULL;
    free(stack);
    dom_node_unref(root);
    return result;
}
