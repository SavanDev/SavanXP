#include "desktop_wallpaper.h"
#include "shared/version.h"
#include "build_number.h"

#include <stdio.h>
#include <string.h>

/* The user's choice, in the profile (savanxp_settings_path). */
#define DESKTOP_WALLPAPER_CONFIG_NAME "desktop.cfg"
/* Big enough for the three keys with a full-length path. */
#define DESKTOP_WALLPAPER_CONFIG_BYTES (DESKTOP_WALLPAPER_PATH_MAX + 96)

/* The old format was one digit, a mode: 0 teal, 1 gradient, 2 grid pattern,
 * 3 /wallpaper.bmp. These are the wallpapers each one became. */
#define DESKTOP_WALLPAPER_LEGACY_GRADIENT DESKTOP_WALLPAPER_DIRECTORY "/Gradient.bmp"
#define DESKTOP_WALLPAPER_LEGACY_GRID DESKTOP_WALLPAPER_DIRECTORY "/Grid.bmp"

static const char *const k_position_names[DESKTOP_WALLPAPER_POSITION_COUNT] = {
    "Center",
    "Tile",
    "Stretch",
};
/* As written in the file: stable, never translated. */
static const char *const k_position_keys[DESKTOP_WALLPAPER_POSITION_COUNT] = {
    "center",
    "tile",
    "stretch",
};

/* ---- configuration ------------------------------------------------------- */

static void copy_path(char *destination, const char *source)
{
    size_t length = 0;

    while (source[length] != '\0' && length + 1 < DESKTOP_WALLPAPER_PATH_MAX)
    {
        destination[length] = source[length];
        ++length;
    }
    destination[length] = '\0';
}

void desktop_wallpaper_default_config(struct desktop_wallpaper_config *config)
{
    if (config == 0)
    {
        return;
    }
    memset(config, 0, sizeof(*config));
    copy_path(config->path, DESKTOP_WALLPAPER_DEFAULT_IMAGE);
    config->position = DESKTOP_WALLPAPER_STRETCH;
    config->color = DESKTOP_WALLPAPER_DEFAULT_COLOR;
}

const char *desktop_wallpaper_position_name(int position)
{
    return position >= 0 && position < DESKTOP_WALLPAPER_POSITION_COUNT ? k_position_names[position] : "";
}

int desktop_wallpaper_config_equal(
    const struct desktop_wallpaper_config *left,
    const struct desktop_wallpaper_config *right)
{
    return left != 0 && right != 0 &&
           left->position == right->position &&
           left->color == right->color &&
           strcmp(left->path, right->path) == 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* Six hex digits, RRGGBB. Returns -1 for anything else. */
static int parse_color(const char *text, uint32_t *color)
{
    uint32_t value = 0;
    int index;

    for (index = 0; index < 6; ++index)
    {
        int digit = hex_digit(text[index]);

        if (digit < 0)
        {
            return -1;
        }
        value = (value << 4) | (uint32_t)digit;
    }
    if (text[6] != '\0')
    {
        return -1;
    }
    *color = value;
    return 0;
}

static void apply_legacy_mode(struct desktop_wallpaper_config *config, int mode)
{
    switch (mode)
    {
    case 0:
        config->path[0] = '\0';
        break;
    case 1:
        copy_path(config->path, DESKTOP_WALLPAPER_LEGACY_GRADIENT);
        config->position = DESKTOP_WALLPAPER_STRETCH;
        break;
    case 2:
        copy_path(config->path, DESKTOP_WALLPAPER_LEGACY_GRID);
        config->position = DESKTOP_WALLPAPER_TILE;
        break;
    default:
        /* The image mode: the default wallpaper is what /wallpaper.bmp was. */
        break;
    }
}

/* One "key=value" line. Unknown keys and malformed values are ignored, so a
 * file written by a newer system still loads what this one understands. */
static void parse_line(struct desktop_wallpaper_config *config, char *line)
{
    char *value = strchr(line, '=');
    int index;

    if (value == 0)
    {
        return;
    }
    *value++ = '\0';

    if (strcmp(line, "wallpaper") == 0)
    {
        /* Only absolute paths: the desktop's working directory is not the
         * dialog's, so a relative one would name two different files. */
        if (value[0] == '\0' || value[0] == '/')
        {
            copy_path(config->path, value);
        }
    }
    else if (strcmp(line, "position") == 0)
    {
        for (index = 0; index < DESKTOP_WALLPAPER_POSITION_COUNT; ++index)
        {
            if (strcmp(value, k_position_keys[index]) == 0)
            {
                config->position = index;
            }
        }
    }
    else if (strcmp(line, "color") == 0)
    {
        (void)parse_color(value, &config->color);
    }
}

int desktop_wallpaper_read_config(struct desktop_wallpaper_config *config)
{
    char text[DESKTOP_WALLPAPER_CONFIG_BYTES];
    char path[256];
    long total = 0;
    int fd;
    char *line;

    if (config == 0)
    {
        return -1;
    }
    desktop_wallpaper_default_config(config);

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

    if (total >= 1 && text[0] >= '0' && text[0] <= '3' &&
        (total == 1 || text[1] == '\n' || text[1] == '\r'))
    {
        apply_legacy_mode(config, text[0] - '0');
        return 0;
    }

    line = text;
    while (*line != '\0')
    {
        char *end = line;

        while (*end != '\0' && *end != '\n' && *end != '\r')
        {
            ++end;
        }
        if (*end != '\0')
        {
            *end++ = '\0';
        }
        parse_line(config, line);
        line = end;
    }
    return 0;
}

int desktop_wallpaper_write_config(const struct desktop_wallpaper_config *config)
{
    char text[DESKTOP_WALLPAPER_CONFIG_BYTES];
    char path[256];
    int position;
    int length;
    int fd;
    long written;

    if (config == 0)
    {
        return -1;
    }
    position = config->position >= 0 && config->position < DESKTOP_WALLPAPER_POSITION_COUNT
        ? config->position
        : DESKTOP_WALLPAPER_STRETCH;
    length = snprintf(text, sizeof(text), "wallpaper=%s\nposition=%s\ncolor=%06x\n",
        config->path, k_position_keys[position], (unsigned int)(config->color & 0xffffffu));
    if (length < 0 || length >= (int)sizeof(text))
    {
        return -1;
    }

    fd = savanxp_settings_path(DESKTOP_WALLPAPER_CONFIG_NAME, 1, path, sizeof(path)) == 0
        ? (int)savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE)
        : -1;
    if (fd < 0)
    {
        return -1;
    }
    written = savanxp_write(fd, text, (unsigned long)length);
    savanxp_close(fd);
    return written == length ? 0 : -1;
}

/* ---- images -------------------------------------------------------------- */

static uint16_t read_le16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

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
    if (image->section_fd > 0)
    {
        savanxp_close(image->section_fd);
    }
    image->section_fd = 0;
    image->width = 0;
    image->height = 0;
}

/*
 * Decodes an uncompressed BMP (BI_RGB) of 24 or 32 bpp, bottom-up or
 * top-down, into BGRX pixels ready for the painter. Anything outside that
 * profile is refused, and the caller falls back to the plain colour.
 *
 * The pixels live in a section of their own, not on the heap: an image is up
 * to 16 MiB and the desktop keeps it for as long as it is the wallpaper.
 */
int desktop_wallpaper_image_load(struct desktop_wallpaper_image *image, const char *path)
{
    unsigned char header[54];
    static unsigned char row[DESKTOP_WALLPAPER_MAX_DIMENSION * 4u];
    int fd;
    uint32_t pixel_offset = 0;
    int32_t raw_height = 0;
    int width = 0;
    int height = 0;
    int top_down = 0;
    unsigned int bytes_per_pixel = 0;
    unsigned long row_stride = 0;
    int loaded = 0;
    int y;

    if (image == 0)
    {
        return -1;
    }
    desktop_wallpaper_image_unload(image);
    if (path == 0 || path[0] == '\0')
    {
        return -1;
    }

    fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0)
    {
        return -1;
    }

    if (read_exact(fd, header, sizeof(header)) == 0 && header[0] == 'B' && header[1] == 'M')
    {
        uint16_t bpp = read_le16(header + 28);
        pixel_offset = read_le32(header + 10);
        width = (int)(int32_t)read_le32(header + 18);
        raw_height = (int32_t)read_le32(header + 22);
        top_down = raw_height < 0;
        height = top_down ? -(int)raw_height : (int)raw_height;

        if (read_le32(header + 14) >= 40u &&           /* BITMAPINFOHEADER or larger */
            read_le16(header + 26) == 1u &&            /* planes */
            (bpp == 24u || bpp == 32u) &&
            read_le32(header + 30) == 0u &&            /* BI_RGB */
            width > 0 && width <= DESKTOP_WALLPAPER_MAX_DIMENSION &&
            height > 0 && height <= DESKTOP_WALLPAPER_MAX_DIMENSION &&
            pixel_offset >= sizeof(header) &&
            seek(fd, (long)pixel_offset, SAVANXP_SEEK_SET) >= 0)
        {
            long section;

            bytes_per_pixel = bpp / 8u;
            row_stride = (((unsigned long)width * bytes_per_pixel) + 3ul) & ~3ul;
            section = section_create(
                (unsigned long)width * (unsigned long)height * sizeof(uint32_t),
                SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
            if (section > 0)
            {
                void *view = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);

                image->section_fd = (int)section;
                if (!result_is_error((long)view))
                {
                    image->pixels = (uint32_t *)view;
                    loaded = 1;
                    for (y = 0; y < height; ++y)
                    {
                        uint32_t *dest;
                        int x;

                        if (read_exact(fd, row, row_stride) < 0)
                        {
                            loaded = 0;
                            break;
                        }
                        dest = image->pixels + ((unsigned long)(top_down ? y : (height - 1 - y)) * (unsigned long)width);
                        for (x = 0; x < width; ++x)
                        {
                            const unsigned char *px = row + ((unsigned long)x * bytes_per_pixel);
                            dest[x] = ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | (uint32_t)px[0];
                        }
                    }
                }
            }
        }
    }

    savanxp_close(fd);
    if (loaded)
    {
        image->width = width;
        image->height = height;
        return 0;
    }
    desktop_wallpaper_image_unload(image);
    return -1;
}

/* ---- painting ------------------------------------------------------------ */

static uint32_t painter_color(uint32_t rgb)
{
    return gfx_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

/* A length on the screen, as it measures in the target. Never below one
 * pixel, so a small image still shows in a preview. */
static int scale_length(int length, int target, int screen)
{
    int scaled;

    if (screen <= 0 || target == screen)
    {
        return length;
    }
    scaled = (int)(((long)length * (long)target) / (long)screen);
    return scaled > 0 ? scaled : 1;
}

/* Copies one copy of the image to dest, scaling only when the sizes differ. */
static void place_image(
    struct sx_painter *painter,
    const struct sx_bitmap *bitmap,
    const struct desktop_wallpaper_image *image,
    struct sx_rect dest,
    int filter)
{
    if (dest.width == image->width && dest.height == image->height)
    {
        sx_painter_blit_bitmap(painter, bitmap, dest.x, dest.y);
        return;
    }
    sx_painter_draw_scaled_bitmap(
        painter,
        bitmap,
        dest,
        sx_rect_make(0, 0, image->width, image->height),
        filter);
}

void desktop_wallpaper_paint(
    struct sx_painter *painter,
    struct sx_rect target,
    int screen_width,
    int screen_height,
    const struct desktop_wallpaper_config *config,
    const struct desktop_wallpaper_image *image)
{
    struct savanxp_fb_info image_info;
    struct sx_bitmap bitmap;
    int position;

    if (painter == 0 || config == 0 || sx_rect_is_empty(target))
    {
        return;
    }
    if (!sx_painter_push_clip(painter, target))
    {
        return;
    }
    sx_painter_fill_rect(painter, target, painter_color(config->color));

    if (image == 0 || image->pixels == 0 || config->path[0] == '\0')
    {
        sx_painter_pop_clip(painter);
        return;
    }

    memset(&image_info, 0, sizeof(image_info));
    image_info.width = (uint32_t)image->width;
    image_info.height = (uint32_t)image->height;
    image_info.pitch = (uint32_t)image->width * (uint32_t)sizeof(uint32_t);
    image_info.bpp = 32u;
    image_info.buffer_size = image_info.pitch * image_info.height;
    sx_bitmap_wrap(&bitmap, image->pixels, &image_info, SX_PIXEL_FORMAT_BGRX8888);

    position = config->position;
    if (position == DESKTOP_WALLPAPER_STRETCH)
    {
        /* Bilinear and not nearest: the image rarely matches the screen, and
         * at an arbitrary factor nearest leaves visible steps across the
         * whole background. The desktop pays it once per change, not per
         * frame, because it caches the result. */
        place_image(painter, &bitmap, image, target, SX_SCALE_BILINEAR);
    }
    else if (position == DESKTOP_WALLPAPER_TILE)
    {
        const int tile_width = scale_length(image->width, target.width, screen_width);
        const int tile_height = scale_length(image->height, target.height, screen_height);
        /* Only the tiles that cross the active clip: a small tile on a large
         * screen is thousands of copies, and the clip is often a fragment. */
        struct sx_rect clip = sx_rect_intersect(sx_painter_clip_bounds(painter), target);
        int first_x = target.x + ((clip.x - target.x) / tile_width) * tile_width;
        int first_y = target.y + ((clip.y - target.y) / tile_height) * tile_height;
        int x;
        int y;

        for (y = first_y; y < clip.y + clip.height; y += tile_height)
        {
            for (x = first_x; x < clip.x + clip.width; x += tile_width)
            {
                place_image(painter, &bitmap, image, sx_rect_make(x, y, tile_width, tile_height), SX_SCALE_NEAREST);
            }
        }
    }
    else
    {
        const int width = scale_length(image->width, target.width, screen_width);
        const int height = scale_length(image->height, target.height, screen_height);

        place_image(
            painter,
            &bitmap,
            image,
            sx_rect_make(target.x + (target.width - width) / 2, target.y + (target.height - height) / 2, width, height),
            SX_SCALE_BILINEAR);
    }
    sx_painter_pop_clip(painter);
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
