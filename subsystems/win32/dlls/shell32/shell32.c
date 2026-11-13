/*
 * shell32.dll -- ShellAboutW, the one function of the W0 matrix: Solitaire's
 * Help > About. Windows shows its own About box with the program's name; this
 * one says the program runs on SavanXP's Win32 subsystem. It is a dialog
 * built in memory and run by user32 (SxwDialogBoxIndirectParamW).
 */
#define SXW_BUILDING_SHELL32
#include <windows.h>

typedef unsigned int u32;

__declspec(dllimport) INT_PTR WINAPI SxwDialogBoxIndirectParamW(HINSTANCE hInstance, const void *lpTemplate, u32 size,
                                                                HWND hWndParent, DLGPROC lpDialogFunc,
                                                                LPARAM dwInitParam);

#define TEMPLATE_SIZE 2048
#define TEXT_SIZE 256

static unsigned char g_template[TEMPLATE_SIZE];
static u32 g_size;

static void put16(u32 v) {
    if (g_size + 2 <= TEMPLATE_SIZE) {
        g_template[g_size++] = (unsigned char)v;
        g_template[g_size++] = (unsigned char)(v >> 8);
    }
}

static void put32(u32 v) {
    put16(v & 0xffffu);
    put16(v >> 16);
}

static void put_text(const WCHAR *text, int length) {
    for (int i = 0; text != NULL && (length < 0 ? text[i] != 0 : i < length); ++i) {
        put16(text[i]);
    }
    put16(0);
}

static void put_item(DWORD style, int x, int y, int cx, int cy, UINT id, u32 class_ordinal, const WCHAR *text,
                     int length) {
    while ((g_size & 3u) != 0) {
        put16(0);
    }
    put32(style | WS_CHILD | WS_VISIBLE);
    put32(0);
    put16((u32)x);
    put16((u32)y);
    put16((u32)cx);
    put16((u32)cy);
    put16(id);
    put16(0xffff);
    put16(class_ordinal);
    put_text(text, length);
    put16(0);
}

static INT_PTR WINAPI about_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (message == WM_INITDIALOG) {
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        EndDialog(hwnd, IDOK);
        return TRUE;
    }
    return FALSE;
}

/* szApp is "title#first line", or one text for both. The icon is not
 * drawn. TRUE once the box is closed. */
int WINAPI ShellAboutW(HWND hWnd, LPCWSTR szApp, LPCWSTR szOtherStuff, HICON hIcon) {
    static const WCHAR k_about[] = {'A', 'b', 'o', 'u', 't', ' ', 0};
    static const WCHAR k_face[] = {'M', 'S', ' ', 'S', 'h', 'e', 'l', 'l', ' ', 'D', 'l', 'g', 0};
    static const WCHAR k_ok[] = {'O', 'K', 0};
    static const WCHAR k_system[] = {'R', 'u', 'n', 'n', 'i', 'n', 'g', ' ', 'o', 'n', ' ', 'S', 'a', 'v', 'a',
                                     'n', 'X', 'P', ',', ' ', 'u', 'n', 'd', 'e', 'r', ' ', 'S', 'S', 'W', ',',
                                     ' ', 't', 'h', 'e', ' ', 'W', 'i', 'n', '3', '2', ' ', 's', 'u', 'b', 's',
                                     'y', 's', 't', 'e', 'm', '.', 0};
    (void)hIcon;
    if (szApp == NULL) {
        return FALSE;
    }
    int hash = 0;
    while (szApp[hash] != 0 && szApp[hash] != '#') {
        ++hash;
    }
    const WCHAR *first = szApp[hash] == '#' ? szApp + hash + 1 : szApp;
    WCHAR caption[TEXT_SIZE];
    int n = 0;
    for (int i = 0; k_about[i] != 0 && n + 1 < TEXT_SIZE; ++i) {
        caption[n++] = k_about[i];
    }
    for (int i = 0; i < hash && n + 1 < TEXT_SIZE; ++i) {
        caption[n++] = szApp[i];
    }
    caption[n] = 0;

    g_size = 0;
    put32(DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    put32(0);
    put16(4);
    put16(0);
    put16(0);
    put16(220);
    put16(94);
    put16(0);
    put16(0);
    put_text(caption, -1);
    put16(8);
    put_text(k_face, -1);
    put_item(SS_LEFT | SS_NOPREFIX, 10, 10, 200, 10, 0xffff, 0x82, first, -1);
    put_item(SS_LEFT | SS_NOPREFIX, 10, 26, 200, 10, 0xffff, 0x82, k_system, -1);
    put_item(SS_LEFT | SS_NOPREFIX, 10, 42, 200, 24, 0xffff, 0x82, szOtherStuff != NULL ? szOtherStuff : k_about + 6,
             -1);
    put_item(BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 160, 72, 50, 14, IDOK, 0x80, k_ok, -1);
    return SxwDialogBoxIndirectParamW(NULL, g_template, g_size, hWnd, about_proc, 0) > 0;
}
