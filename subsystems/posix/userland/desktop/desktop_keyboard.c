#include "desktop.h"

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

static const char *k_row_labels[KBDLAYOUTPOPUP_ROW_COUNT] = {"Espanol", "English"};

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

static void popup_paint(struct savanxp_gfx_context *gfx, int current_layout, int hot_row)
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
        /* La fila del layout activo lleva una marca a la izquierda -- no hay
         * negrita en esta fuente, asi que un prefijo es la forma simple de
         * distinguirla sin depender de otra pasada de dibujo. */
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

static long g_input_fd = -1;
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
    /* Only for the ioctls: this fd must never be read -- /dev/input0's queue
     * is global and windowd is the one that drains it. Setting the layout is a
     * mutating operation, so the descriptor needs write access. */
    g_input_fd = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (g_input_fd >= 0)
    {
        long layout = input_get_layout((int)g_input_fd);
        if (layout == SAVANXP_KEYBOARD_LAYOUT_ES || layout == SAVANXP_KEYBOARD_LAYOUT_EN)
        {
            g_current_layout = (int)layout;
        }
    }
    return 0;
}

static void keyboard_close(void)
{
    if (g_input_fd >= 0)
    {
        savanxp_close((int)g_input_fd);
        g_input_fd = -1;
    }
}

static uint32_t keyboard_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
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
            if (g_input_fd < 0 || input_set_layout((int)g_input_fd, row) < 0)
            {
                puts_fd(2, "desktop: input_set_layout failed\n");
            }
            else
            {
                char digit = (char)('0' + row);
                int config_fd = (int)savanxp_open_mode(
                    "/disk/keyboard.cfg",
                    SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
                if (config_fd >= 0)
                {
                    (void)savanxp_write(config_fd, &digit, 1);
                    savanxp_close(config_fd);
                }
                actions |= DESKTOP_ACTION_CLOSE | DESKTOP_ACTION_TASKBAR_CHANGED;
            }
        }
        g_pressed_row = -1;
    }
    return actions;
}

static void keyboard_paint(struct savanxp_gfx_context *gfx)
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
};
