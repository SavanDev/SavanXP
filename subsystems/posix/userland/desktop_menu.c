#include "libc.h"
#include "desktop_menu.h"

#define DESKTOP_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

static const struct desktop_menu_item k_menu_items[] = {
    {"Shell", "/bin/shellapp", "Terminal and builtins", DESKTOP_ICON_SHELL, DESKTOP_RGB_LITERAL(0, 124, 96), DESKTOP_MENU_ITEM_FLAG_SHORTCUT},
    {"Files", "/bin/filesapp", "Browse /disk and preview files", DESKTOP_ICON_DESKTOP, DESKTOP_RGB_LITERAL(186, 128, 36), DESKTOP_MENU_ITEM_FLAG_SHORTCUT},
    {"About", "/bin/aboutapp", "System overview and help", DESKTOP_ICON_DESKTOP, DESKTOP_RGB_LITERAL(58, 104, 190), DESKTOP_MENU_ITEM_FLAG_SHORTCUT},
    {"Widgets", "/bin/widgetsdemo", "sxgui control gallery", DESKTOP_ICON_DESKTOP, DESKTOP_RGB_LITERAL(96, 110, 140), DESKTOP_MENU_ITEM_FLAG_SHORTCUT},
    {"Doom", "/disk/bin/doomgeneric", "Classic FPS test port", DESKTOP_ICON_DOOM, DESKTOP_RGB_LITERAL(181, 81, 55), DESKTOP_MENU_ITEM_FLAG_SHORTCUT | DESKTOP_MENU_ITEM_FLAG_FULLSCREEN},
    {"Gfx Demo", "/bin/gfxdemo", "2D rendering test", DESKTOP_ICON_GFX_DEMO, DESKTOP_RGB_LITERAL(34, 142, 96), DESKTOP_MENU_ITEM_FLAG_SHORTCUT | DESKTOP_MENU_ITEM_FLAG_FULLSCREEN},
    {"Key Test", "/bin/keytest", "Keyboard diagnostics", DESKTOP_ICON_KEY_TEST, DESKTOP_RGB_LITERAL(41, 111, 188), 0},
    {"Mouse Test", "/bin/mousetest", "Mouse diagnostics", DESKTOP_ICON_MOUSE_TEST, DESKTOP_RGB_LITERAL(156, 104, 38), 0},
};

static const struct desktop_power_item k_power_items[] = {
    {"Apagar", DESKTOP_CONFIRM_SHUTDOWN},
    {"Reiniciar", DESKTOP_CONFIRM_REBOOT},
};

int desktop_menu_item_count(void)
{
    return (int)(sizeof(k_menu_items) / sizeof(k_menu_items[0]));
}

int desktop_power_item_count(void)
{
    return (int)(sizeof(k_power_items) / sizeof(k_power_items[0]));
}

const struct desktop_power_item *desktop_power_item_at(int index)
{
    if (index < 0 || index >= desktop_power_item_count())
    {
        return 0;
    }
    return &k_power_items[index];
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

int desktop_shortcut_count(void)
{
    int index;
    int count = 0;

    for (index = 0; index < desktop_menu_item_count(); ++index)
    {
        if ((k_menu_items[index].flags & DESKTOP_MENU_ITEM_FLAG_SHORTCUT) != 0)
        {
            count += 1;
        }
    }
    return count;
}

const struct desktop_menu_item *desktop_shortcut_at(int index)
{
    int menu_index;
    int shortcut_index = 0;

    if (index < 0)
    {
        return 0;
    }

    for (menu_index = 0; menu_index < desktop_menu_item_count(); ++menu_index)
    {
        if ((k_menu_items[menu_index].flags & DESKTOP_MENU_ITEM_FLAG_SHORTCUT) == 0)
        {
            continue;
        }
        if (shortcut_index == index)
        {
            return &k_menu_items[menu_index];
        }
        shortcut_index += 1;
    }
    return 0;
}
