#pragma once

#include "libc.h"

struct desktop_menu_item
{
    const char *label;
    const char *path;
    const char *subtitle;
    uint32_t accent;
};

int desktop_menu_item_count(void);
const struct desktop_menu_item *desktop_menu_item_at(int index);
const struct desktop_menu_item *desktop_find_menu_item_by_path(const char *path);
