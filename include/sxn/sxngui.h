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
 *     (docs/desktop/OWNED_WINDOWS.md);
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

    /* Periodic refresh for a window that shows data changing on its own.
     * Called every tick_interval_ms while the app runs (0 or NULL: never).
     * Not a precise timer: read the clock inside the hook to measure. */
    unsigned long tick_interval_ms;
    void (*on_tick)(struct sxngui_app *app);

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
};

/* The toolkit's host for SUN. sxngui_app_init installs it; a SUN program
 * that drives the widgets without the frame installs it with sxgui_set_host. */
const struct sxgui_host *sxngui_host(void);

/* Milliseconds since boot, from the monotonic clock. */
unsigned long sxngui_uptime_ms(void);

/* Connect to windowd, publish `presentation` (the program's compiled-in
 * resources, <program>.sunres.h) and bind the widgets to the surface.
 * 0, or negative -errno with nothing left open. */
int sxngui_app_init(struct sxngui_app *app, const struct sxnwm_presentation_source *presentation,
                    struct sxgui_widget *widgets, int widget_count);

/* Ask for a client area of width x height and wait briefly for the answer.
 * 1 when the window took the new size (on_resize has run), 0 if not yet. */
int sxngui_app_set_content_size(struct sxngui_app *app, int width, int height);

void sxngui_app_request_repaint(struct sxngui_app *app);
void sxngui_app_quit(struct sxngui_app *app, int exit_code);

/* Run until sxngui_app_quit or the window is closed; returns the exit code.
 * Closes the window on the way out. */
int sxngui_app_run(struct sxngui_app *app);
