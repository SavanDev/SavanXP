#pragma once

#include "libc.h"
#include "savanxp/sxgui.h"
#include "savanxp/wm_shell_protocol.h"

/*
 * The desktop: one process that owns everything the shell shows -- the
 * background, the taskbar, the Start menu and the taskbar's popups -- as
 * windows with roles (docs/desktop/DESKTOP_ARCHITECTURE.md). windowd starts it
 * as the shell and knows nothing about what any of those windows shows: their
 * sizes, places and behaviour are decided here.
 *
 * desktop.c runs the windows and the loop; every other desktop_*.c is a module
 * that paints one window and reacts to its pointer.
 */

/* Window ids of the desktop process (savanxp_wm_window_request.window_id).
 * 0 is the main window, which windowd gives the background role. The Start
 * menu and the tray popups have an id each because they can be open at the
 * same time; the three tray popups share a corner and an id, so at most one of
 * them is open. The context menu shares the Start menu's id the same way. */
enum desktop_window_id
{
    DESKTOP_WINDOW_BACKGROUND = 0,
    DESKTOP_WINDOW_TASKBAR = 1,
    DESKTOP_WINDOW_START_MENU = 2,
    DESKTOP_WINDOW_TRAY_POPUP = 3,
    /* "Not responding": centred, a question rather than a menu. */
    DESKTOP_WINDOW_HUNG_DIALOG = 4,
};

enum desktop_popup
{
    DESKTOP_POPUP_START_MENU = 0,
    DESKTOP_POPUP_VOLUME = 1,
    DESKTOP_POPUP_CLOCK = 2,
    DESKTOP_POPUP_KEYBOARD = 3,
    DESKTOP_POPUP_HUNG = 4,
    /* The background's right-click menu, in the Start menu's window. */
    DESKTOP_POPUP_CONTEXT_MENU = 5,
    /* A question about an icon (delete it?), in the Start menu's window too. */
    DESKTOP_POPUP_CONFIRM = 6,
    /* Shut Down or Restart, in the Start menu's window as well. */
    DESKTOP_POPUP_SHUTDOWN = 7,
    DESKTOP_POPUP_COUNT = 8,
};

/* Bits a module's pointer or tick handler returns. */
#define DESKTOP_ACTION_NONE 0u
#define DESKTOP_ACTION_REPAINT 0x1u
/* Close the popup that returned it (a layout was chosen, a program launched). */
#define DESKTOP_ACTION_CLOSE 0x2u
/* Something the taskbar shows changed (volume, mute, keyboard layout): it
 * refreshes now instead of at its next poll. */
#define DESKTOP_ACTION_TASKBAR_CHANGED 0x4u

/* The taskbar's strip at the bottom of the screen. Its thickness is the
 * desktop's to choose; windowd reserves whatever the panel asks for. */
#define DESKTOP_TASKBAR_HEIGHT 28

/* One popup kind. `width` and `height` are its surface; `anchor_right` puts it
 * at the right end of the taskbar (the tray corner) instead of the left. A
 * popup opened at a point (desktop_open_popup_at) ignores both anchors. */
struct desktop_popup_ops
{
    int width;
    int height;
    int anchor_right;
    /* Prepares the popup's state each time it opens. Non-zero refuses to open
     * (for instance the volume popup without an audio device). */
    int (*open)(void);
    void (*close)(void);
    uint32_t (*pointer)(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer);
    /* Called every loop; may be 0. */
    uint32_t (*tick)(void);
    void (*paint)(struct savanxp_gfx_context *gfx);
    /* 1: centred on the screen and not dismissed by a press elsewhere (a
     * dialog); 0: anchored to the taskbar, closed by a press elsewhere. */
    int centered;
    /* The pointer is over the desktop background or the taskbar now, so it is
     * no longer over this popup. windowd sends no such event: there is always
     * a window under the pointer, so the popup is simply not told any more.
     * Moving onto an application window is not reported. May be 0. */
    uint32_t (*pointer_left)(void);
    /* The surface size, asked each time it opens, for a popup whose size
     * depends on its content; overrides `width` and `height`. May be 0. */
    void (*measure)(int *width, int *height);
};

extern const struct desktop_popup_ops desktop_startmenu_ops;
extern const struct desktop_popup_ops desktop_volume_ops;
extern const struct desktop_popup_ops desktop_clock_ops;
extern const struct desktop_popup_ops desktop_keyboard_ops;
extern const struct desktop_popup_ops desktop_hung_ops;
extern const struct desktop_popup_ops desktop_contextmenu_ops;
extern const struct desktop_popup_ops desktop_confirm_ops;
extern const struct desktop_popup_ops desktop_shutdown_ops;

/* desktop_contextmenu.c: shows `menu` at the screen point (x, y); the id of
 * the item chosen goes to `command` once the menu is closed. */
void desktop_context_menu(const struct sxgui_menu *menu, void (*command)(int id), int x, int y);
/* The open context menu's keys (arrows, Enter, Esc). */
uint32_t desktop_context_menu_key(const struct savanxp_input_event *event);

/* desktop_confirm.c: a centred question, Yes/No (`yes_no`) or only OK. The
 * answer goes to `answer` (which may be 0), once the dialog is closed. */
void desktop_confirm(const char *title, const char *text, int yes_no, void (*answer)(int yes));
uint32_t desktop_confirm_key(const struct savanxp_input_event *event);

/* desktop_shutdown.c: the Shut Down dialog, opened with `option` chosen. Yes
 * powers off or restarts; the keys go to desktop_shutdown_key while it is
 * open. */
#define DESKTOP_SHUTDOWN_POWER_OFF 0
#define DESKTOP_SHUTDOWN_RESTART 1
void desktop_shutdown_dialog(int option);
uint32_t desktop_shutdown_key(const struct savanxp_input_event *event);

/* desktop_view.c: the icons on the background (docs/desktop/DESKTOP_ICONS.md). */
void desktop_view_init(const struct savanxp_fb_info *info);
void desktop_view_resize(const struct savanxp_fb_info *info);
/* Re-reads the Desktop folder now and then; non-zero when it changed. */
int desktop_view_tick(void);
void desktop_view_paint(struct sx_painter *painter, const struct savanxp_fb_info *info);
uint32_t desktop_view_pointer(const struct savanxp_gui_pointer_event *pointer);
uint32_t desktop_view_key(const struct savanxp_input_event *event);

/* desktop_hung.c: the window the dialog is about (its id is its pid). */
void desktop_hung_set_target(uint32_t window_id, const char *title);
uint32_t desktop_hung_target(void);
/* desktop.c: the user answered for this window; do not ask again until windowd
 * clears and sets its flag anew. */
void desktop_hung_dismissed(uint32_t window_id);

/* What desktop.c offers the modules. */
void desktop_toggle_popup(enum desktop_popup popup);
void desktop_close_popups(void);
void desktop_open_task_manager(void);
int desktop_popup_is_open(enum desktop_popup popup);
/* Closes every other popup and opens this one with its top-left corner at the
 * screen point (x, y), flipped left or up where it would not fit above the
 * taskbar, as Windows places a context menu. */
void desktop_open_popup_at(enum desktop_popup popup, int x, int y);
/* The background must be painted again (an icon changed). */
void desktop_repaint_background(void);
/* The wallpaper changed: it is drawn again, not only the icons over it. */
void desktop_wallpaper_changed(void);
/* The desktop's main window, for launching programs from it. */
const struct savanxp_gfx_context *desktop_main_window(void);
/* Starts a program; it connects to windowd for its own window. */
long desktop_launch(const char *path, uint32_t launch_flags);
/* Sends a request on the shell's request pipe (savanxp/wm_shell_protocol.h). */
void desktop_shell_request(const struct savanxp_wm_shell_request *request);

/* The Win95 bevel every desktop surface draws with. */
void desktop_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken);

/* desktop_taskbar.c */
int desktop_taskbar_init(void);
void desktop_taskbar_shutdown(void);
uint32_t desktop_taskbar_pointer(const struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer);
uint32_t desktop_taskbar_tick(void);
/* Re-reads volume, mute and keyboard layout now. Non-zero if any changed. */
int desktop_taskbar_refresh_indicators(void);
void desktop_taskbar_paint(struct savanxp_gfx_context *gfx);
/* The first window windowd marks as not responding: 1 with its id and title,
 * 0 if none. From the last snapshot of the window list. */
int desktop_taskbar_not_responding(uint32_t *window_id, char *title, size_t capacity);
/* The id of the active application window, from the last snapshot of the
 * window list; 0 when the desktop has the focus or nothing is open. */
uint32_t desktop_taskbar_active_window(void);
/* The small icon of the window that process `pid` shows, from the last
 * snapshot of the window list: the window's own icon, or the generic one when
 * it brought none. 0 when the process has no window. `storage` describes the
 * pixels in place, so the result is valid until the next snapshot. */
struct desktop_embedded_bitmap;
const struct desktop_embedded_bitmap *desktop_taskbar_process_icon(
    uint32_t pid, struct desktop_embedded_bitmap *storage);
