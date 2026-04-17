#include "libc.h"
#include "desktop_menu.h"

#define DESKTOP_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

static const struct desktop_menu_item k_menu_items[] = {
    {"Shell", "/bin/shellapp", "Terminal and builtins", DESKTOP_ICON_SHELL, DESKTOP_RGB_LITERAL(0, 124, 96)},
    {"Doom", "/disk/bin/doomgeneric", "Classic FPS test port", DESKTOP_ICON_DOOM, DESKTOP_RGB_LITERAL(181, 81, 55)},
    {"Gfx Demo", "/bin/gfxdemo", "2D rendering test", DESKTOP_ICON_GFX_DEMO, DESKTOP_RGB_LITERAL(34, 142, 96)},
    {"Key Test", "/bin/keytest", "Keyboard diagnostics", DESKTOP_ICON_KEY_TEST, DESKTOP_RGB_LITERAL(41, 111, 188)},
    {"Mouse Test", "/bin/mousetest", "Mouse diagnostics", DESKTOP_ICON_MOUSE_TEST, DESKTOP_RGB_LITERAL(156, 104, 38)},
};

int desktop_menu_item_count(void)
{
    return (int)(sizeof(k_menu_items) / sizeof(k_menu_items[0]));
}

const struct desktop_menu_item *desktop_menu_item_at(int index)
{
    if (index < 0 || index >= desktop_menu_item_count())
    {
        return 0;
    }
    return &k_menu_items[index];
}

const struct desktop_menu_item *desktop_find_menu_item_by_path(const char *path)
{
    int index;

    if (path == 0)
    {
        return 0;
    }

    for (index = 0; index < desktop_menu_item_count(); ++index)
    {
        if (strcmp(k_menu_items[index].path, path) == 0)
        {
            return &k_menu_items[index];
        }
    }
    return 0;
}
