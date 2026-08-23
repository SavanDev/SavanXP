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
#include "savanxp/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXGUI_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Classic 3D system palette. */
#define SXGUI_COLOR_FACE          SXGUI_RGB(192, 192, 192)
#define SXGUI_COLOR_SHADOW        SXGUI_RGB(128, 128, 128)
#define SXGUI_COLOR_DARK          SXGUI_RGB(0, 0, 0)
#define SXGUI_COLOR_LIGHT         SXGUI_RGB(255, 255, 255)
#define SXGUI_COLOR_TEXT          SXGUI_RGB(0, 0, 0)
#define SXGUI_COLOR_DISABLED_TEXT SXGUI_RGB(128, 128, 128)
#define SXGUI_COLOR_FIELD         SXGUI_RGB(255, 255, 255)
#define SXGUI_COLOR_SELECT        SXGUI_RGB(0, 0, 128)
#define SXGUI_COLOR_SELECT_TEXT   SXGUI_RGB(255, 255, 255)
#define SXGUI_COLOR_WINDOW        SXGUI_RGB(192, 192, 192)

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
    SXGUI_TEXTVIEW
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

    /* textfield (caller-provided editable storage) */
    char *edit_buffer;
    int edit_capacity;
    int caret;

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
    void *user;
};

/* Margen que sxgui_app_autosize deja alrededor del bounding box de los widgets
 * al pedirle su tamano a la ventana. */
#define SXGUI_CONTENT_MARGIN 16

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
