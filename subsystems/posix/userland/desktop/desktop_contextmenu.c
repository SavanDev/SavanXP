#include "desktop.h"

#include "desktop_wallpaper.h"

/*
 * The desktop's context menu: what a right click on the background opens
 * (docs/desktop/DESKTOP_ICONS.md). A popup of the desktop at the pointer,
 * drawn with the toolkit's popup menu so it looks like any menu bar's
 * drop-down. A popup never has the keyboard, so it is mouse only.
 *
 * Its items grow with the desktop: arranging icons and making shortcuts come
 * with the icon grid.
 */

#define CONTEXTMENU_CMD_WALLPAPER 1
#define CONTEXTMENU_CMD_CONSOLE 2

/* The Command Prompt, as Ctrl+Alt+T opens it (docs/desktop/CONSOLE.md). */
static const char *k_console_path = "/bin/console.sxe";

static const struct sxgui_menu_item k_items[] = {
    {"Change Wallpaper", CONTEXTMENU_CMD_WALLPAPER, 0},
    {0, 0, 0},
    {"Open Command Prompt", CONTEXTMENU_CMD_CONSOLE, 0},
};

static const struct sxgui_menu k_menu = {
    "Desktop",
    k_items,
    (int)(sizeof(k_items) / sizeof(k_items[0])),
};

static int g_hot_item = -1;
static uint32_t g_last_buttons = 0;

static void contextmenu_measure(int *width, int *height)
{
    sxgui_popup_menu_size(&k_menu, width, height);
}

static int contextmenu_open(void)
{
    g_hot_item = -1;
    /* It opens on a release, so no button is down over it yet. */
    g_last_buttons = 0;
    return 0;
}

static void contextmenu_close(void)
{
    g_hot_item = -1;
}

static void contextmenu_run(int id)
{
    switch (id)
    {
    case CONTEXTMENU_CMD_WALLPAPER:
        (void)desktop_wallpaper_cycle();
        desktop_repaint_background();
        break;
    case CONTEXTMENU_CMD_CONSOLE:
        (void)desktop_launch(k_console_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
        break;
    default:
        break;
    }
}

static struct sx_rect contextmenu_rect(const struct savanxp_gfx_context *gfx)
{
    return sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
}

/* As on Windows, an item fires when either button is released over it. */
static uint32_t contextmenu_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    const uint32_t buttons = SAVANXP_MOUSE_BUTTON_LEFT | SAVANXP_MOUSE_BUTTON_RIGHT;
    uint32_t up = g_last_buttons & ~pointer->buttons & buttons;
    int item = sxgui_popup_menu_item_at(contextmenu_rect(gfx), &k_menu, pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    g_last_buttons = pointer->buttons;
    if (item != g_hot_item)
    {
        g_hot_item = item;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (up != 0 && sxgui_popup_menu_item_selectable(&k_menu, item))
    {
        contextmenu_run(k_items[item].id);
        actions |= DESKTOP_ACTION_CLOSE;
    }
    return actions;
}

/* Nothing stays highlighted once the pointer is back on the background. */
static uint32_t contextmenu_pointer_left(void)
{
    if (g_hot_item < 0)
    {
        return DESKTOP_ACTION_NONE;
    }
    g_hot_item = -1;
    return DESKTOP_ACTION_REPAINT;
}

static void contextmenu_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    sxgui_popup_menu_paint(&painter, contextmenu_rect(gfx), &k_menu, g_hot_item);
}

const struct desktop_popup_ops desktop_contextmenu_ops = {
    0,
    0,
    0,
    contextmenu_open,
    contextmenu_close,
    contextmenu_pointer,
    0,
    contextmenu_paint,
    0,
    contextmenu_pointer_left,
    contextmenu_measure,
};
