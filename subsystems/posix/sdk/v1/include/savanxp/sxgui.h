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
    /* pre-toolkit pointer hook, simetrico a on_key; non-zero = consumed. Para
     * apps que hacen su propio hit-testing (grids, lienzos) en vez de widgets. */
    int (*on_pointer)(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event);
    void (*on_paint)(struct sxgui_app *app);  /* extra painting after sxgui_paint */
    void (*on_resize)(struct sxgui_app *app); /* widget relayout after RESIZED */

    /* Refresco periodico, para una ventana que muestra datos que cambian solos
     * -- un monitor, un reloj. Se llama cada `tick_interval_ms` mientras la app
     * corre; con el hook en NULL o el intervalo en 0 no pasa nada y el loop se
     * comporta como siempre.
     *
     * No es un temporizador preciso: el loop lo mira entre frames, asi que un
     * repintado largo corre el siguiente tick. Para contar tiempo hay que medir
     * el reloj adentro del hook, no contar llamadas. */
    unsigned long tick_interval_ms;
    void (*on_tick)(struct sxgui_app *app);

    void *user;

    /* Owned by the frame: the window of the active dialog (docs/desktop/OWNED_WINDOWS.md). */
    struct sxgui_dialog_host dialog_host;
    struct savanxp_gfx_context dialog_gfx;
    int dialog_window_open;
    int dialog_last_cursor_shape;
};

/* Margen que sxgui_app_autosize deja a la derecha y abajo del bounding box de
 * los widgets al pedirle su tamano a la ventana. Para que la ventana quede
 * simetrica, la app tiene que arrancar sus widgets en este mismo margen.
 *
 * Vale SXGUI_DIALOG_MARGIN porque quien autodimensiona es una ventana de
 * controles sueltos sobre la cara -- un Acerca de, una galeria --, que se lee
 * como un dialogo grande. Una ventana cuyo contenido es UN control grande
 * (el editor de notepad, la lista de files) usa SXGUI_MARGIN y calcula su
 * layout a partir del tamano de la ventana, no al reves. */
#define SXGUI_CONTENT_MARGIN SXGUI_DIALOG_MARGIN

/* Bounding box de los widgets: el borde derecho e inferior mas lejano. Es el
 * tamano que ocupa el contenido de una app de layout fijo. Devuelve 0 en
 * ambos si no hay ningun rect util (apps que calculan su layout a partir del
 * tamano de la ventana arrancan con todos los rects en cero). */
void sxgui_content_bounds(const struct sxgui_widget *widgets, int widget_count, int *width, int *height);

/* Open the gfx session and bind the widget array. On failure writes
 * "<name>: ..." to fd 2, leaves everything closed and returns < 0.
 *
 * No toca el tamano de la ventana: el WM la abre con su superficie generica y
 * es la app la que pide el suyo -- el tamano natural de una ventana lo sabe la
 * app, que conoce su layout, no el WM. Elegir UNA de las dos:
 *
 *   sxgui_app_autosize()          layout fijo: el bounding box de los widgets
 *   sxgui_app_set_content_size()  cualquier otro tamano que la app decida
 *
 * La segunda existe justamente porque el bounding box no siempre es la
 * respuesta: progman es un launcher y quiere lugar para crecer, filesapp
 * estira sus paneles con la ventana y arranca con los rects en cero. */
int sxgui_app_init(struct sxgui_app *app, const char *name, struct sxgui_widget *widgets, int widget_count);
/* Pide un area util de width x height y espera brevemente a que el WM la
 * aplique, para no mostrar un primer frame con el tamano viejo. Devuelve 1 si
 * el WM ya la aplico, 0 si no llego a tiempo (llega despues como RESIZED). */
int sxgui_app_set_content_size(struct sxgui_app *app, int width, int height);
/* Pide el bounding box de los widgets + SXGUI_CONTENT_MARGIN. No hace nada si
 * los widgets todavia no tienen layout (todos los rects en cero). */
int sxgui_app_autosize(struct sxgui_app *app);
void sxgui_app_request_repaint(struct sxgui_app *app);
void sxgui_app_quit(struct sxgui_app *app, int exit_code);
/* Blocks until the app quits; releases the gfx session and returns exit_code. */
int sxgui_app_run(struct sxgui_app *app);

#ifdef __cplusplus
}
#endif
