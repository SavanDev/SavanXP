#pragma once

#include "libc.h"
#include "sxgfx/wallpaper.h"

/*
 * The desktop background, the POSIX side: what the desktop needs to draw it.
 * It keeps one configuration and one decoded image (desktop_wallpaper_init,
 * _reload, _draw), read from the user's profile (desktop.cfg) and restored on
 * the next boot. The format, the parsing and the painting are shared with
 * Display Properties, the SUN program that edits the configuration
 * (sxgfx/wallpaper.h); the format and the rules are in
 * docs/desktop/DISPLAY_PROPERTIES.md. The configuration file is what keeps
 * the two in step: the desktop re-reads it periodically.
 */

/* ---- configuration ------------------------------------------------------- */

/* Reads the user's configuration, migrating the old one-digit format. Leaves
 * the defaults and returns -1 when there is none. */
int desktop_wallpaper_read_config(struct desktop_wallpaper_config *config);

/* ---- images -------------------------------------------------------------- */

/* Decodes the BMP, PNG and sequential JPEG formats supported by libsximg.
 * Returns 0 on success; on failure the image is left empty. Whatever the
 * image held before is released. */
int desktop_wallpaper_image_load(struct desktop_wallpaper_image *image, const char *path);
void desktop_wallpaper_image_unload(struct desktop_wallpaper_image *image);

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
