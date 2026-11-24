/* Web Viewer's engine libraries in a SUN program
 * (docs/desktop/WEBVIEWER_ENGINE.md): NetSurf's libparserutils, libwapcaplet,
 * libhubbub, libdom and libcss, built for SUN, linked into a native PIE.
 *
 * Runs the engine's self-test, the same code the host test runs, then a page
 * of a few hundred kilobytes, parsed and styled, for the native heap.
 * "ENGINE TEST PASS" or "ENGINE TEST FAIL" ends it; the webengine-test
 * scenario waits for one of them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxn/sxn.h"
#include "webengine.h"
#include "webengine_selftest.h"

static int g_failures;

static void say(const char *text) {
    (void)sxn_log(text);
}

static void report(void *user, const char *line) {
    (void)user;
    char message[200];
    snprintf(message, sizeof message, "enginetest: FAILED %s", line);
    say(message);
    g_failures++;
}

/* Exercise the native heap and adjacent-sibling selection on a large DOM. */
static void check_large_page(unsigned paragraphs) {
    static const char head[] = "<!DOCTYPE html><html><head><style>p.odd { color: red }"
                               " p + p { background: white }"
                               " body em { font-weight: bold }</style></head><body>";
    size_t capacity = sizeof head + paragraphs * 96u + 32u;
    char *page = malloc(capacity);
    if (page == NULL) {
        report(NULL, "a large page fits in memory");
        return;
    }
    size_t length = (size_t)snprintf(page, capacity, "%s", head);
    for (unsigned i = 0; i < paragraphs; i++) {
        length += (size_t)snprintf(page + length, capacity - length,
                                   "<p class=\"%s\">Paragraph %u, <em>with</em> some text.</p>\n",
                                   (i & 1u) != 0 ? "odd" : "even", i);
    }
    length += (size_t)snprintf(page + length, capacity - length, "</body></html>");
    unsigned elements = 0;
    int result = -1;
    if (webengine_init() == 0) {
        result = webengine_check_page((const uint8_t *)page, length, "", &elements);
        webengine_fini();
    }
    free(page);
    char line[160];
    /* html, head, style, body, and a p and an em each. */
    snprintf(line, sizeof line, "a %u KiB page: result %d, %u elements styled", (unsigned)(length / 1024u),
             result, elements);
    say(line);
    if (result != 0 || elements != 4u + paragraphs * 2u) {
        report(NULL, "the large page parses and every element is styled");
    }
}

int main(void) {
    webengine_selftest(report, NULL);
    check_large_page(4000);
    say(g_failures == 0 ? "ENGINE TEST PASS" : "ENGINE TEST FAIL");
    return g_failures == 0 ? 0 : 1;
}
