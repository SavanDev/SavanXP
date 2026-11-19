#include "sxgfx/wallpaper.h"

#include "savanxp/img.h"

#include <stdio.h>
#include <string.h>

/* The old format was one digit, a mode: 0 teal, 1 gradient, 2 grid pattern,
 * 3 /wallpaper.bmp. These are the wallpapers each one became. */
#define DESKTOP_WALLPAPER_LEGACY_GRADIENT DESKTOP_WALLPAPER_DIRECTORY "/Gradient.bmp"
#define DESKTOP_WALLPAPER_LEGACY_GRID DESKTOP_WALLPAPER_DIRECTORY "/Grid.bmp"
/* Where the shipped wallpapers were before they moved under /system. A saved
 * choice that names one of them is read as its new path. */
#define DESKTOP_WALLPAPER_OLD_DIRECTORY "/wallpapers/"

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
        size_t old_length = strlen(DESKTOP_WALLPAPER_OLD_DIRECTORY);

        if (strncmp(value, DESKTOP_WALLPAPER_OLD_DIRECTORY, old_length) == 0)
        {
            (void)snprintf(config->path, sizeof(config->path), "%s/%s", DESKTOP_WALLPAPER_DIRECTORY,
                value + old_length);
        }
        else if (value[0] == '\0' || value[0] == '/')
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

void desktop_wallpaper_parse_config(struct desktop_wallpaper_config *config, char *text)
{
    size_t total;
    char *line;

    if (config == 0)
    {
        return;
    }
    desktop_wallpaper_default_config(config);
    if (text == 0)
    {
        return;
    }
    total = strlen(text);

    if (total >= 1 && text[0] >= '0' && text[0] <= '3' &&
        (total == 1 || text[1] == '\n' || text[1] == '\r'))
    {
        apply_legacy_mode(config, text[0] - '0');
        return;
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
}

int desktop_wallpaper_format_config(const struct desktop_wallpaper_config *config, char *out, size_t capacity)
{
    int position;
    int length;

    if (config == 0 || out == 0 || capacity == 0)
    {
        return -1;
    }
    position = config->position >= 0 && config->position < DESKTOP_WALLPAPER_POSITION_COUNT
        ? config->position
        : DESKTOP_WALLPAPER_STRETCH;
    length = snprintf(out, capacity, "wallpaper=%s\nposition=%s\ncolor=%06x\n",
        config->path, k_position_keys[position], (unsigned int)(config->color & 0xffffffu));
    return length < 0 || (size_t)length >= capacity ? -1 : length;
}

/* ---- images -------------------------------------------------------------- */

/* JPEG selects the first scale that fits the wallpaper output budget; alpha
 * is composited onto white. */
static const struct sx_image_options k_image_options = {
    DESKTOP_WALLPAPER_MAX_DIMENSION, DESKTOP_WALLPAPER_MAX_DIMENSION,
    DESKTOP_WALLPAPER_MAX_DIMENSION * DESKTOP_WALLPAPER_MAX_DIMENSION, 0u
};

int desktop_wallpaper_image_plan(const void *data, size_t size, int *width, int *height)
{
    struct sx_image_metadata decoded = {0};

    if (data == 0 || size == 0 || width == 0 || height == 0 ||
        sx_image_info(data, size, &k_image_options, &decoded) != SX_IMAGE_OK ||
        decoded.width == 0 || decoded.height == 0)
    {
        return -1;
    }
    *width = (int)decoded.width;
    *height = (int)decoded.height;
    return 0;
}

int desktop_wallpaper_image_decode(const void *data, size_t size, uint32_t *pixels, size_t count)
{
    if (data == 0 || pixels == 0)
    {
        return -1;
    }
    return sx_image_decode_into(data, size, &k_image_options, pixels, count, 0) == SX_IMAGE_OK ? 0 : -1;
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
