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

/* The toolkit's host when nothing set one (include/sxgfx/sxgui.h): the core,
 * in libsxgui.so or linked in, asks the program for it. */
const struct sxgui_host *sxgui_default_host(void)
{
    return &k_sun_host;
}

/* ---- the clipboard's formats (docs/desktop/CLIPBOARD.md) ------------------- */

static int sxngui_copy_format_name(char *field, const char *name)
{
    int length = 0;

    if (name == 0)
    {
        return 0;
    }
    while (name[length] != '\0')
    {
        if (length + 1 >= (int)SAVANXP_CLIPBOARD_NAME_CAPACITY)
        {
            return 0;
        }
        field[length] = name[length];
        ++length;
    }
    field[length] = '\0';
    return length != 0;
}

static int sxngui_clipboard_set_formats(const struct sxgui_clipboard_format *formats, int count)
{
    struct savanxp_clipboard_entry entries[SAVANXP_CLIPBOARD_MAX_FORMATS];
    struct savanxp_clipboard_set request;
    long device;
    long result;
    int index;

    if (formats == 0 || count <= 0 || count > (int)SAVANXP_CLIPBOARD_MAX_FORMATS)
    {
        return -SAVANXP_EINVAL;
    }
    memset(entries, 0, sizeof(entries));
    for (index = 0; index < count; ++index)
    {
        if ((formats[index].data == 0 && formats[index].length != 0) ||
            !sxngui_copy_format_name(entries[index].name, formats[index].name))
        {
            return -SAVANXP_EINVAL;
        }
        entries[index].data = (uint64_t)(uintptr_t)formats[index].data;
        entries[index].length = formats[index].length;
    }
    memset(&request, 0, sizeof(request));
    request.entries = (uint64_t)(uintptr_t)entries;
    request.count = (uint32_t)count;

    device = sxn_open_device("clipboard", SXN_DEVICE_WRITE);
    if (device < 0)
    {
        return (int)device;
    }
    result = sxn_device_control(device, CLIP_IOC_SET_FORMATS, (unsigned long)&request);
    (void)sxn_close(device);
    return (int)result;
}

static int sxngui_clipboard_get_format(const char *name, void *buffer, int capacity)
{
    struct savanxp_clipboard_get request;
    long device;
    long result;

    if (capacity < 0 || (buffer == 0 && capacity != 0))
    {
        return -SAVANXP_EINVAL;
    }
    memset(&request, 0, sizeof(request));
    if (!sxngui_copy_format_name(request.name, name))
    {
        return -SAVANXP_EINVAL;
    }
    request.buffer = (uint64_t)(uintptr_t)buffer;
    request.capacity = (uint32_t)capacity;

    device = sxn_open_device("clipboard", SXN_DEVICE_READ);
    if (device < 0)
    {
        return (int)device;
    }
    result = sxn_device_control(device, CLIP_IOC_GET_FORMAT, (unsigned long)&request);
    (void)sxn_close(device);
    return result < 0 ? (int)result : (int)request.length;
}

static int sxngui_clipboard_enum_format(int index, char *name, int name_capacity)
{
    struct savanxp_clipboard_format_info request;
    long device;
    long result;
    int length = 0;

    if (index < 0 || name == 0 || name_capacity <= 0)
    {
        return -SAVANXP_EINVAL;
    }
    memset(&request, 0, sizeof(request));
    request.index = (uint32_t)index;

    device = sxn_open_device("clipboard", SXN_DEVICE_READ);
    if (device < 0)
    {
        return (int)device;
    }
    result = sxn_device_control(device, CLIP_IOC_ENUM_FORMAT, (unsigned long)&request);
    (void)sxn_close(device);
    if (result < 0)
    {
        return (int)result;
    }
    while (request.name[length] != '\0' && length + 1 < name_capacity)
    {
        name[length] = request.name[length];
        ++length;
    }
    name[length] = '\0';
    return (int)request.length;
}

static const struct sxgui_host_clipboard k_sun_host_clipboard = {
    sxngui_clipboard_set_formats,
    sxngui_clipboard_get_format,
    sxngui_clipboard_enum_format,
};

/* Strong, over POSIX's weak one, which the runtime links into every program
 * too (subsystems/posix/sdk/v1/runtime/sxgui_app.c). */
const struct sxgui_host_clipboard *sxgui_default_host_clipboard(void)
{
    return &k_sun_host_clipboard;
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
    sxgui_set_host_clipboard(&k_sun_host_clipboard);
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

long sxngui_app_decline_close(struct sxngui_app *app)
{
    if (app == 0)
    {
        return -SAVANXP_EINVAL;
    }
    return sxnwm_decline_close(&app->wm);
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

/* ---- context menus in popup windows ---------------------------------------------
 *
 * The frame is the popup host of its context: each sxgui_context_menu_open
 * asks the WM for a frameless popup owned by the window, and the loop hands
 * the popup its pointer and paints it. The window keeps the focus, so the
 * menu's keys arrive through the main window, as under sxgui_app_run. */
#define SXNGUI_POPUP_WINDOW_ID 2u

static int sxngui_popup_open(struct sxgui_popup_host *host, int x, int y, int width, int height)
{
    struct sxngui_app *app = (struct sxngui_app *)host->user;

    if (app == 0 || app->popup_window_open || width <= 0 || height <= 0)
    {
        return -1;
    }
    if (sxnwm_popup_open(&app->wm, &app->popup, SXNGUI_POPUP_WINDOW_ID, x, y, (uint32_t)width, (uint32_t)height) < 0)
    {
        return -1;
    }
    sxngui_fill_info(&app->popup_info, &app->popup);
    app->popup_window_open = 1;
    app->needs_repaint = 1;
    return 0;
}

static void sxngui_popup_close(struct sxgui_popup_host *host)
{
    struct sxngui_app *app = (struct sxngui_app *)host->user;

    if (app == 0 || !app->popup_window_open)
    {
        return;
    }
    (void)sxnwm_window_close(&app->wm, &app->popup, SXNGUI_POPUP_WINDOW_ID);
    app->popup_window_open = 0;
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
    /* An open context menu takes the keys before the app does. */
    if (sxgui_context_menu_active(&app->ui))
    {
        if (sxgui_handle_key(&app->ui, key))
        {
            app->needs_repaint = 1;
        }
        return;
    }
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
            /* With a context menu open, a press in the window only closes
             * it: the app's own hook does not see it. */
            if (sxgui_context_menu_active(&app->ui))
            {
                if (sxgui_handle_pointer(&app->ui, &record.payload.pointer))
                {
                    app->needs_repaint = 1;
                }
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

/* The owned windows' input, from the owner's event pipe: the dialog's and
 * the context menu popup's. Without on_pointer: the app's hook thinks in its
 * own window's coordinates, and these are the owned window's. */
static void sxngui_drain_owned(struct sxngui_app *app)
{
    struct savanxp_wm_event record;

    while ((app->dialog_window_open || app->popup_window_open) && sxnwm_poll_owned_input(&app->wm, &record) == 0)
    {
        if (record.window_id == SXNGUI_POPUP_WINDOW_ID)
        {
            if (app->popup_window_open && record.kind == SAVANXP_WM_EVENT_POINTER &&
                sxgui_context_menu_pointer(&app->ui, &record.payload.pointer))
            {
                app->needs_repaint = 1;
            }
            continue;
        }
        if (!app->dialog_window_open || record.window_id != SXNGUI_DIALOG_WINDOW_ID)
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
    if (app->popup_window_open && sxgui_context_menu_windowed(&app->ui))
    {
        sxgui_paint_context_menu_window(&app->ui, app->popup.pixels, &app->popup_info);
        if (sxngui_present(&app->popup) < 0)
        {
            sxgui_context_menu_close(&app->ui);
        }
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
 * dialog or the popup has input, or the next tick is due. */
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
    if (app->dialog_window_open || app->popup_window_open)
    {
        handles[count++] = (int32_t)app->wm.events_fd;
    }
    (void)sxn_wait_many(handles, count, timeout_ms);
}

void sxngui_app_watch_dialog(struct sxngui_app *app, struct sxgui_dialog *dialog,
                             void (*closed)(struct sxngui_app *app, struct sxgui_dialog *dialog, void *user),
                             void *user)
{
    if (app == 0)
    {
        return;
    }
    app->watched_dialog = closed != 0 ? dialog : 0;
    app->watched_closed = dialog != 0 ? closed : 0;
    app->watched_user = user;
}

/* The watched dialog is gone: tell its watcher, once. The handler may watch
 * another dialog, so the watch is cleared before it runs. */
static void sxngui_check_watch(struct sxngui_app *app)
{
    struct sxgui_dialog *dialog = app->watched_dialog;
    void (*closed)(struct sxngui_app *, struct sxgui_dialog *, void *) = app->watched_closed;
    void *user = app->watched_user;

    if (dialog == 0 || app->ui.modal == dialog)
    {
        return;
    }
    app->watched_dialog = 0;
    app->watched_closed = 0;
    app->watched_user = 0;
    if (closed != 0)
    {
        closed(app, dialog, user);
    }
    app->needs_repaint = 1;
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
    app->popup_host.open = sxngui_popup_open;
    app->popup_host.close = sxngui_popup_close;
    app->popup_host.user = app;
    app->ui.popup_host = &app->popup_host;
    next_tick_ms = sxngui_uptime_ms() + app->tick_interval_ms;
    /* An app that answers its own close keeps its waits going while it asks. */
    app->wm.close_handled = app->on_close != 0;

    while (app->running)
    {
        /* Reset first, then look: whatever arrives after the reset sets it
         * again and the wait below returns at once. */
        (void)sxn_event_reset(app->wm.wake_fd);

        /* The main window's close: the app's to answer when it has on_close,
         * once per request -- the request stands until the WM drops it. */
        if (sxnwm_close_requested(&app->wm))
        {
            if (app->on_close == 0)
            {
                sxngui_app_quit(app, 0);
                break;
            }
            if (!app->close_pending)
            {
                app->close_pending = 1;
                app->on_close(app);
                app->needs_repaint = 1;
            }
        }
        else
        {
            app->close_pending = 0;
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
        /* The WM asked the popup to close: a press landed off the menu. */
        if (app->popup_window_open && sxnwm_close_requested(&app->popup))
        {
            sxgui_context_menu_close(&app->ui);
            app->needs_repaint = 1;
        }
        /* The dialog's close button is a Cancel, like ESC. */
        if (app->dialog_window_open && sxnwm_close_requested(&app->dialog))
        {
            sxgui_dialog_end(&app->ui, 0);
            app->needs_repaint = 1;
        }

        sxngui_apply_resize(app);
        sxngui_drain_owned(app);
        sxngui_drain_main(app);
        sxngui_check_watch(app);
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

    sxgui_context_menu_close(&app->ui);
    app->ui.popup_host = 0;
    /* A dialog open at exit goes with its window, it is not abandoned. */
    if (sxgui_dialog_active(&app->ui))
    {
        sxgui_dialog_end(&app->ui, 0);
    }
    app->ui.dialog_host = 0;
    return app->exit_code;
}
