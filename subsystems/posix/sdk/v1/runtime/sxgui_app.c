#include "savanxp/sxgui.h"

#include <string.h>

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

int sxgui_app_run(struct sxgui_app *app)
{
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer_event;

    if (app == 0)
    {
        return 1;
    }

    while (app->running)
    {
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
            app->needs_repaint = 0;
        }
        else
        {
            sleep_ms(16);
        }
    }

    sxgui_app_shutdown(app);
    return app->exit_code;
}
