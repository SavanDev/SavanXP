#include "desktop.h"
#include "savanxp/wm_shell_protocol.h"

#include "desktop_wallpaper.h"

/* The wallpaper keeps its version stamp above the taskbar with a copy of its
 * height (desktop_wallpaper.h): if the bar changes, the stamp follows it. */
_Static_assert(
    DESKTOP_WALLPAPER_TASKBAR_HEIGHT == DESKTOP_TASKBAR_HEIGHT,
    "the taskbar height the wallpaper uses must match the desktop's");

/*
 * The desktop process (docs/desktop/DESKTOP_ARCHITECTURE.md). windowd starts it
 * as the shell and gives its main window the background role; everything else
 * the shell shows is a window it asks for:
 *
 *   id 0  background  wallpaper (main window, role granted by windowd)
 *   id 1  taskbar     panel on the bottom edge
 *   id 2  Start menu  popup, bottom left above the taskbar
 *   id 3  tray popup  volume, clock or keyboard layout, bottom right
 *
 * One loop serves all of them. Popups open and close as windows of this
 * process -- no process per click -- and the taskbar knows what is open
 * without asking anyone.
 */

/* Task Manager, which Ctrl+Shift+Esc opens. */
static const char *k_task_manager_path = "/bin/taskmgr";

/* How often the persisted wallpaper choice is re-read. progman's Options menu
 * changes it by writing the config; it is a rare, manual change, not worth an
 * open+read per pass. */
#define DESKTOP_WALLPAPER_CHECK_MS 500ul
/* How long to wait before asking for the taskbar again after windowd refused
 * it (for instance while it is still busy with a mode change). */
#define DESKTOP_TASKBAR_RETRY_MS 1000ul

struct desktop_popup_slot
{
    uint32_t window_id;
    int open;
    enum desktop_popup kind;
    const struct desktop_popup_ops *ops;
    struct savanxp_gfx_context gfx;
    int needs_repaint;
};

static const struct desktop_popup_ops *const k_popup_ops[DESKTOP_POPUP_COUNT] = {
    &desktop_startmenu_ops,
    &desktop_volume_ops,
    &desktop_clock_ops,
    &desktop_keyboard_ops,
};

static struct savanxp_gfx_context g_background;
static int g_background_repaint = 1;
static uint32_t g_background_buttons = 0;

static struct savanxp_gfx_context g_taskbar;
static int g_taskbar_open = 0;
static int g_taskbar_available = 0;
static int g_taskbar_repaint = 0;
static unsigned long g_taskbar_retry_ms = 0;

static struct desktop_popup_slot g_start_menu = {DESKTOP_WINDOW_START_MENU, 0, DESKTOP_POPUP_START_MENU, 0, {0}, 0};
static struct desktop_popup_slot g_tray = {DESKTOP_WINDOW_TRAY_POPUP, 0, DESKTOP_POPUP_VOLUME, 0, {0}, 0};

void desktop_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken)
{
    uint32_t top_left = sunken ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = sunken ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

long desktop_launch(const char *path, uint32_t launch_flags)
{
    return gfx_desktop_launch_ex(&g_background, path, launch_flags);
}

static struct desktop_popup_slot *slot_for(enum desktop_popup popup)
{
    return popup == DESKTOP_POPUP_START_MENU ? &g_start_menu : &g_tray;
}

static void close_slot(struct desktop_popup_slot *slot)
{
    if (!slot->open)
    {
        return;
    }
    (void)gfx_window_close(&g_background, &slot->gfx);
    slot->ops->close();
    slot->open = 0;
    slot->ops = 0;
    /* The Start button shows whether the menu is open. */
    g_taskbar_repaint = 1;
}

static void open_slot(struct desktop_popup_slot *slot, enum desktop_popup popup)
{
    const struct desktop_popup_ops *ops = k_popup_ops[popup];
    int display_width = (int)g_background.info.width;
    int display_height = (int)g_background.info.height;
    int x = 0;
    int y = 0;

    /* Above the taskbar: the Start menu at its left end, the tray popups at
     * its right end. */
    if (ops->anchor_right)
    {
        x = display_width - 4 - ops->width;
    }
    else
    {
        x = 2;
    }
    y = display_height - DESKTOP_TASKBAR_HEIGHT - ops->height;

    if (ops->open() != 0)
    {
        return;
    }
    if (gfx_window_open_role(
            &g_background,
            &slot->gfx,
            slot->window_id,
            SAVANXP_WM_WINDOW_ROLE_POPUP,
            SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE,
            x,
            y,
            (uint32_t)ops->width,
            (uint32_t)ops->height) < 0)
    {
        puts_fd(2, "desktop: windowd refused a popup\n");
        ops->close();
        return;
    }
    slot->open = 1;
    slot->kind = popup;
    slot->ops = ops;
    slot->needs_repaint = 1;
    g_taskbar_repaint = 1;
}

/* The taskbar's buttons are toggles: the same button closes what it opened.
 * The tray popups share a corner, so opening one closes the other. */
void desktop_toggle_popup(enum desktop_popup popup)
{
    struct desktop_popup_slot *slot = slot_for(popup);
    int was_this = slot->open && slot->kind == popup;

    close_slot(slot);
    if (!was_this)
    {
        open_slot(slot, popup);
    }
}

void desktop_close_popups(void)
{
    close_slot(&g_start_menu);
    close_slot(&g_tray);
}

int desktop_popup_is_open(enum desktop_popup popup)
{
    const struct desktop_popup_slot *slot = slot_for(popup);

    return slot->open && slot->kind == popup;
}

static void apply_actions(uint32_t actions, struct desktop_popup_slot *slot)
{
    if ((actions & DESKTOP_ACTION_REPAINT) != 0 && slot != 0)
    {
        slot->needs_repaint = 1;
    }
    if ((actions & DESKTOP_ACTION_TASKBAR_CHANGED) != 0 && desktop_taskbar_refresh_indicators())
    {
        g_taskbar_repaint = 1;
    }
    if ((actions & DESKTOP_ACTION_CLOSE) != 0 && slot != 0)
    {
        close_slot(slot);
    }
}

static void register_hotkey(uint32_t key, uint32_t modifiers)
{
    struct savanxp_wm_shell_request request;

    memset(&request, 0, sizeof(request));
    request.action = SAVANXP_WM_SHELL_REGISTER_HOTKEY;
    request.key = key;
    request.modifiers = modifiers;
    (void)savanxp_write(SAVANXP_WM_FD_SHELL_REQUEST, &request, sizeof(request));
}

/* The main window never has the focus, so every key it gets is a hotkey this
 * process registered. */
static void handle_hotkey(const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || event->key != SAVANXP_KEY_ESC)
    {
        return;
    }
    if ((event->modifiers & SAVANXP_KEY_MOD_SHIFT) != 0)
    {
        desktop_close_popups();
        (void)desktop_launch(k_task_manager_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
    }
    else
    {
        desktop_toggle_popup(DESKTOP_POPUP_START_MENU);
    }
}

static void paint_background(void)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;

    sx_bitmap_wrap(&bitmap, g_background.pixels, &g_background.info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    desktop_wallpaper_draw(&painter, &g_background.info);
}

static void try_open_taskbar(void)
{
    static int reported = 0;
    long result;

    if (g_taskbar_open || !g_taskbar_available || uptime_ms() < g_taskbar_retry_ms)
    {
        return;
    }
    result = gfx_window_open_role(
        &g_background,
        &g_taskbar,
        DESKTOP_WINDOW_TASKBAR,
        SAVANXP_WM_WINDOW_ROLE_PANEL,
        SAVANXP_WM_PANEL_EDGE_BOTTOM,
        0,
        0,
        0,
        DESKTOP_TASKBAR_HEIGHT);
    if (result < 0)
    {
        if (!reported)
        {
            eprintf("desktop: no taskbar yet (%s), retrying\n", result_error_string(result));
            reported = 1;
        }
        g_taskbar_retry_ms = uptime_ms() + DESKTOP_TASKBAR_RETRY_MS;
        return;
    }
    g_taskbar_open = 1;
    g_taskbar_repaint = 1;
}

/* Background: resize, and a press on it closes the popups. windowd only asks
 * a popup to close for a press on another process; the background is ours. */
static void service_background(void)
{
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;

    while (gfx_poll_event(&g_background, &event) > 0)
    {
        /* windowd resizes the background surface when the video mode changes. */
        if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
        {
            (void)gfx_apply_resize_event(&g_background, &event);
            g_background_repaint = 1;
        }
        else
        {
            handle_hotkey(&event);
        }
    }
    while (gfx_poll_window_pointer(&g_background, &pointer) > 0)
    {
        uint32_t down = pointer.buttons & ~g_background_buttons;

        if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
        {
            desktop_close_popups();
        }
        g_background_buttons = pointer.buttons;
    }
}

static void service_taskbar(void)
{
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;

    if (!g_taskbar_open)
    {
        return;
    }
    while (gfx_poll_event(&g_taskbar, &event) > 0)
    {
        if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
        {
            (void)gfx_apply_resize_event(&g_taskbar, &event);
            g_taskbar_repaint = 1;
        }
    }
    while (gfx_poll_window_pointer(&g_taskbar, &pointer) > 0)
    {
        if ((desktop_taskbar_pointer(&g_taskbar, &pointer) & DESKTOP_ACTION_REPAINT) != 0)
        {
            g_taskbar_repaint = 1;
        }
    }
    if ((desktop_taskbar_tick() & DESKTOP_ACTION_REPAINT) != 0)
    {
        g_taskbar_repaint = 1;
    }
}

static void service_popup(struct desktop_popup_slot *slot)
{
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;

    if (!slot->open)
    {
        return;
    }
    /* windowd asked it to close: a press landed outside the desktop. */
    if (gfx_should_close(&slot->gfx))
    {
        close_slot(slot);
        return;
    }
    while (gfx_poll_event(&slot->gfx, &event) > 0)
    {
    }
    while (slot->open && gfx_poll_window_pointer(&slot->gfx, &pointer) > 0)
    {
        apply_actions(slot->ops->pointer(&slot->gfx, &pointer), slot);
    }
    if (slot->open && slot->ops->tick != 0)
    {
        apply_actions(slot->ops->tick(), slot);
    }
}

/* Returns non-zero if something was presented. A failed present on the main
 * window means windowd is gone: the caller exits. */
static int present_windows(int *lost_session)
{
    int presented = 0;
    struct desktop_popup_slot *slots[2] = {&g_start_menu, &g_tray};
    int index;

    if (g_background_repaint)
    {
        paint_background();
        if (gfx_present(&g_background, g_background.pixels) < 0)
        {
            *lost_session = 1;
            return presented;
        }
        g_background_repaint = 0;
        presented = 1;
    }
    if (g_taskbar_open && g_taskbar_repaint)
    {
        desktop_taskbar_paint(&g_taskbar);
        if (gfx_present(&g_taskbar, g_taskbar.pixels) == 0)
        {
            presented = 1;
        }
        g_taskbar_repaint = 0;
    }
    for (index = 0; index < 2; ++index)
    {
        struct desktop_popup_slot *slot = slots[index];

        if (slot->open && slot->needs_repaint)
        {
            slot->ops->paint(&slot->gfx);
            if (gfx_present(&slot->gfx, slot->gfx.pixels) == 0)
            {
                presented = 1;
            }
            slot->needs_repaint = 0;
        }
    }
    return presented;
}

int main(void)
{
    unsigned long next_wallpaper_check_ms = 0;

    if (gfx_open(&g_background) < 0)
    {
        puts_fd(2, "desktop: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&g_background) < 0)
    {
        puts_fd(2, "desktop: gfx_acquire failed\n");
        gfx_close(&g_background);
        return 1;
    }

    /* Loads the persisted choice and tries to decode /disk/wallpaper.bmp. */
    desktop_wallpaper_init();
    /* Without the shell channels (started by hand, not by windowd) there is
     * no window list to show: the desktop runs as a background only. */
    g_taskbar_available = desktop_taskbar_init() == 0;
    if (g_taskbar_available)
    {
        /* NT 4: Ctrl+Esc opens the Start menu, Ctrl+Shift+Esc Task Manager. */
        register_hotkey(SAVANXP_KEY_ESC, SAVANXP_KEY_MOD_CTRL);
        register_hotkey(SAVANXP_KEY_ESC, SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_SHIFT);
    }

    for (;;)
    {
        int lost_session = 0;
        unsigned long now = uptime_ms();

        if (gfx_should_close(&g_background))
        {
            break;
        }
        try_open_taskbar();
        service_background();
        service_taskbar();
        service_popup(&g_start_menu);
        service_popup(&g_tray);

        if (now >= next_wallpaper_check_ms)
        {
            next_wallpaper_check_ms = now + DESKTOP_WALLPAPER_CHECK_MS;
            if (desktop_wallpaper_reload())
            {
                g_background_repaint = 1;
            }
        }

        if (!present_windows(&lost_session))
        {
            if (lost_session)
            {
                break;
            }
            sleep_ms(16);
        }
    }

    desktop_close_popups();
    if (g_taskbar_open)
    {
        (void)gfx_window_close(&g_background, &g_taskbar);
    }
    desktop_taskbar_shutdown();
    gfx_close(&g_background);
    return 0;
}
