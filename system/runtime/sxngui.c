/* The SXGUI application frame for SUN programs: see include/sxn/sxngui.h. */

#include "sxn/sxngui.h"

#include <string.h>

/* ---- the toolkit's host ----------------------------------------------------- */

unsigned long sxngui_uptime_ms(void)
{
    return (unsigned long)(sxn_clock_ns() / 1000000ull);
}

static int sxngui_clipboard_get_text(char *buffer, int capacity)
{
    struct savanxp_clipboard_info info;
    long device;
    long copied;
    long result;

    if (buffer == 0 || capacity <= 0)
    {
        return -SAVANXP_EINVAL;
    }
    buffer[0] = '\0';
    device = sxn_open_device("clipboard", SXN_DEVICE_READ);
    if (device < 0)
    {
        return (int)device;
    }
    /* The length first, so a truncated paste can be told from a whole one:
     * the read returns what fit, not what there was. */
    result = sxn_device_control(device, CLIP_IOC_GET_INFO, (unsigned long)&info);
    copied = result < 0 ? result : sxn_read(device, buffer, (unsigned long)(capacity - 1));
    (void)sxn_close(device);
    if (copied < 0)
    {
        return (int)copied;
    }
    buffer[copied] = '\0';
    return (int)info.length;
}

static int sxngui_clipboard_set_text(const char *text)
{
    unsigned long length;
    long device;
    long written;

    if (text == 0)
    {
        return -SAVANXP_EINVAL;
    }
    length = (unsigned long)strlen(text);
    device = sxn_open_device("clipboard", SXN_DEVICE_WRITE);
    if (device < 0)
    {
        return (int)device;
    }
    /* A write of nothing never reaches the device, so emptying the clipboard
     * is its own request. A write replaces the whole content. */
    if (length == 0ul)
    {
        written = sxn_device_control(device, CLIP_IOC_CLEAR, 0);
        (void)sxn_close(device);
        return (int)written;
    }
    written = sxn_write(device, text, length);
    (void)sxn_close(device);
    if (written < 0)
    {
        return (int)written;
    }
    return written == (long)length ? 0 : -SAVANXP_EIO;
}

static const struct sxgui_host k_sun_host = {
    sxngui_uptime_ms,
    sxngui_clipboard_get_text,
    sxngui_clipboard_set_text,
};

const struct sxgui_host *sxngui_host(void)
{
    return &k_sun_host;
}

/* ---- the window ---------------------------------------------------------------- */

static void sxngui_fill_info(struct savanxp_fb_info *info, const struct sxnwm_client *window)
{
    *info = window->header->info;
    info->width = window->width;
    info->height = window->height;
}

/* Present the whole surface, after the compositor is done with the previous
 * frame: painting over a frame it is still reading would show half of each. */
static long sxngui_present(struct sxnwm_client *window)
{
    long result;

    result = sxnwm_submit_full(window);
    if (result == -SAVANXP_EAGAIN)
    {
        (void)sxnwm_wait_idle(window, 100);
        result = sxnwm_submit_full(window);
    }
    return result < 0 ? result : 0;
}

static void sxngui_apply_resize(struct sxngui_app *app)
{
    if (sxnwm_check_resize(&app->wm) <= 0)
    {
        return;
    }
    sxngui_fill_info(&app->info, &app->wm);
    sxgui_context_retarget(&app->ui, app->wm.pixels, &app->info);
    if (app->on_resize != 0)
    {
        app->on_resize(app);
    }
    app->needs_repaint = 1;
}

int sxngui_app_init(struct sxngui_app *app, const struct sxnwm_presentation_source *presentation,
                    struct sxgui_widget *widgets, int widget_count)
{
    long result;

    if (app == 0)
    {
        return -SAVANXP_EINVAL;
    }
    memset(app, 0, sizeof(*app));
    sxgui_set_host(&k_sun_host);
    result = sxnwm_connect(&app->wm);
    if (result < 0)
    {
        return (int)result;
    }
    if (presentation != 0)
    {
        (void)sxnwm_set_presentation(&app->wm, presentation);
    }
    sxngui_fill_info(&app->info, &app->wm);
    sxgui_context_init(&app->ui, app->wm.pixels, &app->info, widgets, widget_count);
    app->running = 1;
    app->needs_repaint = 1;
    return 0;
}

int sxngui_app_set_content_size(struct sxngui_app *app, int width, int height)
{
    uint64_t deadline;

    if (app == 0 || width <= 0 || height <= 0 ||
        sxnwm_request_size(&app->wm, (uint32_t)width, (uint32_t)height) < 0)
    {
        return 0;
    }
    /* The WM takes the request on its next pass; 250 ms is plenty. If the
     * answer is late, the loop picks the resize up like any other. */
    deadline = sxn_clock_ns() + 250ull * 1000000ull;
    for (;;)
    {
        (void)sxn_event_reset(app->wm.wake_fd);
        if (app->wm.header->info.width == (uint32_t)width && app->wm.header->info.height == (uint32_t)height)
        {
            break;
        }
        if (sxn_clock_ns() >= deadline)
        {
            return 0;
        }
        (void)sxn_wait_one(app->wm.wake_fd, 20);
    }
    sxngui_apply_resize(app);
    return 1;
}

void sxngui_app_request_repaint(struct sxngui_app *app)
{
    if (app != 0)
    {
        app->needs_repaint = 1;
    }
}

void sxngui_app_quit(struct sxngui_app *app, int exit_code)
{
    if (app != 0)
    {
        app->running = 0;
        app->exit_code = exit_code;
    }
}

/* ---- dialogs in owned windows -------------------------------------------------- *
 *
 * The frame is the dialog host of its context: each sxgui_dialog_begin asks
 * the WM for an owned window, and the loop paints it, presents it and hands
 * it its input. One dialog at a time, as in the toolkit. */
#define SXNGUI_DIALOG_WINDOW_ID 1u

static int sxngui_dialog_open(struct sxgui_dialog_host *host, struct sxgui_dialog *dialog, int width, int height)
{
    struct sxngui_app *app = (struct sxngui_app *)host->user;

    if (app == 0 || app->dialog_window_open || width <= 0 || height <= 0)
    {
        return -1;
    }
    if (sxnwm_window_open(&app->wm, &app->dialog, SXNGUI_DIALOG_WINDOW_ID, (uint32_t)width, (uint32_t)height,
                          dialog->title) < 0)
    {
        return -1;
    }
    sxngui_fill_info(&app->dialog_info, &app->dialog);
    app->dialog_window_open = 1;
    app->dialog_last_cursor_shape = 0;
    app->needs_repaint = 1;
    return 0;
}

static void sxngui_dialog_close(struct sxgui_dialog_host *host, struct sxgui_dialog *dialog)
{
    struct sxngui_app *app = (struct sxngui_app *)host->user;

    (void)dialog;
    if (app == 0 || !app->dialog_window_open)
    {
        return;
    }
    (void)sxnwm_window_close(&app->wm, &app->dialog, SXNGUI_DIALOG_WINDOW_ID);
    app->dialog_window_open = 0;
    app->needs_repaint = 1;
}

/* ---- input ---------------------------------------------------------------------- */

static void sxngui_update_cursor(struct sxngui_app *app, struct sxnwm_client *window, int *last)
{
    int shape = sxgui_cursor_shape(&app->ui);

    if (shape != *last && sxnwm_set_cursor_shape(window, (uint32_t)shape) == 0)
    {
        *last = shape;
    }
}

static void sxngui_key(struct sxngui_app *app, const struct savanxp_input_event *key, int main_window)
{
    if (app->on_key != 0 && app->on_key(app, key))
    {
        app->needs_repaint = 1;
        return;
    }
    if (sxgui_handle_key(&app->ui, key))
    {
        app->needs_repaint = 1;
        return;
    }
    if (main_window && key->type == SAVANXP_INPUT_EVENT_KEY_DOWN && key->key == SAVANXP_KEY_ESC)
    {
        sxngui_app_quit(app, 0);
    }
}

/* The main window's input, from the ring. */
static void sxngui_drain_main(struct sxngui_app *app)
{
    struct savanxp_wm_event record;

    while (sxnwm_poll_input(&app->wm, &record) == 0)
    {
        if (record.kind == SAVANXP_WM_EVENT_KEY)
        {
            sxngui_key(app, &record.payload.key, 1);
        }
        else if (record.kind == SAVANXP_WM_EVENT_POINTER)
        {
            /* With the dialog in its own window the owner is disabled: what
             * was already on its way cannot be read against the dialog. */
            if (sxgui_dialog_windowed(&app->ui))
            {
                continue;
            }
            if (app->on_pointer != 0 && app->on_pointer(app, &record.payload.pointer))
            {
                app->needs_repaint = 1;
                continue;
            }
            if (sxgui_handle_pointer(&app->ui, &record.payload.pointer))
            {
                app->needs_repaint = 1;
            }
            sxngui_update_cursor(app, &app->wm, &app->last_cursor_shape);
        }
    }
}

/* The dialog's input, from the owner's event pipe. Without on_pointer: the
 * app's hook thinks in its own window's coordinates, and these are the
 * dialog's. */
static void sxngui_drain_dialog(struct sxngui_app *app)
{
    struct savanxp_wm_event record;

    if (!app->dialog_window_open)
    {
        return;
    }
    while (app->dialog_window_open && sxnwm_poll_owned_input(&app->wm, &record) == 0)
    {
        if (record.window_id != SXNGUI_DIALOG_WINDOW_ID)
        {
            continue;
        }
        if (record.kind == SAVANXP_WM_EVENT_KEY)
        {
            if (record.payload.key.type != SAVANXP_INPUT_EVENT_RESIZED)
            {
                sxngui_key(app, &record.payload.key, 0);
            }
        }
        else if (record.kind == SAVANXP_WM_EVENT_POINTER)
        {
            if (sxgui_handle_pointer(&app->ui, &record.payload.pointer))
            {
                app->needs_repaint = 1;
            }
            if (app->dialog_window_open)
            {
                sxngui_update_cursor(app, &app->dialog, &app->dialog_last_cursor_shape);
            }
        }
    }
}

/* ---- the loop ---------------------------------------------------------------------- */

static void sxngui_paint(struct sxngui_app *app)
{
    (void)sxnwm_wait_idle(&app->wm, 100);
    /* The toolkit's chrome (menu bar, popups, an in-window dialog) is painted
     * AFTER the app's own drawing, or an open menu would end up under it. */
    sxgui_paint_content(&app->ui);
    if (app->on_paint != 0)
    {
        app->on_paint(app);
    }
    sxgui_paint_overlay(&app->ui);
    if (sxngui_present(&app->wm) < 0)
    {
        sxngui_app_quit(app, 1);
        return;
    }
    if (app->dialog_window_open && sxgui_dialog_windowed(&app->ui))
    {
        sxgui_paint_dialog_window(&app->ui, app->dialog.pixels, &app->dialog_info);
        /* A dialog that cannot be presented is not left open and invisible:
         * it is cancelled and the app goes on. */
        if (sxngui_present(&app->dialog) < 0)
        {
            sxgui_dialog_end(&app->ui, 0);
        }
    }
}

/* Sleep until the window has something (input, a resize, a close), a
 * dialog has input, or the next tick is due. */
static void sxngui_wait(struct sxngui_app *app, unsigned long next_tick_ms)
{
    int32_t handles[2];
    unsigned long count = 0;
    long timeout_ms = -1;

    if (app->on_tick != 0 && app->tick_interval_ms != 0)
    {
        unsigned long now = sxngui_uptime_ms();
        timeout_ms = next_tick_ms > now ? (long)(next_tick_ms - now) : 0;
    }
    handles[count++] = (int32_t)app->wm.wake_fd;
    if (app->dialog_window_open)
    {
        handles[count++] = (int32_t)app->wm.events_fd;
    }
    (void)sxn_wait_many(handles, count, timeout_ms);
}

int sxngui_app_run(struct sxngui_app *app)
{
    unsigned long next_tick_ms;

    if (app == 0 || app->wm.header == 0)
    {
        return 1;
    }
    app->dialog_host.open = sxngui_dialog_open;
    app->dialog_host.close = sxngui_dialog_close;
    app->dialog_host.user = app;
    app->ui.dialog_host = &app->dialog_host;
    next_tick_ms = sxngui_uptime_ms() + app->tick_interval_ms;

    while (app->running)
    {
        /* Reset first, then look: whatever arrives after the reset sets it
         * again and the wait below returns at once. */
        (void)sxn_event_reset(app->wm.wake_fd);

        if (sxnwm_close_requested(&app->wm))
        {
            sxngui_app_quit(app, 0);
            break;
        }
        /* The interval is read on every pass: an app that offers a refresh
         * speed only has to write the field. */
        if (app->on_tick != 0 && app->tick_interval_ms != 0)
        {
            unsigned long now = sxngui_uptime_ms();

            if (now >= next_tick_ms)
            {
                app->on_tick(app);
                next_tick_ms = now + app->tick_interval_ms;
            }
        }
        /* The dialog's close button is a Cancel, like ESC. */
        if (app->dialog_window_open && sxnwm_close_requested(&app->dialog))
        {
            sxgui_dialog_end(&app->ui, 0);
            app->needs_repaint = 1;
        }

        sxngui_apply_resize(app);
        sxngui_drain_dialog(app);
        sxngui_drain_main(app);
        if (!app->running)
        {
            break;
        }

        if (app->needs_repaint)
        {
            app->needs_repaint = 0;
            sxngui_paint(app);
            continue;
        }
        sxngui_wait(app, next_tick_ms);
    }

    /* A dialog open at exit goes with its window, it is not abandoned. */
    if (sxgui_dialog_active(&app->ui))
    {
        sxgui_dialog_end(&app->ui, 0);
    }
    app->ui.dialog_host = 0;
    return app->exit_code;
}
