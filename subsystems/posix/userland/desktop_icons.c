#include "desktop_icons.h"

#include "desktop_icon_assets.h"

static const struct desktop_embedded_bitmap k_small_icons[DESKTOP_ICON_COUNT] = {
    {k_desktop_icon_desktop_16.width, k_desktop_icon_desktop_16.height, k_desktop_icon_desktop_16.pixels},
};

static const struct desktop_embedded_bitmap k_large_icons[DESKTOP_ICON_COUNT] = {
    {k_desktop_icon_desktop_32.width, k_desktop_icon_desktop_32.height, k_desktop_icon_desktop_32.pixels},
};

const struct desktop_embedded_bitmap *desktop_icon_small(enum desktop_icon_id id)
{
    if (id < 0 || id >= DESKTOP_ICON_COUNT)
    {
        return 0;
    }
    return &k_small_icons[id];
}

const struct desktop_embedded_bitmap *desktop_icon_large(enum desktop_icon_id id)
{
    if (id < 0 || id >= DESKTOP_ICON_COUNT)
    {
        return 0;
    }
    return &k_large_icons[id];
}

