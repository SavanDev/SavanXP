#pragma once

#include <stdint.h>

/*
 * The baked-in set came down to the generic icon. Doom, Shell, Notepad, Gfx
 * Demo, Key Test and Mouse Test had their entry here; all six left once each
 * got its own .sxicon (icon_file= or icon=<name> in its .sxres, stamped at
 * build time), and the tables that named them by id (progman_registry.c, and
 * windowd's per-path table until it was removed) were pointed at the generic
 * one as their fallback -- docs/userland/SXE_FORMAT.md, "icon= no longer
 * picks from a catalog".
 *
 * What is left is the universal safety net: the icon for ANY program whose
 * binary cannot be read at all. There can be no entry per app, because there
 * is no other id a binary might fail to have -- the generic one itself is the
 * last resort, not one more option.
 *
 * Plain data: shared by windowd, the desktop and Program Manager, whatever
 * subsystem each is written against.
 */
enum desktop_icon_id
{
    DESKTOP_ICON_DESKTOP = 0,
    DESKTOP_ICON_COUNT
};

struct desktop_embedded_bitmap
{
    uint32_t width;
    uint32_t height;
    const uint32_t *pixels;
};

const struct desktop_embedded_bitmap *desktop_icon_small(enum desktop_icon_id id);
const struct desktop_embedded_bitmap *desktop_icon_large(enum desktop_icon_id id);
