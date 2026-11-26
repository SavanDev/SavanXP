#pragma once

#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/gfx2d.h"

/*
 * The desktop's background, the SUN side: desktop.cfg read from the
 * person's profile, its image decoded, and the background painted with the
 * system's name and version in the corner. The format, the parsing and the
 * painting are shared with Display Properties, which edits the file
 * (system/gfx/wallpaper.c, sxgfx/wallpaper.h); the format and the rules are
 * in docs/desktop/DISPLAY_PROPERTIES.md. The file keeps the two in step: the
 * desktop reads it again now and then.
 *
 * The POSIX side, for a POSIX program, stays in
 * subsystems/posix/userland/desktop_wallpaper.c and display_settings.c.
 */

/* Loads the configuration and decodes its image. */
void desktop_background_init(void);
/* Reads the configuration again and takes it if it changed. 1 when it did
 * (a repaint is due), 0 when not. */
int desktop_background_reload(void);
/* The background and, over it, the system's name and version in the
 * bottom-right corner, above the taskbar. */
void desktop_background_draw(struct sx_painter *painter, const struct savanxp_fb_info *info);

/* The resolution the person chose in Display Properties (display.cfg, one
 * line "mode=<width>x<height>"). 0 with it, -1 when there is none. */
int desktop_read_display_mode(uint32_t *width, uint32_t *height);
