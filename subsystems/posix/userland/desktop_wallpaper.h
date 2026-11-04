#pragma once

#include "libc.h"

/*
 * The desktop background: a solid colour with an optional BMP on top of it,
 * placed centred, tiled or stretched. The choice lives in the user's profile
 * (desktop.cfg) and is restored on the next boot. The format and the rules
 * are in docs/desktop/DISPLAY_PROPERTIES.md.
 *
 * Two kinds of process use this module. The desktop draws the background: it
 * keeps one configuration and one decoded image (desktop_wallpaper_init,
 * _reload, _draw). Display Properties changes it: it reads and writes the
 * configuration and paints previews from images of its own
 * (desktop_wallpaper_read_config, _write_config, _paint). The configuration
 * file is what keeps the two in step: the desktop re-reads it periodically.
 */

/* The shipped wallpapers. Display Properties lists every .bmp in it. */
#define DESKTOP_WALLPAPER_DIRECTORY "/wallpapers"
/* What a fresh profile shows. */
#define DESKTOP_WALLPAPER_DEFAULT_IMAGE DESKTOP_WALLPAPER_DIRECTORY "/Clouds.bmp"
#define DESKTOP_WALLPAPER_DEFAULT_COLOR 0x008080u
#define DESKTOP_WALLPAPER_PATH_MAX 256
/* Larger images are refused rather than decoded. */
#define DESKTOP_WALLPAPER_MAX_DIMENSION 2048

enum desktop_wallpaper_position
{
    DESKTOP_WALLPAPER_CENTER = 0,
    DESKTOP_WALLPAPER_TILE,
    DESKTOP_WALLPAPER_STRETCH,
    DESKTOP_WALLPAPER_POSITION_COUNT
};

struct desktop_wallpaper_config
{
    /* Absolute path of the BMP; empty means no image, only the colour. */
    char path[DESKTOP_WALLPAPER_PATH_MAX];
    int position;   /* enum desktop_wallpaper_position */
    uint32_t color; /* 0xRRGGBB, under the image or alone */
};

/* A decoded BMP, BGRX pixels in a section of their own. Zero-initialise it;
 * pixels == 0 means nothing is loaded. */
struct desktop_wallpaper_image
{
    int section_fd;
    uint32_t *pixels;
    int width;
    int height;
};

/* ---- configuration ------------------------------------------------------- */

void desktop_wallpaper_default_config(struct desktop_wallpaper_config *config);
/* Reads the user's configuration, migrating the old one-digit format. Leaves
 * the defaults and returns -1 when there is none. */
int desktop_wallpaper_read_config(struct desktop_wallpaper_config *config);
/* Returns 0 once written. */
int desktop_wallpaper_write_config(const struct desktop_wallpaper_config *config);
int desktop_wallpaper_config_equal(
    const struct desktop_wallpaper_config *left,
    const struct desktop_wallpaper_config *right);
/* "Center", "Tile", "Stretch". */
const char *desktop_wallpaper_position_name(int position);

/* ---- images -------------------------------------------------------------- */

/* Decodes an uncompressed 24 or 32 bpp BMP. Returns 0 on success; on failure
 * the image is left empty. Whatever the image held before is released. */
int desktop_wallpaper_image_load(struct desktop_wallpaper_image *image, const char *path);
void desktop_wallpaper_image_unload(struct desktop_wallpaper_image *image);

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

/* ---- the desktop's background ------------------------------------------- */

/* Loads the configuration and its image. Only the interactive session calls
 * it: the headless selftests keep the initial state (flat teal, no image) so
 * their frames stay deterministic. */
void desktop_wallpaper_init(void);
/* Re-reads the configuration and applies it if it changed. Returns 1 if it
 * did (and a repaint is due), 0 if not. */
int desktop_wallpaper_reload(void);
/* Paints the background and, over it, the system's name and version in the
 * bottom-right corner. */
void desktop_wallpaper_draw(struct sx_painter *painter, const struct savanxp_fb_info *info);

/* Height of the taskbar, to keep the version stamp above it. This module does
 * not know the desktop's layout, so it is a copy: desktop/desktop.c asserts it
 * matches DESKTOP_TASKBAR_HEIGHT. */
#define DESKTOP_WALLPAPER_TASKBAR_HEIGHT 28
