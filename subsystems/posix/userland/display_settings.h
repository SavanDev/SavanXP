#pragma once

#include "libc.h"

/*
 * The screen's resolution, as the user chose it in Display Properties:
 * display.cfg in the user's profile, one line, "mode=<width>x<height>". The
 * desktop applies it when it starts (gfx_set_display_mode); without the file
 * the session keeps the mode it booted with. See
 * docs/desktop/DISPLAY_PROPERTIES.md.
 */

/* 0 and the saved mode, or -1 when there is none or it cannot be read. */
int display_settings_read_mode(uint32_t *width, uint32_t *height);
/* Returns 0 once written. */
int display_settings_write_mode(uint32_t width, uint32_t height);
