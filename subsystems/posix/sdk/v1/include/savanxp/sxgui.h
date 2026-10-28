#pragma once

/* The platform toolkit (sxgfx/sxgui.h) plus what only a POSIX program has:
 * the application frame over a POSIX window (sxgui_app.c), which is also the
 * toolkit's default host. */

#include "savanxp/libc.h"
#include "sxgfx/sxgui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- application frame (sxgui_app.c) -------------------------------------
 *
 * Owns the gfx session and the main loop every widget app repeats: poll
 * keyboard/pointer, apply window resizes, repaint when something changed,
 * present, throttle. ESC quits unless a hook consumes it first.
 */

struct sxgui_app {
    struct savanxp_gfx_context gfx;   /* exposed: e.g. gfx_desktop_launch(&app->gfx, ...) */
    struct sxgui_context ui;
    long pointer_fd;
    int running;
    int exit_code;
    int needs_repaint;
    int last_sent_cursor_shape;

    /* optional hooks; leave NULL to skip */
    int (*on_key)(struct sxgui_app *app, const struct savanxp_input_event *event); /* pre-toolkit; non-zero = consumed */
    /* pre-toolkit pointer hook, symmetric to on_key; non-zero = consumed. For
     * apps that do their own hit-testing (grids, canvases) instead of widgets. */
    int (*on_pointer)(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event);
    void (*on_paint)(struct sxgui_app *app);  /* extra painting after sxgui_paint */
    void (*on_resize)(struct sxgui_app *app); /* widget relayout after RESIZED */

    /* Periodic refresh, for a window showing data that changes on its own --
     * a monitor, a clock. Called every `tick_interval_ms` while the app runs;
     * with the hook NULL or the interval 0 nothing happens and the loop
     * behaves as always.
     *
     * It is not a precise timer: the loop checks it between frames, so a long
     * repaint delays the next tick. To measure time, read the clock inside
     * the hook; do not count calls. */
    unsigned long tick_interval_ms;
    void (*on_tick)(struct sxgui_app *app);

    void *user;

    /* Owned by the frame: the window of the active dialog (docs/desktop/OWNED_WINDOWS.md). */
    struct sxgui_dialog_host dialog_host;
    struct savanxp_gfx_context dialog_gfx;
    int dialog_window_open;
    int dialog_last_cursor_shape;
};

/* Margin sxgui_app_autosize leaves to the right of and below the widgets'
 * bounding box when it asks the window for its size. For the window to look
 * symmetric, the app has to start its widgets at this same margin.
 *
 * It is SXGUI_DIALOG_MARGIN because the windows that autosize hold loose
 * controls on the face -- an About box, a gallery --, which read as a large
 * dialog. A window whose content is ONE large control (notepad's editor, the
 * files list) uses SXGUI_MARGIN and computes its layout from the window size,
 * not the other way round. */
#define SXGUI_CONTENT_MARGIN SXGUI_DIALOG_MARGIN

/* Bounding box of the widgets: the farthest right and bottom edges. It is the
 * size a fixed-layout app's content takes. Returns 0 for both if there is no
 * usable rect (apps that compute their layout from the window size start with
 * every rect at zero). */
void sxgui_content_bounds(const struct sxgui_widget *widgets, int widget_count, int *width, int *height);

/* Open the gfx session and bind the widget array. On failure writes
 * "<name>: ..." to fd 2, leaves everything closed and returns < 0.
 *
 * It does not touch the window size: the WM opens the window with its generic
 * surface and the app asks for its own -- the natural size of a window is
 * known to the app, which knows its layout, not to the WM. Pick ONE of:
 *
 *   sxgui_app_autosize()          fixed layout: the widgets' bounding box
 *   sxgui_app_set_content_size()  any other size the app decides
 *
 * The second exists precisely because the bounding box is not always the
 * answer: progman is a launcher and wants room to grow, filesapp stretches its
 * panes with the window and starts with every rect at zero. */
int sxgui_app_init(struct sxgui_app *app, const char *name, struct sxgui_widget *widgets, int widget_count);
/* Asks for a usable area of width x height and waits briefly for the WM to
 * apply it, so the first frame is not shown at the old size. Returns 1 if the
 * WM has applied it, 0 if it did not arrive in time (it arrives later as
 * RESIZED). */
int sxgui_app_set_content_size(struct sxgui_app *app, int width, int height);
/* Asks for the widgets' bounding box + SXGUI_CONTENT_MARGIN. Does nothing if
 * the widgets have no layout yet (every rect at zero). */
int sxgui_app_autosize(struct sxgui_app *app);
void sxgui_app_request_repaint(struct sxgui_app *app);
void sxgui_app_quit(struct sxgui_app *app, int exit_code);
/* Blocks until the app quits; releases the gfx session and returns exit_code. */
int sxgui_app_run(struct sxgui_app *app);

#ifdef __cplusplus
}
#endif
