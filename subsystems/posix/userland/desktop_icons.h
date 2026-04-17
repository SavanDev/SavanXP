#pragma once

#include "libc.h"

enum desktop_icon_id
{
    DESKTOP_ICON_DESKTOP = 0,
    DESKTOP_ICON_SHELL,
    DESKTOP_ICON_DOOM,
    DESKTOP_ICON_GFX_DEMO,
    DESKTOP_ICON_KEY_TEST,
    DESKTOP_ICON_MOUSE_TEST,
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
const struct desktop_embedded_bitmap *desktop_menu_strip_art(void);
