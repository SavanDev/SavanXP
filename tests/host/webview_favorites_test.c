/* Web Viewer's favorites list and its text (system/webview/favorites.h). */
#include <stdio.h>
#include <string.h>

#include "favorites.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static struct webview_favorites list;
static struct webview_favorites again;
static char text[WEBVIEW_FAVORITES_TEXT_CAP];

int main(void)
{
    size_t length;
    char url[WEBVIEW_FAVORITE_URL_CAP + 8];

    /* A new profile starts with the two pages the browser is measured by. */
    webview_favorites_defaults(&list);
    CHECK(list.count == 2);
    CHECK(strcmp(list.items[0].url, "https://motherfuckingwebsite.com/") == 0);
    CHECK(strcmp(list.items[0].name, "Motherfucking Website") == 0);
    CHECK(strcmp(list.items[1].url, "http://acid2.acidtests.org/") == 0);
    CHECK(strcmp(list.items[1].name, "The Acid2 Test") == 0);

    /* Adding: the name is tidied, an empty one is the URL, a repeat keeps
     * its place. */
    CHECK(webview_favorites_add(&list, "file:///system/samples/welcome.html", "  Welcome \t to\nWeb  Viewer ") == 2);
    CHECK(strcmp(list.items[2].name, "Welcome to Web Viewer") == 0);
    CHECK(webview_favorites_add(&list, "http://10.0.2.2:8080/", "") == 3);
    CHECK(strcmp(list.items[3].name, "http://10.0.2.2:8080/") == 0);
    CHECK(webview_favorites_add(&list, "http://acid2.acidtests.org/", "Other") == 1);
    CHECK(list.count == 4 && strcmp(list.items[1].name, "The Acid2 Test") == 0);
    CHECK(webview_favorites_add(&list, "", "Nothing") == -1);
    CHECK(webview_favorites_add(&list, "http://a/\tb", "Tab") == -1);
    memset(url, 'a', sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    CHECK(webview_favorites_add(&list, url, "Long") == -1);
    CHECK(list.count == 4);

    /* A long name is cut with dots and fits a menu row. */
    CHECK(webview_favorites_add(&list, "http://long/",
        "A page title that goes on and on well past what a menu row can show") == 4);
    CHECK(strlen(list.items[4].name) < WEBVIEW_FAVORITE_NAME_CAP);
    CHECK(strcmp(list.items[4].name + strlen(list.items[4].name) - 3, "...") == 0);
    CHECK(strncmp(list.items[4].name, "A page title that goes", 22) == 0);

    /* Text and back. */
    length = webview_favorites_format(&list, text, sizeof(text));
    CHECK(length > 0 && length == strlen(text));
    CHECK(strncmp(text, WEBVIEW_FAVORITES_HEADER, strlen(WEBVIEW_FAVORITES_HEADER)) == 0);
    CHECK(strstr(text, "http://acid2.acidtests.org/\tThe Acid2 Test\n") != 0);
    webview_favorites_parse(&again, text, length);
    CHECK(again.count == list.count);
    for (int index = 0; index < list.count && index < again.count; ++index)
    {
        CHECK(strcmp(again.items[index].url, list.items[index].url) == 0);
        CHECK(strcmp(again.items[index].name, list.items[index].name) == 0);
    }
    CHECK(webview_favorites_format(&list, text, 20) == 0);

    /* Removing, and finding. */
    CHECK(webview_favorites_find(&list, "http://10.0.2.2:8080/") == 3);
    CHECK(webview_favorites_remove(&list, 0) == 0);
    CHECK(list.count == 4 && strcmp(list.items[0].url, "http://acid2.acidtests.org/") == 0);
    CHECK(webview_favorites_find(&list, "https://motherfuckingwebsite.com/") == -1);
    CHECK(webview_favorites_remove(&list, 4) == -1 && webview_favorites_remove(&list, -1) == -1);

    /* An emptied list stays empty: its text is the header alone. */
    while (list.count > 0)
    {
        (void)webview_favorites_remove(&list, 0);
    }
    length = webview_favorites_format(&list, text, sizeof(text));
    webview_favorites_parse(&again, text, length);
    CHECK(again.count == 0);

    /* Hand-written text: CRLF, comments, blank lines, a URL without a name. */
    {
        static const char hand[] =
            "# mine\r\n\r\nhttp://x/ \tX page\r\nhttp://y/\n#http://z/\tZ\n   \n";

        webview_favorites_parse(&again, hand, sizeof(hand) - 1);
        CHECK(again.count == 2);
        CHECK(strcmp(again.items[0].url, "http://x/") == 0 && strcmp(again.items[0].name, "X page") == 0);
        CHECK(strcmp(again.items[1].url, "http://y/") == 0 && strcmp(again.items[1].name, "http://y/") == 0);
    }

    /* A full list refuses more, and so does text past the limit. */
    list.count = 0;
    for (int index = 0; index < WEBVIEW_FAVORITES_MAX; ++index)
    {
        snprintf(url, sizeof(url), "http://host/%d", index);
        CHECK(webview_favorites_add(&list, url, "") == index);
    }
    CHECK(webview_favorites_add(&list, "http://host/full", "") == -1);
    length = webview_favorites_format(&list, text, sizeof(text));
    CHECK(length > 0);
    webview_favorites_parse(&again, text, length);
    CHECK(again.count == WEBVIEW_FAVORITES_MAX);

    if (failures)
    {
        return 1;
    }
    puts("WEBVIEW FAVORITES TEST PASS");
    return 0;
}
