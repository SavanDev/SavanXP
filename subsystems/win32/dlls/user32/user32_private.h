/*
 * user32_private.h -- What the parts of user32.dll share: the window and
 * class tables of user32.c, and the entry points menus (menu.c), dialogs
 * (dialog.c) and the system's controls (controls.c) call each other by.
 * Nothing here is exported.
 */
#ifndef SAVANXP_WIN32_USER32_PRIVATE_H
#define SAVANXP_WIN32_USER32_PRIVATE_H

#define SXW_BUILDING_USER32
#include <windows.h>

#include "region.h"
#include "sxw_abi.h"
#include "sxw_gdi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);
__declspec(dllimport) ATOM WINAPI GlobalAddAtomW(LPCWSTR name);
__declspec(dllimport) ATOM WINAPI GlobalFindAtomW(LPCWSTR name);
__declspec(dllimport) ATOM WINAPI GlobalDeleteAtom(ATOM atom);

#define MAX_TEXT 256
#define ERROR_CANNOT_FIND_WND_CLASS 1407u
#define ERROR_CONTROL_ID_NOT_FOUND 1421u
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814u
#define RT_MENU 4u
#define RT_DIALOG 5u

/* The height of the menu bar, a row of items, on the surface above the
 * client area; AdjustWindowRect counts one pixel less, as Windows XP's does,
 * since its caption is a pixel shorter than the metric. */
#define MENU_BAR_HEIGHT 20
#define METRIC_MENU 19
/* SM_CXVSCROLL and SM_CYHSCROLL: a scroll bar's width, its arrows' size. */
#define SCROLL_EXTENT 16

typedef struct wclass {
    int used;
    int windows; /* how many windows of it exist */
    ATOM atom;
    WCHAR name[64];
    UINT style;
    WNDPROC proc;
    int window_extra;
    HINSTANCE instance;
    HICON icon;
    HICON small_icon; /* RegisterClassExW's hIconSm */
    HCURSOR cursor;
    HBRUSH background;
    LPCWSTR menu_name; /* an id, or menu_text */
    WCHAR menu_text[64];
} wclass;

typedef struct dialog_info dialog_info;

/* A window property: an atom (the global table's for a name) and its
 * value. */
typedef struct prop {
    ATOM atom;
    int named; /* the atom was added for a name, and is deleted with it */
    HANDLE data;
} prop;

/* A scroll bar of a window (SB_HORZ, SB_VERT) or a scroll bar control. */
typedef struct scroll_bar {
    int min;
    int max;
    int page;
    int pos;
    int track_pos;
    u32 disabled; /* ESB_DISABLE_LTUP, ESB_DISABLE_RTDN */
} scroll_bar;

typedef struct window window;
struct window {
    int used;
    u32 generation;
    wclass *cls;
    WNDPROC proc;
    DWORD style;
    DWORD ex_style;
    window *parent;
    window *child; /* the topmost child */
    window *next;  /* the next sibling below this one */
    window *owner; /* a top-level window's owner */
    /* In the parent's client coordinates; a top-level window's in the
     * screen's. */
    sxw_rect rect;
    sxw_rect client;
    UINT_PTR id;
    WCHAR text[MAX_TEXT];
    unsigned char *extra;
    /* Invalid area, client coordinates, and whether it needs erasing. */
    sxw_region invalid;
    int erase;
    /* The frame needs WM_NCPAINT before the next WM_PAINT. */
    int frame_dirty;
    /* A top-level window has not had its first WM_SIZE and WM_MOVE. */
    int need_size;
    int destroying;
    /* Top-level only: the host window and the surface every window of the
     * tree draws on, its menu bar at the top of it. */
    u32 host;
    sxw_surface surface;
    int surface_capacity; /* pixels */
    HMENU menu;
    int menu_height;
    /* The keyboard cues it hides (UISF_HIDEFOCUS, UISF_HIDEACCEL). */
    u32 ui_state;
    /* A dialog's (dialog.c), a control's (controls.c). */
    dialog_info *dialog;
    u32 control_state;
    HFONT font;
    /* The thread that made it, whose queue gets its messages. */
    u32 thread;
    /* A message-only window (HWND_MESSAGE): never shown, no surface. */
    int message_only;
    HINSTANCE instance;
    LONG user_data;      /* GWL_USERDATA */
    int extra_size;      /* the class's cbWndExtra */
    prop *props;
    int prop_count;
    int prop_capacity;
    u32 z;               /* a top-level window's place among the others: higher is above */
    scroll_bar bars[2];  /* SB_HORZ, SB_VERT; bars[0] is a scroll bar control's own */
    int modeless;        /* a top-level window shown as SXW_WINDOW_MODELESS (CreateDialog's) */
    void *control;       /* an Edit, ListBox or ComboBox control's own state */
};

#define MAX_WINDOWS 256
extern window g_windows[];
extern window *g_active;
extern window *g_focus;
extern window *g_capture;
extern window *g_focus_target;
/* The last input was a key: windows created now show their keyboard cues. */
extern int g_keyboard_last;
extern POINT g_last_point;

/* user32.c */
LRESULT call_proc(window *w, UINT message, WPARAM wParam, LPARAM lParam);
void set_last_error(DWORD error);
u32 ticks(void);
void zero(void *p, u32 size);
int same_name(const WCHAR *a, const WCHAR *b);
int copy_text(WCHAR *out, const WCHAR *text, int capacity);
int text_length(const WCHAR *text);
ATOM register_system_class(UINT style, WNDPROC proc, LPCWSTR name, ATOM atom, HBRUSH background, int extra);
HWND handle_of(const window *w);
window *window_of(HWND h);
window *checked(HWND h);
window *top_of(window *w);
LRESULT send(window *w, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT send_handle(HWND h, UINT message, WPARAM wParam, LPARAM lParam);
int visible_chain(const window *w);
int client_width(const window *w);
int client_height(const window *w);
void client_origin(const window *w, int *x, int *y);
HDC window_dc(window *w, const sxw_region *clip);
void invalidate(window *w, const sxw_rect *rect, int erase, int children);
void present_all(void);
void present_window(window *top);
void activate(window *w);
int within(const window *w, const window *ancestor);
int next_message(MSG *msg, HWND filter, UINT first, UINT last, int remove, int block);
int input_waiting(void);
u32 get16(const unsigned char *p);
u32 get32(const unsigned char *p);
u32 module_of(HINSTANCE instance);
const unsigned char *find_resource(u32 base, u32 type, LPCWSTR name, u32 *size);
void fill_rect(HDC dc, const RECT *r, COLORREF color);
void fill_brush(HDC dc, const RECT *r, HBRUSH brush);
HBRUSH system_brush(int index);
DWORD WINAPI GetSysColor(int nIndex);
LRESULT default_control_color(UINT message, HDC dc);
void change_ui_state(window *w, WORD action, WORD flags);
void hide_window(window *w);
int registered_name(UINT id, WCHAR *out, int capacity);
int show_host(window *w);
int own_frame(const window *w);
void drop_host(window *w);
void deactivate(window *w);
int size_surface(window *top, int width, int height);
void invalidate_frame(window *w);
void erase_now(window *w, int depth);
void client_screen_origin(const window *w, int *x, int *y);
void frame_of_child(const window *w, int window_width, int window_height, int out[4]);
void nc_paint(window *w);
void link_child_at(window *parent, window *w, window *after);
void unlink_child(window *w);
void send_size_move(window *w);
u32 next_z(void);
window *wclass_window(const wclass *c);
wclass *find_class(LPCWSTR name);
wclass *class_of_atom(ATOM atom);
void remove_class(wclass *c);
void timer_drop(window *w, UINT_PTR id);
int set_timer_internal(window *w, UINT_PTR id, u32 elapse, int system);
void destroy_owned(window *owner);
void free_props(window *w);

/* winpos.c */
#define SWP_NOCLIENTSIZE 0x0800
#define SWP_NOCLIENTMOVE 0x1000
BOOL set_window_pos(window *w, HWND after, int x, int y, int cx, int cy, UINT flags);

/* hook.c */
extern int g_hook_count;
int hook_installed(int id);
LRESULT call_hook(int id, int code, WPARAM wParam, LPARAM lParam);
LRESULT cbt_hook(int code, WPARAM wParam, LPARAM lParam);

/* clipboard.c */
void clipboard_window_gone(window *w);
HGLOBAL make_drop(const char *uris, u32 length, POINT point, int nonclient);

/* caret.c */
void caret_hide_for_paint(window *w);
void caret_show_after_paint(window *w);
void caret_window_gone(window *w);
u32 caret_next_blink(u32 now);
void caret_blink(u32 now);

/* scroll.c */
void nc_paint_scroll_bars(window *w, HDC dc, int left, int top);
LRESULT scroll_nc_hit(window *w, int x, int y);
void scroll_track(window *w, int bar, int x, int y);
void register_scroll_bar(void);

/* menu.c */
HMENU menu_load(HINSTANCE instance, LPCWSTR name);
void menu_destroy(HMENU menu);
void menu_draw_bar(window *top);
void menu_track(window *top, int by_mouse, WCHAR key);
int menu_host_pointer(u32 host, window **owner, int *x, int *y);
int menu_host_close(u32 host);
int menu_command_popup(HMENU menu, UINT id, HMENU *popup, int *index, UINT *state);
int menu_tracking(void);

/* dialog.c */
LRESULT WINAPI DefDlgProcW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void dialog_free(window *w);
DLGPROC *dialog_proc_of(window *w);
void dialog_check_default(window *dlg, window *old, window *control, int by_pointer);

/* edit.c, listbox.c, combo.c */
void register_edit(void);
void register_list_box(void);
void register_combo_box(void);
/* A list box inside a combo box tells the combo box what happened. */
void combo_list_event(window *combo, int event, int index);
int list_box_count(window *list);
int list_box_selection(window *list);
void list_box_text(window *list, int index, WCHAR *out, int capacity);
#define COMBO_LIST_CHOSEN 1   /* Enter chose the item */
#define COMBO_LIST_SELECTED 2 /* the selection moved */
#define COMBO_LIST_CANCEL 3   /* Escape, or a press outside */
#define COMBO_LIST_CLICKED 4  /* a click in the dropped list chose an item */
#define COMBO_LIST_RELEASED 5 /* the press that dropped the list came up */
#define ES_COMBO 0x0200       /* an edit control inside a combo box, as Windows XP marks it */
#define LB_CARETON 0x01a3     /* the combo box's own: the dropped list shows its focus */
#define LB_CARETOFF 0x01a4

/* controls.c */
void register_controls(void);
void draw_arrow(HDC dc, const RECT *button, int direction, int disabled, int pressed);
/* Group boxes and plain static text: the pointer goes through them
 * (HTTRANSPARENT). */
int control_transparent(const window *w);
void draw_edge(HDC dc, RECT *r, COLORREF top_left_outer, COLORREF bottom_right_outer, COLORREF top_left_inner,
               COLORREF bottom_right_inner);
void draw_focus_rect(HDC dc, const RECT *r);
int draw_label(HDC dc, const WCHAR *text, RECT *r, UINT format, int disabled, int hide_prefix);

#endif
