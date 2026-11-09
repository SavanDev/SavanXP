#include "desktop.h"
#include "savanxp/wm_shell_protocol.h"

#include "desktop_wallpaper.h"
#include "display_settings.h"
#include <stdlib.h>
#include <unistd.h>

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
 *   id 0  background  wallpaper and icons (main window, role granted by windowd)
 *   id 1  taskbar     panel on the bottom edge
 *   id 2  Start menu  popup, bottom left above the taskbar; or a context
 *                     menu at the pointer, or an icon's question, centred
 *                     (one at a time: they share the id)
 *   id 3  tray popup  volume, clock or keyboard layout, bottom right
 *   id 4  End Program a program asked to close is not responding (centred)
 *
 * One loop serves all of them. Popups open and close as windows of this
 * process -- no process per click -- and the taskbar knows what is open
 * without asking anyone.
 */

/* Task Manager, which Ctrl+Shift+Esc opens. */
static const char *k_task_manager_path = "/system/taskmgr.sxe";
/* The Command Prompt, which Ctrl+Alt+T opens (docs/desktop/CONSOLE.md). */
static const char *k_console_path = "/system/console.sxe";

/* How often the persisted wallpaper choice is re-read. Display Properties
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
    &desktop_hung_ops,
    &desktop_contextmenu_ops,
    &desktop_confirm_ops,
};

static struct savanxp_gfx_context g_background;
static int g_background_repaint = 1;
static uint32_t g_background_buttons = 0;
/* The wallpaper drawn once, for repainting the icons without drawing it
 * again: an image wallpaper is scaled on every draw. */
static uint32_t *g_wallpaper_cache = 0;
static size_t g_wallpaper_cache_bytes = 0;
static int g_wallpaper_dirty = 1;

static struct savanxp_gfx_context g_taskbar;
static int g_taskbar_open = 0;
static int g_taskbar_available = 0;
static int g_taskbar_repaint = 0;
static unsigned long g_taskbar_retry_ms = 0;

static struct desktop_popup_slot g_start_menu = {DESKTOP_WINDOW_START_MENU, 0, DESKTOP_POPUP_START_MENU, 0, {0}, 0};
static struct desktop_popup_slot g_tray = {DESKTOP_WINDOW_TRAY_POPUP, 0, DESKTOP_POPUP_VOLUME, 0, {0}, 0};
static struct desktop_popup_slot g_hung = {DESKTOP_WINDOW_HUNG_DIALOG, 0, DESKTOP_POPUP_HUNG, 0, {0}, 0};
/* Every popup slot, for what is done to all of them. */
static struct desktop_popup_slot *const k_slots[] = {&g_start_menu, &g_tray, &g_hung};
#define DESKTOP_SLOT_COUNT ((int)(sizeof(k_slots) / sizeof(k_slots[0])))
/* The window whose "not responding" the user already answered. */
static uint32_t g_hung_answered = 0;

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

void desktop_shell_request(const struct savanxp_wm_shell_request *request)
{
    int fd = gfx_shell_request_fd();

    /* Without the shell role there is nobody to ask. */
    if (fd >= 0)
    {
        (void)savanxp_write(fd, request, sizeof(*request));
    }
}

static struct desktop_popup_slot *slot_for(enum desktop_popup popup)
{
    if (popup == DESKTOP_POPUP_HUNG)
    {
        return &g_hung;
    }
    /* A process has four owned windows (SAVANXP_WM_MAX_OWNED_WINDOWS) and
     * the desktop uses them all, so the context menu takes the Start menu's
     * slot: as on Windows, opening either closes the other. */
    if (popup == DESKTOP_POPUP_START_MENU || popup == DESKTOP_POPUP_CONTEXT_MENU || popup == DESKTOP_POPUP_CONFIRM)
    {
        return &g_start_menu;
    }
    return &g_tray;
}

static void close_slot(struct desktop_popup_slot *slot)
{
    if (!slot->open)
    {
        return;
    }
    const struct desktop_popup_ops *ops = slot->ops;

    (void)gfx_window_close(&g_background, &slot->gfx);
    slot->open = 0;
    slot->ops = 0;
    /* The Start button shows whether the menu is open. */
    g_taskbar_repaint = 1;
    /* Last: a module may open another popup from here, even in this slot (a
     * context menu's Delete opens the confirmation). */
    ops->close();
}

/* `at_point`: open with the top-left corner at (x, y) instead of where the
 * popup kind is anchored. */
static void open_slot_at(struct desktop_popup_slot *slot, enum desktop_popup popup, int at_point, int x, int y)
{
    const struct desktop_popup_ops *ops = k_popup_ops[popup];
    int display_width = (int)g_background.info.width;
    int work_height = (int)g_background.info.height - DESKTOP_TASKBAR_HEIGHT;
    int width = ops->width;
    int height = ops->height;

    if (ops->measure != 0)
    {
        ops->measure(&width, &height);
    }
    if (at_point)
    {
        /* Where it does not fit right of or below the point, it opens to the
         * left of or above it, and never over the taskbar. */
        if (x + width > display_width)
        {
            x -= width;
        }
        if (y + height > work_height)
        {
            y -= height;
        }
        x = x < 0 ? 0 : x;
        y = y < 0 ? 0 : y;
    }
    /* Above the taskbar: the Start menu at its left end, the tray popups at
     * its right end. A dialog in the middle of the screen. */
    else if (ops->centered)
    {
        x = (display_width - width) / 2;
        y = (work_height - height) / 2;
    }
    else if (ops->anchor_right)
    {
        x = display_width - 4 - width;
        y = work_height - height;
    }
    else
    {
        x = 2;
        y = work_height - height;
    }

    if (ops->open() != 0)
    {
        return;
    }
    if (gfx_window_open_role(
            &g_background,
            &slot->gfx,
            slot->window_id,
            SAVANXP_WM_WINDOW_ROLE_POPUP,
            ops->centered ? 0u : SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE,
            x,
            y,
            (uint32_t)width,
            (uint32_t)height) < 0)
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

static void close_context_menu(void)
{
    if (g_start_menu.open && g_start_menu.kind == DESKTOP_POPUP_CONTEXT_MENU)
    {
        close_slot(&g_start_menu);
    }
}

static void open_slot(struct desktop_popup_slot *slot, enum desktop_popup popup)
{
    open_slot_at(slot, popup, 0, 0, 0);
}

/* The taskbar's buttons are toggles: the same button closes what it opened.
 * The tray popups share a corner, so opening one closes the other. */
void desktop_toggle_popup(enum desktop_popup popup)
{
    struct desktop_popup_slot *slot = slot_for(popup);
    int was_this = slot->open && slot->kind == popup;

    close_context_menu();
    close_slot(slot);
    if (!was_this)
    {
        open_slot(slot, popup);
    }
}

/* The menus. A question stays until it is answered. */
void desktop_close_popups(void)
{
    if (!(g_start_menu.open && g_start_menu.kind == DESKTOP_POPUP_CONFIRM))
    {
        close_slot(&g_start_menu);
    }
    close_slot(&g_tray);
}

void desktop_open_popup_at(enum desktop_popup popup, int x, int y)
{
    struct desktop_popup_slot *slot = slot_for(popup);

    /* The context menu's slot is the Start menu's, which this closes too. */
    desktop_close_popups();
    close_slot(slot);
    open_slot_at(slot, popup, 1, x, y);
}

void desktop_repaint_background(void)
{
    g_background_repaint = 1;
}

void desktop_wallpaper_changed(void)
{
    g_wallpaper_dirty = 1;
    g_background_repaint = 1;
}

const struct savanxp_gfx_context *desktop_main_window(void)
{
    return &g_background;
}

int desktop_popup_is_open(enum desktop_popup popup)
{
    const struct desktop_popup_slot *slot = slot_for(popup);

    return slot->open && slot->kind == popup;
}

void desktop_hung_dismissed(uint32_t window_id)
{
    g_hung_answered = window_id;
}

/* Follows windowd's "not responding" marks: opens the End Program dialog for
 * the first marked window the user has not answered, and closes it when that
 * window ends or is no longer marked. */
static void service_hung(void)
{
    char title[SAVANXP_WM_WINDOW_TITLE_CAPACITY];
    uint32_t window_id = 0;
    int marked = desktop_taskbar_not_responding(&window_id, title, sizeof(title));

    if (!marked)
    {
        /* Nothing marked: an answer only holds while its mark lasts. */
        g_hung_answered = 0;
        close_slot(&g_hung);
        return;
    }
    if (g_hung.open && desktop_hung_target() != window_id)
    {
        close_slot(&g_hung);
    }
    if (!g_hung.open && window_id != g_hung_answered)
    {
        desktop_hung_set_target(window_id, title);
        open_slot(&g_hung, DESKTOP_POPUP_HUNG);
    }
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
    desktop_shell_request(&request);
}

void desktop_open_task_manager(void)
{
    desktop_close_popups();
    (void)desktop_launch(k_task_manager_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
}

/* The hotkeys this process registered. The main window also has the focus
 * once the background is clicked, so a key here is a hotkey only with the
 * exact modifiers it was registered with. Returns non-zero if it was one. */
static int handle_hotkey(const struct savanxp_input_event *event)
{
    uint32_t modifiers = event->modifiers &
        (SAVANXP_KEY_MOD_SHIFT | SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT);
    int console = (event->key == 't' || event->key == 'T') &&
        modifiers == (SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT);
    int start = event->key == SAVANXP_KEY_ESC && modifiers == SAVANXP_KEY_MOD_CTRL;
    int task_manager = event->key == SAVANXP_KEY_ESC &&
        modifiers == (SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_SHIFT);

    if (!console && !start && !task_manager)
    {
        return 0;
    }
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 1;
    }
    if (console)
    {
        desktop_close_popups();
        (void)desktop_launch(k_console_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
    }
    else if (task_manager)
    {
        desktop_open_task_manager();
    }
    else
    {
        desktop_toggle_popup(DESKTOP_POPUP_START_MENU);
    }
    return 1;
}

/* A key for the desktop itself: the open menu's or question's first, then
 * the icons'. */
static void handle_desktop_key(const struct savanxp_input_event *event)
{
    if (g_start_menu.open && g_start_menu.kind == DESKTOP_POPUP_CONTEXT_MENU)
    {
        apply_actions(desktop_context_menu_key(event), &g_start_menu);
        return;
    }
    if (g_start_menu.open && g_start_menu.kind == DESKTOP_POPUP_CONFIRM)
    {
        apply_actions(desktop_confirm_key(event), &g_start_menu);
        return;
    }
    if ((desktop_view_key(event) & DESKTOP_ACTION_REPAINT) != 0)
    {
        g_background_repaint = 1;
    }
}

static void paint_background(void)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    size_t bytes = (size_t)g_background.info.pitch * g_background.info.height;

    sx_bitmap_wrap(&bitmap, g_background.pixels, &g_background.info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    if (g_wallpaper_cache_bytes != bytes)
    {
        free(g_wallpaper_cache);
        g_wallpaper_cache = (uint32_t *)malloc(bytes);
        g_wallpaper_cache_bytes = g_wallpaper_cache != 0 ? bytes : 0;
        g_wallpaper_dirty = 1;
    }
    if (g_wallpaper_dirty || g_wallpaper_cache == 0)
    {
        desktop_wallpaper_draw(&painter, &g_background.info);
        if (g_wallpaper_cache != 0)
        {
            memcpy(g_wallpaper_cache, g_background.pixels, bytes);
            g_wallpaper_dirty = 0;
        }
    }
    else
    {
        memcpy(g_background.pixels, g_wallpaper_cache, bytes);
    }
    desktop_view_paint(&painter, &g_background.info);
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

/* The pointer reached the background or the taskbar: tell the open popups it
 * left them (desktop_popup_ops.pointer_left). */
static void notify_pointer_left_popups(void)
{
    int index;

    for (index = 0; index < DESKTOP_SLOT_COUNT; ++index)
    {
        struct desktop_popup_slot *slot = k_slots[index];

        if (slot->open && slot->ops != 0 && slot->ops->pointer_left != 0)
        {
            apply_actions(slot->ops->pointer_left(), slot);
        }
    }
}

/* Background: resize, its keys (the hotkeys, and the desktop's own once it
 * has the focus), and its pointer: a press closes the popups (windowd only
 * asks a popup to close for a press on another process; the background is
 * ours), and the icons get the rest. */
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
            desktop_view_resize(&g_background.info);
            desktop_wallpaper_changed();
        }
        else if (!handle_hotkey(&event))
        {
            handle_desktop_key(&event);
        }
    }
    while (gfx_poll_window_pointer(&g_background, &pointer) > 0)
    {
        uint32_t down = pointer.buttons & ~g_background_buttons;

        if ((down & (SAVANXP_MOUSE_BUTTON_LEFT | SAVANXP_MOUSE_BUTTON_RIGHT)) != 0)
        {
            desktop_close_popups();
        }
        else
        {
            notify_pointer_left_popups();
        }
        g_background_buttons = pointer.buttons;
        if ((desktop_view_pointer(&pointer) & DESKTOP_ACTION_REPAINT) != 0)
        {
            g_background_repaint = 1;
        }
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
        notify_pointer_left_popups();
        /* A press on the taskbar is a press outside the context menu. */
        if (pointer.buttons != 0)
        {
            close_context_menu();
        }
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
    for (index = 0; index < DESKTOP_SLOT_COUNT; ++index)
    {
        struct desktop_popup_slot *slot = k_slots[index];

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

    /* It closes its popups and taskbar itself when windowd goes away. */
    gfx_handle_close(1);
    if (gfx_open_shell(&g_background) < 0)
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

    /* Programs started from here inherit the working directory: a file
     * saved under a bare name lands in the user's profile, not at the root,
     * which the user may not write. */
    (void)chdir(savanxp_profile_dir());

    /* The resolution the user chose in Display Properties, if it is not the
     * one the session booted with. windowd resizes the background and its
     * RESIZED arrives with the first events. */
    {
        uint32_t width = 0;
        uint32_t height = 0;

        if (display_settings_read_mode(&width, &height) == 0 &&
            (width != g_background.info.width || height != g_background.info.height))
        {
            long result = gfx_set_display_mode(&g_background, width, height);

            if (result < 0)
            {
                eprintf("desktop: the saved resolution %ux%u was refused (%s)\n",
                    (unsigned)width, (unsigned)height, result_error_string(result));
            }
        }
    }

    /* Loads the persisted choice and decodes its image. */
    desktop_wallpaper_init();
    /* The Desktop folder, created with its first shortcuts if it is not there. */
    desktop_view_init(&g_background.info);
    /* Without the window list there is nothing for a taskbar to show: the
     * desktop runs as a background only. */
    g_taskbar_available = desktop_taskbar_init() == 0;
    if (g_taskbar_available)
    {
        /* NT 4: Ctrl+Esc opens the Start menu, Ctrl+Shift+Esc Task Manager. */
        register_hotkey(SAVANXP_KEY_ESC, SAVANXP_KEY_MOD_CTRL);
        register_hotkey(SAVANXP_KEY_ESC, SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_SHIFT);
        /* The Command Prompt's shortcut key, as a Windows shortcut can carry
         * one. AltGr+T stays a character: windowd never takes AltGr. */
        register_hotkey('t', SAVANXP_KEY_MOD_CTRL | SAVANXP_KEY_MOD_ALT);
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
        service_hung();
        service_popup(&g_hung);
        if (desktop_view_tick())
        {
            g_background_repaint = 1;
        }

        if (now >= next_wallpaper_check_ms)
        {
            next_wallpaper_check_ms = now + DESKTOP_WALLPAPER_CHECK_MS;
            if (desktop_wallpaper_reload())
            {
                desktop_wallpaper_changed();
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
    close_slot(&g_hung);
    if (g_taskbar_open)
    {
        (void)gfx_window_close(&g_background, &g_taskbar);
    }
    desktop_taskbar_shutdown();
    gfx_close(&g_background);
    return 0;
}
