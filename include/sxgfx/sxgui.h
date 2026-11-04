#pragma once

/*
 * sxgui - a minimal Win9x-flavoured widget toolkit on top of sxgfx.
 *
 * Retained but allocation-free: the application owns a flat array of
 * sxgui_widget and the toolkit paints them and dispatches input. Widgets draw
 * directly into the window backbuffer, confined with the painter clip stack.
 */

#include "abi/savanxp_abi.h"
#include "sxgfx/gfx2d.h"
#include "sxgfx/sxchrome.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXGUI_RGB(r, g, b) SXCHROME_RGB(r, g, b)

/* Classic 3D system palette.
 *
 * IT LIVES IN sxgfx/sxchrome.h: it is not the toolkit's, it is the system's --
 * the WM's window frame and an app that paints its own content have to hit
 * the same tones as a button. These names are still the ones the toolkit's and
 * the apps' code use, so they stay as aliases. */
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

/* ---- metrics --------------------------------------------------------------
 *
 * The measurements an app needs to place its controls. They are here rather
 * than in each .c because the point is for two different windows to fall on
 * the SAME grid: if files uses one margin and notepad another, each looks
 * right alone and wrong next to the other.
 *
 * The bevel thickness enters the arithmetic of anything that divides a
 * listbox's width into columns: the usable area is the rect minus
 * SXGUI_BORDER_SUNKEN on each side, and minus SXGUI_SCROLLBAR_THICKNESS if the
 * bar shows. */
/* Each bevel's thickness is defined by whoever draws it (sxgfx/sxchrome.h). */
#define SXGUI_BORDER_RAISED       SXCHROME_BORDER_RAISED
#define SXGUI_BORDER_SUNKEN       SXCHROME_BORDER_SUNKEN
#define SXGUI_BORDER_INSET        SXCHROME_BORDER_INSET
#define SXGUI_SCROLLBAR_THICKNESS 16
/* Listbox row icon. The size is fixed, not taken from the bitmap, so the text
 * indent is the same on every row, with an icon or without. */
#define SXGUI_LISTBOX_ICON_SIZE   16
#define SXGUI_LISTBOX_ICON_GAP    3
/* Icon view cell: the same grid the desktop lays its icons on, so a folder
 * looks alike in Files and on the desktop. The cell's height follows the
 * font (two label lines under the icon). */
#define SXGUI_ICONVIEW_ICON_SIZE  32
#define SXGUI_ICONVIEW_CELL_WIDTH 76
/* Gap between the text and a control's inner edge. A loose label that wants
 * to line up with a field's text moves by BORDER + TEXT_PAD. */
#define SXGUI_TEXT_PAD            3

#define SXGUI_MARGIN              4   /* from the client edge to the control */
#define SXGUI_GAP                 6   /* between neighbouring controls */
#define SXGUI_BUTTON_WIDTH        84
#define SXGUI_BUTTON_HEIGHT       26
#define SXGUI_FIELD_HEIGHT        24
#define SXGUI_STATUS_HEIGHT       22
#define SXGUI_DIALOG_MARGIN       8

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
    SXGUI_TABS,
    SXGUI_TRACKBAR,
    SXGUI_ICONVIEW
};

#define SXGUI_FLAG_VISIBLE  (1u << 0)
#define SXGUI_FLAG_DISABLED (1u << 1)
#define SXGUI_FLAG_HSCROLL  (1u << 2)  /* scrollbar: horizontal orientation */
#define SXGUI_FLAG_SUNKEN   (1u << 3)  /* label: sunken status-bar panel */
/* listbox and iconview: the selection may be empty (value -1), as in an
 * Explorer folder. A click on the background clears it, and from no
 * selection the first arrow key selects the first item. Without the flag a
 * click on the background keeps the selection, as it always has. */
#define SXGUI_FLAG_EMPTY_SELECTION (1u << 4)

#define SXGUI_DOUBLE_CLICK_MS 450UL

/* Why on_action fired; read widget->action inside the callback. */
enum sxgui_action {
    SXGUI_ACTION_CLICK = 0,   /* button press released on the widget */
    SXGUI_ACTION_CHANGE,      /* value/selection/text changed */
    SXGUI_ACTION_ACTIVATE,    /* listbox: double click or Enter on a row */
    SXGUI_ACTION_HEADER       /* listbox: click on a SXGUI_COLUMN_CLICKABLE header;
                               * sxgui_header_column() says which one */
};

/* ---- listbox columns ("details" view) -------------------------------------
 *
 * Caller-owned table. With `columns` set, the listbox draws a fixed header on
 * top and splits each item at TAB, one cell per column: the classic details
 * list (Name | Size | Type). Without `columns` it behaves as always, so it is
 * opt-in and leaves existing listboxes alone.
 */

#define SXGUI_COLUMN_RIGHT (1u << 0)  /* right-aligned cell */
/* The header cell is a button: a click fires SXGUI_ACTION_HEADER. Opt-in, so
 * a list that does not sort never sees the new action. */
#define SXGUI_COLUMN_CLICKABLE (1u << 1)

struct sxgui_column {
    const char *title;
    int width;        /* width in pixels */
    uint32_t flags;
};

/* Cell separator inside an item with columns. */
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
     * listbox: optional per-row icon, parallel to `items` (NULL = none, and a
     * NULL entry leaves that row without an icon but keeps the indent, so the
     * names do not zigzag).
     *
     * The icon goes in the FIRST column and only moves that column's text:
     * the header label stays where it is, as in the classic details view. It
     * is drawn at SXGUI_LISTBOX_ICON_SIZE without scaling, so a bitmap of
     * another size is cropped instead of distorted.
     */
    const struct sx_bitmap *const *item_icons;

    /* listbox: optional column header (NULL = plain list) */
    const struct sxgui_column *columns;
    int column_count;

    /* textfield and textedit (caller-provided editable storage). In textedit
     * the buffer is the whole document, with lines separated by '\n'. */
    char *edit_buffer;
    int edit_capacity;
    int caret;

    /* textedit: the selection anchor, or -1 when nothing is selected. The
     * range is [min(anchor, caret), max(anchor, caret)): the anchor stays where
     * the selection started and the caret is the moving end, which is what
     * makes shift+arrow grow and shrink it on the expected side. */
    int sel_anchor;

    /* textedit: set to 1 whenever the content changes. The toolkit never
     * clears it -- the caller decides what "saved" means. */
    int modified;

    /* textfield: horizontal scroll in pixels to keep the caret visible;
     * listbox: first visible row; iconview: first visible row of the grid */
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

/* ---- popup menu -----------------------------------------------------------
 *
 * One menu's drop-down list on its own, for a menu that does not hang from a
 * menu bar: a context menu shown in a popup window of its own. The menu bar
 * draws its drop-downs with these same calls, so both look alike. The caller
 * owns where the list is shown, which row is hot and what a click means.
 */

/* The size the list needs: the widest item plus the check gutter, and every
 * row and separator. */
void sxgui_popup_menu_size(const struct sxgui_menu *menu, int *width, int *height);
/* Paints the list into `rect` (usually the size above). `hot_item` is the
 * highlighted row, -1 for none; a disabled row is never highlighted. */
void sxgui_popup_menu_paint(struct sx_painter *painter, struct sx_rect rect, const struct sxgui_menu *menu, int hot_item);
/* The row under (x, y), or -1 over a separator or outside `rect`. */
int sxgui_popup_menu_item_at(struct sx_rect rect, const struct sxgui_menu *menu, int x, int y);
/* Non-zero if the row exists, is not a separator and is not disabled. */
int sxgui_popup_menu_item_selectable(const struct sxgui_menu *menu, int index);
/* The next row up (direction -1) or down (+1) from `hot`, skipping
 * separators and wrapping around; -1 if the menu has no rows. */
int sxgui_popup_menu_step(const struct sxgui_menu *menu, int hot, int direction);

/* ---- context menu ---------------------------------------------------------
 *
 * A menu shown at a point of the window, as Win32's TrackPopupMenu: what a
 * right click or the Menu key opens. While it is open the toolkit owns the
 * keyboard (arrows, Enter, Esc) and the pointer; choosing an item closes the
 * menu and calls `on_command` with its id, any other press closes it.
 *
 * Where it shows depends on the context's popup host. Under sxgui_app_run it
 * is a window of its own, so it may leave the window
 * (docs/desktop/DESKTOP_ICONS.md#phase-4-context-menus-in-applications); without a host, or if the WM refuses the window, it is
 * drawn inside the surface, flipped and clamped to fit. The menu table is the
 * caller's and must outlive the menu.
 */

struct sxgui_context;

/* Gives context menus a window of their own. `open` returns 0 once a popup
 * of width x height exists at (x, y) of the window's client area; anything
 * else keeps the menu inside the surface. The host pumps the popup's pointer
 * into sxgui_context_menu_pointer and paints it with
 * sxgui_paint_context_menu_window. */
struct sxgui_popup_host {
    int (*open)(struct sxgui_popup_host *host, int x, int y, int width, int height);
    void (*close)(struct sxgui_popup_host *host);
    void *user;
};

/* Opens `menu` with its top-left corner at (x, y) of the window, closing any
 * menu, drop-down or context menu that was open. */
void sxgui_context_menu_open(
    struct sxgui_context *ctx,
    const struct sxgui_menu *menu,
    int x,
    int y,
    void (*on_command)(int id, void *user),
    void *user);
void sxgui_context_menu_close(struct sxgui_context *ctx);
int sxgui_context_menu_active(const struct sxgui_context *ctx);
/* Non-zero while the open menu lives in a window of the popup host. */
int sxgui_context_menu_windowed(const struct sxgui_context *ctx);
/* For the popup host: a pointer event in the popup window's coordinates.
 * Non-zero if something changed and the popup needs painting. */
int sxgui_context_menu_pointer(struct sxgui_context *ctx, const struct savanxp_gui_pointer_event *event);
/* For the popup host: paints the open menu into the popup window's surface. */
void sxgui_paint_context_menu_window(struct sxgui_context *ctx, uint32_t *pixels, const struct savanxp_fb_info *info);

/* ---- modal dialogs ---------------------------------------------------------
 *
 * A second caller-owned widget array. While a dialog is active it captures all
 * input; Tab cycles inside it and ESC ends it with result 0. Widget rects are
 * RELATIVE to the dialog client area. Comboboxes inside dialogs are not
 * supported.
 *
 * Where it is painted depends on the context's dialog host. Under
 * sxgui_app_run the dialog is its own window owned by the app's window
 * (docs/desktop/OWNED_WINDOWS.md): the WM draws its frame and title and keeps it above
 * the owner, so it no longer has to fit inside the app. Without a host, or if
 * the WM does not create the window, it falls back to a centred overlay inside
 * the app's own surface. The API is the same either way.
 */

struct sxgui_dialog {
    struct sx_rect rect;            /* absolute; computed by sxgui_dialog_begin */
    const char *title;
    struct sxgui_widget *widgets;   /* rects relative to the client area */
    int widget_count;
    /* Widget that starts with the focus, as an index into `widgets`. 0 leaves
     * no focus (the historical default), which only works for a dialog of
     * buttons: one whose purpose is typing something has to put its field
     * here, or the keyboard goes nowhere. */
    int initial_focus;
    /* Button that Enter triggers, as an index into `widgets`. 0 = none. It is
     * drawn with the default button's double border, as in the dialogs of the
     * era, so you can see which one will respond before pressing it. */
    int default_button;
    int saved_focus;                /* owned by the toolkit */
    int result;
    /* Owned by the toolkit: 1 while the dialog lives in its own window (rect is
     * then the whole window surface, origin 0,0), 0 while it is an overlay. */
    int windowed;
};

/* Gives dialogs a window of their own. `open` returns 0 once the window exists
 * with a client area of width x height; anything else keeps the overlay. The
 * host paints and pumps that window (sxgui_app_run is the one host there is). */
struct sxgui_dialog_host {
    int (*open)(struct sxgui_dialog_host *host, struct sxgui_dialog *dialog, int width, int height);
    void (*close)(struct sxgui_dialog_host *host, struct sxgui_dialog *dialog);
    void *user;
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
    /* NULL = dialogs are overlays inside this surface */
    struct sxgui_dialog_host *dialog_host;

    /* cursor shape the pointer should show given the widget currently under
     * it (enum savanxp_cursor_shape); updated by sxgui_handle_pointer */
    int cursor_shape;

    /* NULL = context menus are drawn inside this surface */
    struct sxgui_popup_host *popup_host;
    /* The open context menu (NULL = none), all owned by the toolkit. The rect
     * is in the surface while the menu is drawn inside it, and the popup
     * window's (origin 0,0) while it is windowed. */
    const struct sxgui_menu *context_menu;
    int context_menu_hot;
    int context_menu_windowed;
    struct sx_rect context_menu_rect;
    uint32_t context_menu_buttons;
    void (*context_menu_command)(int id, void *user);
    void *context_menu_user;
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
/* 1 while the active dialog lives in its own window. Pointer events of the
 * app's window must not reach the toolkit then: their coordinates are not the
 * dialog's. */
int sxgui_dialog_windowed(const struct sxgui_context *ctx);
/* Paints the active windowed dialog into its window's backbuffer. */
void sxgui_paint_dialog_window(struct sxgui_context *ctx, uint32_t *pixels, const struct savanxp_fb_info *info);

/* Feed compositor-routed input. Each returns non-zero when the UI changed and
 * a repaint is needed. */
int sxgui_handle_pointer(struct sxgui_context *ctx, const struct savanxp_gui_pointer_event *event);
int sxgui_handle_key(struct sxgui_context *ctx, const struct savanxp_input_event *event);

/* Cursor shape (enum savanxp_cursor_shape) the widget under the pointer
 * wants shown, as of the last sxgui_handle_pointer call. */
int sxgui_cursor_shape(const struct sxgui_context *ctx);

/* Paint every visible widget into the backbuffer. */
void sxgui_paint(struct sxgui_context *ctx);
/* The two halves of sxgui_paint, for apps that paint their own content: the
 * chrome (menu bar, popups, modal dialog) ALWAYS goes on top, so the app's own
 * drawing goes between them. sxgui_app does it for you:
 * content -> on_paint -> overlay. */
void sxgui_paint_content(struct sxgui_context *ctx);
void sxgui_paint_overlay(struct sxgui_context *ctx);

/* ---- chrome for the app's own content --------------------------------------
 *
 * The two bevels of the 3D scheme, for the app that paints its own content
 * and needs it to look like the rest of the system: a board of cells, a grid,
 * a viewer. They are THE SAME ones the widgets use, not a copy -- which keeps
 * an app from drawing a one-pixel border next to a two-pixel button, or from
 * picking the wrong one of the scheme's four greys.
 *
 * They paint only the border, SXGUI_BORDER_RAISED / SXGUI_BORDER_SUNKEN pixels
 * thick per side; filling the interior is the caller's.
 *
 * They are aliases of sxgfx/sxchrome.h, where the drawing lives since the WM
 * uses it too. An app that already links the toolkit can keep these names;
 * one that needs the ONE-pixel bevel, a control's face or the disabled relief
 * goes straight to sxchrome, which does not need the toolkit. */
void sxgui_draw_raised_edge(struct sx_painter *painter, struct sx_rect rect);
void sxgui_draw_sunken_edge(struct sx_painter *painter, struct sx_rect rect);

/* Convenience widget constructors (fill an entry in the caller's array). */
struct sxgui_widget sxgui_label(struct sx_rect rect, const char *text);
struct sxgui_widget sxgui_button(struct sx_rect rect, const char *text, void (*on_action)(struct sxgui_widget *, void *), void *user);
struct sxgui_widget sxgui_checkbox(struct sx_rect rect, const char *text, int checked);
struct sxgui_widget sxgui_listbox(struct sx_rect rect, const char *const *items, int item_count);

/*
 * Icon view ("Large Icons"): the same data as a listbox shown as a grid of
 * SXGUI_ICONVIEW_ICON_SIZE icons with the name under each one, in up to two
 * lines. It reads the listbox's fields -- `items` (only the first cell of an
 * item with columns), `item_icons`, `value` (the selected item) -- so a
 * program switches between both views by changing `kind`. `scroll` counts
 * grid rows, not items. sxgui_listbox_row_at answers the item under a point
 * for both kinds.
 */
struct sxgui_widget sxgui_iconview(struct sx_rect rect, const char *const *items, int item_count);
/* Where item `index` of an icon view sits now (its whole cell), for a
 * program that opens something next to it, such as the keyboard's context
 * menu. A zero-sized rect when the widget is not an icon view. */
struct sx_rect sxgui_iconview_item_rect(const struct sxgui_widget *widget, int index);

/* The column whose header was clicked, while `widget` runs its
 * SXGUI_ACTION_HEADER; -1 at any other time. It lives in the toolkit rather
 * than in the widget so the widget's layout, which libsxgui shares with every
 * program, stays the same. */
int sxgui_header_column(const struct sxgui_widget *widget);
/* The listbox row under (x, y), for a click the toolkit does not handle --
 * the right button that opens a row's context menu. -1 off the rows (the
 * header, the scrollbar, the empty space below the last row). On an icon
 * view, the item whose icon or name is under (x, y), or -1 on the
 * background. */
int sxgui_listbox_row_at(const struct sxgui_widget *widget, int x, int y);
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
 * Classic tab control: the row of tabs on top and the page below, joined in
 * one piece. The active tab is drawn taller and wider than the others and
 * COVERS the page's top edge: through that gap the two parts read as a single
 * sheet, which is what tells a tab from a button. Inactive tabs stay sunk
 * against the edge.
 *
 * `rect` covers both parts -- the row AND the page --, not just the row. The
 * page's content is NOT child widgets: sxgui has no hierarchy. The app asks
 * `sxgui_tabs_page()` where it may draw and places its own controls there, or
 * paints by hand if its content is not widgets.
 *
 * It reuses `items`/`item_count` for the labels and `value` for the active
 * tab; changing it fires `SXGUI_ACTION_CHANGE`. With the focus on it, the left
 * and right arrows move between tabs. */
struct sxgui_widget sxgui_tabs(struct sx_rect rect, const char *const *labels, int label_count, int selected);
/* Height of the tab row alone, for code that needs to reserve the space
 * before the widget is built. */
int sxgui_tabs_height(void);
/* Usable area of the page: the widget's rect minus the row and minus the
 * bevel. It is where the active tab's content goes. */
struct sx_rect sxgui_tabs_page(const struct sxgui_widget *widget);
/* Width the row needs to show every label in full. An app that computes its
 * window size needs it BEFORE the widget is built, and guessing it with a
 * hand-copied formula is how the two drift apart. */
int sxgui_tabs_preferred_width(const char *const *labels, int label_count);
/* Multi-line editor: the document lives in `buffer` (NUL-terminated, lines
 * separated by '\n') and the caller owns it. No word wrap: long lines are
 * clipped to the width and the horizontal scroll follows the caret. */
struct sxgui_widget sxgui_textedit(struct sx_rect rect, char *buffer, int capacity);

/* Puts the focus on a widget by index (-1 = none). Normally the user moves
 * the focus with Tab or a click, but an app whose content IS a control -- an
 * editor, a list -- wants to start with the focus there. */
void sxgui_focus(struct sxgui_context *ctx, int index);

/* Clipboard on a text widget -- `SXGUI_TEXTEDIT` or `SXGUI_TEXTFIELD` --, to
 * hang from an Edit menu. They are the same operations as Ctrl+C/X/V/A,
 * exposed because the keyboard shortcut cannot be the only way: a menu has to
 * be able to trigger them. They return 1 if they changed something, so the
 * caller can tell the user when nothing was selected. `cut` and `paste` mark
 * the widget modified; the caller decides what "saved" means. They keep the
 * `textedit_` name although they serve both widgets, so as not to break their
 * callers. */
int sxgui_textedit_copy(struct sxgui_widget *widget);
int sxgui_textedit_cut(struct sxgui_widget *widget);
int sxgui_textedit_paste(struct sxgui_widget *widget);
void sxgui_textedit_select_all(struct sxgui_widget *widget);
/* 1 if something is selected. For an app that wants to show the state itself
 * -- a status bar, for instance. The menu does not need it: the operations
 * already return 0 when there is nothing to cut or copy. */
int sxgui_textedit_has_selection(const struct sxgui_widget *widget);
struct sxgui_widget sxgui_progress(struct sx_rect rect, int range_min, int range_max, int value);
/* Horizontal slider over [range_min, range_max] in steps of one, a tick per
 * step: Display Properties' resolution. `value` is the step; a press or a drag
 * moves it, and so do the arrows, Home/End and PgUp/PgDn (by `page`, 1 unless
 * the caller sets it). Every move fires SXGUI_ACTION_CHANGE. It is
 * SXGUI_TRACKBAR_HEIGHT tall. */
#define SXGUI_TRACKBAR_HEIGHT 28
struct sxgui_widget sxgui_trackbar(struct sx_rect rect, int range_min, int range_max, int value);

/* ---- host services ---------------------------------------------------------
 *
 * The three things the toolkit needs from the system it runs on: the clock
 * (double clicks) and the clipboard (cut, copy, paste). Each subsystem
 * provides them: a POSIX program gets the POSIX host without asking, a SUN
 * program passes its own to sxgui_set_host. A missing service degrades the
 * toolkit (no double click, nothing to paste) instead of failing.
 */
struct sxgui_host {
    unsigned long (*uptime_ms)(void);
    /* Returns the clipboard's full length (it may exceed capacity), or <= 0. */
    int (*clipboard_get_text)(char *buffer, int capacity);
    /* Returns >= 0 on success. */
    int (*clipboard_set_text)(const char *text);
};

/* NULL goes back to the subsystem's default host. */
void sxgui_set_host(const struct sxgui_host *host);
/* The default host: a weak symbol that returns NULL; the subsystem's runtime
 * overrides it (subsystems/posix/sdk/v1/runtime/sxgui_app.c for POSIX). */
const struct sxgui_host *sxgui_default_host(void);

#ifdef __cplusplus
}
#endif
