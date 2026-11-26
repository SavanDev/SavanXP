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
    /* The WM asked the main window to close: its X, Alt+F4, or the session
     * ending for a shutdown. NULL: the app quits, as always. Set, the app
     * decides -- ask "save changes?" and then sxgui_app_quit, or
     * sxgui_app_decline_close if the user cancels, which also cancels a
     * shutdown. Called once per request. The WM going away still quits. */
    void (*on_close)(struct sxgui_app *app);

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

    /* Owned by the frame: the popup window of the open context menu
     * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications). */
    struct sxgui_popup_host popup_host;
    struct savanxp_gfx_context popup_gfx;
    int popup_window_open;

    /* Owned by the frame: on_close was called for the request now standing. */
    int close_pending;

    /* Drag and drop (docs/desktop/DRAG_AND_DROP.md); leave NULL to skip.
     * As a target: on_drag_over is called for every enter and move of a drag
     * over the main window, at x, y in its client area, with the effects the
     * source allows; it answers the SAVANXP_DRAG_EFFECT_* the app would take
     * there, or 0. Without it nothing is dropped here. on_drag_leave: the
     * drag left, or ended elsewhere. on_drop: dropped at x, y with `effect`;
     * read the formats with dragdata_get_format now. As a source
     * (sxgui_app_drag_start): on_drag_end hears what the drop took, 0 for
     * nothing; the toolkit has already been told the button came up. */
    uint32_t (*on_drag_over)(struct sxgui_app *app, int x, int y, uint32_t effects);
    void (*on_drag_leave)(struct sxgui_app *app);
    void (*on_drop)(struct sxgui_app *app, int x, int y, uint32_t effect);
    void (*on_drag_end)(struct sxgui_app *app, uint32_t effect);

    /* The person changed a setting every window draws with
     * (SAVANXP_WM_SETTINGS_*). The frame has already applied it (new faces
     * in, an open context menu closed) and calls on_resize, so a layout
     * measured from the text is measured again, then repaints; this hears
     * which settings. NULL to skip. */
    void (*on_settings)(struct sxgui_app *app, uint32_t changed);
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
/* Puts the interface faces from /system/fonts in the text primitives' slots
 * (docs/desktop/UI_FONTS.md); how many came from files, 0 when the program
 * does not map libsxfont. sxgui_app_init calls it; a program that draws
 * without sxgui_app (the desktop) calls it before it draws. */
int sxgui_load_interface_faces(void);
/* The person picked other faces (SAVANXP_WM_SETTINGS_FONTS, read with
 * gfx_take_settings_changed): reads appearance.cfg again and installs its
 * faces in place of the loaded ones. sxgui_app does it and repaints; a
 * program with its own loop calls it itself. How many came from files. */
int sxgui_reload_interface_faces(void);
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
/* Turns down the close on_close was called for (gfx_decline_close). */
long sxgui_app_decline_close(struct sxgui_app *app);
/* Drags these formats from the main window, allowing `effects`
 * (SAVANXP_DRAG_EFFECT_*): call it from a pointer handler while the left
 * button is held. The window manager has the pointer until the drag ends;
 * on_drag_end says how. 0, or < 0 when the formats could not be stored. */
int sxgui_app_drag_start(struct sxgui_app *app, const struct sxgui_clipboard_format *formats, int count,
                         uint32_t effects);
/* Blocks until the app quits; releases the gfx session and returns exit_code. */
int sxgui_app_run(struct sxgui_app *app);

#ifdef __cplusplus
}
#endif
