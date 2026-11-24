/* Web Viewer's engine glue: stylesheets and the style walk
 * (webengine.h). Written from libdom's, libhubbub's and libcss' headers and
 * libcss' docs/API. */

#include "webengine.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

int webengine_select_init(void);
void webengine_select_fini(void);

int webengine_init(void) {
    return webengine_select_init();
}

void webengine_fini(void) {
    webengine_select_fini();
}

/* --- Stylesheets ----------------------------------------------------------- */

/* URL-valued properties are selected but do not fetch resources in E3. */
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

static dom_string *sheet_attribute(dom_element *node, const char *name) {
    dom_string *key = NULL;
    dom_string *value = NULL;
    if (dom_string_create_interned((const uint8_t *)name, strlen(name), &key) == DOM_NO_ERR) {
        (void)dom_element_get_attribute(node, key, &value);
        dom_string_unref(key);
    }
    return value;
}

static bool rel_token(const char *rel, const char *token) {
    const char *space = " \t\n\r\f";
    size_t length = strlen(token);
    while (*rel != '\0') {
        rel += strspn(rel, space);
        size_t word = strcspn(rel, space);
        if (word == length && strncasecmp(rel, token, length) == 0) {
            return true;
        }
        rel += word;
    }
    return false;
}

int webengine_visit_sheets(dom_document *document, webengine_sheet_visitor visit, void *user) {
    dom_element *root = NULL;
    if (dom_document_get_document_element(document, &root) != DOM_NO_ERR) {
        return -1;
    }
    int result = 0;
    for (dom_node *node = (dom_node *)root; node != NULL && result == 0;) {
        if (!named(node, "template") && !named(node, "style") && !named(node, "link")) {
            dom_node *child = next_element(node, true);
            node = child != NULL ? child : after_subtree(node, (dom_node *)root);
            continue;
        }
        dom_element *element = (dom_element *)node;
        dom_string *type = sheet_attribute(element, "type");
        bool css = type == NULL || dom_string_byte_length(type) == 0 ||
            strcasecmp(dom_string_data(type), "text/css") == 0;
        dom_string_unref(type);
        if (css && named(node, "style")) {
            result = visit(user, element, NULL);
        } else if (css && named(node, "link")) {
            dom_string *rel = sheet_attribute(element, "rel");
            if (rel != NULL && rel_token(dom_string_data(rel), "stylesheet") &&
                !rel_token(dom_string_data(rel), "alternate")) {
                dom_string *href = sheet_attribute(element, "href");
                if (href != NULL && dom_string_byte_length(href) != 0) {
                    result = visit(user, element, dom_string_data(href));
                }
                dom_string_unref(href);
            }
            dom_string_unref(rel);
        }
        node = after_subtree(node, (dom_node *)root);
    }
    dom_node_unref(root);
    return result;
}

struct sheet_collection {
    struct webengine_styles *styles;
    const char *url;
    webengine_link_sheet link;
    void *user;
};

static int collect_sheet(void *user, dom_element *element, const char *href) {
    struct sheet_collection *collection = user;
    struct webengine_styles *styles = collection->styles;
    dom_string *text = NULL;
    struct webengine_sheet_source source = { .url = collection->url };
    if (href == NULL) {
        if (dom_node_get_text_content(element, &text) != DOM_NO_ERR) {
            return -1;
        }
        if (text != NULL) {
            source.data = (const uint8_t *)dom_string_data(text);
            source.length = dom_string_byte_length(text);
        }
    } else if (collection->link != NULL) {
        source = collection->link(collection->user, element);
    }
    if (source.length == 0 || source.data == NULL) {
        dom_string_unref(text);
        return 0;
    }
    css_stylesheet *sheet = webengine_stylesheet(source.url, source.data, source.length, false);
    dom_string_unref(text);
    if (sheet == NULL) {
        return -1;
    }
    if (styles->sheet_count == styles->sheet_capacity) {
        size_t capacity = styles->sheet_capacity == 0 ? 8 : styles->sheet_capacity * 2;
        css_stylesheet **sheets = realloc(styles->sheets, capacity * sizeof(*sheets));
        if (sheets == NULL) {
            css_stylesheet_destroy(sheet);
            return -1;
        }
        styles->sheets = sheets;
        styles->sheet_capacity = capacity;
    }
    dom_string *media = sheet_attribute(element, "media");
    css_error error = css_select_ctx_append_sheet(styles->select, sheet, CSS_ORIGIN_AUTHOR,
        media != NULL ? dom_string_data(media) : NULL);
    dom_string_unref(media);
    if (error != CSS_OK) {
        css_stylesheet_destroy(sheet);
        return error == CSS_NOMEM ? -1 : 0;
    }
    styles->sheets[styles->sheet_count++] = sheet;
    return 0;
}

int webengine_styles_open(struct webengine_styles *styles, dom_document *document,
                          const char *url, css_stylesheet *ua, webengine_link_sheet link, void *user) {
    memset(styles, 0, sizeof *styles);
    /* Until E4 has a layout viewport: 800x600 CSS pixels at 96 dpi. */
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
    /* libcss checks every preceding candidate for sharing when a sibling
     * selector taints a node. Supplying a reusable, declaration-free inline
     * sheet opts out of that search without affecting selector matching. */
    styles->empty_inline = webengine_stylesheet("", (const uint8_t *)" ", 1, true);
    struct sheet_collection collection = { styles, url, link, user };
    if (styles->empty_inline == NULL || css_select_ctx_create(&styles->select) != CSS_OK ||
        (ua != NULL && css_select_ctx_append_sheet(styles->select, ua, CSS_ORIGIN_UA, NULL) != CSS_OK) ||
        webengine_visit_sheets(document, collect_sheet, &collection) != 0) {
        webengine_styles_close(styles);
        return -1;
    }
    return 0;
}

void webengine_styles_close(struct webengine_styles *styles) {
    if (styles->select != NULL) {
        css_select_ctx_destroy(styles->select);
    }
    for (size_t i = 0; i < styles->sheet_count; i++) {
        css_stylesheet_destroy(styles->sheets[i]);
    }
    if (styles->empty_inline != NULL) {
        css_stylesheet_destroy(styles->empty_inline);
    }
    free(styles->sheets);
    free(styles->units);
    memset(styles, 0, sizeof *styles);
}

css_computed_style *webengine_style_element(struct webengine_styles *styles, dom_element *element,
                                            const css_computed_style *parent) {
    const css_media media = {
        .type = CSS_MEDIA_SCREEN,
        .width = styles->units->viewport_width,
        .height = styles->units->viewport_height,
    };
    css_stylesheet *inline_sheet = NULL;
    dom_string *value = sheet_attribute(element, "style");
    if (value != NULL && dom_string_byte_length(value) != 0) {
        inline_sheet = webengine_stylesheet("", (const uint8_t *)dom_string_data(value),
                                            dom_string_byte_length(value), true);
    }
    dom_string_unref(value);

    css_hint hints[2];
    css_select_results *results = NULL;
    css_error error = css_select_style(styles->select, element, styles->units, &media,
        inline_sheet != NULL ? inline_sheet : styles->empty_inline,
        &webengine_select_handler, hints, &results);
    if (inline_sheet != NULL) {
        css_stylesheet_destroy(inline_sheet);
    }
    if (error != CSS_OK || results == NULL) {
        return NULL;
    }
    css_computed_style *own = results->styles[CSS_PSEUDO_ELEMENT_NONE];
    css_computed_style *style = NULL;
    if (own != NULL && parent == NULL) {
        style = own;
        results->styles[CSS_PSEUDO_ELEMENT_NONE] = NULL;
        styles->units->root_style = style;
    } else if (own != NULL &&
               css_computed_style_compose(parent, own, styles->units, &style) != CSS_OK) {
        style = NULL;
    }
    css_select_results_destroy(results);
    return style;
}

struct frame {
    dom_node *node;
    css_computed_style *style;
};

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
        const css_computed_style *parent = depth > 0 ? stack[depth - 1].style : NULL;
        css_computed_style *style = webengine_style_element(styles, (dom_element *)node, parent);
        if (style == NULL) {
            result = -1;
            break;
        }
        stack[depth] = (struct frame){ node, style };
        result = visit(user, (dom_element *)node, style, (unsigned)depth);
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
            css_computed_style_destroy(top->style);
        }
    }
    while (depth > 0) {
        css_computed_style_destroy(stack[--depth].style);
    }
    styles->units->root_style = NULL;
    free(stack);
    dom_node_unref(root);
    return result;
}
