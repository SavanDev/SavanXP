#include "desktop_icons.h"

#include "desktop_icon_assets.h"

static const struct desktop_embedded_bitmap k_small_icons[DESKTOP_ICON_COUNT] = {
    {k_desktop_icon_desktop_16.width, k_desktop_icon_desktop_16.height, k_desktop_icon_desktop_16.pixels},
    {k_desktop_icon_shell_16.width, k_desktop_icon_shell_16.height, k_desktop_icon_shell_16.pixels},
    {k_desktop_icon_doom_16.width, k_desktop_icon_doom_16.height, k_desktop_icon_doom_16.pixels},
    {k_desktop_icon_gfx_demo_16.width, k_desktop_icon_gfx_demo_16.height, k_desktop_icon_gfx_demo_16.pixels},
    {k_desktop_icon_key_test_16.width, k_desktop_icon_key_test_16.height, k_desktop_icon_key_test_16.pixels},
    {k_desktop_icon_mouse_test_16.width, k_desktop_icon_mouse_test_16.height, k_desktop_icon_mouse_test_16.pixels},
};

static const struct desktop_embedded_bitmap k_large_icons[DESKTOP_ICON_COUNT] = {
    {k_desktop_icon_desktop_32.width, k_desktop_icon_desktop_32.height, k_desktop_icon_desktop_32.pixels},
    {k_desktop_icon_shell_32.width, k_desktop_icon_shell_32.height, k_desktop_icon_shell_32.pixels},
    {k_desktop_icon_doom_32.width, k_desktop_icon_doom_32.height, k_desktop_icon_doom_32.pixels},
    {k_desktop_icon_gfx_demo_32.width, k_desktop_icon_gfx_demo_32.height, k_desktop_icon_gfx_demo_32.pixels},
    {k_desktop_icon_key_test_32.width, k_desktop_icon_key_test_32.height, k_desktop_icon_key_test_32.pixels},
    {k_desktop_icon_mouse_test_32.width, k_desktop_icon_mouse_test_32.height, k_desktop_icon_mouse_test_32.pixels},
};

static const struct desktop_embedded_bitmap k_menu_strip_art = {
    k_desktop_menu_strip_placeholder.width,
    k_desktop_menu_strip_placeholder.height,
    k_desktop_menu_strip_placeholder.pixels,
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

const struct desktop_embedded_bitmap *desktop_menu_strip_art(void)
{
    return &k_menu_strip_art;
}
