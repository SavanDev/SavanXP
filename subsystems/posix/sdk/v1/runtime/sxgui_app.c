#include "savanxp/sxgui.h"
#include "sxfont/ui.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A whole file for the interface faces' loader (sxfont/ui.h). */
static void *sxgui_app_read_file(const char *path, size_t *size)
{
    struct stat info;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return 0;
    }
    void *data = 0;
    if (fstat(fd, &info) == 0 && info.st_size > 0 && info.st_size < 16 * 1024 * 1024)
    {
        size_t done = 0;
        data = malloc((size_t)info.st_size);
        while (data != 0 && done < (size_t)info.st_size)
        {
            ssize_t got = read(fd, (char *)data + done, (size_t)info.st_size - done);
            if (got <= 0)
            {
                break;
            }
            done += (size_t)got;
        }
        if (data != 0 && done != (size_t)info.st_size)
        {
            free(data);
            data = 0;
        }
        *size = done;
    }
    close(fd);
    return data;
}

/* The interface faces from /system/fonts, for a POSIX program that draws
 * without sxgui_app (the desktop): sxgui_app_init calls it too. */
/* The person's appearance.cfg; a process with no profile of its own draws
 * for the session and reads the session's. */
static const char *sxgui_app_appearance_path(char *path, size_t capacity)
{
    const char *profile = savanxp_profile_dir();

    if (profile == 0 || strcmp(profile, "/") == 0)
    {
        return SXFONT_UI_SESSION_SETTINGS;
    }
    return savanxp_settings_path(SXFONT_UI_SETTINGS_NAME, 0, path, capacity) == 0 ? path : 0;
}

int sxgui_load_interface_faces(void)
{
    char path[SXFONT_UI_PATH_MAX];

    return sxfont_ui_load(sxgui_app_read_file, sxgui_app_appearance_path(path, sizeof(path)));
}

int sxgui_reload_interface_faces(void)
{
    char path[SXFONT_UI_PATH_MAX];

    return sxfont_ui_reload(sxgui_app_read_file, sxgui_app_appearance_path(path, sizeof(path)));
}

/* The toolkit's host for every POSIX program, whether it runs sxgui_app or
 * drives the widgets itself (seltest). The toolkit core defines none
 * (system/gfx/sxgui.c): it asks the program, in libsxgui.so as when linked in.
 * Weak, because the runtime's units are linked whole into every program, a SUN
 * one included, and SUN's frame (system/runtime/sxngui.c) defines its own. */
static const struct sxgui_host k_posix_host = {
    uptime_ms,
    clipboard_get_text,
    clipboard_set_text,
};

__attribute__((weak)) const struct sxgui_host *sxgui_default_host(void)
{
    return &k_posix_host;
}

/* The toolkit's format table and libc's calls take the same three fields;
 * the copy keeps either free to grow without the other noticing. */
static int sxgui_posix_set_formats(const struct sxgui_clipboard_format *formats, int count)
{
    struct clipboard_format copied[SAVANXP_CLIPBOARD_MAX_FORMATS];
    int index;

    if (formats == 0 || count <= 0 || count > (int)SAVANXP_CLIPBOARD_MAX_FORMATS)
    {
        return -(int)SAVANXP_EINVAL;
    }
    for (index = 0; index < count; ++index)
    {
        copied[index].name = formats[index].name;
        copied[index].data = formats[index].data;
        copied[index].length = formats[index].length;
    }
    return clipboard_set_formats(copied, count);
}

static const struct sxgui_host_clipboard k_posix_host_clipboard = {
    sxgui_posix_set_formats,
    clipboard_get_format,
    clipboard_enum_format,
};

/* Weak for the same reason as sxgui_default_host. */
__attribute__((weak)) const struct sxgui_host_clipboard *sxgui_default_host_clipboard(void)
{
    return &k_posix_host_clipboard;
}

static void sxgui_app_report(const char *name, const char *what)
{
    puts_fd(2, name != 0 ? name : "sxgui");
    puts_fd(2, ": ");
    puts_fd(2, what);
    puts_fd(2, "\n");
}

void sxgui_content_bounds(const struct sxgui_widget *widgets, int widget_count, int *width, int *height)
{
    int right = 0;
    int bottom = 0;
    int index;

    if (width != 0)
    {
        *width = 0;
    }
    if (height != 0)
    {
        *height = 0;
    }
    if (widgets == 0 || widget_count <= 0)
    {
        return;
    }

    for (index = 0; index < widget_count; ++index)
    {
        const struct sxgui_widget *widget = &widgets[index];
        int widget_right;
        int widget_bottom;

        if (widget->rect.width <= 0 || widget->rect.height <= 0)
        {
            continue;
        }
        widget_right = widget->rect.x + widget->rect.width;
        widget_bottom = widget->rect.y + widget->rect.height;
        if (widget_right > right)
        {
            right = widget_right;
        }
        if (widget_bottom > bottom)
        {
            bottom = widget_bottom;
        }
    }

    if (width != 0)
    {
        *width = right;
    }
    if (height != 0)
    {
        *height = bottom;
    }
}

int sxgui_app_set_content_size(struct sxgui_app *app, int width, int height)
{
    if (app == 0 || width <= 0 || height <= 0)
    {
        return 0;
    }
    if (gfx_request_content_size(&app->gfx, (uint32_t)width, (uint32_t)height) < 0)
    {
        return 0;
    }
    /* 250 ms alcanza de sobra: el WM atiende el pedido en su proxima vuelta
     * del loop. Si no llega, el RESIZED entra por el camino normal y la app
     * simplemente muestra un frame con el tamano viejo. */
    if (gfx_wait_content_size(&app->gfx, 250UL) <= 0)
    {
        return 0;
    }
    sxgui_context_retarget(&app->ui, app->gfx.pixels, &app->gfx.info);
    if (app->on_resize != 0)
    {
        app->on_resize(app);
    }
    app->needs_repaint = 1;
    return 1;
}

int sxgui_app_autosize(struct sxgui_app *app)
{
    int content_width = 0;
    int content_height = 0;

    if (app == 0)
    {
        return 0;
    }
    sxgui_content_bounds(app->ui.widgets, app->ui.widget_count, &content_width, &content_height);
    if (content_width <= 0 || content_height <= 0)
    {
        return 0;
    }
    return sxgui_app_set_content_size(
        app,
        content_width + SXGUI_CONTENT_MARGIN,
        content_height + SXGUI_CONTENT_MARGIN);
}

int sxgui_app_init(struct sxgui_app *app, const char *name, struct sxgui_widget *widgets, int widget_count)
{
    if (app == 0)
    {
        return -1;
    }
    memset(app, 0, sizeof(*app));
    app->pointer_fd = -1;
    (void)sxgui_load_interface_faces();

    if (gfx_open(&app->gfx) < 0)
    {
        sxgui_app_report(name, "gfx_open failed");
        return -1;
    }
    if (gfx_acquire(&app->gfx) < 0)
    {
        sxgui_app_report(name, "gfx_acquire failed");
        gfx_close(&app->gfx);
        return -1;
    }
    app->pointer_fd = gfx_pointer_open();

    sxgui_context_init(&app->ui, app->gfx.pixels, &app->gfx.info, widgets, widget_count);
    app->running = 1;
    app->needs_repaint = 1;
    return 0;
}

void sxgui_app_request_repaint(struct sxgui_app *app)
{
    if (app != 0)
    {
        app->needs_repaint = 1;
    }
}

void sxgui_app_quit(struct sxgui_app *app, int exit_code)
{
    if (app != 0)
    {
        app->running = 0;
        app->exit_code = exit_code;
    }
}

long sxgui_app_decline_close(struct sxgui_app *app)
{
    if (app == 0)
    {
        return -SAVANXP_EINVAL;
    }
    return gfx_decline_close(&app->gfx);
}

/* The main window's close (on_close). The request stands until the WM drops
 * it -- the process ends, or it turned the close down -- so the hook runs
 * once per request, not once per pass. Without the hook the runtime's default
 * close ends the process; with it, a WM that went away still quits, since
 * there is nothing left to show a question on. */
static void sxgui_app_service_close(struct sxgui_app *app)
{
    int requested;

    if (app->on_close == 0)
    {
        return;
    }
    requested = app->gfx.header != 0 &&
        (app->gfx.header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN) != 0u;
    if (!requested && gfx_should_close(&app->gfx))
    {
        sxgui_app_quit(app, 0);
        return;
    }
    if (!requested)
    {
        app->close_pending = 0;
        return;
    }
    if (!app->close_pending)
    {
        app->close_pending = 1;
        app->on_close(app);
        app->needs_repaint = 1;
    }
}

/* --- dialogo en ventana propia (docs/desktop/OWNED_WINDOWS.md) ---------------------
 *
 * El frame es el host de dialogos de su contexto: cada sxgui_dialog_begin pide
 * al WM una ventana con dueno para el dialogo, y el loop de abajo la pinta, la
 * presenta y le reparte su input. Un dialogo a la vez, como el toolkit. */
#define SXGUI_APP_DIALOG_WINDOW_ID 1u

static int sxgui_app_dialog_open(struct sxgui_dialog_host *host, struct sxgui_dialog *dialog, int width, int height)
{
    struct sxgui_app *app = (struct sxgui_app *)host->user;

    if (app == 0 || app->dialog_window_open || width <= 0 || height <= 0)
    {
        return -1;
    }
    if (gfx_window_open(
            &app->gfx,
            &app->dialog_gfx,
            SXGUI_APP_DIALOG_WINDOW_ID,
            (uint32_t)width,
            (uint32_t)height,
            dialog->title) < 0)
    {
        return -1;
    }
    app->dialog_window_open = 1;
    app->dialog_last_cursor_shape = 0;
    app->needs_repaint = 1;
    return 0;
}

static void sxgui_app_dialog_close(struct sxgui_dialog_host *host, struct sxgui_dialog *dialog)
{
    struct sxgui_app *app = (struct sxgui_app *)host->user;

    (void)dialog;
    if (app == 0 || !app->dialog_window_open)
    {
        return;
    }
    (void)gfx_window_close(&app->gfx, &app->dialog_gfx);
    app->dialog_window_open = 0;
    app->needs_repaint = 1;
}

static void sxgui_app_attach_dialog_host(struct sxgui_app *app)
{
    app->dialog_host.open = sxgui_app_dialog_open;
    app->dialog_host.close = sxgui_app_dialog_close;
    app->dialog_host.user = app;
    app->ui.dialog_host = &app->dialog_host;
}

static void sxgui_app_detach_dialog_host(struct sxgui_app *app)
{
    /* Un dialogo abierto al salir se cierra con su ventana, no se abandona. */
    if (sxgui_dialog_active(&app->ui))
    {
        sxgui_dialog_end(&app->ui, 0);
    }
    app->ui.dialog_host = 0;
}

/* --- context menu in a popup window ---------------------------------------
 * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications)
 *
 * The frame is the popup host of its context: each sxgui_context_menu_open
 * asks the WM for a frameless popup owned by the app's window, and the loop
 * below pumps its pointer into the toolkit and paints it. The app's window
 * keeps the focus, so the menu's keys arrive through the main window. */
#define SXGUI_APP_POPUP_WINDOW_ID 2u

static int sxgui_app_popup_open(struct sxgui_popup_host *host, int x, int y, int width, int height)
{
    struct sxgui_app *app = (struct sxgui_app *)host->user;

    if (app == 0 || app->popup_window_open || width <= 0 || height <= 0)
    {
        return -1;
    }
    if (gfx_window_open_role(
            &app->gfx,
            &app->popup_gfx,
            SXGUI_APP_POPUP_WINDOW_ID,
            SAVANXP_WM_WINDOW_ROLE_POPUP,
            SAVANXP_WM_POPUP_DISMISS_ON_OUTSIDE,
            x,
            y,
            (uint32_t)width,
            (uint32_t)height) < 0)
    {
        return -1;
    }
    app->popup_window_open = 1;
    app->needs_repaint = 1;
    return 0;
}

static void sxgui_app_popup_close(struct sxgui_popup_host *host)
{
    struct sxgui_app *app = (struct sxgui_app *)host->user;

    if (app == 0 || !app->popup_window_open)
    {
        return;
    }
    (void)gfx_window_close(&app->gfx, &app->popup_gfx);
    app->popup_window_open = 0;
    app->needs_repaint = 1;
}

static void sxgui_app_attach_popup_host(struct sxgui_app *app)
{
    app->popup_host.open = sxgui_app_popup_open;
    app->popup_host.close = sxgui_app_popup_close;
    app->popup_host.user = app;
    app->ui.popup_host = &app->popup_host;
}

static void sxgui_app_detach_popup_host(struct sxgui_app *app)
{
    sxgui_context_menu_close(&app->ui);
    app->ui.popup_host = 0;
}

static void sxgui_app_shutdown(struct sxgui_app *app)
{
    gfx_release(&app->gfx);
    if (app->pointer_fd >= 0)
    {
        savanxp_close((int)app->pointer_fd);
        app->pointer_fd = -1;
    }
    gfx_close(&app->gfx);
}

int sxgui_app_drag_start(struct sxgui_app *app, const struct sxgui_clipboard_format *formats, int count,
                         uint32_t effects)
{
    struct clipboard_format stored[SAVANXP_CLIPBOARD_MAX_FORMATS];
    int index;
    int result;

    if (app == 0 || formats == 0 || count <= 0 || count > (int)SAVANXP_CLIPBOARD_MAX_FORMATS)
    {
        return -SAVANXP_EINVAL;
    }
    for (index = 0; index < count; ++index)
    {
        stored[index].name = formats[index].name;
        stored[index].data = formats[index].data;
        stored[index].length = formats[index].length;
    }
    result = dragdata_set_formats(stored, count);
    if (result < 0)
    {
        return result;
    }
    return (int)gfx_drag_start(&app->gfx, effects);
}

/* A drag record for the main window (docs/desktop/DRAG_AND_DROP.md). */
static void sxgui_app_drag_event(struct sxgui_app *app, const struct savanxp_wm_drag_event *drag)
{
    switch (drag->phase)
    {
    case SAVANXP_WM_DRAG_ENTER:
    case SAVANXP_WM_DRAG_OVER: {
        uint32_t effect = app->on_drag_over != 0 ? app->on_drag_over(app, drag->x, drag->y, drag->effects) : 0u;
        (void)gfx_drag_reply(&app->gfx, drag->drag, effect & drag->effects);
        app->needs_repaint = 1;
        break;
    }
    case SAVANXP_WM_DRAG_LEAVE:
        if (app->on_drag_leave != 0)
        {
            app->on_drag_leave(app);
        }
        app->needs_repaint = 1;
        break;
    case SAVANXP_WM_DRAG_DROP:
        if (app->on_drop != 0)
        {
            app->on_drop(app, drag->x, drag->y, drag->effect);
        }
        app->needs_repaint = 1;
        break;
    case SAVANXP_WM_DRAG_END: {
        /* The window manager kept the button's release: the toolkit and the
         * app's hook hear it now, off the window, so nothing takes it for a
         * click. */
        struct savanxp_gui_pointer_event release;
        memset(&release, 0, sizeof(release));
        release.x = -1;
        release.y = -1;
        if (app->on_pointer == 0 || !app->on_pointer(app, &release))
        {
            (void)sxgui_handle_pointer(&app->ui, &release);
        }
        if (app->on_drag_end != 0)
        {
            app->on_drag_end(app, drag->effect);
        }
        app->needs_repaint = 1;
        break;
    }
    default:
        break;
    }
}

int sxgui_app_run(struct sxgui_app *app)
{
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer_event;
    struct savanxp_wm_drag_event drag_event;
    unsigned long next_tick_ms = 0;

    if (app == 0)
    {
        return 1;
    }

    next_tick_ms = uptime_ms() + app->tick_interval_ms;
    /* An app that answers its own close keeps the runtime from ending it. */
    if (app->on_close != 0)
    {
        gfx_handle_close(1);
    }
    /* Solo este loop sabe pintar y bombear la ventana de un dialogo: una app con
     * loop propio sigue teniendo el dialogo adentro de su superficie. */
    sxgui_app_attach_dialog_host(app);
    sxgui_app_attach_popup_host(app);

    while (app->running)
    {
        sxgui_app_service_close(app);
        if (!app->running)
        {
            break;
        }
        /* El intervalo se relee en cada vuelta: una app que deja cambiar la
         * velocidad de refresco solo tiene que escribir el campo. */
        if (app->on_tick != 0 && app->tick_interval_ms != 0)
        {
            unsigned long now = uptime_ms();

            if (now >= next_tick_ms)
            {
                app->on_tick(app);
                next_tick_ms = now + app->tick_interval_ms;
            }
        }

        /* El WM le pidio cerrar al dialogo (la X de su marco): es un Cancel,
         * igual que ESC. */
        if (app->dialog_window_open && gfx_should_close(&app->dialog_gfx))
        {
            sxgui_dialog_end(&app->ui, 0);
            app->needs_repaint = 1;
        }

        /* El teclado del dialogo entra por el mismo camino que el de la
         * ventana principal: con un modal activo el toolkit ya lo manda al
         * dialogo, y las apps que miran teclas antes (on_key) siguen viendo lo
         * mismo que cuando el dialogo era un overlay. Su superficie no cambia
         * de tamano, asi que no trae RESIZED. */
        while (app->dialog_window_open && gfx_poll_event(&app->dialog_gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                continue;
            }
            if (app->on_key != 0 && app->on_key(app, &event))
            {
                app->needs_repaint = 1;
                continue;
            }
            if (sxgui_handle_key(&app->ui, &event))
            {
                app->needs_repaint = 1;
            }
        }

        while (app->dialog_window_open && gfx_poll_window_pointer(&app->dialog_gfx, &pointer_event) > 0)
        {
            /* Sin on_pointer: el hook de la app piensa en coordenadas de SU
             * ventana, y estas son del dialogo. */
            if (sxgui_handle_pointer(&app->ui, &pointer_event))
            {
                app->needs_repaint = 1;
            }
            if (app->dialog_window_open)
            {
                int shape = sxgui_cursor_shape(&app->ui);
                if (shape != app->dialog_last_cursor_shape &&
                    gfx_desktop_set_cursor_shape(&app->dialog_gfx, (uint32_t)shape) == 0)
                {
                    app->dialog_last_cursor_shape = shape;
                }
            }
        }

        /* The WM asked the popup to close: a press landed off the menu. */
        if (app->popup_window_open && gfx_should_close(&app->popup_gfx))
        {
            sxgui_context_menu_close(&app->ui);
            app->needs_repaint = 1;
        }
        while (app->popup_window_open && gfx_poll_event(&app->popup_gfx, &event) > 0)
        {
        }
        while (app->popup_window_open && gfx_poll_window_pointer(&app->popup_gfx, &pointer_event) > 0)
        {
            if (sxgui_context_menu_pointer(&app->ui, &pointer_event))
            {
                app->needs_repaint = 1;
            }
        }

        while (gfx_poll_event(&app->gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&app->gfx, &event);
                sxgui_context_retarget(&app->ui, app->gfx.pixels, &app->gfx.info);
                if (app->on_resize != 0)
                {
                    app->on_resize(app);
                }
                app->needs_repaint = 1;
                continue;
            }
            /* An open context menu takes the keys before the app does. */
            if (sxgui_context_menu_active(&app->ui))
            {
                if (sxgui_handle_key(&app->ui, &event))
                {
                    app->needs_repaint = 1;
                }
                continue;
            }
            if (app->on_key != 0 && app->on_key(app, &event))
            {
                app->needs_repaint = 1;
                continue;
            }
            if (sxgui_handle_key(&app->ui, &event))
            {
                app->needs_repaint = 1;
                continue;
            }
            if (event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && event.key == SAVANXP_KEY_ESC)
            {
                sxgui_app_quit(app, 0);
            }
        }

        while (app->pointer_fd >= 0 && gfx_poll_pointer((int)app->pointer_fd, &pointer_event) > 0)
        {
            /* Con el dialogo en su ventana la principal esta deshabilitada: el WM
             * ya no le manda puntero, y lo que quedo en camino no se puede
             * interpretar contra el dialogo. */
            if (sxgui_dialog_windowed(&app->ui))
            {
                continue;
            }
            /* With a context menu open, a press in the window only closes it:
             * the app's own hook does not see it. */
            if (sxgui_context_menu_active(&app->ui))
            {
                if (sxgui_handle_pointer(&app->ui, &pointer_event))
                {
                    app->needs_repaint = 1;
                }
                continue;
            }
            if (app->on_pointer != 0 && app->on_pointer(app, &pointer_event))
            {
                app->needs_repaint = 1;
                continue;
            }
            if (sxgui_handle_pointer(&app->ui, &pointer_event))
            {
                app->needs_repaint = 1;
            }
            {
                int shape = sxgui_cursor_shape(&app->ui);
                if (shape != app->last_sent_cursor_shape &&
                    gfx_desktop_set_cursor_shape(&app->gfx, (uint32_t)shape) == 0)
                {
                    app->last_sent_cursor_shape = shape;
                }
            }
        }

        while (gfx_poll_window_drag(&app->gfx, &drag_event) > 0)
        {
            sxgui_app_drag_event(app, &drag_event);
        }

        /* The person changed a setting every window draws with: the faces
         * first, then everything measured and painted with them again. A
         * context menu was measured with the old faces and is closed. */
        {
            uint32_t changed = gfx_take_settings_changed();

            if (changed != 0u)
            {
                if ((changed & SAVANXP_WM_SETTINGS_FONTS) != 0u)
                {
                    (void)sxgui_reload_interface_faces();
                    if (sxgui_context_menu_active(&app->ui))
                    {
                        sxgui_context_menu_close(&app->ui);
                    }
                }
                if (app->on_resize != 0)
                {
                    app->on_resize(app);
                }
                if (app->on_settings != 0)
                {
                    app->on_settings(app, changed);
                }
                app->needs_repaint = 1;
            }
        }

        if (!app->running)
        {
            break;
        }

        if (app->needs_repaint)
        {
            /* El chrome del toolkit (menu bar, popups, dialogo modal) se pinta
             * DESPUES del dibujo propio de la app: si no, un menu desplegado
             * quedaria tapado por el contenido. */
            sxgui_paint_content(&app->ui);
            if (app->on_paint != 0)
            {
                app->on_paint(app);
            }
            sxgui_paint_overlay(&app->ui);
            if (gfx_present(&app->gfx, app->gfx.pixels) < 0)
            {
                sxgui_app_report(0, "present failed");
                sxgui_app_quit(app, 1);
                break;
            }
            if (app->popup_window_open && sxgui_context_menu_windowed(&app->ui))
            {
                sxgui_paint_context_menu_window(&app->ui, app->popup_gfx.pixels, &app->popup_gfx.info);
                if (gfx_present(&app->popup_gfx, app->popup_gfx.pixels) < 0)
                {
                    sxgui_context_menu_close(&app->ui);
                }
            }
            if (app->dialog_window_open && sxgui_dialog_windowed(&app->ui))
            {
                sxgui_paint_dialog_window(&app->ui, app->dialog_gfx.pixels, &app->dialog_gfx.info);
                /* Un dialogo que no se puede presentar no se deja abierto e
                 * invisible: se cancela y la app sigue. */
                if (gfx_present(&app->dialog_gfx, app->dialog_gfx.pixels) < 0)
                {
                    sxgui_dialog_end(&app->ui, 0);
                }
            }
            app->needs_repaint = 0;
        }
        else
        {
            sleep_ms(16);
        }
    }

    sxgui_app_detach_popup_host(app);
    sxgui_app_detach_dialog_host(app);
    sxgui_app_shutdown(app);
    return app->exit_code;
}
