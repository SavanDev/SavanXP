#pragma once

#include "libc.h"
#include "shortcut.h"
#include "start_pins.h"

/*
 * The Start menu's own shortcuts (docs/desktop/START_MENU.md, "Shortcuts"):
 * the .sxlnk files pinned in the user's "Start Menu" folder (start_pins.h),
 * listed above Programs as Windows' classic Start menu lists what is pinned
 * to it. A shortcut whose target is missing is left out, so a default the
 * build seeds for a program the user supplies (Solitaire's) shows only once
 * the program is there.
 *
 * Each has its file's name without .sxlnk and a 16x16 icon: its icon= or its
 * target's, from a program's .sxicon or, for a Windows program, its own PE
 * icon (sxgfx/pe_icon.h). Sorted by name. No malloc: fixed capacities.
 */

#define START_SHORTCUTS_MAX START_PINS_MAX
#define START_SHORTCUTS_NAME_CAPACITY 32
#define START_SHORTCUTS_ICON_EXTENT 16u

struct start_shortcut
{
    char name[START_SHORTCUTS_NAME_CAPACITY];
    char path[SHORTCUT_PATH_CAPACITY]; /* the .sxlnk */
    int has_icon;
    uint32_t icon[START_SHORTCUTS_ICON_EXTENT * START_SHORTCUTS_ICON_EXTENT]; /* BGRA8888 */
};

/* Reads the folder again; how many shortcuts there are. */
int start_shortcuts_load(void);
int start_shortcuts_count(void);
const struct start_shortcut *start_shortcuts_at(int index);
