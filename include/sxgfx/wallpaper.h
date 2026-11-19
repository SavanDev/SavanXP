#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sxgfx/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The desktop background, the part both subsystems share: a solid colour with
 * an optional image on top of it, placed centred, tiled or stretched. The
 * POSIX desktop draws it (subsystems/posix/userland/desktop_wallpaper.c) and
 * Display Properties, a SUN program (system/display), edits it and previews
 * it. The format and the rules are in docs/desktop/DISPLAY_PROPERTIES.md.
 *
 * This unit makes no system call. Reading and writing desktop.cfg, and the
 * memory a decoded image lives in, belong to each program: it hands this unit
 * the bytes and gets back a configuration, or pixels to paint.
 */

/* The shipped wallpapers. Display lists .bmp, .png, .jpg and .jpeg files. */
#define DESKTOP_WALLPAPER_DIRECTORY "/system/wallpapers"
/* What a fresh profile shows. */
#define DESKTOP_WALLPAPER_DEFAULT_IMAGE DESKTOP_WALLPAPER_DIRECTORY "/Clouds.bmp"
#define DESKTOP_WALLPAPER_DEFAULT_COLOR 0x008080u
#define DESKTOP_WALLPAPER_PATH_MAX 256
/* Decoded output ceiling; larger JPEG sources may fit at a reduced scale. */
#define DESKTOP_WALLPAPER_MAX_DIMENSION 2048
/* The largest file worth reading: the pixels of the largest image accepted at
 * 32 bpp, plus room for any header. */
#define DESKTOP_WALLPAPER_MAX_FILE_BYTES \
    ((unsigned long)DESKTOP_WALLPAPER_MAX_DIMENSION * DESKTOP_WALLPAPER_MAX_DIMENSION * 4ul + 65536ul)

/* Height of the taskbar, for the version stamp to stay above it and for the
 * previews to draw one. This unit does not know the desktop's layout, so it is
 * a copy: desktop/desktop.c asserts it matches DESKTOP_TASKBAR_HEIGHT. */
#define DESKTOP_WALLPAPER_TASKBAR_HEIGHT 28

/* The user's choice, in the profile's Settings folder. */
#define DESKTOP_WALLPAPER_CONFIG_NAME "desktop.cfg"
/* Big enough for the three keys with a full-length path. */
#define DESKTOP_WALLPAPER_CONFIG_BYTES (DESKTOP_WALLPAPER_PATH_MAX + 96)

enum desktop_wallpaper_position
{
    DESKTOP_WALLPAPER_CENTER = 0,
    DESKTOP_WALLPAPER_TILE,
    DESKTOP_WALLPAPER_STRETCH,
    DESKTOP_WALLPAPER_POSITION_COUNT
};

struct desktop_wallpaper_config
{
    /* Absolute path of the image; empty means no image, only the colour. */
    char path[DESKTOP_WALLPAPER_PATH_MAX];
    int position;   /* enum desktop_wallpaper_position */
    uint32_t color; /* 0xRRGGBB, under the image or alone */
};

/* A decoded image, BGRX pixels in a section of their own. Zero-initialise it;
 * pixels == 0 means nothing is loaded. `section` is the handle of the memory,
 * owned by whichever program loaded the image. */
struct desktop_wallpaper_image
{
    long section;
    uint32_t *pixels;
    int width;
    int height;
};

/* ---- configuration ------------------------------------------------------- */

void desktop_wallpaper_default_config(struct desktop_wallpaper_config *config);
/* The defaults, then what `text` (a NUL-terminated desktop.cfg) says, with the
 * old one-digit format migrated. `text` is cut into lines in place. */
void desktop_wallpaper_parse_config(struct desktop_wallpaper_config *config, char *text);
/* desktop.cfg for `config`, NUL-terminated. Its length, or -1 when it does not
 * fit in `capacity` (DESKTOP_WALLPAPER_CONFIG_BYTES always does). */
int desktop_wallpaper_format_config(const struct desktop_wallpaper_config *config, char *out, size_t capacity);
int desktop_wallpaper_config_equal(
    const struct desktop_wallpaper_config *left,
    const struct desktop_wallpaper_config *right);
/* "Center", "Tile", "Stretch". */
const char *desktop_wallpaper_position_name(int position);

/* ---- images -------------------------------------------------------------- */

/* BMP, PNG and sequential JPEG through libsximg, told apart by content, with
 * the same limits for the desktop and the previews. Plan first: 0 and the
 * decoded size, or -1 when the bytes are not an image this system shows.
 * Then decode into width * height pixels: 0, or -1. */
int desktop_wallpaper_image_plan(const void *data, size_t size, int *width, int *height);
int desktop_wallpaper_image_decode(const void *data, size_t size, uint32_t *pixels, size_t count);

/* Paints a background into `target` as it would look on a screen of
 * screen_width x screen_height: at the screen's size it is the desktop
 * itself, smaller it is a preview. `image` may be 0 or empty. */
void desktop_wallpaper_paint(
    struct sx_painter *painter,
    struct sx_rect target,
    int screen_width,
    int screen_height,
    const struct desktop_wallpaper_config *config,
    const struct desktop_wallpaper_image *image);

#ifdef __cplusplus
}
#endif
