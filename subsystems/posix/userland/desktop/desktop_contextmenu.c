#include "desktop.h"

/*
 * The desktop's context menus: the background's and an icon's
 * (docs/desktop/DESKTOP_ICONS.md). A popup of the desktop at the pointer,
 * drawn with the toolkit's popup menu so it looks like any menu bar's
 * drop-down. The popup never has the keyboard; the desktop's main window
 * does once the background is clicked, and passes the menu its keys.
 *
 * Which menu it shows and what its items do is the caller's: this module only
 * shows it and reports the id chosen.
 */

static const struct sxgui_menu *g_menu = 0;
static void (*g_command)(int id) = 0;
static int g_hot_item = -1;
static uint32_t g_last_buttons = 0;
/* The command chosen, run once the popup is closed: it may open another. */
static int g_pending_id = 0;

void desktop_context_menu(const struct sxgui_menu *menu, void (*command)(int id), int x, int y)
{
    g_menu = menu;
    g_command = command;
    desktop_open_popup_at(DESKTOP_POPUP_CONTEXT_MENU, x, y);
}

static void contextmenu_measure(int *width, int *height)
{
    sxgui_popup_menu_size(g_menu, width, height);
}

static int contextmenu_open(void)
{
    if (g_menu == 0)
    {
        return -1;
    }
    g_hot_item = -1;
    g_pending_id = 0;
    /* It opens on a release, so no button is down over it yet. */
    g_last_buttons = 0;
    return 0;
}

static void contextmenu_close(void)
{
    int id = g_pending_id;

    g_hot_item = -1;
    g_pending_id = 0;
    if (id != 0 && g_command != 0)
    {
        g_command(id);
    }
}

static struct sx_rect contextmenu_rect(const struct savanxp_gfx_context *gfx)
{
    return sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
}

static uint32_t contextmenu_choose(int item)
{
    g_pending_id = g_menu->items[item].id;
    return DESKTOP_ACTION_CLOSE;
}

/* As on Windows, an item fires when either button is released over it. */
static uint32_t contextmenu_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    const uint32_t buttons = SAVANXP_MOUSE_BUTTON_LEFT | SAVANXP_MOUSE_BUTTON_RIGHT;
    uint32_t up = g_last_buttons & ~pointer->buttons & buttons;
    int item = sxgui_popup_menu_item_at(contextmenu_rect(gfx), g_menu, pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    g_last_buttons = pointer->buttons;
    if (item != g_hot_item)
    {
        g_hot_item = item;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (up != 0 && sxgui_popup_menu_item_selectable(g_menu, item))
    {
        actions |= contextmenu_choose(item);
    }
    return actions;
}

uint32_t desktop_context_menu_key(const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || g_menu == 0)
    {
        return DESKTOP_ACTION_NONE;
    }
    switch (event->key)
    {
    case SAVANXP_KEY_ESC:
        return DESKTOP_ACTION_CLOSE;
    case SAVANXP_KEY_UP:
    case SAVANXP_KEY_DOWN:
        g_hot_item = sxgui_popup_menu_step(g_menu, g_hot_item, event->key == SAVANXP_KEY_DOWN ? 1 : -1);
        return DESKTOP_ACTION_REPAINT;
    case SAVANXP_KEY_ENTER:
        if (sxgui_popup_menu_item_selectable(g_menu, g_hot_item))
        {
            return contextmenu_choose(g_hot_item);
        }
        return DESKTOP_ACTION_NONE;
    default:
        return DESKTOP_ACTION_NONE;
    }
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
    sxgui_popup_menu_paint(&painter, contextmenu_rect(gfx), g_menu, g_hot_item);
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
