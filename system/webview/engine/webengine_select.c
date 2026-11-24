/* libcss' selection handler over a libdom document (webengine.h).
 *
 * Written from libcss' include/libcss/select.h and docs/API and libdom's
 * headers. Nodes are libdom nodes; libcss only ever sees elements. Every
 * node returned to libcss is borrowed, not referenced: the document does not
 * change while a style is selected, and the tree keeps the node alive.
 *
 * Selection uses screen styles and the static state of the document. */

#include "webengine.h"

#include <string.h>

/* libcss keeps per-node data of its own (sharing and bloom filters); it lives
 * in the node's user data under this key. */
static dom_string *g_node_data_key;

/* --- Small helpers ------------------------------------------------------- */

static bool is_element(dom_node *node) {
    dom_node_type type;
    return node != NULL && dom_node_get_node_type(node, &type) == DOM_NO_ERR &&
           type == DOM_ELEMENT_NODE;
}

/* The nearest element before (or after) `node` among its siblings, borrowed. */
static dom_node *element_sibling(dom_node *node, bool after) {
    dom_node *current = NULL;
    dom_exception error = after ? dom_node_get_next_sibling(node, &current)
                                : dom_node_get_previous_sibling(node, &current);
    while (error == DOM_NO_ERR && current != NULL && !is_element(current)) {
        dom_node *next = NULL;
        error = after ? dom_node_get_next_sibling(current, &next)
                      : dom_node_get_previous_sibling(current, &next);
        dom_node_unref(current);
        current = next;
    }
    if (current != NULL) {
        dom_node_unref(current);
    }
    return error == DOM_NO_ERR ? current : NULL;
}

/* The parent when it is an element (not the document), borrowed. */
static dom_node *element_parent(dom_node *node) {
    dom_node *parent = NULL;
    if (dom_node_get_parent_node(node, &parent) != DOM_NO_ERR || parent == NULL) {
        return NULL;
    }
    dom_node_unref(parent);
    return is_element(parent) ? parent : NULL;
}

/* Whether the element's local name is `name`, ignoring ASCII case, as HTML
 * element names are matched (libdom keeps them in upper case). */
static bool has_name(dom_node *node, lwc_string *name) {
    dom_string *local = NULL;
    if (dom_node_get_local_name(node, &local) != DOM_NO_ERR || local == NULL) {
        return false;
    }
    bool match = dom_string_caseless_lwc_isequal(local, name);
    dom_string_unref(local);
    return match;
}

static bool has_name_text(dom_node *node, const char *name) {
    dom_string *local = NULL;
    if (dom_node_get_local_name(node, &local) != DOM_NO_ERR || local == NULL) {
        return false;
    }
    size_t length = strlen(name);
    bool match = dom_string_byte_length(local) == length;
    for (size_t i = 0; match && i < length; i++) {
        char c = dom_string_data(local)[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        match = c == name[i];
    }
    dom_string_unref(local);
    return match;
}

/* The value of attribute `name`, referenced, or NULL when absent. */
static dom_string *attribute(dom_node *node, const char *name, size_t length) {
    dom_string *key = NULL;
    dom_string *value = NULL;
    if (dom_string_create_interned((const uint8_t *)name, length, &key) != DOM_NO_ERR) {
        return NULL;
    }
    if (dom_element_get_attribute(node, key, &value) != DOM_NO_ERR) {
        value = NULL;
    }
    dom_string_unref(key);
    return value;
}

static dom_string *attribute_lwc(dom_node *node, lwc_string *name) {
    return attribute(node, lwc_string_data(name), lwc_string_length(name));
}

static bool has_attribute_text(dom_node *node, const char *name) {
    dom_string *value = attribute(node, name, strlen(name));
    if (value == NULL) {
        return false;
    }
    dom_string_unref(value);
    return true;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool bytes_equal(const char *a, const char *b, size_t length, bool caseless) {
    for (size_t i = 0; i < length; i++) {
        if (caseless ? lower(a[i]) != lower(b[i]) : a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

enum attribute_test { TEST_EQUAL, TEST_DASH, TEST_INCLUDES, TEST_PREFIX, TEST_SUFFIX, TEST_SUBSTRING };

/* Selectors level 3, section 6.3: [a=v], [a|=v], [a~=v], [a^=v], [a$=v] and
 * [a*=v]. Values compare case-sensitively; an empty v never matches the last
 * four. */
static css_error attribute_test(dom_node *node, const css_qname *qname, lwc_string *wanted,
                                enum attribute_test test, bool *match) {
    *match = false;
    dom_string *value = attribute_lwc(node, qname->name);
    if (value == NULL) {
        return CSS_OK;
    }
    const char *have = dom_string_data(value);
    size_t have_length = dom_string_byte_length(value);
    const char *want = lwc_string_data(wanted);
    size_t want_length = lwc_string_length(wanted);
    switch (test) {
    case TEST_EQUAL:
        *match = have_length == want_length && bytes_equal(have, want, want_length, false);
        break;
    case TEST_DASH:
        *match = (have_length == want_length || (have_length > want_length && have[want_length] == '-')) &&
                 bytes_equal(have, want, want_length, false);
        break;
    case TEST_INCLUDES:
        for (size_t start = 0; want_length > 0 && start < have_length && !*match;) {
            while (start < have_length && is_space(have[start])) {
                start++;
            }
            size_t end = start;
            while (end < have_length && !is_space(have[end])) {
                end++;
            }
            *match = end - start == want_length && bytes_equal(have + start, want, want_length, false);
            start = end;
        }
        break;
    case TEST_PREFIX:
        *match = want_length > 0 && have_length >= want_length && bytes_equal(have, want, want_length, false);
        break;
    case TEST_SUFFIX:
        *match = want_length > 0 && have_length >= want_length &&
                 bytes_equal(have + have_length - want_length, want, want_length, false);
        break;
    case TEST_SUBSTRING:
        for (size_t at = 0; want_length > 0 && at + want_length <= have_length && !*match; at++) {
            *match = bytes_equal(have + at, want, want_length, false);
        }
        break;
    }
    dom_string_unref(value);
    return CSS_OK;
}

/* --- The handler ------------------------------------------------------------ */

static css_error node_name(void *pw, void *node, css_qname *qname) {
    (void)pw;
    dom_string *local = NULL;
    dom_string *ns = NULL;
    qname->ns = NULL;
    qname->name = NULL;
    if (dom_node_get_local_name(node, &local) != DOM_NO_ERR || local == NULL) {
        return CSS_NOMEM;
    }
    /* libdom keeps HTML element names in upper case; CSS writes them in
     * lower case, and matches them without regard to case. */
    dom_string *lowered = NULL;
    dom_exception error = dom_string_tolower(local, true, &lowered);
    dom_string_unref(local);
    if (error != DOM_NO_ERR) {
        return CSS_NOMEM;
    }
    error = dom_string_intern(lowered, &qname->name);
    dom_string_unref(lowered);
    if (error != DOM_NO_ERR) {
        return CSS_NOMEM;
    }
    if (dom_node_get_namespace(node, &ns) == DOM_NO_ERR && ns != NULL) {
        error = dom_string_intern(ns, &qname->ns);
        dom_string_unref(ns);
        if (error != DOM_NO_ERR) {
            lwc_string_unref(qname->name);
            qname->name = NULL;
            return CSS_NOMEM;
        }
    }
    return CSS_OK;
}

static css_error node_classes(void *pw, void *node, lwc_string ***classes, uint32_t *n_classes) {
    (void)pw;
    /* libdom keeps the array and gives a reference to each name, which
     * libcss releases. */
    return dom_element_get_classes(node, classes, n_classes) == DOM_NO_ERR ? CSS_OK : CSS_NOMEM;
}

static css_error node_id(void *pw, void *node, lwc_string **id) {
    (void)pw;
    *id = NULL;
    dom_string *value = attribute(node, "id", 2);
    if (value == NULL) {
        return CSS_OK;
    }
    dom_exception error = dom_string_intern(value, id);
    dom_string_unref(value);
    return error == DOM_NO_ERR ? CSS_OK : CSS_NOMEM;
}

/* libdom's element.h says these two helpers return the node without a
 * reference; their implementation (src/core/element.c) takes one, and says
 * the caller releases it. The implementation is what runs: the reference is
 * dropped here, and the node handed on borrowed like every other. */
static void *borrow(dom_element *element) {
    if (element != NULL) {
        dom_node_unref(element);
    }
    return element;
}

static css_error named_ancestor_node(void *pw, void *node, const css_qname *qname, void **ancestor) {
    (void)pw;
    dom_element *found = NULL;
    dom_element_named_ancestor_node(node, qname->name, &found);
    *ancestor = borrow(found);
    return CSS_OK;
}

static css_error named_parent_node(void *pw, void *node, const css_qname *qname, void **parent) {
    (void)pw;
    dom_element *found = NULL;
    dom_element_named_parent_node(node, qname->name, &found);
    *parent = borrow(found);
    return CSS_OK;
}

static css_error named_sibling_node(void *pw, void *node, const css_qname *qname, void **sibling) {
    (void)pw;
    dom_node *previous = element_sibling(node, false);
    *sibling = previous != NULL && has_name(previous, qname->name) ? previous : NULL;
    return CSS_OK;
}

static css_error named_generic_sibling_node(void *pw, void *node, const css_qname *qname,
                                            void **sibling) {
    (void)pw;
    *sibling = NULL;
    for (dom_node *previous = element_sibling(node, false); previous != NULL;
         previous = element_sibling(previous, false)) {
        if (has_name(previous, qname->name)) {
            *sibling = previous;
            break;
        }
    }
    return CSS_OK;
}

static css_error parent_node(void *pw, void *node, void **parent) {
    (void)pw;
    *parent = element_parent(node);
    return CSS_OK;
}

static css_error sibling_node(void *pw, void *node, void **sibling) {
    (void)pw;
    *sibling = element_sibling(node, false);
    return CSS_OK;
}

static css_error node_has_name(void *pw, void *node, const css_qname *qname, bool *match) {
    (void)pw;
    *match = lwc_string_length(qname->name) == 1 && lwc_string_data(qname->name)[0] == '*'
                 ? true
                 : has_name(node, qname->name);
    return CSS_OK;
}

static css_error node_has_class(void *pw, void *node, lwc_string *name, bool *match) {
    (void)pw;
    return dom_element_has_class(node, name, match) == DOM_NO_ERR ? CSS_OK : CSS_NOMEM;
}

static css_error node_has_id(void *pw, void *node, lwc_string *name, bool *match) {
    (void)pw;
    *match = false;
    dom_string *value = attribute(node, "id", 2);
    if (value != NULL) {
        *match = dom_string_lwc_isequal(value, name);
        dom_string_unref(value);
    }
    return CSS_OK;
}

static css_error node_has_attribute(void *pw, void *node, const css_qname *qname, bool *match) {
    (void)pw;
    dom_string *value = attribute_lwc(node, qname->name);
    *match = value != NULL;
    if (value != NULL) {
        dom_string_unref(value);
    }
    return CSS_OK;
}

static css_error node_has_attribute_equal(void *pw, void *node, const css_qname *qname,
                                          lwc_string *value, bool *match) {
    (void)pw;
    return attribute_test(node, qname, value, TEST_EQUAL, match);
}

static css_error node_has_attribute_dashmatch(void *pw, void *node, const css_qname *qname,
                                              lwc_string *value, bool *match) {
    (void)pw;
    return attribute_test(node, qname, value, TEST_DASH, match);
}

static css_error node_has_attribute_includes(void *pw, void *node, const css_qname *qname,
                                             lwc_string *value, bool *match) {
    (void)pw;
    return attribute_test(node, qname, value, TEST_INCLUDES, match);
}

static css_error node_has_attribute_prefix(void *pw, void *node, const css_qname *qname,
                                           lwc_string *value, bool *match) {
    (void)pw;
    return attribute_test(node, qname, value, TEST_PREFIX, match);
}

static css_error node_has_attribute_suffix(void *pw, void *node, const css_qname *qname,
                                           lwc_string *value, bool *match) {
    (void)pw;
    return attribute_test(node, qname, value, TEST_SUFFIX, match);
}

static css_error node_has_attribute_substring(void *pw, void *node, const css_qname *qname,
                                              lwc_string *value, bool *match) {
    (void)pw;
    return attribute_test(node, qname, value, TEST_SUBSTRING, match);
}

static css_error node_is_root(void *pw, void *node, bool *match) {
    (void)pw;
    *match = element_parent(node) == NULL;
    return CSS_OK;
}

static css_error node_count_siblings(void *pw, void *node, bool same_name, bool after,
                                     int32_t *count) {
    (void)pw;
    *count = 0;
    dom_string *local = NULL;
    lwc_string *name = NULL;
    if (same_name) {
        if (dom_node_get_local_name(node, &local) != DOM_NO_ERR || local == NULL) {
            return CSS_NOMEM;
        }
        dom_exception error = dom_string_intern(local, &name);
        dom_string_unref(local);
        if (error != DOM_NO_ERR) {
            return CSS_NOMEM;
        }
    }
    for (dom_node *sibling = element_sibling(node, after); sibling != NULL;
         sibling = element_sibling(sibling, after)) {
        if (!same_name || has_name(sibling, name)) {
            (*count)++;
        }
    }
    if (name != NULL) {
        lwc_string_unref(name);
    }
    return CSS_OK;
}

/* Selectors 3: no element children and no text, not even white space. */
static css_error node_is_empty(void *pw, void *node, bool *match) {
    (void)pw;
    *match = true;
    dom_node *child = NULL;
    dom_exception error = dom_node_get_first_child(node, &child);
    while (error == DOM_NO_ERR && child != NULL) {
        dom_node_type type;
        if (dom_node_get_node_type(child, &type) == DOM_NO_ERR &&
            (type == DOM_ELEMENT_NODE || type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE ||
             type == DOM_ENTITY_REFERENCE_NODE)) {
            *match = false;
            dom_node_unref(child);
            break;
        }
        dom_node *next = NULL;
        error = dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }
    return CSS_OK;
}

/* HTML's links: a, area and link with an href. */
static css_error node_is_link(void *pw, void *node, bool *match) {
    (void)pw;
    *match = (has_name_text(node, "a") || has_name_text(node, "area") || has_name_text(node, "link")) &&
             has_attribute_text(node, "href");
    return CSS_OK;
}

/* No history, no pointer and no focus reach the engine yet. */
static css_error node_is_false(void *pw, void *node, bool *match) {
    (void)pw;
    (void)node;
    *match = false;
    return CSS_OK;
}

static bool is_form_control(dom_node *node) {
    static const char *const names[] = {"button", "input", "select", "textarea", "option",
                                        "optgroup", "fieldset"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (has_name_text(node, names[i])) {
            return true;
        }
    }
    return false;
}

static css_error node_is_enabled(void *pw, void *node, bool *match) {
    (void)pw;
    *match = is_form_control(node) && !has_attribute_text(node, "disabled");
    return CSS_OK;
}

static css_error node_is_disabled(void *pw, void *node, bool *match) {
    (void)pw;
    *match = is_form_control(node) && has_attribute_text(node, "disabled");
    return CSS_OK;
}

static css_error node_is_checked(void *pw, void *node, bool *match) {
    (void)pw;
    *match = (has_name_text(node, "input") && has_attribute_text(node, "checked")) ||
             (has_name_text(node, "option") && has_attribute_text(node, "selected"));
    return CSS_OK;
}

/* :lang(x): the nearest lang="" on the element or an ancestor is x, or
 * starts with x and a hyphen, ignoring case (Selectors 3, section 6.6.3). */
static css_error node_is_lang(void *pw, void *node, lwc_string *lang, bool *match) {
    (void)pw;
    *match = false;
    for (dom_node *current = node; current != NULL; current = element_parent(current)) {
        dom_string *value = attribute(current, "lang", 4);
        if (value == NULL) {
            continue;
        }
        const char *have = dom_string_data(value);
        size_t have_length = dom_string_byte_length(value);
        size_t want_length = lwc_string_length(lang);
        *match = (have_length == want_length ||
                  (have_length > want_length && have[want_length] == '-')) &&
                 bytes_equal(have, lwc_string_data(lang), want_length, true);
        dom_string_unref(value);
        break;
    }
    return CSS_OK;
}

/* The HTML color attributes retain the legacy names and hex forms that
 * Web Viewer accepted before the CSS cascade moved to libcss. */
static int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool legacy_color(const char *text, const char *end, css_color *out) {
    static const struct {
        const char *name;
        uint32_t color;
    } k_names[] = {
        {"black", 0xff000000u},
        {"silver", 0xffc0c0c0u},
        {"gray", 0xff808080u},
        {"grey", 0xff808080u},
        {"white", 0xffffffffu},
        {"maroon", 0xff800000u},
        {"red", 0xffff0000u},
        {"purple", 0xff800080u},
        {"fuchsia", 0xffff00ffu},
        {"green", 0xff008000u},
        {"lime", 0xff00ff00u},
        {"olive", 0xff808000u},
        {"yellow", 0xffffff00u},
        {"navy", 0xff000080u},
        {"blue", 0xff0000ffu},
        {"teal", 0xff008080u},
        {"aqua", 0xff00ffffu},
        {"orange", 0xffffa500u},
    };
    char lowered[32];
    size_t length = 0;
    size_t i;

    while (text < end && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')) {
        text += 1;
    }
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                          end[-1] == '\r' || end[-1] == ';')) {
        end -= 1;
    }
    if (end > text && text[0] == '#') {
        int digits[6];
        size_t count = 0;

        text += 1;
        while (text < end && count < 6) {
            int digit = hex_digit(*text);

            if (digit < 0) {
                return false;
            }
            digits[count++] = digit;
            text += 1;
        }
        if (text != end || (count != 3 && count != 6)) {
            return false;
        }
        unsigned red = count == 3 ? digits[0] * 17 : digits[0] * 16 + digits[1];
        unsigned green = count == 3 ? digits[1] * 17 : digits[2] * 16 + digits[3];
        unsigned blue = count == 3 ? digits[2] * 17 : digits[4] * 16 + digits[5];
        *out = 0xff000000u | (red << 16) | (green << 8) | blue;
        return true;
    }
    while (text < end && length + 1 < sizeof(lowered)) {
        char value = *text;

        if (value >= 'A' && value <= 'Z') {
            value = (char)(value - 'A' + 'a');
        }
        lowered[length++] = value;
        text += 1;
    }
    if (text != end) {
        return false;
    }
    lowered[length] = '\0';
    for (i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        if (strcmp(lowered, k_names[i].name) == 0) {
            *out = k_names[i].color;
            return true;
        }
    }
    return false;
}

static bool color_hint(dom_node *node, const char *name, uint32_t property, css_hint *hint) {
    dom_string *value = attribute(node, name, strlen(name));
    if (value == NULL) {
        return false;
    }
    const char *text = dom_string_data(value);
    css_color color;
    bool found = legacy_color(text, text + dom_string_byte_length(value), &color);
    dom_string_unref(value);
    if (found) {
        *hint = (css_hint){
            .prop = property,
            .status = property == CSS_PROP_COLOR ? CSS_COLOR_COLOR : CSS_BACKGROUND_COLOR_COLOR,
            .data.color = color,
        };
    }
    return found;
}

static css_error node_presentational_hint(void *pw, void *node, uint32_t *nhints, css_hint **hints) {
    css_hint *values = pw;
    *nhints = 0;
    *hints = values;
    if (values == NULL) {
        return CSS_OK;
    }
    if (has_name_text(node, "body")) {
        *nhints += color_hint(node, "bgcolor", CSS_PROP_BACKGROUND_COLOR, &values[*nhints]);
        *nhints += color_hint(node, "text", CSS_PROP_COLOR, &values[*nhints]);
    } else if (has_name_text(node, "font")) {
        *nhints += color_hint(node, "color", CSS_PROP_COLOR, values);
    } else if (has_name_text(node, "a") && has_attribute_text(node, "href")) {
        for (dom_node *parent = element_parent(node); parent != NULL; parent = element_parent(parent)) {
            if (has_name_text(parent, "body")) {
                *nhints += color_hint(parent, "link", CSS_PROP_COLOR, values);
                break;
            }
        }
    }
    return CSS_OK;
}

/* The values a property takes when no sheet gives it one and it has no
 * initial value of its own in libcss: the user agent's. */
static css_error ua_default_for_property(void *pw, uint32_t property, css_hint *hint) {
    (void)pw;
    switch (property) {
    case CSS_PROP_COLOR:
        hint->data.color = 0xff000000; /* opaque black, AARRGGBB */
        hint->status = CSS_COLOR_COLOR;
        return CSS_OK;
    case CSS_PROP_FONT_FAMILY:
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
        return CSS_OK;
    case CSS_PROP_QUOTES:
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
        return CSS_OK;
    case CSS_PROP_VOICE_FAMILY:
        hint->data.strings = NULL;
        hint->status = 0;
        return CSS_OK;
    default:
        return CSS_INVALID;
    }
}

/* libdom tells the owner of user data what happens to its node; libcss is
 * told the same about its own. */
static void node_data_changed(dom_node_operation operation, dom_string *key, void *data,
                              struct dom_node *source, struct dom_node *destination) {
    if (data == NULL || g_node_data_key == NULL || !dom_string_isequal(key, g_node_data_key)) {
        return;
    }
    switch (operation) {
    case DOM_NODE_CLONED:
        css_libcss_node_data_handler(&webengine_select_handler, CSS_NODE_CLONED, NULL, source,
                                     destination, data);
        break;
    case DOM_NODE_DELETED:
        css_libcss_node_data_handler(&webengine_select_handler, CSS_NODE_DELETED, NULL, source, NULL,
                                     data);
        break;
    case DOM_NODE_RENAMED:
    case DOM_NODE_IMPORTED:
    case DOM_NODE_ADOPTED:
        css_libcss_node_data_handler(&webengine_select_handler, CSS_NODE_MODIFIED, NULL, source, NULL,
                                     data);
        break;
    }
}

static css_error set_libcss_node_data(void *pw, void *node, void *libcss_node_data) {
    (void)pw;
    void *previous = NULL;
    if (dom_node_set_user_data(node, g_node_data_key, libcss_node_data, node_data_changed,
                               &previous) != DOM_NO_ERR) {
        return CSS_NOMEM;
    }
    /* On reselection, release the old cache. A NULL set comes from libcss
     * after it has already destroyed the data on invalidation. */
    if (libcss_node_data != NULL && previous != NULL && previous != libcss_node_data) {
        css_libcss_node_data_handler(&webengine_select_handler, CSS_NODE_DELETED,
                                     NULL, node, NULL, previous);
    }
    return CSS_OK;
}

static css_error get_libcss_node_data(void *pw, void *node, void **libcss_node_data) {
    (void)pw;
    *libcss_node_data = NULL;
    return dom_node_get_user_data(node, g_node_data_key, libcss_node_data) == DOM_NO_ERR ? CSS_OK
                                                                                         : CSS_NOMEM;
}

css_select_handler webengine_select_handler = {
    .handler_version = CSS_SELECT_HANDLER_VERSION_1,
    .node_name = node_name,
    .node_classes = node_classes,
    .node_id = node_id,
    .named_ancestor_node = named_ancestor_node,
    .named_parent_node = named_parent_node,
    .named_sibling_node = named_sibling_node,
    .named_generic_sibling_node = named_generic_sibling_node,
    .parent_node = parent_node,
    .sibling_node = sibling_node,
    .node_has_name = node_has_name,
    .node_has_class = node_has_class,
    .node_has_id = node_has_id,
    .node_has_attribute = node_has_attribute,
    .node_has_attribute_equal = node_has_attribute_equal,
    .node_has_attribute_dashmatch = node_has_attribute_dashmatch,
    .node_has_attribute_includes = node_has_attribute_includes,
    .node_has_attribute_prefix = node_has_attribute_prefix,
    .node_has_attribute_suffix = node_has_attribute_suffix,
    .node_has_attribute_substring = node_has_attribute_substring,
    .node_is_root = node_is_root,
    .node_count_siblings = node_count_siblings,
    .node_is_empty = node_is_empty,
    .node_is_link = node_is_link,
    .node_is_visited = node_is_false,
    .node_is_hover = node_is_false,
    .node_is_active = node_is_false,
    .node_is_focus = node_is_false,
    .node_is_enabled = node_is_enabled,
    .node_is_disabled = node_is_disabled,
    .node_is_checked = node_is_checked,
    .node_is_target = node_is_false,
    .node_is_lang = node_is_lang,
    .node_presentational_hint = node_presentational_hint,
    .ua_default_for_property = ua_default_for_property,
    .set_libcss_node_data = set_libcss_node_data,
    .get_libcss_node_data = get_libcss_node_data,
};

int webengine_select_init(void) {
    static const char key[] = "savanxp:webengine:libcss";
    if (g_node_data_key != NULL) {
        return 0;
    }
    return dom_string_create_interned((const uint8_t *)key, sizeof key - 1, &g_node_data_key) ==
                   DOM_NO_ERR
               ? 0
               : -1;
}

void webengine_select_fini(void) {
    if (g_node_data_key != NULL) {
        dom_string_unref(g_node_data_key);
        g_node_data_key = NULL;
    }
}
