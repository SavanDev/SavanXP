#include "libc.h"
#include "windowd_appinfo.h"

#define WINDOWD_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

/* Las apps de diagnostico se compilan solo si el build las pide (build.ps1
 * -NoTestApps las excluye del rootfs y de esta tabla a la vez). */
#ifndef DESKTOP_INCLUDE_TEST_APPS
#define DESKTOP_INCLUDE_TEST_APPS 1
#endif

/* Presentacion por path: nombre, icono y color de la barra de titulo. Un path
 * ausente no es un error -- la ventana usa defaults genericos. */
static const struct windowd_appinfo k_window_items[] = {
    {"Program Manager", "/bin/progman", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(66, 92, 150)},
    {"Shell", "/bin/shellapp", DESKTOP_ICON_SHELL, WINDOWD_RGB_LITERAL(0, 124, 96)},
    {"Files", "/bin/filesapp", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(186, 128, 36)},
    {"About", "/bin/aboutapp", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(58, 104, 190)},
    {"Notepad", "/bin/notepad", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(120, 100, 60)},
    {"Doom", "/disk/bin/doomgeneric", DESKTOP_ICON_DOOM, WINDOWD_RGB_LITERAL(181, 81, 55)},
#if DESKTOP_INCLUDE_TEST_APPS
    {"Widgets", "/bin/widgetsdemo", DESKTOP_ICON_DESKTOP, WINDOWD_RGB_LITERAL(96, 110, 140)},
    {"Gfx Demo", "/bin/gfxdemo", DESKTOP_ICON_GFX_DEMO, WINDOWD_RGB_LITERAL(34, 142, 96)},
    {"Key Test", "/bin/keytest", DESKTOP_ICON_KEY_TEST, WINDOWD_RGB_LITERAL(41, 111, 188)},
    {"Mouse Test", "/bin/mousetest", DESKTOP_ICON_MOUSE_TEST, WINDOWD_RGB_LITERAL(156, 104, 38)},
#endif
};

const struct windowd_appinfo *windowd_appinfo_for_path(const char *path)
{
    int index;
    const int count = (int)(sizeof(k_window_items) / sizeof(k_window_items[0]));

    if (path == 0)
    {
        return 0;
    }

    for (index = 0; index < count; ++index)
    {
        if (strcmp(k_window_items[index].path, path) == 0)
        {
            return &k_window_items[index];
        }
    }
    return 0;
}
