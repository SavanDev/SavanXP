/* Web Viewer's favorites: the list and its text (favorites.h). */

#include "favorites.h"

#include <string.h>

static const struct {
    const char *url;
    const char *name;
} k_default_favorites[] = {
    {"https://motherfuckingwebsite.com/", "Motherfucking Website"},
    {"http://acid2.acidtests.org/", "The Acid2 Test"},
};

/* Copies `name` into `out`: control characters dropped, runs of spaces and
 * tabs made one space, ends trimmed, and "..." when it is cut. */
static void webview_favorite_name(char *out, const char *name)
{
    size_t used = 0;
    int cut = 0;

    for (; *name != '\0'; ++name)
    {
        unsigned char value = (unsigned char)*name;

        if (value == ' ' || value == '\t' || value == '\r' || value == '\n')
        {
            if (used == 0 || out[used - 1] == ' ')
            {
                continue;
            }
            value = ' ';
        }
        else if (value < 32 || value == 127)
        {
            continue;
        }
        if (used + 1 >= WEBVIEW_FAVORITE_NAME_CAP)
        {
            cut = 1;
            break;
        }
        out[used++] = (char)value;
    }
    if (cut)
    {
        used = WEBVIEW_FAVORITE_NAME_CAP - 4;
        /* Never leave half a UTF-8 sequence before the dots. */
        while (used > 0 && ((unsigned char)out[used] & 0xC0u) == 0x80u)
        {
            --used;
        }
        while (used > 0 && out[used - 1] == ' ')
        {
            --used;
        }
        memcpy(out + used, "...", 3);
        used += 3;
    }
    while (used > 0 && out[used - 1] == ' ')
    {
        --used;
    }
    out[used] = '\0';
}

void webview_favorites_defaults(struct webview_favorites *list)
{
    list->count = 0;
    for (size_t index = 0; index < sizeof(k_default_favorites) / sizeof(k_default_favorites[0]); ++index)
    {
        (void)webview_favorites_add(list, k_default_favorites[index].url, k_default_favorites[index].name);
    }
}

void webview_favorites_parse(struct webview_favorites *list, const char *text, size_t length)
{
    size_t position = 0;

    list->count = 0;
    while (position < length && list->count < WEBVIEW_FAVORITES_MAX)
    {
        size_t end = position;
        size_t tab;
        char url[WEBVIEW_FAVORITE_URL_CAP];
        char name[WEBVIEW_FAVORITE_URL_CAP];
        size_t url_length;
        size_t name_length;

        while (end < length && text[end] != '\n')
        {
            ++end;
        }
        tab = position;
        while (tab < end && text[tab] != '\t')
        {
            ++tab;
        }
        url_length = tab - position;
        while (url_length > 0 && (text[position + url_length - 1] == '\r' ||
                                     text[position + url_length - 1] == ' '))
        {
            --url_length;
        }
        if (url_length > 0 && text[position] != '#' && url_length < sizeof(url))
        {
            memcpy(url, text + position, url_length);
            url[url_length] = '\0';
            name_length = tab < end ? end - tab - 1 : 0;
            if (name_length >= sizeof(name))
            {
                name_length = sizeof(name) - 1;
            }
            if (name_length > 0)
            {
                memcpy(name, text + tab + 1, name_length);
            }
            name[name_length] = '\0';
            (void)webview_favorites_add(list, url, name);
        }
        position = end + 1;
    }
}

size_t webview_favorites_format(const struct webview_favorites *list, char *out, size_t capacity)
{
    size_t used = sizeof(WEBVIEW_FAVORITES_HEADER) - 1;

    if (capacity <= used)
    {
        return 0;
    }
    memcpy(out, WEBVIEW_FAVORITES_HEADER, used);
    for (int index = 0; index < list->count; ++index)
    {
        const struct webview_favorite *item = &list->items[index];
        size_t url_length = strlen(item->url);
        size_t name_length = strlen(item->name);

        if (used + url_length + 1 + name_length + 1 >= capacity)
        {
            return 0;
        }
        memcpy(out + used, item->url, url_length);
        used += url_length;
        out[used++] = '\t';
        memcpy(out + used, item->name, name_length);
        used += name_length;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return used;
}

int webview_favorites_find(const struct webview_favorites *list, const char *url)
{
    for (int index = 0; index < list->count; ++index)
    {
        if (strcmp(list->items[index].url, url) == 0)
        {
            return index;
        }
    }
    return -1;
}

int webview_favorites_add(struct webview_favorites *list, const char *url, const char *name)
{
    struct webview_favorite *item;
    size_t url_length = strlen(url);
    int existing = webview_favorites_find(list, url);

    if (existing >= 0)
    {
        return existing;
    }
    if (url_length == 0 || url_length >= WEBVIEW_FAVORITE_URL_CAP || list->count >= WEBVIEW_FAVORITES_MAX)
    {
        return -1;
    }
    for (size_t index = 0; index < url_length; ++index)
    {
        unsigned char value = (unsigned char)url[index];

        if (value < 32 || value == 127)
        {
            return -1;
        }
    }
    item = &list->items[list->count];
    memcpy(item->url, url, url_length + 1);
    webview_favorite_name(item->name, name != 0 ? name : "");
    if (item->name[0] == '\0')
    {
        webview_favorite_name(item->name, url);
    }
    return list->count++;
}

int webview_favorites_remove(struct webview_favorites *list, int index)
{
    if (index < 0 || index >= list->count)
    {
        return -1;
    }
    memmove(&list->items[index], &list->items[index + 1],
        (size_t)(list->count - index - 1) * sizeof(list->items[0]));
    list->count -= 1;
    return 0;
}
