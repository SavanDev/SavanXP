#include "start_shortcuts.h"

#include "start_pins.h"

#include "savanxp/sxe.h"
#include "sxe/sxe_format.h"
#include "sxgfx/pe_icon.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* See start_shortcuts.h. */

/* The largest Windows program whose icon is read: Solitaire is 57 KiB. */
#define START_SHORTCUTS_PE_MAX (1024u * 1024u)

static struct start_shortcut g_shortcuts[START_SHORTCUTS_MAX];
static int g_count = 0;

_Alignas(4) static uint8_t g_icon_scratch[SXE_ICON_MAX_BYTES];
static uint8_t g_pe_scratch[START_SHORTCUTS_PE_MAX];

static int ends_with_nocase(const char *text, const char *suffix)
{
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);

    if (length < suffix_length)
    {
        return 0;
    }
    for (size_t i = 0; i < suffix_length; ++i)
    {
        char c = text[length - suffix_length + i];

        if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != suffix[i])
        {
            return 0;
        }
    }
    return 1;
}

static int exists(const char *path)
{
    struct stat info;

    return path[0] != '\0' && stat(path, &info) == 0;
}

/* A Windows program's own icon. */
static int icon_from_pe(const char *path, uint32_t *out)
{
    int fd = open(path, O_RDONLY);
    size_t total = 0;

    if (fd < 0)
    {
        return 0;
    }
    for (;;)
    {
        ssize_t count = read(fd, g_pe_scratch + total, sizeof(g_pe_scratch) - total);

        if (count <= 0)
        {
            break;
        }
        total += (size_t)count;
        if (total == sizeof(g_pe_scratch))
        {
            break;
        }
    }
    close(fd);
    return pe_icon_from_file(g_pe_scratch, total, START_SHORTCUTS_ICON_EXTENT, out);
}

/* A program's .sxicon: its 16x16 image, or the nearest one sampled to it. */
static int icon_from_sxe(const char *path, uint32_t *out)
{
    struct sxe_file file;
    struct sxe_icons icons;
    const struct sxe_icon_entry *entry;
    const uint32_t *pixels;
    int ok = 0;

    if (sxe_file_open(&file, path) != SXE_OK)
    {
        return 0;
    }
    if (sxe_file_load_icons(&file, g_icon_scratch, sizeof(g_icon_scratch), &icons) == SXE_OK &&
        (entry = sxe_icons_best(&icons, START_SHORTCUTS_ICON_EXTENT)) != 0 &&
        (pixels = sxe_icons_pixels(&icons, entry)) != 0 && entry->width > 0 && entry->height > 0)
    {
        for (uint32_t y = 0; y < START_SHORTCUTS_ICON_EXTENT; ++y)
        {
            for (uint32_t x = 0; x < START_SHORTCUTS_ICON_EXTENT; ++x)
            {
                out[y * START_SHORTCUTS_ICON_EXTENT + x] =
                    pixels[(y * entry->height / START_SHORTCUTS_ICON_EXTENT) * entry->width +
                           x * entry->width / START_SHORTCUTS_ICON_EXTENT];
            }
        }
        ok = 1;
    }
    sxe_file_close(&file);
    return ok;
}

static void add(const char *folder, const char *file_name)
{
    struct start_shortcut *s;
    struct shortcut link;
    char path[SHORTCUT_PATH_CAPACITY];
    size_t name_length = strlen(file_name) - (sizeof(SHORTCUT_EXTENSION) - 1u);
    int written = snprintf(path, sizeof(path), "%s/%s", folder, file_name);

    if (g_count >= START_SHORTCUTS_MAX || written < 0 || (size_t)written >= sizeof(path) ||
        shortcut_load(path, &link) != 0 || !exists(link.target))
    {
        return;
    }
    s = &g_shortcuts[g_count];
    memset(s, 0, sizeof(*s));
    if (name_length >= sizeof(s->name))
    {
        name_length = sizeof(s->name) - 1u;
    }
    memcpy(s->name, file_name, name_length);
    memcpy(s->path, path, (size_t)written + 1u);
    {
        const char *source = link.icon[0] != '\0' ? link.icon : link.target;

        s->has_icon = ends_with_nocase(source, ".exe") ? icon_from_pe(source, s->icon) : icon_from_sxe(source, s->icon);
    }
    /* Kept sorted by name. */
    for (int i = 0; i < g_count; ++i)
    {
        if (strcmp(s->name, g_shortcuts[i].name) < 0)
        {
            struct start_shortcut moved = *s;

            memmove(&g_shortcuts[i + 1], &g_shortcuts[i], (size_t)(g_count - i) * sizeof(*s));
            g_shortcuts[i] = moved;
            break;
        }
    }
    g_count += 1;
}

static void scan(const char *folder)
{
    DIR *handle = opendir(folder);
    struct dirent *entry;

    if (handle == 0)
    {
        return;
    }
    while ((entry = readdir(handle)) != 0)
    {
        if (entry->d_name[0] != '.' && shortcut_is_shortcut_name(entry->d_name))
        {
            add(folder, entry->d_name);
        }
    }
    closedir(handle);
}

int start_shortcuts_load(void)
{
    char folder[SHORTCUT_PATH_CAPACITY];

    g_count = 0;
    if (start_pins_folder(folder, sizeof(folder)) == 0)
    {
        scan(folder);
    }
    return g_count;
}

int start_shortcuts_count(void)
{
    return g_count;
}

const struct start_shortcut *start_shortcuts_at(int index)
{
    return index >= 0 && index < g_count ? &g_shortcuts[index] : 0;
}
