#pragma once

#include "libc.h"
#include "desktop_icons.h"

struct desktop_menu_item
{
    const char *label;
    const char *path;
    const char *subtitle;
    enum desktop_icon_id icon_id;
    uint32_t accent;
};

int desktop_menu_item_count(void);
const struct desktop_menu_item *desktop_menu_item_at(int index);
const struct desktop_menu_item *desktop_find_menu_item_by_path(const char *path);
