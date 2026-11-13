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

#define MAX_TEXT 256
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_INVALID_MENU_HANDLE 1401u
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

typedef struct wclass {
    int used;
    ATOM atom;
    WCHAR name[64];
    UINT style;
    WNDPROC proc;
    int window_extra;
    HINSTANCE instance;
    HICON icon;
    HCURSOR cursor;
    HBRUSH background;
    LPCWSTR menu_name; /* an id, or menu_text */
    WCHAR menu_text[64];
} wclass;

typedef struct dialog_info dialog_info;

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
};

extern window g_windows[];
extern window *g_active;
extern window *g_focus;
extern window *g_capture;
/* The last input was a key: windows created now show their keyboard cues. */
extern int g_keyboard_last;
extern POINT g_last_point;

/* user32.c */
void set_last_error(DWORD error);
u32 ticks(void);
void zero(void *p, u32 size);
int same_name(const WCHAR *a, const WCHAR *b);
int copy_text(WCHAR *out, const WCHAR *text, int capacity);
int text_length(const WCHAR *text);
ATOM register_system_class(UINT style, WNDPROC proc, LPCWSTR name, ATOM atom, HBRUSH background);
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
void dialog_check_default(window *dlg, window *old, window *control, int by_pointer);

/* controls.c */
void register_controls(void);
/* Group boxes and plain static text: the pointer goes through them
 * (HTTRANSPARENT). */
int control_transparent(const window *w);
void draw_edge(HDC dc, RECT *r, COLORREF top_left_outer, COLORREF bottom_right_outer, COLORREF top_left_inner,
               COLORREF bottom_right_inner);
void draw_focus_rect(HDC dc, const RECT *r);
int draw_label(HDC dc, const WCHAR *text, RECT *r, UINT format, int disabled, int hide_prefix);

#endif
