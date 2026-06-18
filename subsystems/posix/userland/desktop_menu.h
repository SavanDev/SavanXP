#pragma once

#include "libc.h"
#include "desktop_icons.h"

#define DESKTOP_MENU_ITEM_FLAG_SHORTCUT 0x00000001u

struct desktop_menu_item
{
    const char *label;
    const char *path;
    const char *subtitle;
    enum desktop_icon_id icon_id;
    uint32_t accent;
    uint32_t flags;
};

int desktop_menu_item_count(void);
const struct desktop_menu_item *desktop_menu_item_at(int index);
const struct desktop_menu_item *desktop_find_menu_item_by_path(const char *path);
int desktop_shortcut_count(void);
const struct desktop_menu_item *desktop_shortcut_at(int index);
