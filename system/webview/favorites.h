#pragma once

/* Web Viewer's favorites (docs/desktop/WEBVIEWER.md#favorites): the list the
 * Favorites menu shows, and the text it is kept as in the person's profile.
 * No system calls here, so the host test (tests/host/webview_favorites_test.c)
 * links it as it is; webview.c reads and writes the file. */

#include <stddef.h>

#define WEBVIEW_FAVORITES_MAX 24
#define WEBVIEW_FAVORITE_URL_CAP 512 /* SXNHTTP_URL_CAP, with its terminator */
#define WEBVIEW_FAVORITE_NAME_CAP 48 /* what a menu row shows, with its terminator */

/* The setting's file name (sxn_settings_path) and its first line. */
#define WEBVIEW_FAVORITES_FILE "webview-favorites"
#define WEBVIEW_FAVORITES_HEADER "# Web Viewer favorites: URL, a tab, the name\n"

/* The largest text webview_favorites_format writes. */
#define WEBVIEW_FAVORITES_TEXT_CAP \
    (sizeof(WEBVIEW_FAVORITES_HEADER) + \
     WEBVIEW_FAVORITES_MAX * (WEBVIEW_FAVORITE_URL_CAP + WEBVIEW_FAVORITE_NAME_CAP + 1))

struct webview_favorite {
    char url[WEBVIEW_FAVORITE_URL_CAP];
    char name[WEBVIEW_FAVORITE_NAME_CAP];
};

struct webview_favorites {
    struct webview_favorite items[WEBVIEW_FAVORITES_MAX];
    int count;
};

/* The list a profile starts with, before it has a file of its own. */
void webview_favorites_defaults(struct webview_favorites *list);

/* Replaces the list with the one in `text`: a line per favorite, the URL, a
 * tab and the name. Blank lines and lines starting with '#' are skipped, a
 * line without a name is named by its URL, and a line whose URL does not fit
 * is dropped. Past WEBVIEW_FAVORITES_MAX the rest is ignored. */
void webview_favorites_parse(struct webview_favorites *list, const char *text, size_t length);

/* The list as text, header first, into `out` (NUL-terminated). Its length,
 * or 0 when it does not fit. */
size_t webview_favorites_format(const struct webview_favorites *list, char *out, size_t capacity);

/* The index of the favorite for `url`, or -1. */
int webview_favorites_find(const struct webview_favorites *list, const char *url);

/* Adds `url` at the end, named `name` (spaces collapsed, cut with "..." when
 * long) or by the URL when the name is empty. A URL already there keeps its
 * place. The favorite's index; -1 when the list is full, the URL is empty or
 * does not fit. */
int webview_favorites_add(struct webview_favorites *list, const char *url, const char *name);

/* Removes the favorite at `index`, the rest moving up. 0, or -1. */
int webview_favorites_remove(struct webview_favorites *list, int index);
