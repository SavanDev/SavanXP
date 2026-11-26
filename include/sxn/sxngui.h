#pragma once

/* sxngui: the SXGUI application frame for SUN programs (docs/userland/SUN.md).
 *
 * SXGUI-C, the toolkit, is platform code that makes no system call
 * (docs/desktop/PLATFORM_GRAPHICS.md); what it needs from the system it takes
 * from a struct sxgui_host. This is SUN's side of that bargain, the
 * counterpart of the POSIX sxgui_app:
 *
 *   - the host: the clock for double clicks (sxn_clock_ns) and the clipboard
 *     (/dev/clipboard through sxn_open_device);
 *   - a window from windowd (sxnwm.h), with the toolkit painting into it;
 *   - dialogs in owned windows of their own, as POSIX programs have them
 *     (docs/desktop/OWNED_WINDOWS.md), and modeless windows beside the main
 *     one (struct sxngui_window);
 *   - a loop that sleeps on the window's events and the next tick, and
 *     repaints only what asked to be repainted.
 *
 * The names differ from the POSIX frame's (sxngui_app_*, not sxgui_app_*)
 * because a native program links the whole user runtime, POSIX frame
 * included, and the linker keeps only what is called.
 *
 * Built from system/runtime/sxngui.c, linked into every native program.
 */

#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxnwm.h"

#define SXNGUI_MAX_WINDOWS 2 /* modeless windows open at once */

struct sxngui_window;

struct sxngui_app {
    struct sxnwm_client wm;
    /* The window's size as the toolkit sees it; follows resizes. */
    struct savanxp_fb_info info;
    struct sxgui_context ui;
    int running;
    int exit_code;
    int needs_repaint;
    int last_cursor_shape;

    /* Optional hooks, as in the POSIX frame; leave NULL to skip. */
    int (*on_key)(struct sxngui_app *app, const struct savanxp_input_event *event); /* non-zero = consumed */
    int (*on_pointer)(struct sxngui_app *app, const struct savanxp_gui_pointer_event *event);
    void (*on_paint)(struct sxngui_app *app); /* the app's own painting, after the widgets */
    void (*on_resize)(struct sxngui_app *app); /* relayout after a new size */
    /* The WM asked the main window to close: its X, Alt+F4, or the session
     * ending for a shutdown. NULL: the app quits. Set, the app decides -- ask
     * "save changes?" and then sxngui_app_quit, or sxngui_app_decline_close if
     * the user cancels, which also cancels a shutdown. Once per request. */
    void (*on_close)(struct sxngui_app *app);

    /* Periodic refresh for a window that shows data changing on its own.
     * Called every tick_interval_ms while the app runs (0 or NULL: never).
     * Not a precise timer: read the clock inside the hook to measure. */
    unsigned long tick_interval_ms;
    void (*on_tick)(struct sxngui_app *app);

    /* Called on the UI thread after sxngui_app_wake, which any thread may
     * call: how a worker hands its results to the window, PostMessage's
     * role. Wakes coalesce: several before the loop looks are one call, so
     * the hook drains whatever the worker queued. NULL: wakes only repaint. */
    void (*on_wake)(struct sxngui_app *app);

    void *user;

    /* Owned by the frame: the window of the active dialog. */
    struct sxgui_dialog_host dialog_host;
    struct sxnwm_client dialog;
    struct savanxp_fb_info dialog_info;
    int dialog_window_open;
    int dialog_last_cursor_shape;

    /* Owned by the frame: the popup window of the open context menu
     * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications). */
    struct sxgui_popup_host popup_host;
    struct sxnwm_client popup;
    struct savanxp_fb_info popup_info;
    int popup_window_open;

    /* Owned by the frame: the dialog sxngui_app_watch_dialog watches. */
    struct sxgui_dialog *watched_dialog;
    void (*watched_closed)(struct sxngui_app *app, struct sxgui_dialog *dialog, void *user);
    void *watched_user;

    /* Owned by the frame: on_close was called for the request now standing. */
    int close_pending;

    /* Owned by the frame: what sxngui_app_wake sets. The event lives until
     * the process ends, so a worker that wakes a window already gone does
     * no harm. */
    long wake_event;
    int wake_pending;

    /* Owned by the frame: the modeless windows open now. */
    struct sxngui_window *windows[SXNGUI_MAX_WINDOWS];

    /* Drag and drop, as in the POSIX frame (docs/desktop/DRAG_AND_DROP.md);
     * leave NULL to skip. on_drag_over answers the SAVANXP_DRAG_EFFECT_*
     * the app would take at x, y, or 0; on_drop reads the formats from
     * /dev/dragdata (sxn_dragdata_open); on_drag_end, for a drag
     * sxngui_app_drag_start began, hears what the drop took. */
    uint32_t (*on_drag_over)(struct sxngui_app *app, int x, int y, uint32_t effects);
    void (*on_drag_leave)(struct sxngui_app *app);
    void (*on_drop)(struct sxngui_app *app, int x, int y, uint32_t effect);
    void (*on_drag_end)(struct sxngui_app *app, uint32_t effect);
};

/* A modeless window (docs/desktop/OWNED_WINDOWS.md#modeless-windows):
 * CreateDialog's window, where a dialog is DialogBox's. Framed, fixed in
 * size, above its owner and not a task, like a dialog; but the main window
 * stays usable, and a modal dialog of the app leaves this window usable too.
 * It has its own widgets in its own toolkit context, run by the app's loop
 * on the UI thread: its input, its painting, its close button.
 *
 * The caller owns the struct and keeps it alive while the window is open. */
struct sxngui_window {
    struct sxngui_app *app;
    struct sxnwm_client wm;
    struct savanxp_fb_info info;
    struct sxgui_context ui;
    uint32_t window_id;
    int open;
    int needs_repaint;
    int last_cursor_shape;

    /* Optional hooks; leave NULL to skip. */
    int (*on_key)(struct sxngui_window *window, const struct savanxp_input_event *event); /* non-zero = consumed */
    void (*on_paint)(struct sxngui_window *window); /* after its widgets */
    /* Its close button or Alt+F4. The window closes after the hook returns,
     * whatever it did; NULL: it just closes. */
    void (*on_close)(struct sxngui_window *window);
    void *user;
};

/* The toolkit's host for SUN. sxngui_app_init installs it; a SUN program
 * that drives the widgets without the frame installs it with sxgui_set_host. */
const struct sxgui_host *sxngui_host(void);

/* Milliseconds since boot, from the monotonic clock. */
unsigned long sxngui_uptime_ms(void);

/* Puts the interface faces from /system/fonts in the text primitives' slots
 * (include/sxfont/ui.h); how many came from files, 0 when the program does
 * not map libsxfont. sxngui_app_init calls it; a SUN program that draws
 * without the frame (windowd) calls it before it draws. */
int sxngui_load_interface_faces(void);

/* Connect to windowd, publish `presentation` (the program's compiled-in
 * resources, <program>.sunres.h), load the interface faces and bind the
 * widgets to the surface. 0, or negative -errno with nothing left open. */
int sxngui_app_init(struct sxngui_app *app, const struct sxnwm_presentation_source *presentation,
                    struct sxgui_widget *widgets, int widget_count);

/* Ask for a client area of width x height and wait briefly for the answer.
 * 1 when the window took the new size (on_resize has run), 0 if not yet. */
int sxngui_app_set_content_size(struct sxngui_app *app, int width, int height);

void sxngui_app_request_repaint(struct sxngui_app *app);
/* From any thread: makes the loop call on_wake on the UI thread and repaint.
 * 0, or -EINVAL for an app sxngui_app_init did not set up. */
long sxngui_app_wake(struct sxngui_app *app);
void sxngui_app_quit(struct sxngui_app *app, int exit_code);
/* Turns down the close on_close was called for (sxnwm_decline_close). */
long sxngui_app_decline_close(struct sxngui_app *app);

/* Calls `closed` once `dialog` is no longer the active one, however it went:
 * its own button, ESC, the window's close button, or another dialog begun
 * over it. It runs after the event that closed the dialog, with the watch
 * already cleared, so it may begin another dialog and watch that one; read
 * dialog->result to tell an OK from a cancel. One watch at a time: a new one
 * replaces the old one, and NULL `dialog` clears it. A program asking for a
 * file through sxnfiledlg.h does not need this; the file dialog does. */
void sxngui_app_watch_dialog(struct sxngui_app *app, struct sxgui_dialog *dialog,
                             void (*closed)(struct sxngui_app *app, struct sxgui_dialog *dialog, void *user),
                             void *user);

/* Open `window` beside `app`'s main window: `width` x `height` client
 * pixels, centred over the main window, titled `title`, with `widgets` laid
 * out in its client area. The hooks and `user` set before the call are
 * kept. 0, -EBUSY when SXNGUI_MAX_WINDOWS are already open, or the WM's
 * -errno (no room on screen, no free slot). */
int sxngui_window_open(struct sxngui_app *app, struct sxngui_window *window, const char *title, int width,
                       int height, struct sxgui_widget *widgets, int widget_count);

/* Close it. Closing one that is not open does nothing. */
void sxngui_window_close(struct sxngui_window *window);

/* Repaint it on the loop's next pass. */
void sxngui_window_request_repaint(struct sxngui_window *window);

/* Drags these formats from the main window, allowing `effects`, from a
 * pointer handler while the left button is held (sxgui_app_drag_start's SUN
 * side). 0, or < 0 when the formats could not be stored. */
int sxngui_app_drag_start(struct sxngui_app *app, const struct sxgui_clipboard_format *formats, int count,
                          uint32_t effects);
/* Run until sxngui_app_quit or the window is closed; returns the exit code.
 * Closes the window, and any modeless window still open, on the way out. */
int sxngui_app_run(struct sxngui_app *app);
