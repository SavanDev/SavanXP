#include "desktop.h"

#include <stdio.h>

/*
 * "Not responding" (docs/desktop/DESKTOP_ARCHITECTURE.md#closing-a-window):
 * windowd never ends a process. When a window asked to close keeps running,
 * windowd marks it in the window list and this dialog offers to end it, as
 * Windows' End Program box does. End Now kills the process -- the window
 * entry's id is its pid -- and Wait leaves it alone until it is asked again.
 *
 * A popup of the desktop like the others, but centred and not dismissed by a
 * press elsewhere: it is a question, not a menu.
 */

#define HUNG_WIDTH 340
#define HUNG_HEIGHT 120
#define HUNG_CAPTION_HEIGHT 20
#define HUNG_MARGIN 12
#define HUNG_BUTTON_WIDTH 84
#define HUNG_BUTTON_HEIGHT 24
#define HUNG_BUTTON_GAP 8

enum hung_button
{
    HUNG_BUTTON_NONE = -1,
    HUNG_BUTTON_END = 0,
    HUNG_BUTTON_WAIT = 1,
};

static const char *const k_button_labels[2] = {"End Now", "Wait"};

static uint32_t g_target_id = 0;
static char g_title[SAVANXP_WM_WINDOW_TITLE_CAPACITY];
static int g_pressed = HUNG_BUTTON_NONE;
static int g_hot = HUNG_BUTTON_NONE;
static uint32_t g_last_buttons = 0;

void desktop_hung_set_target(uint32_t window_id, const char *title)
{
    size_t index = 0;

    g_target_id = window_id;
    while (title != 0 && title[index] != '\0' && index + 1u < sizeof(g_title))
    {
        g_title[index] = title[index];
        index += 1u;
    }
    g_title[index] = '\0';
}

uint32_t desktop_hung_target(void)
{
    return g_target_id;
}

static struct sx_rect button_rect(int button)
{
    int right = HUNG_WIDTH - HUNG_MARGIN;
    int x = right - (2 - button) * HUNG_BUTTON_WIDTH - (1 - button) * HUNG_BUTTON_GAP;

    return sx_rect_make(x, HUNG_HEIGHT - HUNG_MARGIN - HUNG_BUTTON_HEIGHT, HUNG_BUTTON_WIDTH, HUNG_BUTTON_HEIGHT);
}

static int button_at(int x, int y)
{
    int button;

    for (button = HUNG_BUTTON_END; button <= HUNG_BUTTON_WAIT; ++button)
    {
        struct sx_rect rect = button_rect(button);

        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height)
        {
            return button;
        }
    }
    return HUNG_BUTTON_NONE;
}

static int hung_open(void)
{
    g_pressed = HUNG_BUTTON_NONE;
    g_hot = HUNG_BUTTON_NONE;
    g_last_buttons = 0;
    return g_target_id != 0 ? 0 : -1;
}

static void hung_close(void)
{
}

static uint32_t hung_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int button = button_at(pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    (void)gfx;
    g_last_buttons = pointer->buttons;
    if (button != g_hot)
    {
        g_hot = button;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed = button;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (button != HUNG_BUTTON_NONE && button == g_pressed)
        {
            if (button == HUNG_BUTTON_END && g_target_id != 0)
            {
                /* windowd takes the window down when it sees the process gone. */
                (void)savanxp_kill((int)g_target_id, SAVANXP_SIGKILL);
            }
            desktop_hung_dismissed(g_target_id);
            actions |= DESKTOP_ACTION_CLOSE;
        }
        g_pressed = HUNG_BUTTON_NONE;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    return actions;
}

static void hung_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    char line[SAVANXP_WM_WINDOW_TITLE_CAPACITY + 32];
    int text_height = gfx_text_height();
    int button;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, frame, 0);

    sx_painter_fill_rect(&painter, sx_rect_make(2, 2, frame.width - 4, HUNG_CAPTION_HEIGHT), SXGUI_COLOR_SELECT);
    snprintf(line, sizeof(line), "End Program - %s", g_title);
    sx_painter_draw_text(&painter, 6, 2 + (HUNG_CAPTION_HEIGHT - text_height) / 2, line, SXGUI_COLOR_SELECT_TEXT);

    snprintf(line, sizeof(line), "%s is not responding.", g_title);
    sx_painter_draw_text(&painter, HUNG_MARGIN, HUNG_CAPTION_HEIGHT + HUNG_MARGIN, line, SXGUI_COLOR_TEXT);
    sx_painter_draw_text(&painter, HUNG_MARGIN, HUNG_CAPTION_HEIGHT + HUNG_MARGIN + text_height + 4,
        "End it now? Unsaved work will be lost.", SXGUI_COLOR_TEXT);

    for (button = HUNG_BUTTON_END; button <= HUNG_BUTTON_WAIT; ++button)
    {
        struct sx_rect rect = button_rect(button);
        int sunken = button == g_pressed && button == g_hot;
        int label_x = rect.x + (rect.width - gfx_text_width(k_button_labels[button])) / 2 + (sunken ? 1 : 0);
        int label_y = rect.y + (rect.height - text_height) / 2 + (sunken ? 1 : 0);

        sx_painter_fill_rect(&painter, rect, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, rect, sunken);
        sx_painter_draw_text(&painter, label_x, label_y, k_button_labels[button], SXGUI_COLOR_TEXT);
    }
}

const struct desktop_popup_ops desktop_hung_ops = {
    HUNG_WIDTH,
    HUNG_HEIGHT,
    0,
    hung_open,
    hung_close,
    hung_pointer,
    0,
    hung_paint,
    1,
};
