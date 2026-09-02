#pragma once

#include "libc.h"

enum desktop_icon_id
{
    DESKTOP_ICON_DESKTOP = 0,
    DESKTOP_ICON_SHELL,
    /* Doom tenia su entrada aca (DESKTOP_ICON_DOOM / app-spider.png). Se fue
     * del set: Doom se construye con un build aparte y ahora trae su propio
     * icono en sdk/doomgeneric/icon.png, via icon_file= en su .sxres. */
    DESKTOP_ICON_GFX_DEMO,
    DESKTOP_ICON_KEY_TEST,
    DESKTOP_ICON_MOUSE_TEST,
    DESKTOP_ICON_NOTEPAD,
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
