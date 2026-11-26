#include "desktop_background.h"

#include "build_number.h"
#include "shared/version.h"
#include "sxgfx/pixels.h"
#include "sxgfx/wallpaper.h"
#include "sxn/sxn.h"
#include "sxn/sxnprofile.h"

#include <string.h>

/* The taskbar the version stamp sits above (desktop.h has its own copy). */
_Static_assert(DESKTOP_WALLPAPER_TASKBAR_HEIGHT == 28, "the stamp follows the taskbar's height");

/* ---- the person's files ------------------------------------------------- */

/* Reads a whole small file into `text`, NUL-terminated. 0, or -1. */
static int read_small_file(const char *path, char *text, size_t capacity)
{
    long handle = sxn_open_file(path, SXN_FILE_READ);
    size_t total = 0;

    if (handle < 0)
    {
        return -1;
    }
    while (total + 1 < capacity)
    {
        long result = sxn_read(handle, text + total, (unsigned long)(capacity - 1 - total));

        if (result <= 0)
        {
            break;
        }
        total += (size_t)result;
    }
    (void)sxn_close(handle);
    text[total] = '\0';
    return 0;
}

/* The configuration, migrating the old one-digit format; the defaults and
 * -1 when there is none. */
static int read_config(struct desktop_wallpaper_config *config)
{
    char path[DESKTOP_WALLPAPER_PATH_MAX];
    char text[DESKTOP_WALLPAPER_CONFIG_BYTES];

    desktop_wallpaper_default_config(config);
    if (sxn_settings_path(DESKTOP_WALLPAPER_CONFIG_NAME, 0, path, sizeof(path)) != 0 ||
        read_small_file(path, text, sizeof(text)) != 0)
    {
        return -1;
    }
    desktop_wallpaper_parse_config(config, text);
    return 0;
}

static int parse_number(const char **cursor, uint32_t *value)
{
    const char *text = *cursor;
    uint32_t result = 0;
    int digits = 0;

    while (*text >= '0' && *text <= '9' && digits < 6)
    {
        result = result * 10u + (uint32_t)(*text - '0');
        ++text;
        ++digits;
    }
    if (digits == 0)
    {
        return -1;
    }
    *cursor = text;
    *value = result;
    return 0;
}

int desktop_read_display_mode(uint32_t *width, uint32_t *height)
{
    char path[DESKTOP_WALLPAPER_PATH_MAX];
    char text[64];
    const char *cursor;

    if (width == 0 || height == 0 || sxn_settings_path("display.cfg", 0, path, sizeof(path)) != 0 ||
        read_small_file(path, text, sizeof(text)) != 0)
    {
        return -1;
    }
    cursor = strstr(text, "mode=");
    if (cursor == 0)
    {
        return -1;
    }
    cursor += 5;
    if (parse_number(&cursor, width) < 0 || *cursor != 'x')
    {
        return -1;
    }
    ++cursor;
    if (parse_number(&cursor, height) < 0 || *width == 0 || *height == 0)
    {
        return -1;
    }
    return 0;
}

/* ---- the image ------------------------------------------------------------
 *
 * The file is read into a section dropped once decoded, and the pixels live
 * in a section of their own, off the heap: the desktop keeps at most 16 MiB
 * of them for as long as the image is its wallpaper. */

static void *map_section(uint64_t bytes, long *section)
{
    long view;

    *section = sxn_section_create(bytes, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (*section <= 0)
    {
        *section = 0;
        return 0;
    }
    view = sxn_map_view(*section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (view <= 0)
    {
        (void)sxn_close(*section);
        *section = 0;
        return 0;
    }
    return (void *)(uintptr_t)view;
}

static void unmap_section(void *view, long section)
{
    if (view != 0)
    {
        (void)sxn_unmap_view((uint64_t)(uintptr_t)view);
    }
    if (section > 0)
    {
        (void)sxn_close(section);
    }
}

static void image_unload(struct desktop_wallpaper_image *image)
{
    unmap_section(image->pixels, image->section);
    memset(image, 0, sizeof(*image));
}

/* BMP, PNG or sequential JPEG, by content, with the limits the preview
 * shares (desktop_wallpaper_image_plan). 0 with the image decoded, or -1
 * with it empty, and then the plain colour shows. */
static int image_load(struct desktop_wallpaper_image *image, const char *path)
{
    struct sxn_file_info info;
    unsigned char *data = 0;
    long data_section = 0;
    long handle;
    size_t done = 0;
    int width = 0;
    int height = 0;
    int loaded = 0;

    image_unload(image);
    if (path == 0 || path[0] != '/' || sxn_query_path(path, &info) < 0 || info.type != SXN_FILE_TYPE_FILE ||
        info.size == 0 || info.size > DESKTOP_WALLPAPER_MAX_FILE_BYTES)
    {
        return -1;
    }
    handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0)
    {
        return -1;
    }
    data = (unsigned char *)map_section(info.size, &data_section);
    while (data != 0 && done < info.size)
    {
        long result = sxn_read(handle, data + done, (unsigned long)(info.size - done));

        if (result <= 0)
        {
            break;
        }
        done += (size_t)result;
    }
    (void)sxn_close(handle);
    if (data != 0 && done == info.size &&
        desktop_wallpaper_image_plan(data, (size_t)info.size, &width, &height) == 0)
    {
        size_t count = (size_t)width * (size_t)height;

        image->pixels = (uint32_t *)map_section((uint64_t)count * sizeof(uint32_t), &image->section);
        loaded = image->pixels != 0 &&
                 desktop_wallpaper_image_decode(data, (size_t)info.size, image->pixels, count) == 0;
    }
    unmap_section(data, data_section);
    if (!loaded)
    {
        image_unload(image);
        return -1;
    }
    image->width = width;
    image->height = height;
    return 0;
}

/* ---- the background -------------------------------------------------------- */

static struct desktop_wallpaper_config g_config = {
    {0},
    DESKTOP_WALLPAPER_STRETCH,
    DESKTOP_WALLPAPER_DEFAULT_COLOR,
};
static struct desktop_wallpaper_image g_image;

/* Takes a new configuration, decoding the image again only if it changed. */
static void adopt_config(const struct desktop_wallpaper_config *config)
{
    if (strcmp(config->path, g_config.path) != 0 || g_image.pixels == 0)
    {
        (void)image_load(&g_image, config->path);
    }
    g_config = *config;
}

void desktop_background_init(void)
{
    struct desktop_wallpaper_config config;

    (void)read_config(&config);
    adopt_config(&config);
}

int desktop_background_reload(void)
{
    struct desktop_wallpaper_config config;

    if (read_config(&config) < 0 || desktop_wallpaper_config_equal(&config, &g_config))
    {
        return 0;
    }
    adopt_config(&config);
    return 1;
}

/*
 * The system's name and version in the bottom-right corner, over the
 * background: what Windows test builds did and ReactOS always does. It is part
 * of the BACKGROUND and not a window, so it stays below everything.
 *
 * It sits above the taskbar, which is composed over the background at the
 * foot of the screen. Without a taskbar there is a gap of its height, which is
 * better than the text depending on whether a client started or not.
 */
#define DESKTOP_BACKGROUND_STAMP_MARGIN 8

static void draw_version_stamp(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    static const char *const lines[] = {
        SAVANXP_SYSTEM_NAME,
        SAVANXP_EDITION,
        "Version " SAVANXP_FULL_VERSION_NUMBER,
    };
    const int line_count = (int)(sizeof(lines) / sizeof(lines[0]));
    const int line_height = gfx_text_height();
    int bottom = (int)info->height - DESKTOP_WALLPAPER_TASKBAR_HEIGHT - DESKTOP_BACKGROUND_STAMP_MARGIN;
    int index;

    for (index = 0; index < line_count; ++index)
    {
        int x = (int)info->width - DESKTOP_BACKGROUND_STAMP_MARGIN - gfx_text_width(lines[index]);
        int y = bottom - (line_count - index) * line_height;

        /* A one-pixel shadow: white alone is lost on a light image. */
        sx_painter_draw_text(painter, x + 1, y + 1, lines[index], gfx_rgb(0, 0, 0));
        sx_painter_draw_text(painter, x, y, lines[index], gfx_rgb(255, 255, 255));
    }
}

void desktop_background_draw(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    if (painter == 0 || info == 0)
    {
        return;
    }
    desktop_wallpaper_paint(
        painter,
        sx_rect_make(0, 0, (int)info->width, (int)info->height),
        (int)info->width,
        (int)info->height,
        &g_config,
        &g_image);
    draw_version_stamp(painter, info);
}
