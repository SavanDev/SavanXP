#include "desktop.h"

#include <stdio.h>

/*
 * A question from the desktop (docs/desktop/DESKTOP_ICONS.md): "delete this
 * icon?", or a message with only OK when there is nothing to choose. Centred
 * like the End Program dialog, and not dismissed by a press elsewhere. It
 * shares the Start menu's window: a context menu's item is what opens it,
 * and the menu is closed by then.
 *
 * The popup never has the keyboard, but the desktop does: Enter answers with
 * the default button, Esc with No, and the arrows and Tab move between them.
 */

#define CONFIRM_WIDTH 340
#define CONFIRM_HEIGHT 112
#define CONFIRM_CAPTION_HEIGHT 20
#define CONFIRM_MARGIN 12
#define CONFIRM_BUTTON_WIDTH 84
#define CONFIRM_BUTTON_HEIGHT 24
#define CONFIRM_BUTTON_GAP 8
#define CONFIRM_TEXT_CAPACITY 160

static char g_title[64];
static char g_text[CONFIRM_TEXT_CAPACITY];
static int g_button_count = 2;
static void (*g_answer)(int yes) = 0;
static int g_focus = 0;
static int g_pressed = -1;
static int g_hot = -1;
static uint32_t g_last_buttons = 0;
/* The answer given, delivered once the popup is closed. -1 = none yet. */
static int g_result = -1;

void desktop_confirm(const char *title, const char *text, int yes_no, void (*answer)(int yes))
{
    snprintf(g_title, sizeof(g_title), "%s", title != 0 ? title : "");
    snprintf(g_text, sizeof(g_text), "%s", text != 0 ? text : "");
    g_button_count = yes_no ? 2 : 1;
    g_answer = answer;
    desktop_close_popups();
    desktop_toggle_popup(DESKTOP_POPUP_CONFIRM);
}

static const char *button_label(int button)
{
    if (g_button_count == 1)
    {
        return "OK";
    }
    return button == 0 ? "Yes" : "No";
}

static struct sx_rect button_rect(int button)
{
    int total = g_button_count * CONFIRM_BUTTON_WIDTH + (g_button_count - 1) * CONFIRM_BUTTON_GAP;
    int x = (CONFIRM_WIDTH - total) / 2 + button * (CONFIRM_BUTTON_WIDTH + CONFIRM_BUTTON_GAP);

    return sx_rect_make(x, CONFIRM_HEIGHT - CONFIRM_MARGIN - CONFIRM_BUTTON_HEIGHT, CONFIRM_BUTTON_WIDTH, CONFIRM_BUTTON_HEIGHT);
}

static int button_at(int x, int y)
{
    int button;

    for (button = 0; button < g_button_count; ++button)
    {
        if (sx_rect_contains_point(button_rect(button), x, y))
        {
            return button;
        }
    }
    return -1;
}

static int confirm_open(void)
{
    g_focus = 0;
    g_pressed = -1;
    g_hot = -1;
    g_last_buttons = 0;
    g_result = -1;
    return 0;
}

static void confirm_close(void)
{
    void (*answer)(int yes) = g_answer;
    int result = g_result;

    g_answer = 0;
    /* Closed without an answer (the session went away): it is a No. */
    if (answer != 0)
    {
        answer(g_button_count == 2 && result == 0);
    }
}

static uint32_t confirm_answer(int button)
{
    g_result = button;
    return DESKTOP_ACTION_CLOSE;
}

static uint32_t confirm_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
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
        if (button >= 0)
        {
            g_focus = button;
        }
        actions |= DESKTOP_ACTION_REPAINT;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        int pressed = g_pressed;

        g_pressed = -1;
        actions |= DESKTOP_ACTION_REPAINT;
        if (button >= 0 && button == pressed)
        {
            actions |= confirm_answer(button);
        }
    }
    return actions;
}

uint32_t desktop_confirm_key(const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return DESKTOP_ACTION_NONE;
    }
    switch (event->key)
    {
    case SAVANXP_KEY_ENTER:
    case ' ':
        return confirm_answer(g_focus);
    case SAVANXP_KEY_ESC:
        return confirm_answer(g_button_count - 1);
    case SAVANXP_KEY_LEFT:
    case SAVANXP_KEY_RIGHT:
    case SAVANXP_KEY_TAB:
        g_focus = (g_focus + 1) % g_button_count;
        return DESKTOP_ACTION_REPAINT;
    default:
        if (g_button_count == 2 && (event->ascii == 'y' || event->ascii == 'Y'))
        {
            return confirm_answer(0);
        }
        if (g_button_count == 2 && (event->ascii == 'n' || event->ascii == 'N'))
        {
            return confirm_answer(1);
        }
        return DESKTOP_ACTION_NONE;
    }
}

static void confirm_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    int text_height = gfx_text_height();
    int button;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, frame, 0);
    sx_painter_fill_rect(&painter, sx_rect_make(2, 2, frame.width - 4, CONFIRM_CAPTION_HEIGHT), SXGUI_COLOR_SELECT);
    sx_painter_draw_text(&painter, 6, 2 + (CONFIRM_CAPTION_HEIGHT - text_height) / 2, g_title, SXGUI_COLOR_SELECT_TEXT);
    sx_painter_draw_text(&painter, CONFIRM_MARGIN, CONFIRM_CAPTION_HEIGHT + CONFIRM_MARGIN + 4, g_text, SXGUI_COLOR_TEXT);

    for (button = 0; button < g_button_count; ++button)
    {
        struct sx_rect rect = button_rect(button);
        int sunken = button == g_pressed && button == g_hot;
        const char *label = button_label(button);
        int label_x = rect.x + (rect.width - gfx_text_width(label)) / 2 + (sunken ? 1 : 0);
        int label_y = rect.y + (rect.height - text_height) / 2 + (sunken ? 1 : 0);

        sx_painter_fill_rect(&painter, rect, SXGUI_COLOR_FACE);
        /* The default button has the era's extra dark border. */
        if (button == g_focus)
        {
            sx_painter_fill_rect(&painter, sx_rect_make(rect.x - 1, rect.y - 1, rect.width + 2, 1), SXGUI_COLOR_TEXT);
            sx_painter_fill_rect(&painter, sx_rect_make(rect.x - 1, rect.y + rect.height, rect.width + 2, 1), SXGUI_COLOR_TEXT);
            sx_painter_fill_rect(&painter, sx_rect_make(rect.x - 1, rect.y, 1, rect.height), SXGUI_COLOR_TEXT);
            sx_painter_fill_rect(&painter, sx_rect_make(rect.x + rect.width, rect.y, 1, rect.height), SXGUI_COLOR_TEXT);
        }
        desktop_bevel(&painter, rect, sunken);
        sx_painter_draw_text(&painter, label_x, label_y, label, SXGUI_COLOR_TEXT);
    }
}

const struct desktop_popup_ops desktop_confirm_ops = {
    CONFIRM_WIDTH,
    CONFIRM_HEIGHT,
    0,
    confirm_open,
    confirm_close,
    confirm_pointer,
    0,
    confirm_paint,
    1,
    0,
    0,
};
