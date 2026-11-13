#pragma once

#include "libc.h"
#include "shortcut.h"

/*
 * The folder of what is pinned to the Start menu (docs/desktop/START_MENU.md,
 * "Shortcuts"): "Start Menu" in the user's profile, one .sxlnk per pin. The
 * desktop reads it (start_shortcuts.h) and unpins from it; Files pins to it,
 * as it sends to the Desktop folder.
 *
 * The folder is the user's alone. The system only seeds it: the first time
 * it is needed and missing, it is created with a copy of each .sxlnk in
 * /system/start, the defaults the build installs (Windows' Default User
 * profile). Seeded once, so a default the user unpins stays unpinned.
 */

#define START_PINS_MAX 8
#define START_PINS_DEFAULTS_DIR "/system/start"
#define START_PINS_FOLDER "Start Menu" /* in the profile */

enum start_pin_result
{
    START_PIN_DONE = 0,
    START_PIN_ALREADY, /* a pin with the same target and argument is there */
    START_PIN_FULL,    /* START_PINS_MAX pins already */
    START_PIN_FAILED,
};

/* The folder's path, created and seeded if it is missing. 0, or -1 when it
 * cannot be made. */
int start_pins_folder(char *out, size_t capacity);
/* Non-zero for what can be pinned: a program (shortcut_is_program_path), a
 * Windows program (.exe), or a shortcut, whose copy is pinned. */
int start_pins_can_pin(const char *path);
/* Pins `path`: a shortcut to it named as Send To Desktop names one, without
 * ".exe" for a Windows program, which starts in its own folder. */
enum start_pin_result start_pins_pin(const char *path);
/* Removes the pin `shortcut_path`, a .sxlnk in the folder. 0, or -1. */
int start_pins_unpin(const char *shortcut_path);
