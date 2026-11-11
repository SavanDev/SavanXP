#include "desktop.h"

#include "shared/version.h"
#include "start_logo.h"

/*
 * The Shut Down dialog, the one Windows 95 shows: the computer on the left,
 * "Are you sure you want to:" and a choice between shutting down and
 * restarting, answered with Yes or No. The Start menu's Shut Down... and
 * Restart... both open it, each with its own option already chosen; the other
 * stays one click away, as in the original.
 *
 * Like the confirmation it lives in the Start menu's window (the desktop has no
 * owned window left for it), centred and not dismissed by a press elsewhere.
 * Opening it takes the keyboard from the active application
 * (SAVANXP_WM_SHELL_TAKE_FOCUS), and No gives it back. The desktop routes the
 * keys here while it is open: the arrows and the mnemonics S and R choose, Tab moves between the options and the buttons,
 * Enter answers with the default button and Esc with No. The real Yes powers
 * the machine off and is not exercised in automation.
 */

#define SHUTDOWN_WIDTH 346
#define SHUTDOWN_HEIGHT 160
#define SHUTDOWN_CAPTION_HEIGHT 18
#define SHUTDOWN_MARGIN 12
#define SHUTDOWN_ICON_X 16
#define SHUTDOWN_ICON_Y 34
#define SHUTDOWN_TEXT_X 60
#define SHUTDOWN_QUESTION_Y 34
#define SHUTDOWN_OPTION_Y 58
#define SHUTDOWN_OPTION_STEP 22
#define SHUTDOWN_OPTION_HEIGHT 18
#define SHUTDOWN_OPTION_GAP 6
#define SHUTDOWN_BUTTON_WIDTH 75
#define SHUTDOWN_BUTTON_HEIGHT 23
#define SHUTDOWN_BUTTON_GAP 6

#define SHUTDOWN_OPTION_COUNT 2

/* What has the keyboard: the option group or one of the buttons. */
#define SHUTDOWN_FOCUS_OPTIONS 0
#define SHUTDOWN_FOCUS_YES 1
#define SHUTDOWN_FOCUS_NO 2
#define SHUTDOWN_FOCUS_COUNT 3

#define SHUTDOWN_PART_NONE (-1)
#define SHUTDOWN_PART_YES 0
#define SHUTDOWN_PART_NO 1
#define SHUTDOWN_PART_OPTION 2 /* + the option's index */

static const char *const k_option_labels[SHUTDOWN_OPTION_COUNT] = {
    "Shut down the computer?",
    "Restart the computer?",
};
/* The mnemonic of each option: its first letter, underlined. */
static const char k_option_keys[SHUTDOWN_OPTION_COUNT] = {'s', 'r'};

static int g_option = DESKTOP_SHUTDOWN_POWER_OFF;
static int g_focus = SHUTDOWN_FOCUS_OPTIONS;
static int g_pressed = SHUTDOWN_PART_NONE;
static int g_hot = SHUTDOWN_PART_NONE;
static uint32_t g_last_buttons = 0;
/* Yes was the answer; acted on once the dialog is closed. */
static int g_confirmed = 0;
/* The application window that had the keyboard before the dialog took it. */
static uint32_t g_previous_window = 0;

static void shell_request(uint32_t action, uint32_t window_id)
{
    struct savanxp_wm_shell_request request;

    memset(&request, 0, sizeof(request));
    request.action = action;
    request.window_id = window_id;
    desktop_shell_request(&request);
}

void desktop_shutdown_dialog(int option)
{
    g_option = (option == DESKTOP_SHUTDOWN_RESTART) ? DESKTOP_SHUTDOWN_RESTART : DESKTOP_SHUTDOWN_POWER_OFF;
    desktop_close_popups();
    if (!desktop_popup_is_open(DESKTOP_POPUP_SHUTDOWN))
    {
        desktop_toggle_popup(DESKTOP_POPUP_SHUTDOWN);
    }
}

static struct sx_rect button_rect(int button)
{
    int y = SHUTDOWN_HEIGHT - SHUTDOWN_MARGIN - SHUTDOWN_BUTTON_HEIGHT;
    int no_x = SHUTDOWN_WIDTH - SHUTDOWN_MARGIN - SHUTDOWN_BUTTON_WIDTH;

    if (button == SHUTDOWN_PART_YES)
    {
        return sx_rect_make(no_x - SHUTDOWN_BUTTON_GAP - SHUTDOWN_BUTTON_WIDTH, y, SHUTDOWN_BUTTON_WIDTH, SHUTDOWN_BUTTON_HEIGHT);
    }
    return sx_rect_make(no_x, y, SHUTDOWN_BUTTON_WIDTH, SHUTDOWN_BUTTON_HEIGHT);
}

/* The circle and its label: clicking either chooses the option. */
static struct sx_rect option_rect(int option)
{
    int width = SXCHROME_RADIO_SIZE + SHUTDOWN_OPTION_GAP + gfx_text_width(k_option_labels[option]) + 2;

    return sx_rect_make(
        SHUTDOWN_TEXT_X + 4,
        SHUTDOWN_OPTION_Y + option * SHUTDOWN_OPTION_STEP,
        width,
        SHUTDOWN_OPTION_HEIGHT);
}

static int part_at(int x, int y)
{
    int option;

    if (sx_rect_contains_point(button_rect(SHUTDOWN_PART_YES), x, y))
    {
        return SHUTDOWN_PART_YES;
    }
    if (sx_rect_contains_point(button_rect(SHUTDOWN_PART_NO), x, y))
    {
        return SHUTDOWN_PART_NO;
    }
    for (option = 0; option < SHUTDOWN_OPTION_COUNT; ++option)
    {
        if (sx_rect_contains_point(option_rect(option), x, y))
        {
            return SHUTDOWN_PART_OPTION + option;
        }
    }
    return SHUTDOWN_PART_NONE;
}

static int shutdown_open(void)
{
    g_focus = SHUTDOWN_FOCUS_OPTIONS;
    g_pressed = SHUTDOWN_PART_NONE;
    g_hot = SHUTDOWN_PART_NONE;
    g_last_buttons = 0;
    g_confirmed = 0;
    g_previous_window = desktop_taskbar_active_window();
    shell_request(SAVANXP_WM_SHELL_TAKE_FOCUS, 0);
    return 0;
}

static void shutdown_close(void)
{
    uint32_t previous = g_previous_window;

    g_previous_window = 0;
    /* Closed without a Yes (No, Esc, the session went away): the keyboard
     * goes back to the window that had it, and nothing else happens. */
    if (!g_confirmed)
    {
        if (previous != 0)
        {
            shell_request(SAVANXP_WM_SHELL_ACTIVATE, previous);
        }
        return;
    }
    g_confirmed = 0;
    if (g_option == DESKTOP_SHUTDOWN_RESTART)
    {
        (void)power_reboot();
    }
    else
    {
        (void)power_shutdown();
    }
}

static uint32_t shutdown_answer(int yes)
{
    g_confirmed = yes;
    return DESKTOP_ACTION_CLOSE;
}

static uint32_t shutdown_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int part = part_at(pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    (void)gfx;
    g_last_buttons = pointer->buttons;
    if (part != g_hot)
    {
        g_hot = part;
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed = part;
        if (part >= SHUTDOWN_PART_OPTION)
        {
            /* An option is chosen on the press, as a radio button is. */
            g_option = part - SHUTDOWN_PART_OPTION;
            g_focus = SHUTDOWN_FOCUS_OPTIONS;
        }
        else if (part == SHUTDOWN_PART_YES)
        {
            g_focus = SHUTDOWN_FOCUS_YES;
        }
        else if (part == SHUTDOWN_PART_NO)
        {
            g_focus = SHUTDOWN_FOCUS_NO;
        }
        actions |= DESKTOP_ACTION_REPAINT;
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        int pressed = g_pressed;

        g_pressed = SHUTDOWN_PART_NONE;
        actions |= DESKTOP_ACTION_REPAINT;
        if (part == pressed && (part == SHUTDOWN_PART_YES || part == SHUTDOWN_PART_NO))
        {
            actions |= shutdown_answer(part == SHUTDOWN_PART_YES);
        }
    }
    return actions;
}

uint32_t desktop_shutdown_key(const struct savanxp_input_event *event)
{
    int option;

    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return DESKTOP_ACTION_NONE;
    }
    switch (event->key)
    {
    case SAVANXP_KEY_ENTER:
        return shutdown_answer(g_focus != SHUTDOWN_FOCUS_NO);
    case SAVANXP_KEY_ESC:
        return shutdown_answer(0);
    case SAVANXP_KEY_UP:
    case SAVANXP_KEY_DOWN:
        /* The arrows move within the group, wrapping, as Windows does. */
        g_option = (g_option + 1) % SHUTDOWN_OPTION_COUNT;
        g_focus = SHUTDOWN_FOCUS_OPTIONS;
        return DESKTOP_ACTION_REPAINT;
    case SAVANXP_KEY_LEFT:
    case SAVANXP_KEY_RIGHT:
        if (g_focus != SHUTDOWN_FOCUS_OPTIONS)
        {
            g_focus = (g_focus == SHUTDOWN_FOCUS_YES) ? SHUTDOWN_FOCUS_NO : SHUTDOWN_FOCUS_YES;
            return DESKTOP_ACTION_REPAINT;
        }
        return DESKTOP_ACTION_NONE;
    case SAVANXP_KEY_TAB:
        g_focus = (g_focus + 1) % SHUTDOWN_FOCUS_COUNT;
        return DESKTOP_ACTION_REPAINT;
    case ' ':
        if (g_focus == SHUTDOWN_FOCUS_OPTIONS)
        {
            return DESKTOP_ACTION_NONE;
        }
        return shutdown_answer(g_focus == SHUTDOWN_FOCUS_YES);
    default:
        if (event->ascii == 'y' || event->ascii == 'Y')
        {
            return shutdown_answer(1);
        }
        if (event->ascii == 'n' || event->ascii == 'N')
        {
            return shutdown_answer(0);
        }
        for (option = 0; option < SHUTDOWN_OPTION_COUNT; ++option)
        {
            if (event->ascii == k_option_keys[option] || event->ascii == k_option_keys[option] - 'a' + 'A')
            {
                g_option = option;
                g_focus = SHUTDOWN_FOCUS_OPTIONS;
                return DESKTOP_ACTION_REPAINT;
            }
        }
        return DESKTOP_ACTION_NONE;
    }
}

static void draw_asset(struct sx_painter *painter, const struct savanxp_embedded_bitmap_asset *asset, int x, int y)
{
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;

    if (asset == 0 || asset->pixels == 0 || asset->width == 0 || asset->height == 0)
    {
        return;
    }
    info.width = asset->width;
    info.height = asset->height;
    info.pitch = asset->width * 4u;
    info.bpp = 32;
    info.buffer_size = info.pitch * asset->height;
    sx_bitmap_wrap(&bitmap, (uint32_t *)asset->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, x, y);
}

static void paint_button(struct sx_painter *painter, int button, const char *label)
{
    struct sx_rect rect = button_rect(button);
    int sunken = (button == g_pressed && button == g_hot);
    int focused = (button == SHUTDOWN_PART_YES) ? (g_focus == SHUTDOWN_FOCUS_YES) : (g_focus == SHUTDOWN_FOCUS_NO);
    /* The default button carries the dark border: the focused one, or Yes
     * while the options have the keyboard. */
    int is_default = focused || (button == SHUTDOWN_PART_YES && g_focus == SHUTDOWN_FOCUS_OPTIONS);
    struct sx_rect face = rect;
    int text_height = gfx_text_height();
    int label_x;
    int label_y;

    if (is_default)
    {
        sx_painter_draw_frame(painter, rect, SXGUI_COLOR_TEXT);
        face = sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2);
    }
    if (sunken)
    {
        sx_painter_fill_rect(painter, face, SXGUI_COLOR_FACE);
        sx_painter_draw_frame(painter, face, SXGUI_COLOR_SHADOW);
    }
    else
    {
        sxchrome_fill_raised(painter, face, SXGUI_COLOR_FACE, 0);
    }
    label_x = rect.x + (rect.width - gfx_text_width(label)) / 2 + (sunken ? 1 : 0);
    label_y = rect.y + (rect.height - text_height) / 2 + (sunken ? 1 : 0);
    sx_painter_draw_text(painter, label_x, label_y, label, SXGUI_COLOR_TEXT);
    if (focused)
    {
        sxchrome_draw_focus_rect(painter, sx_rect_make(face.x + 3, face.y + 3, face.width - 6, face.height - 6));
    }
}

static void shutdown_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    int text_height = gfx_text_height();
    int option;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    sxchrome_draw_raised(&painter, frame);
    sx_painter_fill_rect(&painter, sx_rect_make(3, 3, frame.width - 6, SHUTDOWN_CAPTION_HEIGHT), SXGUI_COLOR_SELECT);
    sx_painter_draw_text(
        &painter,
        6,
        3 + (SHUTDOWN_CAPTION_HEIGHT - text_height) / 2,
        "Shut Down " SAVANXP_SYSTEM_NAME,
        SXGUI_COLOR_SELECT_TEXT);

    draw_asset(&painter, &k_shutdown_icon_large, SHUTDOWN_ICON_X, SHUTDOWN_ICON_Y);
    sx_painter_draw_text(&painter, SHUTDOWN_TEXT_X, SHUTDOWN_QUESTION_Y, "Are you sure you want to:", SXGUI_COLOR_TEXT);

    for (option = 0; option < SHUTDOWN_OPTION_COUNT; ++option)
    {
        struct sx_rect rect = option_rect(option);
        const char *label = k_option_labels[option];
        int text_x = rect.x + SXCHROME_RADIO_SIZE + SHUTDOWN_OPTION_GAP;
        int text_y = rect.y + (rect.height - text_height) / 2;
        char first[2] = {label[0], '\0'};

        sxchrome_draw_radio(
            &painter,
            sx_rect_make(rect.x, rect.y + (rect.height - SXCHROME_RADIO_SIZE) / 2, SXCHROME_RADIO_SIZE, SXCHROME_RADIO_SIZE),
            option == g_option,
            1);
        sx_painter_draw_text(&painter, text_x, text_y, label, SXGUI_COLOR_TEXT);
        /* The mnemonic: the first letter underlined, under the baseline. */
        sx_painter_hline(&painter, text_x, text_y + text_height - 1, gfx_text_width(first), SXGUI_COLOR_TEXT);
        if (g_focus == SHUTDOWN_FOCUS_OPTIONS && option == g_option)
        {
            sxchrome_draw_focus_rect(
                &painter,
                sx_rect_make(text_x - 2, text_y - 1, gfx_text_width(label) + 4, text_height + 2));
        }
    }

    paint_button(&painter, SHUTDOWN_PART_YES, "Yes");
    paint_button(&painter, SHUTDOWN_PART_NO, "No");
}

const struct desktop_popup_ops desktop_shutdown_ops = {
    SHUTDOWN_WIDTH,
    SHUTDOWN_HEIGHT,
    0,
    shutdown_open,
    shutdown_close,
    shutdown_pointer,
    0,
    shutdown_paint,
    1,
    0,
    0,
};
