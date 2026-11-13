/*
 * windows.h -- The subsystem's own Windows header: the types, constants and
 * declarations the DLLs and fixtures under subsystems/win32/ use, and
 * nothing else. It grows with the W0 matrix; it is not the Windows SDK.
 */
#ifndef SAVANXP_WIN32_WINDOWS_H
#define SAVANXP_WIN32_WINDOWS_H

#define WINAPI __stdcall
#define DECLSPEC_IMPORT __declspec(dllimport)
#define DECLSPEC_EXPORT __declspec(dllexport)

typedef int BOOL;
typedef long LONG;
typedef unsigned int UINT;
typedef unsigned long ULONG_PTR;
typedef ULONG_PTR SIZE_T;
typedef char CHAR;
typedef unsigned short WCHAR;
typedef CHAR *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef void *HINSTANCE;
typedef void *HMODULE;
typedef void *HLOCAL;
typedef struct HKEY__ *HKEY;
typedef HKEY *PHKEY;
typedef DWORD REGSAM;
typedef int(WINAPI *FARPROC)(void);
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef DWORD *LPDWORD;
typedef struct _OVERLAPPED OVERLAPPED, *LPOVERLAPPED;
typedef struct _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef unsigned int WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef WORD ATOM;
typedef DWORD COLORREF;
typedef struct HWND__ *HWND;
typedef struct HDC__ *HDC;
typedef struct HMENU__ *HMENU;
typedef struct HICON__ *HICON;
typedef HICON HCURSOR;
typedef void *HGDIOBJ;
typedef struct HBRUSH__ *HBRUSH;
typedef struct HPEN__ *HPEN;
typedef struct HFONT__ *HFONT;
typedef struct HBITMAP__ *HBITMAP;
typedef struct HACCEL__ *HACCEL;
typedef unsigned long UINT_PTR;
typedef long INT_PTR;
typedef LRESULT(WINAPI *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void(WINAPI *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef void(WINAPI *LINEDDAPROC)(int, int, LPARAM);
typedef INT_PTR(WINAPI *DLGPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *LPRECT;
typedef const RECT *LPCRECT;

typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *LPPOINT;

typedef struct tagSIZE {
    LONG cx;
    LONG cy;
} SIZE, *LPSIZE;

typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;

typedef struct tagWNDCLASSW {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
} WNDCLASSW;

typedef struct tagWNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXW;

typedef struct tagCREATESTRUCTW {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy;
    int cx;
    int y;
    int x;
    LONG style;
    LPCWSTR lpszName;
    LPCWSTR lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTW;

typedef struct tagWINDOWPOS {
    HWND hwnd;
    HWND hwndInsertAfter;
    int x;
    int y;
    int cx;
    int cy;
    UINT flags;
} WINDOWPOS;

typedef struct tagMINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
} MINMAXINFO;

typedef struct tagDRAWITEMSTRUCT {
    UINT CtlType;
    UINT CtlID;
    UINT itemID;
    UINT itemAction;
    UINT itemState;
    HWND hwndItem;
    HDC hDC;
    RECT rcItem;
    ULONG_PTR itemData;
} DRAWITEMSTRUCT, *LPDRAWITEMSTRUCT;

typedef struct tagINITCOMMONCONTROLSEX {
    DWORD dwSize;
    DWORD dwICC;
} INITCOMMONCONTROLSEX, *LPINITCOMMONCONTROLSEX;

typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct tagTEXTMETRICW {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    WCHAR tmFirstChar;
    WCHAR tmLastChar;
    WCHAR tmDefaultChar;
    WCHAR tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} TEXTMETRICW, *LPTEXTMETRICW;

typedef struct tagBITMAP {
    LONG bmType;
    LONG bmWidth;
    LONG bmHeight;
    LONG bmWidthBytes;
    WORD bmPlanes;
    WORD bmBitsPixel;
    LPVOID bmBits;
} BITMAP, *LPBITMAP;

typedef struct tagACCEL {
    BYTE fVirt;
    WORD key;
    WORD cmd;
} ACCEL;

typedef void(WINAPI *PIMAGE_TLS_CALLBACK)(LPVOID DllHandle, DWORD Reason, LPVOID Reserved);

typedef struct _IMAGE_TLS_DIRECTORY32 {
    DWORD StartAddressOfRawData;
    DWORD EndAddressOfRawData;
    DWORD AddressOfIndex;
    DWORD AddressOfCallBacks;
    DWORD SizeOfZeroFill;
    DWORD Characteristics;
} IMAGE_TLS_DIRECTORY32;

#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_MORE_DATA 234L

#define LMEM_FIXED 0x0000u
#define LMEM_MOVEABLE 0x0002u
#define LMEM_NOCOMPACT 0x0010u
#define LMEM_NODISCARD 0x0020u
#define LMEM_ZEROINIT 0x0040u
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)

#define STARTF_USESHOWWINDOW 0x00000001u
#define SW_SHOWDEFAULT 10

#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)0x80000000u)
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)0x80000001u)
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)0x80000002u)
#define REG_NONE 0u
#define REG_SZ 1u
#define REG_EXPAND_SZ 2u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define REG_MULTI_SZ 7u
#define REG_OPTION_NON_VOLATILE 0u
#define REG_CREATED_NEW_KEY 1u
#define REG_OPENED_EXISTING_KEY 2u
#define KEY_QUERY_VALUE 0x0001u
#define KEY_SET_VALUE 0x0002u
#define KEY_READ 0x20019u
#define KEY_ALL_ACCESS 0xf003fu

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1

/* ---- user32 and gdi32 constants ------------------------------------------ */

#define MAKEINTRESOURCEW(i) ((LPWSTR)(ULONG_PTR)(WORD)(i))
#define LOWORD(l) ((WORD)((DWORD)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD)(l) >> 16))
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)((WORD)(l) | ((DWORD)(WORD)(h) << 16)))
#define RGB(r, g, b) ((COLORREF)((BYTE)(r) | ((WORD)(BYTE)(g) << 8) | ((DWORD)(BYTE)(b) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000a
#define WM_SETTEXT 0x000c
#define WM_GETTEXT 0x000d
#define WM_GETTEXTLENGTH 0x000e
#define WM_PAINT 0x000f
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001c
#define WM_CANCELMODE 0x001f
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_GETICON 0x007f
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_NCMOUSEMOVE 0x00a0
#define WM_NCLBUTTONDOWN 0x00a1
#define WM_NCLBUTTONUP 0x00a2
#define WM_NCLBUTTONDBLCLK 0x00a3
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_PARENTNOTIFY 0x0210
#define WM_CAPTURECHANGED 0x0215
#define WM_NEXTDLGCTL 0x0028
#define WM_DRAWITEM 0x002b
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_HELP 0x0053
#define WM_NOTIFYFORMAT 0x0055
#define WM_CONTEXTMENU 0x007b
#define WM_GETDLGCODE 0x0087
#define WM_INITDIALOG 0x0110
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MENUSELECT 0x011f
#define WM_MENUCHAR 0x0120
#define WM_ENTERIDLE 0x0121
#define WM_UNINITMENUPOPUP 0x0125
#define WM_CHANGEUISTATE 0x0127
#define WM_UPDATEUISTATE 0x0128
#define WM_QUERYUISTATE 0x0129
#define WM_CTLCOLORMSGBOX 0x0132
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC 0x0138
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_QUERYNEWPALETTE 0x030f
#define BM_GETCHECK 0x00f0
#define BM_SETCHECK 0x00f1
#define BM_GETSTATE 0x00f2
#define BM_SETSTATE 0x00f3
#define BM_SETSTYLE 0x00f4
#define BM_CLICK 0x00f5
#define DM_GETDEFID 0x0400
#define DM_SETDEFID 0x0401
#define WM_USER 0x0400

#define WS_OVERLAPPED 0x00000000L
#define WS_TABSTOP 0x00010000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_GROUP 0x00020000L
#define WS_THICKFRAME 0x00040000L
#define WS_SYSMENU 0x00080000L
#define WS_HSCROLL 0x00100000L
#define WS_VSCROLL 0x00200000L
#define WS_DLGFRAME 0x00400000L
#define WS_BORDER 0x00800000L
#define WS_CAPTION 0x00c00000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_DISABLED 0x08000000L
#define WS_VISIBLE 0x10000000L
#define WS_MINIMIZE 0x20000000L
#define WS_CHILD 0x40000000L
#define WS_POPUP 0x80000000L
#define WS_OVERLAPPEDWINDOW \
    (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_DLGMODALFRAME 0x00000001L
#define WS_EX_NOPARENTNOTIFY 0x00000004L
#define WS_EX_CONTEXTHELP 0x00000400L
#define WS_EX_CONTROLPARENT 0x00010000L
#define CW_USEDEFAULT ((int)0x80000000)

#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_CLASSDC 0x0040
#define CS_PARENTDC 0x0080
#define CS_SAVEBITS 0x0800

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9

#define SC_CLOSE 0xf060
#define SC_KEYMENU 0xf100

#define BS_PUSHBUTTON 0x0000L
#define BS_DEFPUSHBUTTON 0x0001L
#define BS_CHECKBOX 0x0002L
#define BS_AUTOCHECKBOX 0x0003L
#define BS_RADIOBUTTON 0x0004L
#define BS_3STATE 0x0005L
#define BS_AUTO3STATE 0x0006L
#define BS_GROUPBOX 0x0007L
#define BS_USERBUTTON 0x0008L
#define BS_AUTORADIOBUTTON 0x0009L
#define BS_OWNERDRAW 0x000bL
#define BS_TYPEMASK 0x000fL
#define BS_LEFTTEXT 0x0020L
#define BN_CLICKED 0
#define BN_DOUBLECLICKED 5
#define BST_UNCHECKED 0x0000
#define BST_CHECKED 0x0001
#define BST_INDETERMINATE 0x0002
#define BST_PUSHED 0x0004
#define BST_FOCUS 0x0008
#define SS_LEFT 0x0000L
#define SS_CENTER 0x0001L
#define SS_RIGHT 0x0002L
#define SS_TYPEMASK 0x001fL
#define SS_NOPREFIX 0x0080L
#define DS_MODALFRAME 0x0080L
#define DS_SETFONT 0x0040L
#define DS_CONTEXTHELP 0x2000L
#define DS_CENTER 0x0800L
#define DS_NOIDLEMSG 0x0100L
#define DS_CONTROL 0x0400L
#define DS_3DLOOK 0x0004L
#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTTAB 0x0002
#define DLGC_WANTALLKEYS 0x0004
#define DLGC_WANTMESSAGE 0x0004
#define DLGC_HASSETSEL 0x0008
#define DLGC_DEFPUSHBUTTON 0x0010
#define DLGC_UNDEFPUSHBUTTON 0x0020
#define DLGC_RADIOBUTTON 0x0040
#define DLGC_WANTCHARS 0x0080
#define DLGC_STATIC 0x0100
#define DLGC_BUTTON 0x2000
#define DC_HASDEFID 0x534b
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_DEFBUTTON1 0x00000000L
#define MB_DEFBUTTON2 0x00000100L
#define MB_DEFBUTTON3 0x00000200L
#define MB_APPLMODAL 0x00000000L
#define MB_TYPEMASK 0x0000000fL
#define MB_ICONMASK 0x000000f0L
#define MB_DEFMASK 0x00000f00L
#define MF_BYCOMMAND 0x0000L
#define MF_ENABLED 0x0000L
#define MF_GRAYED 0x0001L
#define MF_DISABLED 0x0002L
#define MF_CHECKED 0x0008L
#define MF_POPUP 0x0010L
#define MF_MENUBARBREAK 0x0020L
#define MF_MENUBREAK 0x0040L
#define MF_HILITE 0x0080L
#define MF_OWNERDRAW 0x0100L
#define MF_BYPOSITION 0x0400L
#define MF_SEPARATOR 0x0800L
#define MF_SYSMENU 0x2000L
#define MF_MOUSESELECT 0x8000L
#define MF_END 0x0080L
#define ODT_MENU 1
#define ODT_BUTTON 4
#define ODT_STATIC 5
#define ODA_DRAWENTIRE 0x0001
#define ODA_SELECT 0x0002
#define ODA_FOCUS 0x0004
#define ODS_SELECTED 0x0001
#define ODS_GRAYED 0x0002
#define ODS_DISABLED 0x0004
#define ODS_CHECKED 0x0008
#define ODS_FOCUS 0x0010
#define ODS_DEFAULT 0x0020
#define ODS_NOACCEL 0x0100
#define ODS_NOFOCUSRECT 0x0200
#define UIS_SET 1
#define UIS_CLEAR 2
#define UIS_INITIALIZE 3
#define UISF_HIDEFOCUS 0x1
#define UISF_HIDEACCEL 0x2
#define HELP_CONTEXT 0x0001L
#define HELP_QUIT 0x0002L
#define HELP_INDEX 0x0003L
#define HELP_FINDER 0x000bL
#define SC_MOUSEMENU 0xf090
#define SC_DEFAULT 0xf160
#define SC_CONTEXTHELP 0xf180

#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2

#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2

#define HTCLIENT 1
#define MA_ACTIVATE 1

#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010

#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define QS_ALLINPUT 0x04ff
#define WAIT_TIMEOUT 258L

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0d
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1b
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F4 0x73
#define VK_F10 0x79

#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define IDC_IBEAM MAKEINTRESOURCEW(32513)
#define IDC_WAIT MAKEINTRESOURCEW(32514)
#define IDC_CROSS MAKEINTRESOURCEW(32515)

#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_MENU 4
#define COLOR_WINDOW 5
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_BTNFACE 15
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_BTNSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_BTNTEXT 18
#define COLOR_BTNHIGHLIGHT 20
#define COLOR_3DDKSHADOW 21
#define COLOR_3DLIGHT 22
#define COLOR_3DFACE COLOR_BTNFACE
#define COLOR_3DSHADOW COLOR_BTNSHADOW
#define COLOR_3DHILIGHT COLOR_BTNHIGHLIGHT
#define CLR_INVALID 0xffffffffu

#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define SYSTEM_FONT 13
#define DEFAULT_GUI_FONT 17

#define SRCCOPY 0x00cc0020u
#define PATCOPY 0x00f00021u
#define PATINVERT 0x005a0049u
#define DSTINVERT 0x00550009u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00ff0062u

#define R2_BLACK 1
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOP 11
#define R2_COPYPEN 13
#define R2_WHITE 16

#define OPAQUE 2
#define TRANSPARENT 1

#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define PLANES 14
#define NUMCOLORS 24
#define LOGPIXELSX 88
#define LOGPIXELSY 90

#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define ANSI_CHARSET 0

#define DT_TOP 0x0000
#define DT_LEFT 0x0000
#define DT_CENTER 0x0001
#define DT_RIGHT 0x0002
#define DT_VCENTER 0x0004
#define DT_BOTTOM 0x0008
#define DT_WORDBREAK 0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_NOCLIP 0x0100
#define DT_CALCRECT 0x0400
#define DT_NOPREFIX 0x0800

#define ICC_WIN95_CLASSES 0x000000ff
#define ICC_STANDARD_CLASSES 0x00004000

#define MAKEINTRESOURCEA(i) ((LPSTR)(ULONG_PTR)(WORD)(i))
#define IDI_APPLICATION MAKEINTRESOURCEW(32512)
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define LR_DEFAULTCOLOR 0x0000
#define LR_DEFAULTSIZE 0x0040
#define LR_SHARED 0x8000
#define FVIRTKEY 0x01
#define FSHIFT 0x04
#define FCONTROL 0x08
#define FALT 0x10

#ifndef SXW_BUILDING_KERNEL32
#define SXW_KERNEL32_API DECLSPEC_IMPORT
#else
#define SXW_KERNEL32_API
#endif

SXW_KERNEL32_API HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
SXW_KERNEL32_API LPWSTR WINAPI GetCommandLineW(void);
SXW_KERNEL32_API void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
SXW_KERNEL32_API HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
SXW_KERNEL32_API HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
SXW_KERNEL32_API FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
SXW_KERNEL32_API HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes);
SXW_KERNEL32_API HLOCAL WINAPI LocalFree(HLOCAL hMem);
SXW_KERNEL32_API int WINAPI lstrlenW(LPCWSTR lpString);
SXW_KERNEL32_API int WINAPI MulDiv(int nNumber, int nNumerator, int nDenominator);
SXW_KERNEL32_API BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                                       LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
SXW_KERNEL32_API __declspec(noreturn) void WINAPI ExitProcess(unsigned int uExitCode);


#ifndef SXW_BUILDING_ADVAPI32
#define SXW_ADVAPI32_API DECLSPEC_IMPORT
#else
#define SXW_ADVAPI32_API
#endif

SXW_ADVAPI32_API LONG WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass,
                                             DWORD dwOptions, REGSAM samDesired,
                                             LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                                             LPDWORD lpdwDisposition);
SXW_ADVAPI32_API LONG WINAPI RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired,
                                           PHKEY phkResult);
SXW_ADVAPI32_API LONG WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
                                              LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
SXW_ADVAPI32_API LONG WINAPI RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
                                              LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
SXW_ADVAPI32_API LONG WINAPI RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType,
                                            const BYTE *lpData, DWORD cbData);
SXW_ADVAPI32_API LONG WINAPI RegCloseKey(HKEY hKey);


#ifndef SXW_BUILDING_USER32
#define SXW_USER32_API DECLSPEC_IMPORT
#else
#define SXW_USER32_API
#endif

SXW_USER32_API ATOM WINAPI RegisterClassW(const WNDCLASSW *lpWndClass);
SXW_USER32_API ATOM WINAPI RegisterClassExW(const WNDCLASSEXW *lpWndClass);
SXW_USER32_API HWND WINAPI CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
                                           DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                                           HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
SXW_USER32_API BOOL WINAPI DestroyWindow(HWND hWnd);
SXW_USER32_API BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
SXW_USER32_API BOOL WINAPI UpdateWindow(HWND hWnd);
SXW_USER32_API BOOL WINAPI MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
SXW_USER32_API BOOL WINAPI EnableWindow(HWND hWnd, BOOL bEnable);
SXW_USER32_API HWND WINAPI SetFocus(HWND hWnd);
SXW_USER32_API BOOL WINAPI IsIconic(HWND hWnd);
SXW_USER32_API BOOL WINAPI GetClientRect(HWND hWnd, LPRECT lpRect);
SXW_USER32_API HWND WINAPI GetDesktopWindow(void);
SXW_USER32_API BOOL WINAPI AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu);
SXW_USER32_API BOOL WINAPI ClientToScreen(HWND hWnd, LPPOINT lpPoint);
SXW_USER32_API LRESULT WINAPI DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
SXW_USER32_API BOOL WINAPI GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
SXW_USER32_API BOOL WINAPI PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax,
                                        UINT wRemoveMsg);
SXW_USER32_API LRESULT WINAPI DispatchMessageW(const MSG *lpMsg);
SXW_USER32_API BOOL WINAPI TranslateMessage(const MSG *lpMsg);
SXW_USER32_API BOOL WINAPI PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
SXW_USER32_API void WINAPI PostQuitMessage(int nExitCode);
SXW_USER32_API DWORD WINAPI MsgWaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles, BOOL fWaitAll,
                                                      DWORD dwMilliseconds, DWORD dwWakeMask);
SXW_USER32_API UINT WINAPI RegisterWindowMessageW(LPCWSTR lpString);
SXW_USER32_API HDC WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);
SXW_USER32_API BOOL WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint);
SXW_USER32_API HDC WINAPI GetDC(HWND hWnd);
SXW_USER32_API int WINAPI ReleaseDC(HWND hWnd, HDC hDC);
SXW_USER32_API BOOL WINAPI InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase);
SXW_USER32_API int WINAPI DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);
SXW_USER32_API int WINAPI FrameRect(HDC hDC, const RECT *lprc, HBRUSH hbr);
SXW_USER32_API BOOL WINAPI InvertRect(HDC hDC, const RECT *lprc);
SXW_USER32_API DWORD WINAPI GetSysColor(int nIndex);
SXW_USER32_API HWND WINAPI SetCapture(HWND hWnd);
SXW_USER32_API BOOL WINAPI ReleaseCapture(void);
SXW_USER32_API HWND WINAPI GetCapture(void);
SXW_USER32_API short WINAPI GetKeyState(int nVirtKey);
SXW_USER32_API BOOL WINAPI SetCursorPos(int X, int Y);
SXW_USER32_API int WINAPI ShowCursor(BOOL bShow);
SXW_USER32_API UINT_PTR WINAPI SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc);
SXW_USER32_API BOOL WINAPI KillTimer(HWND hWnd, UINT_PTR uIDEvent);
SXW_USER32_API BOOL WINAPI CopyRect(LPRECT lprcDst, const RECT *lprcSrc);
SXW_USER32_API BOOL WINAPI InflateRect(LPRECT lprc, int dx, int dy);
SXW_USER32_API BOOL WINAPI IntersectRect(LPRECT lprcDst, const RECT *lprcSrc1, const RECT *lprcSrc2);
SXW_USER32_API BOOL WINAPI PtInRect(const RECT *lprc, POINT pt);
SXW_USER32_API HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName);
SXW_USER32_API HBITMAP WINAPI LoadBitmapA(HINSTANCE hInstance, LPCSTR lpBitmapName);
SXW_USER32_API int WINAPI LoadStringW(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax);
SXW_USER32_API int WINAPI LoadStringA(HINSTANCE hInstance, UINT uID, LPSTR lpBuffer, int cchBufferMax);
SXW_USER32_API HICON WINAPI LoadIconW(HINSTANCE hInstance, LPCWSTR lpIconName);
SXW_USER32_API HANDLE WINAPI LoadImageW(HINSTANCE hInst, LPCWSTR name, UINT type, int cx, int cy, UINT fuLoad);
SXW_USER32_API HACCEL WINAPI LoadAcceleratorsW(HINSTANCE hInstance, LPCWSTR lpTableName);
SXW_USER32_API int WINAPI TranslateAcceleratorW(HWND hWnd, HACCEL hAccTable, LPMSG lpMsg);
SXW_USER32_API HMENU WINAPI GetMenu(HWND hWnd);
SXW_USER32_API BOOL WINAPI EnableMenuItem(HMENU hMenu, UINT uIDEnableItem, UINT uEnable);
SXW_USER32_API INT_PTR WINAPI DialogBoxParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent,
                                              DLGPROC lpDialogFunc, LPARAM dwInitParam);
SXW_USER32_API BOOL WINAPI EndDialog(HWND hDlg, INT_PTR nResult);
SXW_USER32_API HWND WINAPI GetDlgItem(HWND hDlg, int nIDDlgItem);
SXW_USER32_API BOOL WINAPI CheckDlgButton(HWND hDlg, int nIDButton, UINT uCheck);
SXW_USER32_API UINT WINAPI IsDlgButtonChecked(HWND hDlg, int nIDButton);
SXW_USER32_API BOOL WINAPI CheckRadioButton(HWND hDlg, int nIDFirstButton, int nIDLastButton, int nIDCheckButton);
SXW_USER32_API int WINAPI MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType);
SXW_USER32_API BOOL WINAPI WinHelpW(HWND hWndMain, LPCWSTR lpszHelp, UINT uCommand, ULONG_PTR dwData);

#ifndef SXW_BUILDING_SHELL32
#define SXW_SHELL32_API DECLSPEC_IMPORT
#else
#define SXW_SHELL32_API
#endif

SXW_SHELL32_API int WINAPI ShellAboutW(HWND hWnd, LPCWSTR szApp, LPCWSTR szOtherStuff, HICON hIcon);

#ifndef SXW_BUILDING_COMCTL32
#define SXW_COMCTL32_API DECLSPEC_IMPORT
#else
#define SXW_COMCTL32_API
#endif

SXW_COMCTL32_API BOOL WINAPI InitCommonControlsEx(const INITCOMMONCONTROLSEX *picce);

#ifndef SXW_BUILDING_GDI32
#define SXW_GDI32_API DECLSPEC_IMPORT
#else
#define SXW_GDI32_API
#endif

SXW_GDI32_API HGDIOBJ WINAPI GetStockObject(int i);
SXW_GDI32_API HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h);
SXW_GDI32_API BOOL WINAPI DeleteObject(HGDIOBJ ho);
SXW_GDI32_API HBRUSH WINAPI CreateSolidBrush(COLORREF color);
SXW_GDI32_API BOOL WINAPI SetBrushOrgEx(HDC hdc, int x, int y, LPPOINT lppt);
SXW_GDI32_API BOOL WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop);
SXW_GDI32_API int WINAPI SetROP2(HDC hdc, int rop2);
SXW_GDI32_API BOOL WINAPI MoveToEx(HDC hdc, int x, int y, LPPOINT lppt);
SXW_GDI32_API BOOL WINAPI LineTo(HDC hdc, int x, int y);
SXW_GDI32_API BOOL WINAPI LineDDA(int xStart, int yStart, int xEnd, int yEnd, LINEDDAPROC lpProc, LPARAM data);
SXW_GDI32_API COLORREF WINAPI SetPixel(HDC hdc, int x, int y, COLORREF color);
SXW_GDI32_API COLORREF WINAPI GetPixel(HDC hdc, int x, int y);
SXW_GDI32_API BOOL WINAPI PtVisible(HDC hdc, int x, int y);
SXW_GDI32_API int WINAPI GetDeviceCaps(HDC hdc, int index);
SXW_GDI32_API HFONT WINAPI CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
                                       DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
                                       DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality,
                                       DWORD iPitchAndFamily, LPCWSTR pszFaceName);
SXW_GDI32_API BOOL WINAPI TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c);
SXW_GDI32_API COLORREF WINAPI SetTextColor(HDC hdc, COLORREF color);
SXW_GDI32_API COLORREF WINAPI SetBkColor(HDC hdc, COLORREF color);
SXW_GDI32_API BOOL WINAPI GetTextExtentPoint32W(HDC hdc, LPCWSTR lpString, int c, LPSIZE psizl);
SXW_GDI32_API BOOL WINAPI GetTextMetricsW(HDC hdc, LPTEXTMETRICW lptm);
SXW_GDI32_API HDC WINAPI CreateCompatibleDC(HDC hdc);
SXW_GDI32_API BOOL WINAPI DeleteDC(HDC hdc);
SXW_GDI32_API HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int cx, int cy);
SXW_GDI32_API BOOL WINAPI BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);
SXW_GDI32_API BOOL WINAPI StretchBlt(HDC hdcDest, int xDest, int yDest, int wDest, int hDest, HDC hdcSrc, int xSrc,
                                     int ySrc, int wSrc, int hSrc, DWORD rop);
SXW_GDI32_API int WINAPI GetObjectA(HANDLE h, int c, LPVOID pv);
SXW_GDI32_API BOOL WINAPI GetDCOrgEx(HDC hdc, LPPOINT lppt);

#endif
