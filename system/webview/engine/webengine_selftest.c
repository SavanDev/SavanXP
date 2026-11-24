/* The engine's self-test (webengine_selftest.h): one document through
 * libhubbub, libdom and libcss, and what each must make of it. The host test
 * (tests/host/webengine_test.c) and the SUN program enginetest run the same
 * code, so the libraries are proven in both builds. */

#include "webengine_selftest.h"

#include "webengine.h"

#include <stdio.h>
#include <string.h>

/* Windows-1252 (the <meta> says so, past the first bytes), entities,
 * implied end tags, a table's stray text, and one rule per kind of
 * selector. \x93 and \x94 are curly quotes in Windows-1252. */
static const char k_document[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en-GB\"><head><title>E1</title>\n"
    "<meta charset=\"windows-1252\">\n"
    "<style>\n"
    "body { color: #102030; font-size: 20px }\n"
    "p.note { color: red }\n"
    "#main > em { font-weight: bold }\n"
    "li:first-child { text-decoration: underline }\n"
    "li + li { margin-top: 7px }\n"
    "ul li:last-child { display: none }\n"
    "a:link { color: blue }\n"
    "[data-x|=\"en\"] { width: 33px }\n"
    ":lang(en) span.l { font-style: italic }\n"
    "h1 { font-size: 2em }\n"
    "td:empty { width: 5px }\n"
    "</style></head>\n"
    "<body>\n"
    "<div id=\"main\"><em>bold</em><span><em>not bold</em></span></div>\n"
    "<p class=\"other note\">red &amp; &eacute; \x93quoted\x94\n"
    "<p>inherits\n"
    "<ul><li>one<li>two<li>three</ul>\n"
    "<a href=\"x.html\">link</a><a name=\"n\">anchor</a>\n"
    "<div data-x=\"en-US\">w</div><div data-x=\"english\">x</div>\n"
    "<span class=\"l\">it</span>\n"
    "<h1>big</h1>\n"
    "<table><tr><td>cell</td><td></td></tr>stray</table>\n"
    "<p style=\"color: #00ff00\">inline</p>\n"
    "</body></html>\n";

/* What a test needs of one element, read while its style is alive. */
struct record {
    char name[12];
    unsigned depth;
    char parent[12];
    css_color color;
    uint8_t font_weight;
    uint8_t text_decoration;
    uint8_t display;
    uint8_t font_style;
    uint8_t width_type;
    css_fixed width;
    uint8_t margin_type;
    css_fixed margin_top;
    css_unit margin_unit;
    css_fixed font_size;
    css_unit font_size_unit;
};

#define MAX_RECORDS 64

struct walk {
    struct record records[MAX_RECORDS];
    unsigned count;
    unsigned elements;
};

static void local_name(dom_node *node, char *out, size_t capacity) {
    dom_string *name = NULL;
    out[0] = '\0';
    if (node == NULL || dom_node_get_local_name(node, &name) != DOM_NO_ERR || name == NULL) {
        return;
    }
    size_t length = dom_string_byte_length(name);
    if (length >= capacity) {
        length = capacity - 1;
    }
    /* In lower case: libdom keeps HTML names in upper case. */
    for (size_t i = 0; i < length; i++) {
        char c = dom_string_data(name)[i];
        out[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    out[length] = '\0';
    dom_string_unref(name);
}

static int record_element(void *user, dom_element *element, const css_computed_style *style,
                          unsigned depth) {
    struct walk *walk = user;
    walk->elements++;
    if (walk->count == MAX_RECORDS) {
        return 0;
    }
    struct record *record = &walk->records[walk->count++];
    memset(record, 0, sizeof *record);
    local_name((dom_node *)element, record->name, sizeof record->name);
    dom_node *parent = NULL;
    if (dom_node_get_parent_node(element, &parent) == DOM_NO_ERR && parent != NULL) {
        local_name(parent, record->parent, sizeof record->parent);
        dom_node_unref(parent);
    }
    record->depth = depth;
    css_computed_color(style, &record->color);
    record->font_weight = css_computed_font_weight(style);
    record->text_decoration = css_computed_text_decoration(style);
    record->display = css_computed_display(style, depth == 0);
    record->font_style = css_computed_font_style(style);
    record->width_type = css_computed_width(style, &record->width, &(css_unit){0});
    record->margin_type = css_computed_margin_top(style, &record->margin_top, &record->margin_unit);
    css_computed_font_size(style, &record->font_size, &record->font_size_unit);
    return 0;
}

/* The nth (from 0) element called `name`. */
static const struct record *find(const struct walk *walk, const char *name, unsigned nth) {
    for (unsigned i = 0; i < walk->count; i++) {
        if (strcmp(walk->records[i].name, name) == 0 && nth-- == 0) {
            return &walk->records[i];
        }
    }
    return NULL;
}

struct checker {
    webengine_report report;
    void *user;
    int failures;
};

static void check(struct checker *checker, int ok, const char *what) {
    if (!ok) {
        checker->failures++;
        checker->report(checker->user, what);
    }
}

#define CHECK(condition, what) check(checker, (condition), (what))

static unsigned count_by_tag(dom_document *document) {
    dom_string *all = NULL;
    dom_nodelist *list = NULL;
    uint32_t length = 0;
    if (dom_string_create((const uint8_t *)"*", 1, &all) != DOM_NO_ERR) {
        return 0;
    }
    if (dom_document_get_elements_by_tag_name(document, all, &list) == DOM_NO_ERR && list != NULL) {
        dom_nodelist_get_length(list, &length);
        dom_nodelist_unref(list);
    }
    dom_string_unref(all);
    return length;
}

static int contains(dom_document *document, const char *bytes) {
    dom_element *root = NULL;
    dom_string *text = NULL;
    int found = 0;
    if (dom_document_get_document_element(document, &root) != DOM_NO_ERR || root == NULL) {
        return 0;
    }
    if (dom_node_get_text_content(root, &text) == DOM_NO_ERR && text != NULL) {
        size_t length = dom_string_byte_length(text);
        size_t want = strlen(bytes);
        const char *data = dom_string_data(text);
        for (size_t at = 0; at + want <= length && !found; at++) {
            found = memcmp(data + at, bytes, want) == 0;
        }
        dom_string_unref(text);
    }
    dom_node_unref(root);
    return found;
}

int webengine_check_page(const uint8_t *data, size_t length, const char *url, unsigned *elements) {
    char encoding[64];
    dom_document *document = webengine_parse_html(data, length, NULL, encoding, sizeof encoding);
    if (document == NULL) {
        return -1;
    }
    struct webengine_styles styles;
    struct walk walk = {0};
    int result = -1;
    if (webengine_styles_open(&styles, document, url, NULL, NULL, NULL) == 0) {
        result = webengine_style_document(&styles, document, record_element, &walk);
        webengine_styles_close(&styles);
    }
    /* Every element the DOM holds was styled, no more and no fewer. */
    if (result == 0 && walk.elements != count_by_tag(document)) {
        result = -2;
    }
    if (elements != NULL) {
        *elements = walk.elements;
    }
    dom_node_unref(document);
    return result;
}

static void check_document(struct checker *checker) {
    char encoding[64] = "";
    dom_document *document = webengine_parse_html((const uint8_t *)k_document, sizeof k_document - 1,
                                                  NULL, encoding, sizeof encoding);
    CHECK(document != NULL, "the document parses");
    if (document == NULL) {
        return;
    }
    char message[128];
    snprintf(message, sizeof message, "the <meta> charset wins: encoding is '%s'", encoding);
    CHECK(strcmp(encoding, "WINDOWS-1252") == 0 || strcmp(encoding, "windows-1252") == 0 ||
              strcmp(encoding, "Windows-1252") == 0,
          message);
    CHECK(contains(document, "\xe2\x80\x9cquoted\xe2\x80\x9d"), "Windows-1252 quotes decode to UTF-8");
    CHECK(contains(document, "red & \xc3\xa9"), "&amp; and &eacute; decode");

    struct webengine_styles styles;
    struct walk walk = {0};
    CHECK(webengine_styles_open(&styles, document, "http://test/e1.html", NULL, NULL, NULL) == 0,
          "the <style> sheet loads");
    CHECK(styles.sheet_count == 1, "one author sheet");
    int walked = webengine_style_document(&styles, document, record_element, &walk);
    webengine_styles_close(&styles);
    CHECK(walked == 0, "every element is styled");
    CHECK(walk.elements == count_by_tag(document), "the walk meets every element of the DOM");
    dom_node_unref(document);

    const struct record *record;
    record = find(&walk, "html", 0);
    CHECK(record != NULL && record->depth == 0, "html is the root");
    record = find(&walk, "ul", 0);
    CHECK(record != NULL && strcmp(record->parent, "body") == 0, "<ul> closes the open <p>");
    CHECK(find(&walk, "li", 2) != NULL && find(&walk, "li", 3) == NULL, "three <li>, each closing the last");
    record = find(&walk, "table", 0);
    CHECK(record != NULL && strcmp(record->parent, "body") == 0, "the table stays in body");
    CHECK(find(&walk, "tbody", 0) != NULL, "the parser inserts the table's tbody");

    record = find(&walk, "body", 0);
    CHECK(record != NULL && record->color == 0xff102030, "body: color #102030");
    record = find(&walk, "p", 0);
    CHECK(record != NULL && record->color == 0xffff0000, "p.note matches a class among several");
    record = find(&walk, "p", 1);
    CHECK(record != NULL && record->color == 0xff102030, "a plain p inherits body's color");
    record = find(&walk, "p", 2);
    CHECK(record != NULL && record->color == 0xff00ff00, "style=\"\" applies");
    record = find(&walk, "em", 0);
    CHECK(record != NULL && record->font_weight == CSS_FONT_WEIGHT_BOLD, "#main > em matches a child");
    record = find(&walk, "em", 1);
    CHECK(record != NULL && record->font_weight != CSS_FONT_WEIGHT_BOLD,
          "#main > em does not match a grandchild");
    record = find(&walk, "li", 0);
    CHECK(record != NULL && record->text_decoration == CSS_TEXT_DECORATION_UNDERLINE &&
              record->margin_top == 0,
          "li:first-child, and no li + li before it");
    record = find(&walk, "li", 1);
    CHECK(record != NULL && record->margin_type == CSS_MARGIN_SET &&
              record->margin_top == INTTOFIX(7) && record->margin_unit == CSS_UNIT_PX &&
              record->text_decoration != CSS_TEXT_DECORATION_UNDERLINE,
          "li + li: margin-top 7px");
    record = find(&walk, "li", 2);
    CHECK(record != NULL && record->display == CSS_DISPLAY_NONE, "ul li:last-child: display none");
    record = find(&walk, "a", 0);
    CHECK(record != NULL && record->color == 0xff0000ff, "a:link with an href");
    record = find(&walk, "a", 1);
    CHECK(record != NULL && record->color == 0xff102030, "an <a> without href is no link");
    record = find(&walk, "div", 1);
    CHECK(record != NULL && record->width_type == CSS_WIDTH_SET && record->width == INTTOFIX(33),
          "[data-x|=en] matches en-US");
    record = find(&walk, "div", 2);
    CHECK(record != NULL && record->width_type != CSS_WIDTH_SET, "[data-x|=en] does not match english");
    record = find(&walk, "span", 1);
    CHECK(record != NULL && record->font_style == CSS_FONT_STYLE_ITALIC, ":lang(en) from html's en-GB");
    record = find(&walk, "h1", 0);
    CHECK(record != NULL && record->font_size == INTTOFIX(40) && record->font_size_unit == CSS_UNIT_PX,
          "h1: 2em of body's 20px is 40px");
    record = find(&walk, "td", 0);
    CHECK(record != NULL && record->width_type != CSS_WIDTH_SET, "td:empty skips a cell with text");
    record = find(&walk, "td", 1);
    CHECK(record != NULL && record->width_type == CSS_WIDTH_SET && record->width == INTTOFIX(5),
          "td:empty matches an empty cell");
}

static void check_errors(struct checker *checker) {
    /* Broken markup still makes a document; broken CSS still makes a sheet,
     * dropping what it cannot read (CSS 2.1, section 4.2). */
    static const char broken_html[] = "<p><b><i>misnested</b></i><div<<>>";
    unsigned elements = 0;
    CHECK(webengine_check_page((const uint8_t *)broken_html, sizeof broken_html - 1, "", &elements) == 0 &&
              elements >= 4,
          "misnested markup parses and styles");
    static const char broken_css[] = "p { color: ; } {{ ;; } q { color: red";
    css_stylesheet *sheet = webengine_stylesheet("", (const uint8_t *)broken_css, sizeof broken_css - 1, false);
    CHECK(sheet != NULL, "broken CSS still makes a sheet");
    if (sheet != NULL) {
        css_stylesheet_destroy(sheet);
    }
    CHECK(webengine_check_page((const uint8_t *)"", 0, "", &elements) == 0 && elements == 3,
          "an empty page is html, head and body");

    static const char utf8_html[] = "<p>Espa\xc3\xb1" "a";
    dom_document *document = webengine_parse_html((const uint8_t *)utf8_html, sizeof utf8_html - 1,
                                                  NULL, NULL, 0);
    CHECK(document != NULL && contains(document, "Espa\xc3\xb1" "a"), "unlabelled UTF-8 is preserved");
    dom_node_unref(document);
}

int webengine_selftest(webengine_report report, void *user) {
    struct checker state = {.report = report, .user = user};
    struct checker *checker = &state;
    if (webengine_init() != 0) {
        report(user, "webengine_init");
        return 1;
    }
    check_document(checker);
    check_errors(checker);
    webengine_fini();
    return state.failures;
}
