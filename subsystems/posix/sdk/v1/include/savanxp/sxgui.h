#pragma once

/*
 * sxgui - a minimal Win9x-flavoured widget toolkit on top of sxgfx.
 *
 * Retained but allocation-free: the application owns a flat array of
 * sxgui_widget and the toolkit paints them and dispatches input. Widgets draw
 * directly into the window backbuffer, confined with the painter clip stack.
 */

#include "savanxp/libc.h"
#include "savanxp/gfx2d.h"
#include "savanxp/sxchrome.h"
#include "savanxp/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXGUI_RGB(r, g, b) SXCHROME_RGB(r, g, b)

/* Classic 3D system palette.
 *
 * VIVE EN savanxp/sxchrome.h: no es del toolkit, es del sistema -- el marco de
 * ventana del WM y una app que pinta contenido propio tienen que dar con los
 * mismos tonos que un boton. Estos nombres siguen siendo los que usa el codigo
 * del toolkit y de las apps, asi que se quedan como alias. */
#define SXGUI_COLOR_FACE          SXCHROME_COLOR_FACE
#define SXGUI_COLOR_SHADOW        SXCHROME_COLOR_SHADOW
#define SXGUI_COLOR_DARK          SXCHROME_COLOR_DARK
#define SXGUI_COLOR_BEVEL         SXCHROME_COLOR_BEVEL
#define SXGUI_COLOR_LIGHT         SXCHROME_COLOR_LIGHT
#define SXGUI_COLOR_TEXT          SXCHROME_COLOR_TEXT
#define SXGUI_COLOR_DISABLED_TEXT SXCHROME_COLOR_DISABLED_TEXT
#define SXGUI_COLOR_FIELD         SXCHROME_COLOR_FIELD
#define SXGUI_COLOR_SELECT        SXCHROME_COLOR_SELECT
#define SXGUI_COLOR_SELECT_TEXT   SXCHROME_COLOR_SELECT_TEXT
#define SXGUI_COLOR_WINDOW        SXCHROME_COLOR_WINDOW

/* ---- metricas -------------------------------------------------------------
 *
 * Las medidas que una app necesita para ubicar sus controles. Estan aca y no
 * cada una en su .c porque el punto es que dos ventanas distintas caigan en la
 * MISMA grilla: si files usa 8 de margen y notepad 5, las dos se ven bien por
 * separado y mal uno al lado del otro.
 *
 * El grosor del bisel entra en la cuenta de cualquiera que reparta el ancho de
 * un listbox entre columnas: el area util es el rect menos SXGUI_BORDER_SUNKEN
 * de cada lado, y menos SXGUI_SCROLLBAR_THICKNESS si aparece la barra. */
/* El espesor de cada bisel lo define quien lo dibuja (savanxp/sxchrome.h). */
#define SXGUI_BORDER_RAISED       SXCHROME_BORDER_RAISED
#define SXGUI_BORDER_SUNKEN       SXCHROME_BORDER_SUNKEN
#define SXGUI_BORDER_INSET        SXCHROME_BORDER_INSET
#define SXGUI_SCROLLBAR_THICKNESS 16
/* Icono de fila de listbox. El tamano es fijo y no sale del bitmap para que la
 * sangria del texto sea la misma en todas las filas, tenga icono o no. */
#define SXGUI_LISTBOX_ICON_SIZE   16
#define SXGUI_LISTBOX_ICON_GAP    3
/* Separacion del texto al borde interno de un control. Una etiqueta suelta que
 * quiera alinear con el texto de un campo se corre BORDER + TEXT_PAD. */
#define SXGUI_TEXT_PAD            3

#define SXGUI_MARGIN              8   /* del borde del cliente al control */
#define SXGUI_GAP                 6   /* entre controles vecinos */
#define SXGUI_BUTTON_WIDTH        84
#define SXGUI_BUTTON_HEIGHT       26
#define SXGUI_FIELD_HEIGHT        24
#define SXGUI_STATUS_HEIGHT       22
#define SXGUI_DIALOG_MARGIN       12

enum sxgui_kind {
    SXGUI_LABEL = 0,
    SXGUI_BUTTON,
    SXGUI_CHECKBOX,
    SXGUI_LISTBOX,
    SXGUI_TEXTFIELD,
    SXGUI_SCROLLBAR,
    SXGUI_GROUPBOX,
    SXGUI_PROGRESS,
    SXGUI_RADIO,
    SXGUI_COMBOBOX,
    SXGUI_TEXTVIEW,
    SXGUI_TEXTEDIT,
    SXGUI_TABS
};

#define SXGUI_FLAG_VISIBLE  (1u << 0)
#define SXGUI_FLAG_DISABLED (1u << 1)
#define SXGUI_FLAG_HSCROLL  (1u << 2)  /* scrollbar: horizontal orientation */
#define SXGUI_FLAG_SUNKEN   (1u << 3)  /* label: sunken status-bar panel */

#define SXGUI_DOUBLE_CLICK_MS 450UL

/* Why on_action fired; read widget->action inside the callback. */
enum sxgui_action {
    SXGUI_ACTION_CLICK = 0,   /* button press released on the widget */
    SXGUI_ACTION_CHANGE,      /* value/selection/text changed */
    SXGUI_ACTION_ACTIVATE     /* listbox: double click or Enter on a row */
};

/* ---- listbox columns (vista "detalles") -----------------------------------
 *
 * Tabla caller-owned. Con `columns` puesto, el listbox dibuja una cabecera
 * fija arriba y parte cada item por TAB, una celda por columna: es la lista de
 * detalles clasica (Nombre | Tamano | Tipo). Sin `columns` se comporta igual
 * que siempre, asi que es opt-in y no toca a los listbox existentes.
 */

#define SXGUI_COLUMN_RIGHT (1u << 0)  /* celda alineada a la derecha */

struct sxgui_column {
    const char *title;
    int width;        /* ancho en pixeles */
    uint32_t flags;
};

/* Separador de celdas dentro de un item con columnas. */
#define SXGUI_COLUMN_SEPARATOR '	'

struct sxgui_widget {
    int kind;
    struct sx_rect rect;
    const char *text;            /* label / button caption */
    uint32_t flags;

    /* runtime interaction state, owned by the toolkit */
    int hover;
    int pressed;
    int focused;

    int value;                   /* checkbox: 0/1; listbox: selected row */

    /* listbox */
    const char *const *items;
    int item_count;

    /*
     * listbox: icono opcional por fila, paralelo a `items` (NULL = ninguno, y
     * una entrada NULL deja esa fila sin icono pero conserva la sangria, para
     * que los nombres no queden en zigzag).
     *
     * El icono va en la PRIMERA columna y corre solo el texto de esa columna:
     * el rotulo de la cabecera se queda donde esta, como en la vista de
     * detalles clasica. Se dibuja a SXGUI_LISTBOX_ICON_SIZE sin escalar, asi
     * que un bitmap de otro tamano se recorta en vez de deformarse.
     */
    const struct sx_bitmap *const *item_icons;

    /* listbox: cabecera de columnas opcional (NULL = lista simple) */
    const struct sxgui_column *columns;
    int column_count;

    /* textfield y textedit (caller-provided editable storage). En textedit el
     * buffer es el documento entero, con lineas separadas por '\n'. */
    char *edit_buffer;
    int edit_capacity;
    int caret;

    /* textedit: ancla de la seleccion, o -1 si no hay nada seleccionado. El
     * rango es [min(anchor, caret), max(anchor, caret)): el ancla se queda
     * donde empezo la seleccion y el caret es el extremo movil, que es lo que
     * hace que shift+flecha crezca y se achique por donde uno espera. */
    int sel_anchor;

    /* textedit: se pone en 1 cada vez que el contenido cambia. El toolkit
     * nunca lo baja -- es el llamador el que decide que significa "guardado". */
    int modified;

    /* textfield: horizontal scroll in pixels to keep the caret visible;
     * listbox: first visible row */
    int scroll;

    /* scrollbar: value ranges over [range_min, range_max]; page sizes the
     * thumb and is the track-click step */
    int range_min;
    int range_max;
    int page;

    /* radio: widgets sharing a group id are mutually exclusive */
    int group;

    /* set by the toolkit right before on_action runs (enum sxgui_action) */
    int action;

    void (*on_action)(struct sxgui_widget *widget, void *user);
    void *user;
};

/* ---- menu bar -------------------------------------------------------------
 *
 * Caller-owned tables; the toolkit paints the bar as a strip at the top of
 * the surface and the open drop-down as an overlay. Widgets should be laid
 * out below sxgui_menubar_height().
 */

#define SXGUI_MENU_DISABLED (1u << 0)
#define SXGUI_MENU_CHECKED  (1u << 1)

struct sxgui_menu_item {
    const char *text;   /* NULL = separator */
    int id;
    uint32_t flags;
};

struct sxgui_menu {
    const char *title;
    const struct sxgui_menu_item *items;
    int item_count;
};

struct sxgui_menubar {
    const struct sxgui_menu *menus;
    int menu_count;
    int open_menu;   /* -1 = closed; owned by the toolkit */
    int hot_item;    /* highlighted row of the open menu; owned by the toolkit */
    void (*on_command)(int id, void *user);
    void *user;
};

/* ---- modal dialogs ---------------------------------------------------------
 *
 * A second caller-owned widget array painted as a centred overlay. While a
 * dialog is active it captures all input; Tab cycles inside it and ESC ends
 * it with result 0. Widget rects are RELATIVE to the dialog client area.
 * Comboboxes inside dialogs are not supported.
 */

struct sxgui_dialog {
    struct sx_rect rect;            /* absolute; computed by sxgui_dialog_begin */
    const char *title;
    struct sxgui_widget *widgets;   /* rects relative to the client area */
    int widget_count;
    /* Widget que arranca con el foco, en indices de `widgets`. 0 lo deja sin
     * foco (el default historico), que solo sirve si el dialogo es de botones:
     * uno cuyo objeto es escribir algo tiene que poner aca su campo, o el
     * teclado no llega a ningun lado. */
    int initial_focus;
    /* Boton que dispara Enter, en indices de `widgets`. 0 = ninguno. Se dibuja
     * con el borde doble del boton por defecto, como en los dialogos de la
     * epoca, asi que se ve cual va a responder antes de apretarlo. */
    int default_button;
    int saved_focus;                /* owned by the toolkit */
    int result;
};

struct sxgui_context {
    struct sx_bitmap target;
    struct sx_painter painter;
    struct sxgui_widget *widgets;
    int widget_count;
    int focus_index;
    uint32_t last_buttons;
    int pointer_x;
    int pointer_y;
    int shift_down;

    /* pointer capture while the left button is held (thumb drags, presses) */
    int capture_index;
    int capture_part;
    int capture_offset;

    /* double-click tracking */
    unsigned long last_click_ms;
    int last_click_index;

    /* combobox dropdown state; the popup paints as an overlay inside the
     * window backbuffer, there are no child windows */
    int popup_owner;
    struct sx_rect popup_rect;
    int popup_hot;
    int popup_scroll;

    /* optional menu bar (NULL = none) */
    struct sxgui_menubar *menubar;

    /* active modal dialog (NULL = none); modal_route flags re-entrant
     * dispatch against the dialog's widget array */
    struct sxgui_dialog *modal;
    int modal_route;

    /* cursor shape the pointer should show given the widget currently under
     * it (enum savanxp_cursor_shape); updated by sxgui_handle_pointer */
    int cursor_shape;
};

/* Bind the toolkit to a window backbuffer and a widget array. */
void sxgui_context_init(
    struct sxgui_context *ctx,
    uint32_t *pixels,
    const struct savanxp_fb_info *info,
    struct sxgui_widget *widgets,
    int widget_count);

/* Re-bind the painting target after a window resize. */
void sxgui_context_retarget(struct sxgui_context *ctx, uint32_t *pixels, const struct savanxp_fb_info *info);

/* Attach (or detach with NULL) a menu bar; resets its open/hot state. */
void sxgui_set_menubar(struct sxgui_context *ctx, struct sxgui_menubar *bar);
int sxgui_menubar_height(void);

/* Open a modal dialog centred on the surface; width/height size the client
 * area. End it (typically from a dialog button callback) with a result code;
 * read it afterwards from dialog->result. */
void sxgui_dialog_begin(struct sxgui_context *ctx, struct sxgui_dialog *dialog, int width, int height);
void sxgui_dialog_end(struct sxgui_context *ctx, int result);
int sxgui_dialog_active(const struct sxgui_context *ctx);

/* Feed compositor-routed input. Each returns non-zero when the UI changed and
 * a repaint is needed. */
int sxgui_handle_pointer(struct sxgui_context *ctx, const struct savanxp_gui_pointer_event *event);
int sxgui_handle_key(struct sxgui_context *ctx, const struct savanxp_input_event *event);

/* Cursor shape (enum savanxp_cursor_shape) the widget under the pointer
 * wants shown, as of the last sxgui_handle_pointer call. */
int sxgui_cursor_shape(const struct sxgui_context *ctx);

/* Paint every visible widget into the backbuffer. */
void sxgui_paint(struct sxgui_context *ctx);
/* Las dos mitades de sxgui_paint, para apps que pintan contenido propio: el
 * chrome (menu bar, popups, dialogo modal) va SIEMPRE por encima, asi que el
 * dibujo propio se intercala entre ambas. sxgui_app lo hace por vos:
 * content -> on_paint -> overlay. */
void sxgui_paint_content(struct sxgui_context *ctx);
void sxgui_paint_overlay(struct sxgui_context *ctx);

/* ---- chrome para contenido propio ----------------------------------------
 *
 * Los dos biseles del esquema 3D, para la app que pinta su propio contenido y
 * necesita que se vea como el resto del sistema: un tablero de celdas, una
 * grilla, un visor. Son LOS MISMOS que usan los widgets, no una copia -- que es
 * lo que evita que una app dibuje un borde de un pixel al lado de un boton de
 * dos, o que use el gris equivocado de los cuatro del esquema.
 *
 * Pintan solo el borde, SXGUI_BORDER_RAISED / SXGUI_BORDER_SUNKEN pixeles de
 * espesor por lado; el relleno del interior es del llamador.
 *
 * Son alias de savanxp/sxchrome.h, que es donde vive el dibujo desde que el WM
 * tambien lo consume. Una app que ya linkea el toolkit puede seguir usando
 * estos nombres; una que necesite el bisel de UN pixel, la cara de un control o
 * el relieve de deshabilitado va directo a sxchrome, que no pide el toolkit. */
void sxgui_draw_raised_edge(struct sx_painter *painter, struct sx_rect rect);
void sxgui_draw_sunken_edge(struct sx_painter *painter, struct sx_rect rect);

/* Convenience widget constructors (fill an entry in the caller's array). */
struct sxgui_widget sxgui_label(struct sx_rect rect, const char *text);
struct sxgui_widget sxgui_button(struct sx_rect rect, const char *text, void (*on_action)(struct sxgui_widget *, void *), void *user);
struct sxgui_widget sxgui_checkbox(struct sx_rect rect, const char *text, int checked);
struct sxgui_widget sxgui_listbox(struct sx_rect rect, const char *const *items, int item_count);
struct sxgui_widget sxgui_textfield(struct sx_rect rect, char *edit_buffer, int edit_capacity);
struct sxgui_widget sxgui_scrollbar(struct sx_rect rect, int range_min, int range_max, int page, int value);
struct sxgui_widget sxgui_groupbox(struct sx_rect rect, const char *text);
struct sxgui_widget sxgui_radio(struct sx_rect rect, const char *text, int group, int checked);
struct sxgui_widget sxgui_combobox(struct sx_rect rect, const char *const *items, int item_count, int selected);
/* Read-only multi-line text panel; reuses items/item_count as lines and
 * scrolls like a listbox (embedded scrollbar, keyboard paging). */
struct sxgui_widget sxgui_textview(struct sx_rect rect, const char *const *lines, int line_count);

/* ---- tabs -----------------------------------------------------------------
 *
 * Control de pestanias clasico: la fila de pestanias arriba y la pagina abajo,
 * unidas en una sola pieza. La pestania activa se dibuja mas alta y mas ancha
 * que las demas y TAPA el borde superior de la pagina: por ese hueco las dos
 * partes se leen como una sola hoja, que es lo que distingue una pestania de
 * un boton. Las inactivas quedan hundidas contra el borde.
 *
 * `rect` cubre las dos partes -- la fila Y la pagina --, no solo la fila. El
 * contenido de la pagina NO son widgets hijos: sxgui no tiene jerarquia. La
 * app pregunta por `sxgui_tabs_page()` donde puede dibujar y ubica ahi sus
 * propios controles, o pinta a mano si su contenido no son widgets.
 *
 * Reusa `items`/`item_count` para los rotulos y `value` para la pestania
 * activa; cambiarla dispara `SXGUI_ACTION_CHANGE`. Con el foco puesto, las
 * flechas izquierda y derecha se mueven entre pestanias. */
struct sxgui_widget sxgui_tabs(struct sx_rect rect, const char *const *labels, int label_count, int selected);
/* Alto de la fila de pestanias sola, para quien necesite reservar el espacio
 * antes de tener el widget armado. */
int sxgui_tabs_height(void);
/* Area util de la pagina: el rect del widget menos la fila y menos el bisel.
 * Es donde va el contenido de la pestania activa. */
struct sx_rect sxgui_tabs_page(const struct sxgui_widget *widget);
/* Ancho que necesita la fila para mostrar todos los rotulos enteros. Una app
 * que calcula el tamano de su ventana lo necesita ANTES de tener el widget
 * armado, y adivinarlo con la formula copiada a mano es como se desincroniza. */
int sxgui_tabs_preferred_width(const char *const *labels, int label_count);
/* Editor multilinea: el documento vive en `buffer` (terminado en NUL, lineas
 * separadas por '\n') y lo posee el llamador. Sin word wrap: las lineas largas
 * se recortan al ancho y el scroll horizontal sigue al caret. */
struct sxgui_widget sxgui_textedit(struct sx_rect rect, char *buffer, int capacity);

/* Pone el foco en un widget por indice (-1 = ninguno). Lo normal es que el foco
 * lo mueva el usuario con Tab o el click, pero una app cuyo contenido ES un
 * control -- un editor, una lista -- quiere arrancar con el foco ahi. */
void sxgui_focus(struct sxgui_context *ctx, int index);

/* Portapapeles sobre un widget de texto -- `SXGUI_TEXTEDIT` o `SXGUI_TEXTFIELD`
 * --, para colgar de un menu Edicion. Son las mismas operaciones que hacen
 * Ctrl+C/X/V/A, expuestas porque el atajo de teclado no puede ser la unica via:
 * un menu tiene que poder dispararlas. Devuelven 1 si cambiaron algo, de modo
 * que el llamador puede avisar cuando no habia nada seleccionado. `cut` y
 * `paste` marcan el widget modificado; el llamador decide que significa
 * "guardado". Conservan el nombre `textedit_` aunque sirvan a los dos widgets,
 * para no romper a quien ya los llama. */
int sxgui_textedit_copy(struct sxgui_widget *widget);
int sxgui_textedit_cut(struct sxgui_widget *widget);
int sxgui_textedit_paste(struct sxgui_widget *widget);
void sxgui_textedit_select_all(struct sxgui_widget *widget);
/* 1 si hay algo seleccionado. Para una app que quiera reflejar el estado por
 * su cuenta -- una barra de estado, por ejemplo. El menu no lo necesita: las
 * operaciones ya devuelven 0 cuando no hay nada que cortar o copiar. */
int sxgui_textedit_has_selection(const struct sxgui_widget *widget);
struct sxgui_widget sxgui_progress(struct sx_rect rect, int range_min, int range_max, int value);

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
