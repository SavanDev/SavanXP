#include "desktop.h"

#include "sxn/sxn.h"
#include "sxn/sxnprofile.h"

/*
 * The keyboard layout selector (ES/EN): a popup of the desktop, anchored in
 * the tray corner above the layout button. Mouse only; it closes as soon as a
 * row is chosen.
 */

#define KBDLAYOUTPOPUP_WIDTH 96
#define KBDLAYOUTPOPUP_HEIGHT 44
#define KBDLAYOUTPOPUP_MARGIN 2
#define KBDLAYOUTPOPUP_ROW_COUNT 2
#define KBDLAYOUTPOPUP_ROW_HEIGHT 20

static const char *k_row_labels[KBDLAYOUTPOPUP_ROW_COUNT] = {"Spanish", "English"};

static struct sx_rect popup_row_rect(const struct savanxp_fb_info *info, int row)
{
    return sx_rect_make(
        KBDLAYOUTPOPUP_MARGIN,
        KBDLAYOUTPOPUP_MARGIN + row * KBDLAYOUTPOPUP_ROW_HEIGHT,
        (int)info->width - (KBDLAYOUTPOPUP_MARGIN * 2),
        KBDLAYOUTPOPUP_ROW_HEIGHT);
}

static int popup_hit(const struct savanxp_fb_info *info, int x, int y)
{
    int row;

    for (row = 0; row < KBDLAYOUTPOPUP_ROW_COUNT; ++row)
    {
        struct sx_rect rect = popup_row_rect(info, row);
        if (x >= rect.x && x < rect.x + rect.width &&
            y >= rect.y && y < rect.y + rect.height)
        {
            return row;
        }
    }
    return -1;
}

static void popup_paint(struct desktop_surface *gfx, int current_layout, int hot_row)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    int row;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, frame, 0);

    for (row = 0; row < KBDLAYOUTPOPUP_ROW_COUNT; ++row)
    {
        struct sx_rect rect = popup_row_rect(&gfx->info, row);
        int selected = row == hot_row;
        uint32_t background = selected ? SXGUI_COLOR_SELECT : SXGUI_COLOR_FACE;
        uint32_t text_color = selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT;
        int text_x = rect.x + 4;
        int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

        sx_painter_fill_rect(&painter, rect, background);
        /* The active layout's row carries a mark on its left -- this face has
         * no bold, so a prefix is the simple way to set it apart without
         * another drawing pass. */
        sx_painter_draw_text(
            &painter,
            text_x,
            text_y,
            row == current_layout ? "> " : "  ",
            text_color);
        sx_painter_draw_text(
            &painter,
            text_x + gfx_text_width("> "),
            text_y,
            k_row_labels[row],
            text_color);
    }
}

static long g_keyboard_device = -1;
static int g_current_layout = SAVANXP_KEYBOARD_LAYOUT_ES;
static int g_pressed_row = -1;
static int g_hot_row = -1;
static uint32_t g_last_buttons = 0;

static int keyboard_open(void)
{
    g_pressed_row = -1;
    g_hot_row = -1;
    g_last_buttons = 0;
    g_current_layout = SAVANXP_KEYBOARD_LAYOUT_ES;
    /* The layout without the keys: /dev/input0 is windowd's, and the desktop
     * runs as the user. Setting the layout is a mutating operation, so the
     * handle needs write access. */
    g_keyboard_device = sxn_open_device("keyboard", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
    if (g_keyboard_device >= 0)
    {
        long layout = sxn_device_control(g_keyboard_device, INPUT_IOC_GET_LAYOUT, 0);
        if (layout == SAVANXP_KEYBOARD_LAYOUT_ES || layout == SAVANXP_KEYBOARD_LAYOUT_EN)
        {
            g_current_layout = (int)layout;
        }
    }
    return 0;
}

static void keyboard_close(void)
{
    if (g_keyboard_device >= 0)
    {
        (void)sxn_close(g_keyboard_device);
        g_keyboard_device = -1;
    }
}

static uint32_t keyboard_pointer(struct desktop_surface *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int row = popup_hit(&gfx->info, pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    g_last_buttons = pointer->buttons;
    if (row != g_hot_row)
    {
        g_hot_row = row;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed_row = row;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (row >= 0 && row == g_pressed_row)
        {
            if (g_keyboard_device < 0 ||
                sxn_device_control(g_keyboard_device, INPUT_IOC_SET_LAYOUT, (unsigned long)row) < 0)
            {
                desktop_log("desktop: INPUT_IOC_SET_LAYOUT failed\n");
            }
            else
            {
                char digit = (char)('0' + row);
                char path[256];
                /* The user's, in the profile; init applies it at boot. */
                if (sxn_settings_path("keyboard.cfg", 1, path, sizeof(path)) == 0)
                {
                    (void)sxn_write_file(path, &digit, 1);
                }
                actions |= DESKTOP_ACTION_CLOSE | DESKTOP_ACTION_TASKBAR_CHANGED;
            }
        }
        g_pressed_row = -1;
    }
    return actions;
}

static void keyboard_paint(struct desktop_surface *gfx)
{
    popup_paint(gfx, g_current_layout, g_hot_row);
}

const struct desktop_popup_ops desktop_keyboard_ops = {
    KBDLAYOUTPOPUP_WIDTH,
    KBDLAYOUTPOPUP_HEIGHT,
    1,
    keyboard_open,
    keyboard_close,
    keyboard_pointer,
    0,
    keyboard_paint,
    0,
    0,
    0,
};
