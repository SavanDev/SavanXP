/* Web Viewer's boxes on the host (docs/desktop/WEBVIEWER_ENGINE.md#the-box-tree):
 * each page of the corpus with an expectation beside it (NAME.html and
 * NAME.expect) is parsed, styled with the engine's user-agent sheet and its
 * own <style>, and made into boxes; the dump must match the expectation line
 * for line. Lines of the expectation starting with '#' are comments.
 *
 * The fonts are fixed, so the expectations do not depend on the system's:
 * face 0 (under 18px) is 7px a character, face 1 (18px and over) 10px,
 * face 2 (monospace) 8px; bold adds a pixel a character.
 *
 * `--print PAGE.html` prints a page's dump instead of checking it. */

#include "webengine.h"
#include "webengine_box.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_face(void *user, int size, bool bold, bool monospace) {
    (void)user;
    (void)bold;
    return monospace ? 2 : size >= 18 ? 1 : 0;
}

static int test_width(void *user, int face, bool bold, const char *text, size_t length) {
    (void)user;
    static const int advance[3] = { 7, 10, 8 };
    int characters = 0;
    for (size_t i = 0; i < length; i++) {
        characters += ((unsigned char)text[i] & 0xc0) != 0x80;
    }
    return characters * (advance[face] + (bold ? 1 : 0));
}

static void test_metrics(void *user, int face, int *ascent, int *descent) {
    (void)user;
    *ascent = face == 1 ? 16 : 12;
    *descent = 4;
}

static const struct webengine_fonts k_fonts = { NULL, test_face, test_width, test_metrics };

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc(size > 0 ? (size_t)size + 1 : 1);
    size_t got = data != NULL && size > 0 ? fread(data, 1, (size_t)size, file) : 0;
    fclose(file);
    if (data == NULL || got != (size_t)(size > 0 ? size : 0)) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    *length = got;
    return data;
}

struct lines {
    char **line;
    size_t count;
    size_t capacity;
};

static void collect(void *user, const char *line) {
    struct lines *lines = user;
    if (lines->count == lines->capacity) {
        lines->capacity = lines->capacity == 0 ? 64 : lines->capacity * 2;
        lines->line = realloc(lines->line, lines->capacity * sizeof(*lines->line));
    }
    lines->line[lines->count++] = strdup(line);
}

static void free_lines(struct lines *lines) {
    for (size_t i = 0; i < lines->count; i++) {
        free(lines->line[i]);
    }
    free(lines->line);
}

/* The dump of the page at `path`, into `out`. 0, or -1. */
static int dump_page(const char *path, css_stylesheet *ua, struct lines *out) {
    size_t length = 0;
    char *html = read_file(path, &length);
    if (html == NULL) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        return -1;
    }
    dom_document *document = webengine_parse_html((const uint8_t *)html, length, NULL, NULL, 0);
    free(html);
    if (document == NULL) {
        fprintf(stderr, "FAIL: %s does not parse\n", path);
        return -1;
    }
    struct webengine_styles styles;
    int result = -1;
    if (webengine_styles_open(&styles, document, path, ua, NULL, NULL) == 0) {
        struct webengine_tree *tree = webengine_tree_build(document, &styles, &k_fonts);
        if (tree != NULL) {
            webengine_tree_dump(tree, collect, out);
            webengine_tree_free(tree);
            result = 0;
        }
        webengine_styles_close(&styles);
    }
    dom_node_unref(document);
    if (result != 0) {
        fprintf(stderr, "FAIL: %s: no boxes\n", path);
    }
    return result;
}

static int check_page(const char *html_path, const char *expect_path, css_stylesheet *ua) {
    size_t length = 0;
    char *expect = read_file(expect_path, &length);
    if (expect == NULL) {
        fprintf(stderr, "FAIL: cannot read %s\n", expect_path);
        return 1;
    }
    struct lines expected = { 0 };
    for (char *line = expect; line != NULL && *line != '\0';) {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        if (line[0] != '#' && line[0] != '\0') {
            collect(&expected, line);
        }
        line = end != NULL ? end + 1 : NULL;
    }
    free(expect);
    struct lines got = { 0 };
    int failures = dump_page(html_path, ua, &got) != 0;
    for (size_t i = 0; failures == 0 && (i < expected.count || i < got.count); i++) {
        const char *want = i < expected.count ? expected.line[i] : "(nothing)";
        const char *have = i < got.count ? got.line[i] : "(nothing)";
        if (strcmp(want, have) != 0) {
            fprintf(stderr, "FAIL: %s, box %zu:\n  expected: %s\n  got:      %s\n", html_path, i + 1, want, have);
            failures = 1;
        }
    }
    if (failures == 0) {
        printf("%s: %zu boxes as expected\n", html_path, got.count);
    }
    free_lines(&expected);
    free_lines(&got);
    return failures;
}

int main(int argc, char **argv) {
    if (argc != 2 && !(argc == 3 && strcmp(argv[1], "--print") == 0)) {
        fprintf(stderr, "usage: webengine-layout-test <corpus directory> | --print <page.html>\n");
        return 2;
    }
    if (webengine_init() != 0) {
        fprintf(stderr, "FAIL: webengine_init\n");
        return 1;
    }
    css_stylesheet *ua = webengine_stylesheet("", (const uint8_t *)webengine_ua_css, webengine_ua_css_length, false);
    if (ua == NULL) {
        fprintf(stderr, "FAIL: the user-agent sheet does not parse\n");
        webengine_fini();
        return 1;
    }
    int failures = 0;
    if (argc == 3) {
        struct lines got = { 0 };
        failures = dump_page(argv[2], ua, &got) != 0;
        for (size_t i = 0; i < got.count; i++) {
            printf("%s\n", got.line[i]);
        }
        free_lines(&got);
    } else {
        DIR *dir = opendir(argv[1]);
        unsigned pages = 0;
        struct dirent *entry;
        while (dir != NULL && (entry = readdir(dir)) != NULL) {
            size_t length = strlen(entry->d_name);
            if (length < 8 || strcmp(entry->d_name + length - 7, ".expect") != 0) {
                continue;
            }
            char html[1024];
            char expect[1024];
            snprintf(expect, sizeof(expect), "%s/%s", argv[1], entry->d_name);
            snprintf(html, sizeof(html), "%s/%.*s.html", argv[1], (int)(length - 7), entry->d_name);
            failures += check_page(html, expect, ua);
            pages++;
        }
        if (dir != NULL) {
            closedir(dir);
        }
        if (pages == 0) {
            fprintf(stderr, "FAIL: no expectation in %s\n", argv[1]);
            failures++;
        }
    }
    css_stylesheet_destroy(ua);
    webengine_fini();
    if (failures != 0) {
        fprintf(stderr, "webengine-layout-test: %d failed\n", failures);
        return 1;
    }
    printf("webengine-layout-test: pass\n");
    return 0;
}
