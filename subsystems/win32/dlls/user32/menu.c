/*
 * menu.c -- Menus (docs/userland/WIN32.md, "Menus"): menu resources, a
 * window's menu bar, its popups and the modal loop that tracks them by
 * pointer and by keyboard, with the messages Windows XP sends their owner.
 *
 * The menu bar is drawn on the top-level window's surface, above the client
 * area. A popup is a host window of its own (SXW_WINDOW_POPUP) at the point
 * of the owner's surface Windows XP puts it, which never takes the keyboard.
 * While a menu is tracked the owner has the capture, as on Windows: pointer
 * input on a popup arrives as the owner's, in its client coordinates.
 */
#include "user32_private.h"

#define MAX_MENUS 96
#define MAX_ITEMS 48
#define MENU_HANDLE_TAG 0x00060000u
#define MSGF_MENU 2
#define MNC_IGNORE 0
#define MNC_CLOSE 1
#define MNC_EXECUTE 2
#define MNC_SELECT 3
#define NO_ITEM (-1)

/* Windows XP's classic metrics with its 8-point menu font. */
#define BAR_ITEM_TOP 1
#define BAR_ITEM_HEIGHT 18
#define BAR_ITEM_PAD 6
#define POPUP_ITEM_HEIGHT 17
#define POPUP_SEPARATOR_HEIGHT 9
#define POPUP_BORDER 2
#define POPUP_MARGIN_BOTTOM 2
#define POPUP_TEXT_LEFT 19   /* from the item's left: the check mark's column */
#define POPUP_TEXT_RIGHT 21  /* after the widest text: the arrow's column */
#define POPUP_TAB_GAP 12     /* between the text and the accelerator column */

typedef struct menu_item {
    UINT state; /* MF_GRAYED, MF_DISABLED, MF_CHECKED, MF_DEFAULT */
    UINT type;  /* MF_POPUP, MF_SEPARATOR, MF_OWNERDRAW, MF_BITMAP, the breaks */
    UINT id;
    WCHAR *text; /* the menu's own copy, NUL-terminated; 0 for none */
    int length;
    HMENU popup;
    ULONG_PTR data; /* an owner-drawn item's value */
    int height;     /* an owner-drawn item's, from WM_MEASUREITEM */
    int width;
    RECT rect; /* the bar's in surface coordinates, a popup's in its own */
} menu_item;

typedef struct menu {
    int used;
    int count;
    menu_item items[MAX_ITEMS];
    int width;
    int height;
    int tab_column; /* the accelerators' x */
} menu;

static menu g_menus[MAX_MENUS];
static HFONT g_menu_font;
/* The window an owner-drawn item is measured and drawn by. */
static window *g_menu_owner;

static void offset_rect(RECT *r, int dx, int dy) {
    r->left += dx;
    r->right += dx;
    r->top += dy;
    r->bottom += dy;
}

static menu *menu_of(HMENU h) {
    const u32 v = (u32)h;
    const u32 index = (v & 0xffffu) / 4u;
    if ((v & 0xffff0000u) != MENU_HANDLE_TAG || (v & 3u) != 0 || index == 0 || index > MAX_MENUS) {
        return 0;
    }
    menu *m = &g_menus[index - 1u];
    return m->used ? m : 0;
}

static HMENU new_menu(void) {
    for (int i = 0; i < MAX_MENUS; ++i) {
        if (!g_menus[i].used) {
            zero(&g_menus[i], sizeof(g_menus[i]));
            g_menus[i].used = 1;
            return (HMENU)(MENU_HANDLE_TAG | (u32)(i + 1) * 4u);
        }
    }
    return NULL;
}

/* ---- Items ------------------------------------------------------------------------- */

static int set_text(menu_item *item, const WCHAR *text, int length) {
    WCHAR *copy = 0;
    if (text != 0) {
        copy = SxwHeapAllocate((u32)(length + 1) * sizeof(WCHAR), 0);
        if (copy == 0) {
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        for (int i = 0; i < length; ++i) {
            copy[i] = text[i];
        }
        copy[length] = 0;
    }
    if (item->text != 0) {
        SxwHeapFree(item->text);
    }
    item->text = copy;
    item->length = copy != 0 ? length : 0;
    return 1;
}

/* ---- Resources --------------------------------------------------------------------- */

/* One level of a MENU resource (the header-less item list after the
 * four-byte header), up to the item with MF_END; NULL when it is malformed. */
static HMENU parse_level(const unsigned char **at, const unsigned char *end, int depth) {
    HMENU h = new_menu();
    menu *m = menu_of(h);
    if (m == 0 || depth > 4) {
        return NULL;
    }
    for (;;) {
        const unsigned char *p = *at;
        if (p + 2 > end) {
            return NULL;
        }
        const u32 flags = get16(p);
        p += 2;
        menu_item item;
        zero(&item, sizeof(item));
        if ((flags & MF_POPUP) == 0) {
            if (p + 2 > end) {
                return NULL;
            }
            item.id = get16(p);
            p += 2;
        }
        const WCHAR *text = (const WCHAR *)p;
        while (p + 2 <= end && get16(p) != 0) {
            p += 2;
        }
        if (p + 2 > end) {
            return NULL;
        }
        const int length = (int)((p - (const unsigned char *)text) / 2);
        p += 2;
        item.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED);
        item.type = flags & (MF_POPUP | MF_MENUBARBREAK | MF_MENUBREAK);
        if ((flags & MF_POPUP) == 0 && item.id == 0 && length == 0) {
            item.type |= MF_SEPARATOR;
        } else if (!set_text(&item, text, length)) {
            return NULL;
        }
        *at = p;
        if ((flags & MF_POPUP) != 0) {
            item.popup = parse_level(at, end, depth + 1);
            if (item.popup == NULL) {
                return NULL;
            }
        }
        if (m->count < MAX_ITEMS) {
            m->items[m->count++] = item;
        }
        if ((flags & MF_END) != 0) {
            return h;
        }
    }
}

HMENU menu_load(HINSTANCE instance, LPCWSTR name) {
    u32 size = 0;
    const unsigned char *data = find_resource(module_of(instance), RT_MENU, name, &size);
    if (data == 0 || size < 4 || get16(data) != 0) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return NULL;
    }
    const unsigned char *at = data + 4 + get16(data + 2);
    HMENU h = parse_level(&at, data + size, 0);
    if (h == NULL) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
    }
    return h;
}

HMENU WINAPI LoadMenuW(HINSTANCE hInstance, LPCWSTR lpMenuName) {
    return menu_load(hInstance, lpMenuName);
}

static void free_item(menu_item *item, int popups) {
    if (item->text != 0) {
        SxwHeapFree(item->text);
        item->text = 0;
    }
    if (popups && item->popup != NULL) {
        menu_destroy(item->popup);
    }
}

void menu_destroy(HMENU h) {
    menu *m = menu_of(h);
    if (m == 0) {
        return;
    }
    m->used = 0;
    for (int i = 0; i < m->count; ++i) {
        free_item(&m->items[i], 1);
    }
}

/* The item with a command id, searched through the popups; its menu and
 * index. */
static menu_item *find_command(HMENU h, UINT id, HMENU *owner_menu, int *index) {
    menu *m = menu_of(h);
    if (m == 0) {
        return 0;
    }
    for (int i = 0; i < m->count; ++i) {
        menu_item *item = &m->items[i];
        if (item->popup != NULL) {
            menu_item *found = find_command(item->popup, id, owner_menu, index);
            if (found != 0) {
                return found;
            }
        } else if (item->id == id) {
            *owner_menu = h;
            *index = i;
            return item;
        }
    }
    return 0;
}

/* An item by position (MF_BYPOSITION) or by command, and the menu that
 * holds it. */
static menu_item *find_item(HMENU h, UINT which, UINT flags, HMENU *in, int *index) {
    menu *m = menu_of(h);
    if (m == 0) {
        return 0;
    }
    if ((flags & MF_BYPOSITION) != 0) {
        if (which >= (UINT)m->count) {
            return 0;
        }
        *in = h;
        *index = (int)which;
        return &m->items[which];
    }
    return find_command(h, which, in, index);
}

int menu_command_popup(HMENU bar, UINT id, HMENU *popup, int *index, UINT *state) {
    menu *m = menu_of(bar);
    if (m == 0) {
        return 0;
    }
    for (int i = 0; i < m->count; ++i) {
        HMENU in;
        int at;
        if (m->items[i].popup != NULL) {
            menu_item *item = find_command(m->items[i].popup, id, &in, &at);
            if (item != 0) {
                *popup = m->items[i].popup;
                *index = i;
                *state = item->state;
                return 1;
            }
        }
    }
    return 0;
}

HMENU WINAPI GetMenu(HWND hWnd) {
    window *w = checked(hWnd);
    return w != 0 && (w->style & WS_CHILD) == 0 ? w->menu : NULL;
}

/* The previous state (MF_GRAYED, MF_DISABLED), or -1 when there is no such
 * item. Nothing is redrawn: Windows waits for DrawMenuBar too. */
BOOL WINAPI EnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable) {
    if (menu_of(hMenu) == 0) {
        set_last_error(ERROR_INVALID_MENU_HANDLE);
        return -1;
    }
    HMENU in;
    int index;
    menu_item *item = find_item(hMenu, uIDEnableItem, uEnable, &in, &index);
    if (item == 0) {
        return -1;
    }
    const UINT old = item->state & (MF_GRAYED | MF_DISABLED);
    item->state = (item->state & ~(UINT)(MF_GRAYED | MF_DISABLED)) | (uEnable & (MF_GRAYED | MF_DISABLED));
    return (BOOL)old;
}

DWORD WINAPI CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck) {
    HMENU in;
    int index;
    menu_item *item = find_item(hMenu, uIDCheckItem, uCheck, &in, &index);
    if (item == 0) {
        return (DWORD)-1;
    }
    const DWORD old = item->state & MF_CHECKED;
    item->state = (item->state & ~(UINT)MF_CHECKED) | (uCheck & MF_CHECKED);
    return old;
}

/* An item's flags; a popup's with its item count in the high byte, as
 * Windows answers. A separator is disabled and grayed (fixtures/hooks). */
UINT WINAPI GetMenuState(HMENU hMenu, UINT uId, UINT uFlags) {
    HMENU in;
    int index;
    menu_item *item = find_item(hMenu, uId, uFlags, &in, &index);
    if (item == 0) {
        return (UINT)-1;
    }
    if (item->popup != NULL) {
        menu *sub = menu_of(item->popup);
        return ((UINT)(sub != 0 ? sub->count : 0) << 8) | (item->state & 0xffu) | MF_POPUP |
               (item->type & (MF_MENUBARBREAK | MF_MENUBREAK | MF_OWNERDRAW | MF_BITMAP));
    }
    return item->state | (item->type & (MF_SEPARATOR | MF_MENUBARBREAK | MF_MENUBREAK | MF_OWNERDRAW | MF_BITMAP));
}

int WINAPI GetMenuStringW(HMENU hMenu, UINT uIDItem, LPWSTR lpString, int cchMax, UINT flags) {
    HMENU in;
    int index;
    menu_item *item = find_item(hMenu, uIDItem, flags, &in, &index);
    if (item == 0) {
        return 0;
    }
    if (lpString == NULL || cchMax <= 0) {
        return item->length;
    }
    int n = 0;
    while (n < item->length && n + 1 < cchMax) {
        lpString[n] = item->text[n];
        ++n;
    }
    lpString[n] = 0;
    return n;
}

int WINAPI GetMenuItemCount(HMENU hMenu) {
    menu *m = menu_of(hMenu);
    if (m == 0) {
        set_last_error(ERROR_INVALID_MENU_HANDLE);
        return -1;
    }
    return m->count;
}

UINT WINAPI GetMenuItemID(HMENU hMenu, int nPos) {
    menu *m = menu_of(hMenu);
    if (m == 0 || nPos < 0 || nPos >= m->count || m->items[nPos].popup != NULL) {
        return (UINT)-1;
    }
    return m->items[nPos].id;
}

HMENU WINAPI GetSubMenu(HMENU hMenu, int nPos) {
    menu *m = menu_of(hMenu);
    if (m == 0 || nPos < 0 || nPos >= m->count) {
        return NULL;
    }
    return m->items[nPos].popup;
}

HMENU WINAPI CreateMenu(void) {
    HMENU h = new_menu();
    if (h == NULL) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    }
    return h;
}

HMENU WINAPI CreatePopupMenu(void) {
    return CreateMenu();
}

BOOL WINAPI IsMenu(HMENU hMenu) {
    return menu_of(hMenu) != 0;
}

BOOL WINAPI DestroyMenu(HMENU hMenu) {
    if (menu_of(hMenu) == 0) {
        set_last_error(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    menu_destroy(hMenu);
    return TRUE;
}

/* An item as AppendMenu, InsertMenu and ModifyMenu describe it. */
static int fill_item(menu_item *item, UINT flags, UINT_PTR id, LPCWSTR content) {
    item->state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_DEFAULT | MF_HILITE);
    item->type = flags & (MF_POPUP | MF_SEPARATOR | MF_OWNERDRAW | MF_BITMAP | MF_MENUBARBREAK | MF_MENUBREAK |
                          MF_RIGHTJUSTIFY);
    item->popup = (flags & MF_POPUP) != 0 ? (HMENU)id : NULL;
    item->id = (flags & MF_POPUP) != 0 ? (UINT)-1 : (UINT)id;
    item->data = 0;
    item->height = item->width = 0;
    if ((flags & MF_SEPARATOR) != 0) {
        item->state |= MF_GRAYED | MF_DISABLED;
        item->id = (flags & MF_POPUP) != 0 ? item->id : (UINT)id;
        return set_text(item, 0, 0);
    }
    if ((flags & (MF_OWNERDRAW | MF_BITMAP)) != 0) {
        item->data = (ULONG_PTR)content;
        return set_text(item, 0, 0);
    }
    if (content == NULL) {
        return set_text(item, 0, 0);
    }
    int length = 0;
    while (content[length] != 0) {
        ++length;
    }
    return set_text(item, content, length);
}

static BOOL insert_item(HMENU hMenu, int at, UINT flags, UINT_PTR id, LPCWSTR content) {
    menu *m = menu_of(hMenu);
    if (m == 0) {
        set_last_error(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    if (m->count == MAX_ITEMS) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (at < 0 || at > m->count) {
        at = m->count;
    }
    menu_item item;
    zero(&item, sizeof(item));
    if (!fill_item(&item, flags, id, content)) {
        return FALSE;
    }
    for (int i = m->count; i > at; --i) {
        m->items[i] = m->items[i - 1];
    }
    m->items[at] = item;
    ++m->count;
    return TRUE;
}

BOOL WINAPI AppendMenuW(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem) {
    return insert_item(hMenu, -1, uFlags, uIDNewItem, lpNewItem);
}

/* Before the item at a position (past the end: at the end) or before the
 * item with a command, in whichever popup holds it. */
BOOL WINAPI InsertMenuW(HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem) {
    if (menu_of(hMenu) == 0) {
        set_last_error(ERROR_INVALID_MENU_HANDLE);
        return FALSE;
    }
    if ((uFlags & MF_BYPOSITION) != 0) {
        return insert_item(hMenu, (int)uPosition, uFlags & ~(UINT)MF_BYPOSITION, uIDNewItem, lpNewItem);
    }
    HMENU in;
    int index;
    if (find_command(hMenu, uPosition, &in, &index) == 0) {
        return insert_item(hMenu, -1, uFlags, uIDNewItem, lpNewItem);
    }
    return insert_item(in, index, uFlags, uIDNewItem, lpNewItem);
}

BOOL WINAPI ModifyMenuW(HMENU hMnu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem) {
    HMENU in;
    int index;
    menu_item *item = find_item(hMnu, uPosition, uFlags, &in, &index);
    if (item == 0) {
        return FALSE;
    }
    if (item->popup != NULL && ((uFlags & MF_POPUP) == 0 || (HMENU)uIDNewItem != item->popup)) {
        menu_destroy(item->popup);
    }
    return fill_item(item, uFlags & ~(UINT)MF_BYPOSITION, uIDNewItem, lpNewItem);
}

static BOOL take_item(HMENU hMenu, UINT uPosition, UINT uFlags, int destroy) {
    HMENU in;
    int index;
    menu_item *item = find_item(hMenu, uPosition, uFlags, &in, &index);
    if (item == 0) {
        return FALSE;
    }
    menu *m = menu_of(in);
    free_item(item, destroy);
    for (int i = index; i + 1 < m->count; ++i) {
        m->items[i] = m->items[i + 1];
    }
    --m->count;
    return TRUE;
}

/* Deleting an item destroys its popup; removing it leaves the popup. */
BOOL WINAPI DeleteMenu(HMENU hMenu, UINT uPosition, UINT uFlags) {
    return take_item(hMenu, uPosition, uFlags, 1);
}

BOOL WINAPI RemoveMenu(HMENU hMenu, UINT uPosition, UINT uFlags) {
    return take_item(hMenu, uPosition, uFlags, 0);
}

/* Check marks are drawn by the menu itself; bitmaps for them are taken and
 * not used. */
BOOL WINAPI SetMenuItemBitmaps(HMENU hMenu, UINT uPosition, UINT uFlags, HBITMAP hBitmapUnchecked,
                               HBITMAP hBitmapChecked) {
    (void)hBitmapUnchecked;
    (void)hBitmapChecked;
    HMENU in;
    int index;
    return find_item(hMenu, uPosition, uFlags, &in, &index) != 0;
}

/* 13 by 13, Windows XP's at 96 dots per inch. */
LONG WINAPI GetMenuCheckMarkDimensions(void) {
    return (LONG)MAKELPARAM(13, 13);
}

/* The window menu: the frame is the window manager's, so this menu is
 * never shown; it answers what a program asks it. */
HMENU WINAPI GetSystemMenu(HWND hWnd, BOOL bRevert) {
    static HMENU s_menu;
    window *w = checked(hWnd);
    if (w == 0 || bRevert) {
        return NULL;
    }
    if (menu_of(s_menu) == 0) {
        static const WCHAR k_restore[] = {'&', 'R', 'e', 's', 't', 'o', 'r', 'e', 0};
        static const WCHAR k_move[] = {'&', 'M', 'o', 'v', 'e', 0};
        static const WCHAR k_size[] = {'&', 'S', 'i', 'z', 'e', 0};
        static const WCHAR k_minimize[] = {'M', 'i', '&', 'n', 'i', 'm', 'i', 'z', 'e', 0};
        static const WCHAR k_maximize[] = {'M', 'a', '&', 'x', 'i', 'm', 'i', 'z', 'e', 0};
        static const WCHAR k_close[] = {'&', 'C', 'l', 'o', 's', 'e', '\t', 'A', 'l', 't', '+', 'F', '4', 0};
        s_menu = CreatePopupMenu();
        AppendMenuW(s_menu, MF_STRING | MF_GRAYED, 0xf120, k_restore);
        AppendMenuW(s_menu, MF_STRING, 0xf010, k_move);
        AppendMenuW(s_menu, MF_STRING, 0xf000, k_size);
        AppendMenuW(s_menu, MF_STRING, 0xf020, k_minimize);
        AppendMenuW(s_menu, MF_STRING, 0xf030, k_maximize);
        AppendMenuW(s_menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(s_menu, MF_STRING, SC_CLOSE, k_close);
    }
    return s_menu;
}

/* A new menu bar for a top-level window, or none. The old one is the
 * program's to destroy. The client area gives its top to the bar. */
BOOL WINAPI SetMenu(HWND hWnd, HMENU hMenu) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    if ((w->style & WS_CHILD) != 0 || (hMenu != NULL && menu_of(hMenu) == 0)) {
        set_last_error(hMenu != NULL && menu_of(hMenu) == 0 ? ERROR_INVALID_MENU_HANDLE : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    w->menu = hMenu;
    const int height = hMenu != NULL ? MENU_BAR_HEIGHT : 0;
    if (height != w->menu_height) {
        const int surface_height = client_height(w) + w->menu_height;
        w->menu_height = height;
        w->client.bottom = w->client.top + (surface_height - height > 0 ? surface_height - height : 0);
        set_window_pos(w, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                                                SWP_FRAMECHANGED);
        if (window_of(hWnd) != 0) {
            send(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client_width(w), client_height(w)));
            invalidate(w, 0, 1, 1);
        }
    }
    menu_draw_bar(w);
    return TRUE;
}

BOOL WINAPI DrawMenuBar(HWND hWnd) {
    window *w = checked(hWnd);
    if (w == 0) {
        return FALSE;
    }
    menu_draw_bar(top_of(w));
    return TRUE;
}

/* ---- Layout and drawing ---------------------------------------------------------------- */

static HDC surface_dc(sxw_surface *surface, int width, int height) {
    sxw_region all;
    sxw_rect r = {0, 0, width, height};
    sxw_region_set(&all, &r);
    HDC dc = (HDC)SxwCreateSurfaceDC(surface, 0, 0, &all);
    if (g_menu_font == NULL) {
        static const WCHAR k_face[] = {'M', 'S', ' ', 'S', 'h', 'e', 'l', 'l', ' ', 'D', 'l', 'g', 0};
        g_menu_font = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, k_face);
    }
    SelectObject(dc, g_menu_font);
    SxwSetBkMode(dc, TRANSPARENT);
    return dc;
}

/* The width of an item's text up to its tab (the label) and after it (the
 * accelerator), '&' left out. */
static void text_widths(HDC dc, const menu_item *item, int *label, int *accelerator) {
    int tab = 0;
    while (tab < item->length && item->text[tab] != '\t') {
        ++tab;
    }
    RECT r = {0, 0, 0, 0};
    WCHAR part[MAX_TEXT];
    int n = 0;
    for (int i = 0; i < tab && n + 1 < MAX_TEXT; ++i) {
        part[n++] = item->text[i];
    }
    part[n] = 0;
    r.left = r.top = r.right = r.bottom = 0;
    draw_label(dc, part, &r, DT_CALCRECT, 0, 0);
    *label = r.right;
    n = 0;
    for (int i = tab + 1; i < item->length && n + 1 < MAX_TEXT; ++i) {
        part[n++] = item->text[i];
    }
    part[n] = 0;
    r.left = r.top = r.right = r.bottom = 0;
    if (n != 0) {
        draw_label(dc, part, &r, DT_CALCRECT, 0, 0);
    }
    *accelerator = n != 0 ? r.right : 0;
}

/* Bar items from the left, in surface coordinates. */
static void layout_bar(window *top, HDC dc) {
    menu *m = menu_of(top->menu);
    if (m == 0) {
        return;
    }
    int x = 0;
    for (int i = 0; i < m->count; ++i) {
        menu_item *item = &m->items[i];
        int label;
        int accelerator;
        text_widths(dc, item, &label, &accelerator);
        item->rect.left = x;
        item->rect.top = BAR_ITEM_TOP;
        item->rect.right = x + label + 2 * BAR_ITEM_PAD;
        item->rect.bottom = BAR_ITEM_TOP + BAR_ITEM_HEIGHT;
        x = item->rect.right;
    }
}

static void layout_popup(menu *m, HDC dc) {
    int widest = 0;
    int widest_accelerator = 0;
    int y = POPUP_BORDER;
    for (int i = 0; i < m->count; ++i) {
        menu_item *item = &m->items[i];
        int height = (item->type & MF_SEPARATOR) != 0 ? POPUP_SEPARATOR_HEIGHT : POPUP_ITEM_HEIGHT;
        if ((item->type & MF_OWNERDRAW) != 0) {
            MEASUREITEMSTRUCT mi;
            zero(&mi, sizeof(mi));
            mi.CtlType = ODT_MENU;
            mi.itemID = item->id;
            mi.itemHeight = POPUP_ITEM_HEIGHT;
            mi.itemData = item->data;
            if (g_menu_owner != 0) {
                send(g_menu_owner, WM_MEASUREITEM, 0, (LPARAM)&mi);
            }
            item->width = (int)mi.itemWidth;
            item->height = (int)mi.itemHeight;
            height = item->height;
            widest = item->width > widest ? item->width : widest;
        }
        item->rect.top = y;
        item->rect.bottom = y + height;
        y += height;
        if ((item->type & (MF_SEPARATOR | MF_OWNERDRAW)) == 0) {
            int label;
            int accelerator;
            text_widths(dc, item, &label, &accelerator);
            widest = label > widest ? label : widest;
            widest_accelerator = accelerator > widest_accelerator ? accelerator : widest_accelerator;
        }
    }
    m->tab_column = POPUP_BORDER + POPUP_TEXT_LEFT + widest + (widest_accelerator != 0 ? POPUP_TAB_GAP : 0);
    m->width = m->tab_column + widest_accelerator + POPUP_TEXT_RIGHT + POPUP_BORDER;
    m->height = y + POPUP_MARGIN_BOTTOM + POPUP_BORDER;
    for (int i = 0; i < m->count; ++i) {
        m->items[i].rect.left = POPUP_BORDER;
        m->items[i].rect.right = m->width - POPUP_BORDER;
    }
}

/* Text with the classic grayed look: an etched copy below and right in the
 * highlight colour, then the shadow colour, unless highlighted. */
static void item_text(HDC dc, const WCHAR *text, int length, RECT *r, UINT format, COLORREF color, int grayed,
                      int highlighted, int hide_prefix) {
    WCHAR part[MAX_TEXT];
    int n = 0;
    for (int i = 0; i < length && n + 1 < MAX_TEXT; ++i) {
        part[n++] = text[i];
    }
    part[n] = 0;
    if (grayed && !highlighted) {
        RECT shadow = *r;
        offset_rect(&shadow, 1, 1);
        SetTextColor(dc, GetSysColor(COLOR_BTNHIGHLIGHT));
        draw_label(dc, part, &shadow, format, 0, hide_prefix);
        SetTextColor(dc, GetSysColor(COLOR_BTNSHADOW));
    } else {
        SetTextColor(dc, grayed ? GetSysColor(COLOR_BTNSHADOW) : color);
    }
    draw_label(dc, part, r, format, 0, hide_prefix);
}

/* ---- Tracking ------------------------------------------------------------------------- */

typedef struct tracker {
    int active;
    window *owner;    /* hears the menu's messages */
    window *top;      /* the top-level window it is drawn over */
    HMENU bar;
    int bar_item;     /* NO_ITEM when nothing on the bar is selected */
    int popup_open;
    HMENU popup;
    int popup_item;
    u32 host;         /* the popup's host window */
    int x;            /* where the popup is on the owner's surface */
    int y;
    int show_accel;   /* entered by keyboard: mnemonics are underlined */
    int end;
    UINT command;
} tracker;

static tracker g_track;
/* The popup's pixels, kept from one popup to the next. */
static sxw_surface g_popup_surface;
static int g_popup_capacity;

int menu_tracking(void) {
    return g_track.active;
}

void menu_draw_bar(window *top) {
    menu *m = menu_of(top->menu);
    if (m == 0 || top->menu_height == 0 || top->surface.pixels == 0) {
        return;
    }
    HDC dc = surface_dc(&top->surface, top->surface.width, top->menu_height);
    layout_bar(top, dc);
    RECT all = {0, 0, top->surface.width, top->menu_height};
    fill_rect(dc, &all, GetSysColor(COLOR_MENU));
    const int tracked = g_track.active && g_track.top == top;
    /* A disabled window's bar (its owner of a dialog or message box) is drawn
     * in COLOR_GRAYTEXT, flat, as Windows XP draws it. */
    const int disabled = (top->style & WS_DISABLED) != 0;
    for (int i = 0; i < m->count; ++i) {
        menu_item *item = &m->items[i];
        RECT r = item->rect;
        const int selected = tracked && g_track.bar_item == i;
        if (selected && g_track.popup_open) {
            RECT edge = r;
            draw_edge(dc, &edge, GetSysColor(COLOR_BTNSHADOW), GetSysColor(COLOR_BTNHIGHLIGHT), CLR_INVALID,
                      CLR_INVALID);
            offset_rect(&r, 1, 1);
        } else if (selected) {
            RECT edge = r;
            draw_edge(dc, &edge, GetSysColor(COLOR_BTNHIGHLIGHT), GetSysColor(COLOR_BTNSHADOW), CLR_INVALID,
                      CLR_INVALID);
        }
        item_text(dc, item->text, item->length, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                  GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_MENUTEXT), !disabled && (item->state & MF_GRAYED) != 0, 0,
                  !(tracked && g_track.show_accel));
    }
    DeleteObject(dc);
    sxw_rect dirty = {0, 0, top->surface.width, top->menu_height};
    sxw_region_union_rect(&top->surface.dirty, &dirty);
}

/* The check mark Windows XP draws for MF_CHECKED: a 7-pixel tick in the
 * 13-pixel check box at the item's left, centred down the item
 * (fixtures/controls). */
static void draw_check(HDC dc, const RECT *item, COLORREF color) {
    static const char *const k_tick[7] = {"......#", ".....##", "#...###", "##.###.", "#####..", ".###...", "..#...."};
    const int top = item->top + (item->bottom - item->top - 13) / 2 + 5;
    const int left = item->left + 5;
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 7; ++x) {
            if (k_tick[y][x] == '#') {
                SetPixel(dc, left + x, top + y, color);
            }
        }
    }
}

static void draw_popup(void) {
    menu *m = menu_of(g_track.popup);
    if (m == 0 || g_track.host == 0) {
        return;
    }
    HDC dc = surface_dc(&g_popup_surface, m->width, m->height);
    RECT all = {0, 0, m->width, m->height};
    fill_rect(dc, &all, GetSysColor(COLOR_MENU));
    RECT edge = all;
    draw_edge(dc, &edge, GetSysColor(COLOR_3DLIGHT), GetSysColor(COLOR_3DDKSHADOW), GetSysColor(COLOR_BTNHIGHLIGHT),
              GetSysColor(COLOR_BTNSHADOW));
    for (int i = 0; i < m->count; ++i) {
        menu_item *item = &m->items[i];
        RECT r = item->rect;
        if ((item->type & MF_SEPARATOR) != 0) {
            const int mid = r.top + POPUP_SEPARATOR_HEIGHT / 2;
            RECT line = {r.left + 2, mid, r.right - 2, mid + 1};
            fill_rect(dc, &line, GetSysColor(COLOR_BTNSHADOW));
            offset_rect(&line, 0, 1);
            fill_rect(dc, &line, GetSysColor(COLOR_BTNHIGHLIGHT));
            continue;
        }
        const int highlighted = g_track.popup_item == i;
        if ((item->type & MF_OWNERDRAW) != 0) {
            DRAWITEMSTRUCT d;
            zero(&d, sizeof(d));
            d.CtlType = ODT_MENU;
            d.itemID = item->id;
            d.itemAction = ODA_DRAWENTIRE;
            d.itemState = (highlighted ? ODS_SELECTED : 0) | ((item->state & MF_GRAYED) != 0 ? ODS_GRAYED : 0) |
                          ((item->state & MF_DISABLED) != 0 ? ODS_DISABLED : 0) |
                          ((item->state & MF_CHECKED) != 0 ? ODS_CHECKED : 0);
            d.hwndItem = (HWND)g_track.popup;
            d.hDC = dc;
            d.rcItem = r;
            d.itemData = item->data;
            send(g_track.owner, WM_DRAWITEM, 0, (LPARAM)&d);
            continue;
        }
        if (highlighted) {
            fill_rect(dc, &r, GetSysColor(COLOR_HIGHLIGHT));
        }
        const COLORREF color = GetSysColor(highlighted ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
        const int grayed = (item->state & MF_GRAYED) != 0;
        int tab = 0;
        while (tab < item->length && item->text[tab] != '\t') {
            ++tab;
        }
        if ((item->state & MF_CHECKED) != 0) {
            draw_check(dc, &r, grayed ? GetSysColor(COLOR_GRAYTEXT) : color);
        }
        RECT text = {r.left + POPUP_TEXT_LEFT, r.top, r.right, r.bottom};
        item_text(dc, item->text, tab, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE, color, grayed, highlighted,
                  !g_track.show_accel);
        if (tab < item->length) {
            RECT accelerator = {m->tab_column, r.top, r.right, r.bottom};
            item_text(dc, item->text + tab + 1, item->length - tab - 1, &accelerator,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE, color, grayed, highlighted, 1);
        }
    }
    DeleteObject(dc);
    RECT whole = {0, 0, m->width, m->height};
    u32 size[2] = {(u32)m->width, (u32)m->height};
    u32 arguments[6] = {g_track.host, (u32)g_popup_surface.pixels, (u32)g_popup_surface.stride * 4u, (u32)size,
                        (u32)&whole, 1};
    SxwService(SXW_SVC_WINDOW_PRESENT, arguments);
    sxw_region_clear(&g_popup_surface.dirty);
}

static void select_message(UINT item, UINT flags, HMENU h) {
    send(g_track.owner, WM_MENUSELECT, MAKELPARAM(item, flags), (LPARAM)h);
}

/* WM_MENUSELECT for the selected bar item, or popup item. */
static void select_bar(int index, int mouse) {
    g_track.bar_item = index;
    select_message((UINT)index, MF_POPUP | MF_HILITE | (mouse ? MF_MOUSESELECT : 0), g_track.bar);
    menu_draw_bar(g_track.top);
}

static void select_popup_item(int index, int mouse) {
    menu *m = menu_of(g_track.popup);
    if (m == 0 || index == g_track.popup_item) {
        return;
    }
    g_track.popup_item = index;
    if (index != NO_ITEM) {
        const menu_item *item = &m->items[index];
        UINT flags = MF_HILITE | item->state | (mouse ? MF_MOUSESELECT : 0);
        if ((item->type & MF_SEPARATOR) != 0) {
            flags |= MF_SEPARATOR;
        }
        select_message(item->id, flags, g_track.popup);
    }
    draw_popup();
}

/* Hides the popup; with select, the bar item it hung from is selected
 * again, as Windows XP says when the popup goes but the menu stays. */
static void close_popup(int select, int mouse) {
    if (!g_track.popup_open) {
        return;
    }
    g_track.popup_open = 0;
    if (g_track.host != 0) {
        u32 arguments[1] = {g_track.host};
        SxwService(SXW_SVC_WINDOW_DESTROY, arguments);
        g_track.host = 0;
    }
    send(g_track.owner, WM_UNINITMENUPOPUP, (WPARAM)g_track.popup, 0);
    g_track.popup = NULL;
    g_track.popup_item = NO_ITEM;
    if (select && g_track.bar_item != NO_ITEM) {
        select_message((UINT)g_track.bar_item, MF_POPUP | MF_HILITE | (mouse ? MF_MOUSESELECT : 0), g_track.bar);
    }
    if (g_track.bar != NULL) {
        menu_draw_bar(g_track.top);
    }
}

/* Shows the popup of the selected bar item, its first item selected when
 * the keyboard opened it. */
/* Shows a popup at (x, y) of the owner's surface, moved left or up by its
 * width or height as align (TPM_*) asks. WM_INITMENUPOPUP has been sent. */
static void show_popup(HMENU popup, int x, int y, UINT align, int select_first) {
    menu *m = menu_of(popup);
    if (m == 0 || !g_track.popup_open) {
        return;
    }
    g_menu_owner = g_track.owner;
    HDC measure = surface_dc(&g_track.top->surface, 1, 1);
    layout_popup(m, measure);
    DeleteObject(measure);
    if (m->width * m->height > g_popup_capacity) {
        unsigned int *pixels = SxwHeapAllocate((u32)(m->width * m->height) * 4u, 1);
        if (pixels == 0) {
            return;
        }
        if (g_popup_surface.pixels != 0) {
            SxwHeapFree(g_popup_surface.pixels);
        }
        g_popup_surface.pixels = pixels;
        g_popup_capacity = m->width * m->height;
    }
    g_popup_surface.stride = g_popup_surface.width = m->width;
    g_popup_surface.height = m->height;
    if ((align & TPM_CENTERALIGN) != 0) {
        x -= m->width / 2;
    } else if ((align & TPM_RIGHTALIGN) != 0) {
        x -= m->width;
    }
    if ((align & TPM_VCENTERALIGN) != 0) {
        y -= m->height / 2;
    } else if ((align & TPM_BOTTOMALIGN) != 0) {
        y -= m->height;
    }
    g_track.x = x;
    g_track.y = y;
    static const WCHAR k_none[] = {0};
    u32 size[2] = {0, 0};
    u32 arguments[8] = {(u32)k_none, (u32)m->width, (u32)m->height, (u32)size, g_track.top->host,
                        SXW_WINDOW_POPUP, (u32)g_track.x, (u32)g_track.y};
    g_track.host = SxwService(SXW_SVC_WINDOW_CREATE, arguments);
    if (g_track.bar != NULL) {
        menu_draw_bar(g_track.top);
    }
    if (select_first) {
        for (int i = 0; i < m->count; ++i) {
            if ((m->items[i].type & MF_SEPARATOR) == 0) {
                select_popup_item(i, 0);
                break;
            }
        }
    }
    draw_popup();
}

/* Shows the popup of the selected bar item, its first item selected when
 * the keyboard opened it. */
static void open_popup(int select_first) {
    menu *bar = menu_of(g_track.bar);
    if (bar == 0 || g_track.bar_item == NO_ITEM) {
        return;
    }
    menu_item *item = &bar->items[g_track.bar_item];
    if (item->popup == NULL) {
        return;
    }
    g_track.popup = item->popup;
    g_track.popup_item = NO_ITEM;
    g_track.popup_open = 1;
    send(g_track.owner, WM_INITMENUPOPUP, (WPARAM)item->popup, MAKELPARAM(g_track.bar_item, 0));
    show_popup(item->popup, item->rect.left, item->rect.bottom, 0, select_first);
}

/* The next item that is not a separator, step 1 or -1, wrapping. */
static int next_item(const menu *m, int from, int step) {
    int i = from;
    for (int n = 0; n < m->count; ++n) {
        i = i == NO_ITEM ? (step > 0 ? 0 : m->count - 1) : (i + step + m->count) % m->count;
        if ((m->items[i].type & MF_SEPARATOR) == 0) {
            return i;
        }
    }
    return NO_ITEM;
}

static void switch_bar(int index, int mouse, int show) {
    if (index == g_track.bar_item) {
        return;
    }
    close_popup(1, mouse);
    select_bar(index, mouse);
    if (show) {
        open_popup(!mouse);
    }
}

/* Runs the selected popup item; the menu ends when it can. */
static void execute(void) {
    menu *m = menu_of(g_track.popup);
    if (m == 0 || g_track.popup_item == NO_ITEM) {
        return;
    }
    const menu_item *item = &m->items[g_track.popup_item];
    if ((item->state & (MF_GRAYED | MF_DISABLED)) != 0 || (item->type & MF_SEPARATOR) != 0 ||
        item->popup != NULL) {
        return;
    }
    g_track.command = item->id;
    g_track.end = 1;
}

/* The item whose mnemonic is c, from after the selected one; NO_ITEM. */
static int find_mnemonic(const menu *m, WCHAR c, int from) {
    if (c >= 'a' && c <= 'z') {
        c = (WCHAR)(c - 32);
    }
    for (int n = 1; n <= m->count; ++n) {
        const int i = ((from == NO_ITEM ? -1 : from) + n) % m->count;
        const menu_item *item = &m->items[i];
        for (int k = 0; k + 1 < item->length; ++k) {
            if (item->text[k] == '&') {
                WCHAR next = item->text[k + 1];
                if (next >= 'a' && next <= 'z') {
                    next = (WCHAR)(next - 32);
                }
                if (next == '&') {
                    ++k;
                    continue;
                }
                if (next == c) {
                    return i;
                }
                break;
            }
        }
    }
    return NO_ITEM;
}

/* A key the menu acts on: 1; 0 for one that types a mnemonic. */
static int key_down(int vk) {
    menu *bar = menu_of(g_track.bar);
    menu *popup = menu_of(g_track.popup);
    switch (vk) {
    case VK_ESCAPE:
        if (g_track.popup_open && g_track.bar != NULL) {
            close_popup(1, 0);
        } else {
            g_track.end = 1;
        }
        break;
    case VK_LEFT:
    case VK_RIGHT:
        if (bar != 0) {
            const int open = g_track.popup_open;
            int to = next_item(bar, g_track.bar_item, vk == VK_RIGHT ? 1 : -1);
            g_track.bar_item = to == g_track.bar_item ? NO_ITEM : g_track.bar_item;
            switch_bar(to, 0, open);
        }
        break;
    case VK_DOWN:
    case VK_UP:
        if (!g_track.popup_open) {
            open_popup(1);
        } else if (popup != 0) {
            select_popup_item(next_item(popup, g_track.popup_item, vk == VK_DOWN ? 1 : -1), 0);
        }
        break;
    case VK_RETURN:
        if (!g_track.popup_open) {
            open_popup(1);
        } else {
            execute();
        }
        break;
    case VK_MENU:
    case VK_F10:
        g_track.end = 1;
        break;
    default:
        return 0;
    }
    return 1;
}

static void character(WCHAR c) {
    if (c == 0 || c == '\r' || c == '\x1b' || c == '\t') {
        return;
    }
    HMENU h = g_track.popup_open ? g_track.popup : g_track.bar;
    menu *m = menu_of(h);
    if (m == 0) {
        return;
    }
    int found = find_mnemonic(m, c, g_track.popup_open ? g_track.popup_item : g_track.bar_item);
    if (found == NO_ITEM) {
        const LRESULT answer = send(g_track.owner, WM_MENUCHAR,
                                    MAKELPARAM(c, g_track.popup_open ? MF_POPUP : 0), (LPARAM)h);
        if (HIWORD(answer) == MNC_CLOSE) {
            g_track.end = 1;
        } else if (HIWORD(answer) == MNC_EXECUTE || HIWORD(answer) == MNC_SELECT) {
            found = LOWORD(answer) < (WORD)m->count ? (int)LOWORD(answer) : NO_ITEM;
        }
        if (found == NO_ITEM) {
            return;
        }
    }
    if (g_track.popup_open) {
        select_popup_item(found, 0);
        execute();
    } else {
        switch_bar(found, 0, 1);
    }
}

/* Which bar or popup item a point of the owner's surface is on: 1 for the
 * bar, 2 for the popup, 0 for neither. */
static int item_at(int x, int y, int *index) {
    menu *popup = menu_of(g_track.popup);
    if (g_track.popup_open && popup != 0 && x >= g_track.x && y >= g_track.y && x < g_track.x + popup->width &&
        y < g_track.y + popup->height) {
        *index = NO_ITEM;
        for (int i = 0; i < popup->count; ++i) {
            const RECT *r = &popup->items[i].rect;
            if (x - g_track.x >= r->left && x - g_track.x < r->right && y - g_track.y >= r->top &&
                y - g_track.y < r->bottom) {
                *index = i;
            }
        }
        return 2;
    }
    menu *bar = menu_of(g_track.bar);
    if (bar != 0 && y >= 0 && y < g_track.top->menu_height) {
        for (int i = 0; i < bar->count; ++i) {
            if (x >= bar->items[i].rect.left && x < bar->items[i].rect.right) {
                *index = i;
                return 1;
            }
        }
    }
    return 0;
}

static void pointer(UINT message, LPARAM lParam) {
    const int x = GET_X_LPARAM(lParam);
    const int y = GET_Y_LPARAM(lParam) + g_track.top->menu_height;
    int index = NO_ITEM;
    const int on = item_at(x, y, &index);
    switch (message) {
    case WM_MOUSEMOVE:
        if (on == 1 && index != g_track.bar_item) {
            switch_bar(index, 1, 1);
        } else if (on == 2 && (g_track.popup_item != index || index == NO_ITEM)) {
            if (index != NO_ITEM) {
                select_popup_item(index, 1);
            }
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (on == 1 && index != g_track.bar_item) {
            switch_bar(index, 1, 1);
        } else if (on == 1 && !g_track.popup_open) {
            open_popup(0);
        } else if (on == 0) {
            g_track.end = 1;
        }
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (on == 2 && index != NO_ITEM) {
            select_popup_item(index, 1);
            execute();
        }
        break;
    default:
        break;
    }
}

/* SC_MOUSEMENU and SC_KEYMENU: the modal loop. by_mouse starts it on the
 * bar item under the pointer, key on the item whose mnemonic it is, and
 * neither on the first item of the bar. */
static void run_loop(window *top);

void menu_track(window *top, int by_mouse, WCHAR key) {
    menu *bar = menu_of(top->menu);
    if (bar == 0 || g_track.active || bar->count == 0) {
        return;
    }
    zero(&g_track, sizeof(g_track));
    g_track.active = 1;
    g_track.owner = top;
    g_track.top = top;
    g_track.bar = top->menu;
    g_track.bar_item = NO_ITEM;
    g_track.popup_item = NO_ITEM;
    g_track.show_accel = !by_mouse;
    window *old_capture = g_capture;
    g_capture = top;
    if (old_capture != 0 && old_capture != top) {
        send(old_capture, WM_CAPTURECHANGED, 0, (LPARAM)handle_of(top));
    }
    send(top, WM_ENTERMENULOOP, FALSE, 0);
    send(top, WM_INITMENU, (WPARAM)top->menu, 0);
    if (by_mouse) {
        int index = NO_ITEM;
        const int x = g_last_point.x - top->client.left;
        const int y = g_last_point.y - top->client.top + top->menu_height;
        if (item_at(x, y, &index) == 1) {
            select_bar(index, 1);
            open_popup(0);
        } else {
            g_track.end = 1;
        }
    } else if (key != 0 && key != ' ') {
        const int index = find_mnemonic(bar, key, NO_ITEM);
        if (index != NO_ITEM) {
            select_bar(index, 0);
            open_popup(1);
        } else {
            g_track.end = 1;
        }
    } else {
        select_bar(0, 0);
    }

    run_loop(top);

    close_popup(0, 0);
    if (g_capture == top) {
        g_capture = 0;
        send(top, WM_CAPTURECHANGED, 0, 0);
    }
    select_message(0, 0xffff, NULL);
    send(top, WM_EXITMENULOOP, FALSE, 0);
    const UINT command = g_track.command;
    g_track.active = 0;
    menu_draw_bar(top);
    if (command != 0) {
        PostMessageW(handle_of(top), WM_COMMAND, command, 0);
    }
}

/* The menu's modal loop: every message passes the WH_MSGFILTER hook with
 * MSGF_MENU, input goes to the menu, the rest is dispatched. */
static void run_loop(window *top) {
    int idle_sent = 0;
    while (!g_track.end) {
        MSG m;
        if (!next_message(&m, NULL, 0, 0, 1, 0)) {
            if (!idle_sent) {
                idle_sent = 1;
                send(g_track.owner, WM_ENTERIDLE, MSGF_MENU, 0);
                continue;
            }
            next_message(&m, NULL, 0, 0, 1, 1);
        }
        idle_sent = 0;
        if (CallMsgFilterW(&m, MSGF_MENU)) {
            continue;
        }
        if (m.message == WM_QUIT) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        if (!window_of(handle_of(top)) || top->destroying) {
            break;
        }
        if (m.message >= WM_MOUSEMOVE && m.message <= WM_MBUTTONDBLCLK && m.hwnd == handle_of(top)) {
            pointer(m.message, m.lParam);
        } else if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            if (!key_down((int)m.wParam)) {
                TranslateMessage(&m);
            }
        } else if (m.message == WM_CHAR || m.message == WM_SYSCHAR) {
            character((WCHAR)m.wParam);
        } else if (m.message == WM_KEYUP || m.message == WM_SYSKEYUP ||
                   (m.message >= WM_MOUSEMOVE && m.message <= WM_MBUTTONDBLCLK)) {
            /* The menu's: nothing else sees it. */
        } else {
            DispatchMessageW(&m);
        }
    }
}

/* A popup menu at a screen point, tracked until an item is chosen or it
 * is dismissed. hWnd hears the menu's messages; the chosen command is
 * posted to it as WM_COMMAND, or returned with TPM_RETURNCMD. Popups of
 * popups are not opened here. */
BOOL WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const RECT *prcRect) {
    (void)nReserved;
    (void)prcRect;
    window *owner = checked(hWnd);
    menu *m = menu_of(hMenu);
    if (owner == 0 || m == 0) {
        if (m == 0) {
            set_last_error(ERROR_INVALID_MENU_HANDLE);
        }
        return FALSE;
    }
    window *top = top_of(owner);
    if (g_track.active || top->host == 0) {
        return FALSE;
    }
    zero(&g_track, sizeof(g_track));
    g_track.active = 1;
    g_track.owner = owner;
    g_track.top = top;
    g_track.bar = NULL;
    g_track.bar_item = NO_ITEM;
    g_track.popup_item = NO_ITEM;
    g_track.show_accel = g_keyboard_last;
    window *old_capture = g_capture;
    g_capture = top;
    if (old_capture != 0 && old_capture != top) {
        send(old_capture, WM_CAPTURECHANGED, 0, (LPARAM)handle_of(top));
    }
    send(owner, WM_ENTERMENULOOP, TRUE, 0);
    if ((uFlags & TPM_NONOTIFY) == 0) {
        send(owner, WM_INITMENUPOPUP, (WPARAM)hMenu, 0);
    }
    g_track.popup = hMenu;
    g_track.popup_open = 1;
    /* The screen point on the top-level window's surface. */
    const int sx = x - top->client.left;
    const int sy = y - top->client.top + top->menu_height;
    show_popup(hMenu, sx, sy, uFlags, 0);
    run_loop(top);
    close_popup(0, 0);
    if (g_capture == top) {
        g_capture = 0;
        send(top, WM_CAPTURECHANGED, 0, 0);
    }
    select_message(0, 0xffff, NULL);
    send(owner, WM_EXITMENULOOP, TRUE, 0);
    const UINT command = g_track.command;
    g_track.active = 0;
    if ((uFlags & TPM_RETURNCMD) != 0) {
        return (BOOL)command;
    }
    if (command != 0 && (uFlags & TPM_NONOTIFY) == 0) {
        PostMessageW(hWnd, WM_COMMAND, command, 0);
    }
    return TRUE;
}

/* Pointer input on the popup's host window, as the owner's: 1 when host is
 * the popup, with *x and *y moved to the owner's surface. */
int menu_host_pointer(u32 host, window **owner, int *x, int *y) {
    if (!g_track.active || host == 0 || host != g_track.host) {
        return 0;
    }
    *owner = g_track.top;
    *x += g_track.x;
    *y += g_track.y;
    return 1;
}

/* A press outside the program's windows closed the popup. */
int menu_host_close(u32 host) {
    if (!g_track.active || host == 0 || host != g_track.host) {
        return 0;
    }
    g_track.end = 1;
    return 1;
}
