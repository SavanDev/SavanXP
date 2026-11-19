#include "desktop_wallpaper.h"
#include "shared/version.h"
#include "build_number.h"

#include <stdio.h>
#include <string.h>

/* ---- configuration ------------------------------------------------------- */

int desktop_wallpaper_read_config(struct desktop_wallpaper_config *config)
{
    char text[DESKTOP_WALLPAPER_CONFIG_BYTES];
    char path[256];
    long total = 0;
    int fd;

    if (config == 0)
    {
        return -1;
    }
    desktop_wallpaper_default_config(config);

    /* The profile's Settings folder, as savanxp_settings_path finds it. */
    fd = savanxp_settings_path(DESKTOP_WALLPAPER_CONFIG_NAME, 0, path, sizeof(path)) == 0
        ? (int)savanxp_open_mode(path, SAVANXP_OPEN_READ)
        : -1;
    if (fd < 0)
    {
        return -1;
    }
    while (total < (long)sizeof(text) - 1)
    {
        long result = savanxp_read(fd, text + total, (unsigned long)(sizeof(text) - 1 - (unsigned long)total));

        if (result <= 0)
        {
            break;
        }
        total += result;
    }
    savanxp_close(fd);
    text[total] = '\0';
    desktop_wallpaper_parse_config(config, text);
    return 0;
}

/* ---- images -------------------------------------------------------------- */

static int read_exact(int fd, void *buffer, unsigned long count)
{
    unsigned char *out = (unsigned char *)buffer;
    unsigned long done = 0;

    while (done < count)
    {
        long result = savanxp_read(fd, out + done, count - done);
        if (result <= 0)
        {
            return -1;
        }
        done += (unsigned long)result;
    }
    return 0;
}

/* Memory of its own for a large buffer, outside the heap: a section mapped
 * read-write. Returns the view, or 0 with *section_fd left at 0. */
static void *map_private_section(unsigned long bytes, int *section_fd)
{
    long section;
    void *view;

    *section_fd = 0;
    section = section_create(bytes, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section <= 0)
    {
        return 0;
    }
    view = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)view))
    {
        savanxp_close((int)section);
        return 0;
    }
    *section_fd = (int)section;
    return view;
}

static void unmap_private_section(void *view, int section_fd)
{
    if (view != 0)
    {
        (void)unmap_view(view);
    }
    if (section_fd > 0)
    {
        savanxp_close(section_fd);
    }
}

void desktop_wallpaper_image_unload(struct desktop_wallpaper_image *image)
{
    if (image == 0)
    {
        return;
    }
    if (image->pixels != 0)
    {
        (void)unmap_view(image->pixels);
        image->pixels = 0;
    }
    if (image->section > 0)
    {
        savanxp_close((int)image->section);
    }
    image->section = 0;
    image->width = 0;
    image->height = 0;
}

/* Decode BMP, PNG or sequential JPEG by content, with the limits the preview
 * shares (desktop_wallpaper_image_plan). A failure leaves the plain colour.
 *
 * File bytes and destination pixels live in private sections. The input
 * section is dropped after decode; the desktop keeps at most 16 MiB of
 * pixels outside the heap for as long as this image is its wallpaper.
 */
int desktop_wallpaper_image_load(struct desktop_wallpaper_image *image, const char *path)
{
    struct savanxp_stat info = {0};
    unsigned char *data = 0;
    int data_fd = 0;
    int section_fd = 0;
    int width = 0;
    int height = 0;
    int planned = 0;
    int fd;
    int loaded = 0;

    if (image == 0)
    {
        return -1;
    }
    desktop_wallpaper_image_unload(image);
    if (path == 0 || path[0] == '\0' || savanxp_stat(path, &info) != 0 || info.st_size == 0 ||
        info.st_size > DESKTOP_WALLPAPER_MAX_FILE_BYTES)
    {
        return -1;
    }
    fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0)
    {
        return -1;
    }
    data = (unsigned char *)map_private_section((unsigned long)info.st_size, &data_fd);
    if (data != 0 && read_exact(fd, data, (unsigned long)info.st_size) == 0)
    {
        planned = desktop_wallpaper_image_plan(data, (size_t)info.st_size, &width, &height) == 0;
    }
    if (planned)
    {
        size_t count = (size_t)width * (size_t)height;

        image->pixels = (uint32_t *)map_private_section((unsigned long)(count * sizeof(uint32_t)), &section_fd);
        image->section = section_fd;
        loaded = image->pixels != 0 &&
                 desktop_wallpaper_image_decode(data, (size_t)info.st_size, image->pixels, count) == 0;
    }
    savanxp_close(fd);
    unmap_private_section(data, data_fd);
    if (loaded)
    {
        image->width = width;
        image->height = height;
        return 0;
    }
    desktop_wallpaper_image_unload(image);
    return -1;
}

/* ---- the desktop's background ------------------------------------------- */

/* The initial state is what the headless selftests draw: teal, no image. */
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
        (void)desktop_wallpaper_image_load(&g_image, config->path);
    }
    g_config = *config;
}

void desktop_wallpaper_init(void)
{
    struct desktop_wallpaper_config config;

    (void)desktop_wallpaper_read_config(&config);
    adopt_config(&config);
}

int desktop_wallpaper_reload(void)
{
    struct desktop_wallpaper_config config;

    if (desktop_wallpaper_read_config(&config) < 0 ||
        desktop_wallpaper_config_equal(&config, &g_config))
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
#define DESKTOP_WALLPAPER_STAMP_MARGIN 8

static void draw_version_stamp(struct sx_painter *painter, const struct savanxp_fb_info *info)
{
    static const char *const lines[] = {
        SAVANXP_SYSTEM_NAME,
        SAVANXP_EDITION,
        "Version " SAVANXP_FULL_VERSION_NUMBER,
    };
    const int line_count = (int)(sizeof(lines) / sizeof(lines[0]));
    const int line_height = gfx_text_height();
    int bottom = (int)info->height - DESKTOP_WALLPAPER_TASKBAR_HEIGHT - DESKTOP_WALLPAPER_STAMP_MARGIN;
    int index;

    for (index = 0; index < line_count; ++index)
    {
        int x = (int)info->width - DESKTOP_WALLPAPER_STAMP_MARGIN - gfx_text_width(lines[index]);
        int y = bottom - (line_count - index) * line_height;

        /* A one-pixel shadow: white alone is lost on a light image. */
        sx_painter_draw_text(painter, x + 1, y + 1, lines[index], gfx_rgb(0, 0, 0));
        sx_painter_draw_text(painter, x, y, lines[index], gfx_rgb(255, 255, 255));
    }
}

void desktop_wallpaper_draw(struct sx_painter *painter, const struct savanxp_fb_info *info)
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
