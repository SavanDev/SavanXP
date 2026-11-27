/*
 * windows.h -- The subsystem's own Windows header: the types, constants and
 * declarations the DLLs and fixtures under subsystems/win32/ use, and
 * nothing else. It grows with the W0 matrix; it is not the Windows SDK.
 */
#ifndef SAVANXP_WIN32_WINDOWS_H
#define SAVANXP_WIN32_WINDOWS_H

#define WINAPI __stdcall
#define CALLBACK __stdcall
#define DECLSPEC_IMPORT __declspec(dllimport)
#define DECLSPEC_EXPORT __declspec(dllexport)

typedef int BOOL;
typedef long LONG;
typedef unsigned int UINT;
typedef int INT;
typedef int *LPINT;
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
typedef short SHORT;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef HANDLE *LPHANDLE;
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

#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_NONCONTINUABLE 0x1u
#define EXCEPTION_UNWINDING 0x2u
#define EXCEPTION_EXIT_UNWIND 0x4u
#define EXCEPTION_STACK_INVALID 0x8u
#define EXCEPTION_NESTED_CALL 0x10u
#define EXCEPTION_NONCONTINUABLE_EXCEPTION 0xc0000025u
#define EXCEPTION_INVALID_DISPOSITION 0xc0000026u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;

typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution,
    ExceptionContinueSearch,
    ExceptionNestedException,
    ExceptionCollidedUnwind,
} EXCEPTION_DISPOSITION;

#define CONTEXT_CONTROL 0x10001u
#define CONTEXT_INTEGER 0x10002u

typedef struct _FLOATING_SAVE_AREA {
    DWORD ControlWord, StatusWord, TagWord;
    DWORD ErrorOffset, ErrorSelector, DataOffset, DataSelector;
    BYTE RegisterArea[80];
    DWORD Cr0NpxState;
} FLOATING_SAVE_AREA;

typedef struct _CONTEXT {
    DWORD ContextFlags;
    DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    FLOATING_SAVE_AREA FloatSave;
    DWORD SegGs, SegFs, SegEs, SegDs;
    DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;
    DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;
    BYTE ExtendedRegisters[512];
} CONTEXT;

typedef struct _EXCEPTION_POINTERS {
    EXCEPTION_RECORD *ExceptionRecord;
    CONTEXT *ContextRecord;
} EXCEPTION_POINTERS;

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
typedef void *HGLOBAL;
typedef void *HRSRC;
typedef DWORD LCID;
typedef LONG *LPLONG;

typedef struct tagLOGFONTW {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONTW;

typedef struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
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
} STARTUPINFOW, *LPSTARTUPINFOW;

typedef struct _RTL_CRITICAL_SECTION {
    void *DebugInfo;
    LONG LockCount;      /* -1 when free */
    LONG RecursionCount;
    HANDLE OwningThread; /* the owner's thread id */
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA;

typedef struct _OSVERSIONINFOEXA {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    CHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXA;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    long long QuadPart;
} LARGE_INTEGER;
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

/* WM_HELP's lParam. */
typedef struct tagHELPINFO {
    UINT cbSize;
    int iContextType;
    int iCtrlId;
    HANDLE hItemHandle;
    ULONG_PTR dwContextId;
    POINT MousePos;
} HELPINFO, *LPHELPINFO;
#define HELPINFO_WINDOW 1

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

typedef struct tagPANOSE {
    BYTE bFamilyType;
    BYTE bSerifStyle;
    BYTE bWeight;
    BYTE bProportion;
    BYTE bContrast;
    BYTE bStrokeVariation;
    BYTE bArmStyle;
    BYTE bLetterform;
    BYTE bMidline;
    BYTE bXHeight;
} PANOSE;

typedef struct _OUTLINETEXTMETRICW {
    UINT otmSize;
    TEXTMETRICW otmTextMetrics;
    BYTE otmFiller;
    PANOSE otmPanoseNumber;
    UINT otmfsSelection;
    UINT otmfsType;
    int otmsCharSlopeRise;
    int otmsCharSlopeRun;
    int otmItalicAngle;
    UINT otmEMSquare;
    int otmAscent;
    int otmDescent;
    UINT otmLineGap;
    UINT otmsCapEmHeight;
    UINT otmsXHeight;
    RECT otmrcFontBox;
    int otmMacAscent;
    int otmMacDescent;
    UINT otmMacLineGap;
    UINT otmusMinimumPPEM;
    POINT otmptSubscriptSize;
    POINT otmptSubscriptOffset;
    POINT otmptSuperscriptSize;
    POINT otmptSuperscriptOffset;
    UINT otmsStrikeoutSize;
    int otmsStrikeoutPosition;
    int otmsUnderscoreSize;
    int otmsUnderscorePosition;
    char *otmpFamilyName; /* offsets from the structure's start */
    char *otmpFaceName;
    char *otmpStyleName;
    char *otmpFullName;
} OUTLINETEXTMETRICW, *LPOUTLINETEXTMETRICW;

typedef struct tagFONTSIGNATURE {
    DWORD fsUsb[4];
    DWORD fsCsb[2];
} FONTSIGNATURE, *LPFONTSIGNATURE;

typedef struct tagCHARSETINFO {
    UINT ciCharset;
    UINT ciACP;
    FONTSIGNATURE fs;
} CHARSETINFO;

typedef struct tagNEWTEXTMETRICW {
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
    DWORD ntmFlags;
    UINT ntmSizeEM;
    UINT ntmCellHeight;
    UINT ntmAvgWidth;
} NEWTEXTMETRICW;

typedef struct tagNEWTEXTMETRICEXW {
    NEWTEXTMETRICW ntmTm;
    FONTSIGNATURE ntmFontSig;
} NEWTEXTMETRICEXW;

typedef struct tagENUMLOGFONTEXW {
    LOGFONTW elfLogFont;
    WCHAR elfFullName[64];
    WCHAR elfStyle[32];
    WCHAR elfScript[32];
} ENUMLOGFONTEXW;

typedef int(CALLBACK *FONTENUMPROCW)(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM);

typedef struct tagRGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} RGBQUAD;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO;

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

#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INVALID_NAME 123u
#define ERROR_NEGATIVE_SEEK 131u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_FILENAME_EXCED_RANGE 206u
#define ERROR_DIRECTORY 267u

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
#define FILE_SHARE_DELETE 0x4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_HIDDEN 0x02u
#define FILE_ATTRIBUTE_SYSTEM 0x04u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xffffffff)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define FS_CASE_SENSITIVE 0x1u
#define FS_CASE_IS_PRESERVED 0x2u
#define FS_UNICODE_STORED_ON_DISK 0x4u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259u

#define LMEM_FIXED 0x0000u
#define LMEM_MOVEABLE 0x0002u
#define LMEM_NOCOMPACT 0x0010u
#define LMEM_NODISCARD 0x0020u
#define LMEM_ZEROINIT 0x0040u
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)
#define LMEM_MODIFY 0x0080u
#define LMEM_DISCARDABLE 0x0F00u
#define LMEM_INVALID_HANDLE 0x8000u
#define GMEM_FIXED 0x0000u
#define GMEM_MOVEABLE 0x0002u
#define GMEM_NOCOMPACT 0x0010u
#define GMEM_NODISCARD 0x0020u
#define GMEM_ZEROINIT 0x0040u
#define GMEM_MODIFY 0x0080u
#define GMEM_DISCARDABLE 0x0100u
#define GMEM_NOT_BANKED 0x1000u
#define GMEM_SHARE 0x2000u
#define GMEM_DDESHARE 0x2000u
#define GMEM_NOTIFY 0x4000u
#define GMEM_INVALID_HANDLE 0x8000u
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define TLS_OUT_OF_INDEXES 0xffffffffu
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_NOT_LOCKED 158u
#define ERROR_RESOURCE_DATA_NOT_FOUND 1812u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813u
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814u
#define ERROR_NO_UNICODE_TRANSLATION 1113u
#define CP_ACP 0u
#define CP_OEMCP 1u
#define CP_THREAD_ACP 3u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 0x08u

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
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3

/* ---- user32 and gdi32 constants ------------------------------------------ */

#define MAKEINTRESOURCEW(i) ((LPWSTR)(ULONG_PTR)(WORD)(i))
#define LOWORD(l) ((WORD)((DWORD)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD)(l) >> 16))
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)((WORD)(l) | ((DWORD)(WORD)(h) << 16)))
#define MAKELONG(l, h) ((LONG)(DWORD)((WORD)(l) | ((DWORD)(WORD)(h) << 16)))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)((WORD)(l) | ((DWORD)(WORD)(h) << 16)))
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
#define HWND_MESSAGE ((HWND)(long)-3)

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
#define CS_GLOBALCLASS 0x4000

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
#define WAIT_FAILED ((DWORD)0xffffffff)

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
#define COLOR_WINDOWFRAME 6
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

/* WP6: text and pictures (WIN32_WORDPAD_ROADMAP.md). */
#define SRCAND 0x008800c6u
#define SRCINVERT 0x00660046u
#define SRCPAINT 0x00ee0086u
#define SYMBOL_CHARSET 2
#define MAC_CHARSET 77
#define SHIFTJIS_CHARSET 128
#define HANGEUL_CHARSET 129
#define GB2312_CHARSET 134
#define CHINESEBIG5_CHARSET 136
#define GREEK_CHARSET 161
#define TURKISH_CHARSET 162
#define VIETNAMESE_CHARSET 163
#define HEBREW_CHARSET 177
#define ARABIC_CHARSET 178
#define BALTIC_CHARSET 186
#define RUSSIAN_CHARSET 204
#define THAI_CHARSET 222
#define EASTEUROPE_CHARSET 238
#define OEM_CHARSET 255
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define FF_DONTCARE 0x00
#define FF_ROMAN 0x10
#define FF_SWISS 0x20
#define FF_MODERN 0x30
#define FF_SCRIPT 0x40
#define FF_DECORATIVE 0x50
#define DEFAULT_QUALITY 0
#define DRAFT_QUALITY 1
#define PROOF_QUALITY 2
#define NONANTIALIASED_QUALITY 3
#define ANTIALIASED_QUALITY 4
#define TMPF_FIXED_PITCH 0x01
#define TMPF_VECTOR 0x02
#define TMPF_TRUETYPE 0x04
#define TMPF_DEVICE 0x08
#define RASTER_FONTTYPE 0x0001
#define DEVICE_FONTTYPE 0x0002
#define TRUETYPE_FONTTYPE 0x0004
#define NTM_ITALIC 0x00000001
#define NTM_BOLD 0x00000020
#define NTM_REGULAR 0x00000040
#define ETO_OPAQUE 0x0002
#define ETO_CLIPPED 0x0004
#define ETO_GLYPH_INDEX 0x0010
#define TA_NOUPDATECP 0
#define TA_UPDATECP 1
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_TOP 0
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define GDI_ERROR 0xffffffffu
#define PS_SOLID 0
#define PS_NULL 5
#define BI_RGB 0
#define DIB_RGB_COLORS 0

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
/* ---- WP5: user32 for MFC (WIN32_WORDPAD_ROADMAP.md) ------------------------ */
typedef struct HHOOK__ *HHOOK;
typedef struct HDWP__ *HDWP;
typedef struct HRGN__ *HRGN;
typedef HANDLE HDROP;
typedef LRESULT(CALLBACK *HOOKPROC)(int code, WPARAM wParam, LPARAM lParam);
typedef BOOL(CALLBACK *WNDENUMPROC)(HWND, LPARAM);
typedef struct tagCBT_CREATEWNDW {
    CREATESTRUCTW *lpcs;
    HWND hwndInsertAfter;
} CBT_CREATEWNDW;
typedef struct tagCBTACTIVATESTRUCT {
    BOOL fMouse;
    HWND hWndActive;
} CBTACTIVATESTRUCT;
typedef struct tagCWPSTRUCT {
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
} CWPSTRUCT;
typedef struct tagCWPRETSTRUCT {
    LRESULT lResult;
    LPARAM lParam;
    WPARAM wParam;
    UINT message;
    HWND hwnd;
} CWPRETSTRUCT;
typedef struct tagSTYLESTRUCT {
    DWORD styleOld;
    DWORD styleNew;
} STYLESTRUCT;
typedef struct tagNCCALCSIZE_PARAMS {
    RECT rgrc[3];
    WINDOWPOS *lppos;
} NCCALCSIZE_PARAMS;
typedef struct tagSCROLLINFO {
    UINT cbSize;
    UINT fMask;
    int nMin;
    int nMax;
    UINT nPage;
    int nPos;
    int nTrackPos;
} SCROLLINFO, *LPSCROLLINFO;
typedef struct tagWINDOWPLACEMENT {
    UINT length;
    UINT flags;
    UINT showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT rcNormalPosition;
} WINDOWPLACEMENT;
typedef struct tagMEASUREITEMSTRUCT {
    UINT CtlType;
    UINT CtlID;
    UINT itemID;
    UINT itemWidth;
    UINT itemHeight;
    ULONG_PTR itemData;
} MEASUREITEMSTRUCT;
typedef struct tagDELETEITEMSTRUCT {
    UINT CtlType;
    UINT CtlID;
    UINT itemID;
    HWND hwndItem;
    ULONG_PTR itemData;
} DELETEITEMSTRUCT;
typedef struct tagCOMPAREITEMSTRUCT {
    UINT CtlType;
    UINT CtlID;
    HWND hwndItem;
    UINT itemID1;
    ULONG_PTR itemData1;
    UINT itemID2;
    ULONG_PTR itemData2;
    DWORD dwLocaleId;
} COMPAREITEMSTRUCT;
typedef struct _DROPFILES {
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
} DROPFILES;
#pragma pack(push, 2)
typedef struct {
    DWORD style;
    DWORD dwExtendedStyle;
    WORD cdit;
    short x;
    short y;
    short cx;
    short cy;
} DLGTEMPLATE;
#pragma pack(pop)
#define WH_MSGFILTER (-1)
#define WH_KEYBOARD 2
#define WH_GETMESSAGE 3
#define WH_CALLWNDPROC 4
#define WH_CBT 5
#define WH_MOUSE 7
#define WH_CALLWNDPROCRET 12
#define HC_ACTION 0
#define HCBT_MOVESIZE 0
#define HCBT_MINMAX 1
#define HCBT_QS 2
#define HCBT_CREATEWND 3
#define HCBT_DESTROYWND 4
#define HCBT_ACTIVATE 5
#define HCBT_CLICKSKIPPED 6
#define HCBT_KEYSKIPPED 7
#define HCBT_SYSCOMMAND 8
#define HCBT_SETFOCUS 9
#define MSGF_DIALOGBOX 0
#define MSGF_MESSAGEBOX 1
#define MSGF_MENU 2
#define MSGF_SCROLLBAR 5
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define DWL_MSGRESULT 0
#define DWL_DLGPROC 4
#define DWL_USER 8
#define DLGWINDOWEXTRA 30
#define ERROR_INVALID_INDEX 1413u
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_INVALID_MENU_HANDLE 1401u
#define ERROR_INVALID_HOOK_HANDLE 1404u
#define ERROR_CLASS_DOES_NOT_EXIST 1411u
#define ERROR_CLASS_HAS_WINDOWS 1412u
#define ERROR_HOOK_NEEDS_HMOD 1428u
#define ERROR_INVALID_HOOK_FILTER 1426u
#define ERROR_CLIPBOARD_NOT_OPEN 1418u
#define ERROR_HOTKEY_NOT_REGISTERED 1419u
#define ERROR_NOT_FOUND 1168u
#define GW_HWNDFIRST 0
#define GW_HWNDLAST 1
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_CHILD 5
#define GW_ENABLEDPOPUP 6
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOSENDCHANGING 0x0400
#define SWP_DEFERERASE 0x2000
#define SWP_ASYNCWINDOWPOS 0x4000
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)(long)-1)
#define HWND_NOTOPMOST ((HWND)(long)-2)
#define WM_SETREDRAW 0x000b
#define WM_CHILDACTIVATE 0x0022
#define WM_SETICON 0x0080
#define WM_MEASUREITEM 0x002c
#define WM_DELETEITEM 0x002d
#define WM_VKEYTOITEM 0x002e
#define WM_CHARTOITEM 0x002f
#define WM_COMPAREITEM 0x0039
#define WM_NOTIFY 0x004e
#define WM_STYLECHANGING 0x007c
#define WM_STYLECHANGED 0x007d
#define WM_NCRBUTTONDOWN 0x00a4
#define WM_NCRBUTTONUP 0x00a5
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_MOUSEWHEEL 0x020a
#define WM_DROPFILES 0x0233
#define WM_CUT 0x0300
#define WM_COPY 0x0301
#define WM_PASTE 0x0302
#define WM_CLEAR 0x0303
#define WM_UNDO 0x0304
#define WM_RENDERFORMAT 0x0305
#define WM_RENDERALLFORMATS 0x0306
#define WM_DESTROYCLIPBOARD 0x0307
#define WM_DRAWCLIPBOARD 0x0308
#define WM_CHANGECBCHAIN 0x030d
#define WM_CLIPBOARDUPDATE 0x031d
#define WM_CTLCOLOR_FIRST WM_CTLCOLORMSGBOX
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_ACCEPTFILES 0x00000010L
#define WS_EX_TRANSPARENT 0x00000020L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_WINDOWEDGE 0x00000100L
#define WS_EX_CLIENTEDGE 0x00000200L
#define WS_EX_STATICEDGE 0x00020000L
#define WS_EX_APPWINDOW 0x00040000L
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL 2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINELEFT 0
#define SB_LINEDOWN 1
#define SB_LINERIGHT 1
#define SB_PAGEUP 2
#define SB_PAGELEFT 2
#define SB_PAGEDOWN 3
#define SB_PAGERIGHT 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_LEFT 6
#define SB_BOTTOM 7
#define SB_RIGHT 7
#define SB_ENDSCROLL 8
#define SBS_HORZ 0x0000L
#define SBS_VERT 0x0001L
#define SIF_RANGE 0x0001
#define SIF_PAGE 0x0002
#define SIF_POS 0x0004
#define SIF_DISABLENOSCROLL 0x0008
#define SIF_TRACKPOS 0x0010
#define SIF_ALL (SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS)
#define ESB_ENABLE_BOTH 0x0000
#define ESB_DISABLE_LTUP 0x0001
#define ESB_DISABLE_RTDN 0x0002
#define ESB_DISABLE_BOTH 0x0003
#define SW_SCROLLCHILDREN 0x0001
#define SW_INVALIDATE 0x0002
#define SW_ERASE 0x0004
#define RDW_INVALIDATE 0x0001
#define RDW_INTERNALPAINT 0x0002
#define RDW_ERASE 0x0004
#define RDW_VALIDATE 0x0008
#define RDW_NOINTERNALPAINT 0x0010
#define RDW_NOERASE 0x0020
#define RDW_NOCHILDREN 0x0040
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
#define RDW_FRAME 0x0400
#define RDW_NOFRAME 0x0800
#define NULLREGION 1
#define SIMPLEREGION 2
#define COMPLEXREGION 3
#define ERROR_REGION 0
#define HTERROR (-2)
#define HTTRANSPARENT (-1)
#define HTNOWHERE 0
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTBORDER 18
#define MF_STRING 0x0000L
#define MF_UNCHECKED 0x0000L
#define MF_BITMAP 0x0004L
#define MF_DEFAULT 0x1000L
#define MF_RIGHTJUSTIFY 0x4000L
#define TPM_LEFTBUTTON 0x0000
#define TPM_RIGHTBUTTON 0x0002
#define TPM_LEFTALIGN 0x0000
#define TPM_CENTERALIGN 0x0004
#define TPM_RIGHTALIGN 0x0008
#define TPM_TOPALIGN 0x0000
#define TPM_VCENTERALIGN 0x0010
#define TPM_BOTTOMALIGN 0x0020
#define TPM_NONOTIFY 0x0080
#define TPM_RETURNCMD 0x0100
#define ES_LEFT 0x0000L
#define ES_CENTER 0x0001L
#define ES_RIGHT 0x0002L
#define ES_MULTILINE 0x0004L
#define ES_UPPERCASE 0x0008L
#define ES_LOWERCASE 0x0010L
#define ES_PASSWORD 0x0020L
#define ES_AUTOVSCROLL 0x0040L
#define ES_AUTOHSCROLL 0x0080L
#define ES_NOHIDESEL 0x0100L
#define ES_READONLY 0x0800L
#define ES_WANTRETURN 0x1000L
#define ES_NUMBER 0x2000L
#define EM_GETSEL 0x00b0
#define EM_SETSEL 0x00b1
#define EM_GETRECT 0x00b2
#define EM_SETRECT 0x00b3
#define EM_SETRECTNP 0x00b4
#define EM_SCROLL 0x00b5
#define EM_LINESCROLL 0x00b6
#define EM_SCROLLCARET 0x00b7
#define EM_GETMODIFY 0x00b8
#define EM_SETMODIFY 0x00b9
#define EM_GETLINECOUNT 0x00ba
#define EM_LINEINDEX 0x00bb
#define EM_LINELENGTH 0x00c1
#define EM_REPLACESEL 0x00c2
#define EM_GETLINE 0x00c4
#define EM_LIMITTEXT 0x00c5
#define EM_SETLIMITTEXT EM_LIMITTEXT
#define EM_CANUNDO 0x00c6
#define EM_UNDO 0x00c7
#define EM_LINEFROMCHAR 0x00c9
#define EM_SETPASSWORDCHAR 0x00cc
#define EM_EMPTYUNDOBUFFER 0x00cd
#define EM_GETFIRSTVISIBLELINE 0x00ce
#define EM_SETREADONLY 0x00cf
#define EM_GETPASSWORDCHAR 0x00d2
#define EM_SETMARGINS 0x00d3
#define EM_GETMARGINS 0x00d4
#define EM_GETLIMITTEXT 0x00d5
#define EM_POSFROMCHAR 0x00d6
#define EM_CHARFROMPOS 0x00d7
#define WB_LEFT 0
#define WB_RIGHT 1
#define WB_ISDELIMITER 2
#define EC_LEFTMARGIN 0x0001
#define EC_RIGHTMARGIN 0x0002
#define EC_USEFONTINFO 0xffff
#define EN_SETFOCUS 0x0100
#define EN_KILLFOCUS 0x0200
#define EN_CHANGE 0x0300
#define EN_UPDATE 0x0400
#define EN_ERRSPACE 0x0500
#define EN_MAXTEXT 0x0501
#define EN_HSCROLL 0x0601
#define EN_VSCROLL 0x0602
#define LBS_NOTIFY 0x0001L
#define LBS_SORT 0x0002L
#define LBS_NOREDRAW 0x0004L
#define LBS_MULTIPLESEL 0x0008L
#define LBS_OWNERDRAWFIXED 0x0010L
#define LBS_OWNERDRAWVARIABLE 0x0020L
#define LBS_HASSTRINGS 0x0040L
#define LBS_USETABSTOPS 0x0080L
#define LBS_NOINTEGRALHEIGHT 0x0100L
#define LBS_MULTICOLUMN 0x0200L
#define LBS_WANTKEYBOARDINPUT 0x0400L
#define LBS_EXTENDEDSEL 0x0800L
#define LBS_DISABLENOSCROLL 0x1000L
#define LBS_NODATA 0x2000L
#define LBS_COMBOBOX 0x8000L
#define LB_OKAY 0
#define LB_ERR (-1)
#define LB_ERRSPACE (-2)
#define LB_ADDSTRING 0x0180
#define LB_INSERTSTRING 0x0181
#define LB_DELETESTRING 0x0182
#define LB_RESETCONTENT 0x0184
#define LB_SETSEL 0x0185
#define LB_SETCURSEL 0x0186
#define LB_GETSEL 0x0187
#define LB_GETCURSEL 0x0188
#define LB_GETTEXT 0x0189
#define LB_GETTEXTLEN 0x018a
#define LB_GETCOUNT 0x018b
#define LB_SELECTSTRING 0x018c
#define LB_GETTOPINDEX 0x018e
#define LB_FINDSTRING 0x018f
#define LB_GETSELCOUNT 0x0190
#define LB_GETSELITEMS 0x0191
#define LB_SETTOPINDEX 0x0197
#define LB_GETITEMRECT 0x0198
#define LB_GETITEMDATA 0x0199
#define LB_SETITEMDATA 0x019a
#define LB_SETCARETINDEX 0x019e
#define LB_GETCARETINDEX 0x019f
#define LB_SETITEMHEIGHT 0x01a0
#define LB_GETITEMHEIGHT 0x01a1
#define LB_FINDSTRINGEXACT 0x01a2
#define LB_INITSTORAGE 0x01a8
#define LB_ITEMFROMPOINT 0x01a9
#define LBN_ERRSPACE (-2)
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2
#define LBN_SELCANCEL 3
#define LBN_SETFOCUS 4
#define LBN_KILLFOCUS 5
#define CBS_SIMPLE 0x0001L
#define CBS_DROPDOWN 0x0002L
#define CBS_DROPDOWNLIST 0x0003L
#define CBS_OWNERDRAWFIXED 0x0010L
#define CBS_OWNERDRAWVARIABLE 0x0020L
#define CBS_AUTOHSCROLL 0x0040L
#define CBS_OEMCONVERT 0x0080L
#define CBS_SORT 0x0100L
#define CBS_HASSTRINGS 0x0200L
#define CBS_NOINTEGRALHEIGHT 0x0400L
#define CBS_DISABLENOSCROLL 0x0800L
#define CBS_UPPERCASE 0x2000L
#define CBS_LOWERCASE 0x4000L
#define CB_OKAY 0
#define CB_ERR (-1)
#define CB_ERRSPACE (-2)
#define CB_GETEDITSEL 0x0140
#define CB_LIMITTEXT 0x0141
#define CB_SETEDITSEL 0x0142
#define CB_ADDSTRING 0x0143
#define CB_DELETESTRING 0x0144
#define CB_GETCOUNT 0x0146
#define CB_GETCURSEL 0x0147
#define CB_GETLBTEXT 0x0148
#define CB_GETLBTEXTLEN 0x0149
#define CB_INSERTSTRING 0x014a
#define CB_RESETCONTENT 0x014b
#define CB_FINDSTRING 0x014c
#define CB_SELECTSTRING 0x014d
#define CB_SETCURSEL 0x014e
#define CB_SHOWDROPDOWN 0x014f
#define CB_GETITEMDATA 0x0150
#define CB_SETITEMDATA 0x0151
#define CB_GETDROPPEDCONTROLRECT 0x0152
#define CB_SETITEMHEIGHT 0x0153
#define CB_GETITEMHEIGHT 0x0154
#define CB_SETEXTENDEDUI 0x0155
#define CB_GETEXTENDEDUI 0x0156
#define CB_GETDROPPEDSTATE 0x0157
#define CB_FINDSTRINGEXACT 0x0158
#define CB_GETTOPINDEX 0x015b
#define CB_SETTOPINDEX 0x015c
#define CB_GETDROPPEDWIDTH 0x015f
#define CB_SETDROPPEDWIDTH 0x0160
#define CB_INITSTORAGE 0x0161
#define CBN_ERRSPACE (-1)
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define CBN_EDITUPDATE 6
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define CBN_SELENDOK 9
#define CBN_SELENDCANCEL 10
#define ODT_LISTBOX 2
#define ODT_COMBOBOX 3
#define ODS_COMBOBOXEDIT 0x1000
#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_METAFILEPICT 3
#define CF_OEMTEXT 7
#define CF_DIB 8
#define CF_UNICODETEXT 13
#define CF_ENHMETAFILE 14
#define CF_HDROP 15
#define CF_LOCALE 16
#define CF_DIBV5 17
#define CF_PRIVATEFIRST 0x0200
#define CF_GDIOBJFIRST 0x0300
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_INSERT 0x2d
#define VK_DELETE 0x2e
#define VK_F3 0x72
#define VK_F4_KEY VK_F4
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define IDC_SIZEALL MAKEINTRESOURCEW(32646)
#define COLOR_INFOTEXT 23
#define COLOR_INFOBK 24
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYVTHUMB 9
#define SM_CXHTHUMB 10
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CYMENU 15
#define SM_CYVSCROLL 20
#define SM_CXHSCROLL 21
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SM_CXEDGE 45
#define SM_CYEDGE 46
#define SM_CYMENUCHECK 72
#define BDR_RAISEDOUTER 0x0001
#define BDR_SUNKENOUTER 0x0002
#define BDR_RAISEDINNER 0x0004
#define BDR_SUNKENINNER 0x0008
#define EDGE_RAISED (BDR_RAISEDOUTER | BDR_RAISEDINNER)
#define EDGE_SUNKEN (BDR_SUNKENOUTER | BDR_SUNKENINNER)
#define EDGE_ETCHED (BDR_SUNKENOUTER | BDR_RAISEDINNER)
#define EDGE_BUMP (BDR_RAISEDOUTER | BDR_SUNKENINNER)
#define BF_LEFT 0x0001
#define BF_TOP 0x0002
#define BF_RIGHT 0x0004
#define BF_BOTTOM 0x0008
#define BF_RECT (BF_LEFT | BF_TOP | BF_RIGHT | BF_BOTTOM)
#define BF_MIDDLE 0x0800
#define BF_SOFT 0x1000
#define BF_ADJUST 0x2000
#define BF_FLAT 0x4000
#define BF_MONO 0x8000

#ifndef SXW_BUILDING_KERNEL32
#define SXW_KERNEL32_API DECLSPEC_IMPORT
#else
#define SXW_KERNEL32_API
#endif

SXW_KERNEL32_API HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
SXW_KERNEL32_API LPWSTR WINAPI GetCommandLineW(void);
SXW_KERNEL32_API DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
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
SXW_KERNEL32_API void WINAPI RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);
SXW_KERNEL32_API DWORD WINAPI GetLastError(void);
SXW_KERNEL32_API DWORD WINAPI GetCurrentThreadId(void);
SXW_KERNEL32_API DWORD WINAPI GetCurrentProcessId(void);
SXW_KERNEL32_API void WINAPI SetLastError(DWORD dwErrCode);
SXW_KERNEL32_API HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
SXW_KERNEL32_API HGLOBAL WINAPI GlobalReAlloc(HGLOBAL hMem, SIZE_T dwBytes, UINT uFlags);
SXW_KERNEL32_API LPVOID WINAPI GlobalLock(HGLOBAL hMem);
SXW_KERNEL32_API BOOL WINAPI GlobalUnlock(HGLOBAL hMem);
SXW_KERNEL32_API SIZE_T WINAPI GlobalSize(HGLOBAL hMem);
SXW_KERNEL32_API UINT WINAPI GlobalFlags(HGLOBAL hMem);
SXW_KERNEL32_API HGLOBAL WINAPI GlobalHandle(LPCVOID pMem);
SXW_KERNEL32_API HGLOBAL WINAPI GlobalFree(HGLOBAL hMem);
SXW_KERNEL32_API HLOCAL WINAPI LocalReAlloc(HLOCAL hMem, SIZE_T uBytes, UINT uFlags);
SXW_KERNEL32_API LPVOID WINAPI LocalLock(HLOCAL hMem);
SXW_KERNEL32_API BOOL WINAPI LocalUnlock(HLOCAL hMem);
SXW_KERNEL32_API int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr,
                                                int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
SXW_KERNEL32_API void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
SXW_KERNEL32_API void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime);
SXW_KERNEL32_API void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
SXW_KERNEL32_API BOOL WINAPI FileTimeToSystemTime(const FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime);
SXW_KERNEL32_API BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime);
SXW_KERNEL32_API BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *lpFileTime, LPFILETIME lpLocalFileTime);
SXW_KERNEL32_API BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *lpLocalFileTime, LPFILETIME lpFileTime);
SXW_KERNEL32_API HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                           LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                                           DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
SXW_KERNEL32_API BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                                      LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
SXW_KERNEL32_API DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG *lpDistanceToMoveHigh,
                                             DWORD dwMoveMethod);
SXW_KERNEL32_API DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
SXW_KERNEL32_API BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime,
                                         LPFILETIME lpLastWriteTime);
SXW_KERNEL32_API BOOL WINAPI SetEndOfFile(HANDLE hFile);
SXW_KERNEL32_API BOOL WINAPI FlushFileBuffers(HANDLE hFile);
SXW_KERNEL32_API BOOL WINAPI CloseHandle(HANDLE hObject);
SXW_KERNEL32_API DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName);
SXW_KERNEL32_API BOOL WINAPI DeleteFileW(LPCWSTR lpFileName);
SXW_KERNEL32_API BOOL WINAPI CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
SXW_KERNEL32_API BOOL WINAPI RemoveDirectoryW(LPCWSTR lpPathName);
SXW_KERNEL32_API BOOL WINAPI MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName);
SXW_KERNEL32_API HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
SXW_KERNEL32_API BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
SXW_KERNEL32_API BOOL WINAPI FindClose(HANDLE hFindFile);
SXW_KERNEL32_API DWORD WINAPI GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer,
                                               LPWSTR *lpFilePart);
SXW_KERNEL32_API DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer);
SXW_KERNEL32_API BOOL WINAPI SetCurrentDirectoryW(LPCWSTR lpPathName);
SXW_KERNEL32_API BOOL WINAPI GetVolumeInformationW(LPCWSTR lpRootPathName, LPWSTR lpVolumeNameBuffer,
                                                   DWORD nVolumeNameSize, LPDWORD lpVolumeSerialNumber,
                                                   LPDWORD lpMaximumComponentLength, LPDWORD lpFileSystemFlags,
                                                   LPWSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);
#define NORM_IGNORECASE 0x1u
SXW_KERNEL32_API int WINAPI CompareStringW(LCID Locale, DWORD dwCmpFlags, LPCWSTR lpString1, int cchCount1,
                                          LPCWSTR lpString2, int cchCount2);
SXW_KERNEL32_API int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr,
                                                int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte,
                                                LPCSTR lpDefaultChar, BOOL *lpUsedDefaultChar);


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
SXW_USER32_API BOOL WINAPI SystemParametersInfoA(UINT uiAction, UINT uiParam, LPVOID pvParam, UINT fWinIni);
SXW_USER32_API BOOL WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam, LPVOID pvParam, UINT fWinIni);
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
SXW_USER32_API HCURSOR WINAPI SetCursor(HCURSOR hCursor);
SXW_USER32_API HBITMAP WINAPI LoadBitmapA(HINSTANCE hInstance, LPCSTR lpBitmapName);
SXW_USER32_API HBITMAP WINAPI LoadBitmapW(HINSTANCE hInstance, LPCWSTR lpBitmapName);
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
/* WP5: hooks, subclassing, the window tree, positions, carets, scrolling,
 * menus made at run time, modeless dialogs, the standard controls' helpers
 * and the clipboard. */
SXW_USER32_API HHOOK WINAPI SetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
SXW_USER32_API BOOL WINAPI UnhookWindowsHookEx(HHOOK hhk);
SXW_USER32_API LRESULT WINAPI CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam);
SXW_USER32_API BOOL WINAPI CallMsgFilterW(LPMSG lpMsg, int nCode);
SXW_USER32_API LONG WINAPI GetWindowLongW(HWND hWnd, int nIndex);
SXW_USER32_API LONG WINAPI SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong);
SXW_USER32_API LRESULT WINAPI CallWindowProcW(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam,
                                              LPARAM lParam);
SXW_USER32_API BOOL WINAPI SetPropW(HWND hWnd, LPCWSTR lpString, HANDLE hData);
SXW_USER32_API HANDLE WINAPI GetPropW(HWND hWnd, LPCWSTR lpString);
SXW_USER32_API HANDLE WINAPI RemovePropW(HWND hWnd, LPCWSTR lpString);
SXW_USER32_API HWND WINAPI GetParent(HWND hWnd);
SXW_USER32_API HWND WINAPI SetParent(HWND hWndChild, HWND hWndNewParent);
SXW_USER32_API HWND WINAPI GetWindow(HWND hWnd, UINT uCmd);
SXW_USER32_API HWND WINAPI GetTopWindow(HWND hWnd);
SXW_USER32_API BOOL WINAPI IsChild(HWND hWndParent, HWND hWnd);
SXW_USER32_API BOOL WINAPI IsWindow(HWND hWnd);
SXW_USER32_API BOOL WINAPI IsWindowVisible(HWND hWnd);
SXW_USER32_API BOOL WINAPI IsWindowEnabled(HWND hWnd);
SXW_USER32_API BOOL WINAPI IsZoomed(HWND hWnd);
SXW_USER32_API BOOL WINAPI EnumChildWindows(HWND hWndParent, WNDENUMPROC lpEnumFunc, LPARAM lParam);
SXW_USER32_API BOOL WINAPI EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam);
SXW_USER32_API int WINAPI GetClassNameW(HWND hWnd, LPWSTR lpClassName, int nMaxCount);
SXW_USER32_API BOOL WINAPI GetClassInfoW(HINSTANCE hInstance, LPCWSTR lpClassName, WNDCLASSW *lpWndClass);
SXW_USER32_API BOOL WINAPI UnregisterClassW(LPCWSTR lpClassName, HINSTANCE hInstance);
SXW_USER32_API int WINAPI GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount);
SXW_USER32_API int WINAPI GetWindowTextLengthW(HWND hWnd);
SXW_USER32_API BOOL WINAPI SetWindowTextW(HWND hWnd, LPCWSTR lpString);
SXW_USER32_API int WINAPI GetDlgCtrlID(HWND hWnd);
SXW_USER32_API BOOL WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect);
SXW_USER32_API BOOL WINAPI ScreenToClient(HWND hWnd, LPPOINT lpPoint);
SXW_USER32_API int WINAPI MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints);
SXW_USER32_API HWND WINAPI WindowFromPoint(POINT Point);
SXW_USER32_API HWND WINAPI ChildWindowFromPoint(HWND hWndParent, POINT Point);
SXW_USER32_API BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy,
                                        UINT uFlags);
SXW_USER32_API HDWP WINAPI BeginDeferWindowPos(int nNumWindows);
SXW_USER32_API HDWP WINAPI DeferWindowPos(HDWP hWinPosInfo, HWND hWnd, HWND hWndInsertAfter, int x, int y, int cx,
                                          int cy, UINT uFlags);
SXW_USER32_API BOOL WINAPI EndDeferWindowPos(HDWP hWinPosInfo);
SXW_USER32_API BOOL WINAPI BringWindowToTop(HWND hWnd);
SXW_USER32_API BOOL WINAPI GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT *lpwndpl);
SXW_USER32_API BOOL WINAPI SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT *lpwndpl);
SXW_USER32_API HWND WINAPI GetFocus(void);
SXW_USER32_API HWND WINAPI GetActiveWindow(void);
SXW_USER32_API HWND WINAPI SetActiveWindow(HWND hWnd);
SXW_USER32_API HWND WINAPI GetForegroundWindow(void);
SXW_USER32_API BOOL WINAPI SetForegroundWindow(HWND hWnd);
SXW_USER32_API HWND WINAPI GetLastActivePopup(HWND hWnd);
SXW_USER32_API BOOL WINAPI ShowOwnedPopups(HWND hWnd, BOOL fShow);
SXW_USER32_API BOOL WINAPI RedrawWindow(HWND hWnd, const RECT *lprcUpdate, HRGN hrgnUpdate, UINT flags);
SXW_USER32_API BOOL WINAPI ValidateRect(HWND hWnd, const RECT *lpRect);
SXW_USER32_API BOOL WINAPI InvalidateRgn(HWND hWnd, HRGN hRgn, BOOL bErase);
SXW_USER32_API BOOL WINAPI GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase);
SXW_USER32_API HDC WINAPI GetWindowDC(HWND hWnd);
SXW_USER32_API HDC WINAPI GetDCEx(HWND hWnd, HRGN hrgnClip, DWORD flags);
SXW_USER32_API HWND WINAPI WindowFromDC(HDC hDC);
SXW_USER32_API BOOL WINAPI LockWindowUpdate(HWND hWndLock);
SXW_USER32_API LRESULT WINAPI SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
SXW_USER32_API LRESULT WINAPI SendMessageTimeoutW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, UINT fuFlags,
                                                  UINT uTimeout, ULONG_PTR *lpdwResult);
SXW_USER32_API BOOL WINAPI InSendMessage(void);
SXW_USER32_API BOOL WINAPI WaitMessage(void);
SXW_USER32_API DWORD WINAPI GetMessagePos(void);
SXW_USER32_API LONG WINAPI GetMessageTime(void);
SXW_USER32_API short WINAPI GetAsyncKeyState(int vKey);
SXW_USER32_API BOOL WINAPI MessageBeep(UINT uType);
SXW_USER32_API int WINAPI GetSystemMetrics(int nIndex);
SXW_USER32_API BOOL WINAPI CreateCaret(HWND hWnd, HBITMAP hBitmap, int nWidth, int nHeight);
SXW_USER32_API BOOL WINAPI DestroyCaret(void);
SXW_USER32_API BOOL WINAPI ShowCaret(HWND hWnd);
SXW_USER32_API BOOL WINAPI HideCaret(HWND hWnd);
SXW_USER32_API BOOL WINAPI SetCaretPos(int X, int Y);
SXW_USER32_API BOOL WINAPI GetCaretPos(LPPOINT lpPoint);
SXW_USER32_API UINT WINAPI GetCaretBlinkTime(void);
SXW_USER32_API BOOL WINAPI SetCaretBlinkTime(UINT uMSeconds);
SXW_USER32_API int WINAPI ScrollWindowEx(HWND hWnd, int dx, int dy, const RECT *prcScroll, const RECT *prcClip,
                                         HRGN hrgnUpdate, LPRECT prcUpdate, UINT flags);
SXW_USER32_API BOOL WINAPI ScrollWindow(HWND hWnd, int XAmount, int YAmount, const RECT *lpRect,
                                        const RECT *lpClipRect);
SXW_USER32_API int WINAPI SetScrollInfo(HWND hwnd, int nBar, const SCROLLINFO *lpsi, BOOL redraw);
SXW_USER32_API BOOL WINAPI GetScrollInfo(HWND hwnd, int nBar, LPSCROLLINFO lpsi);
SXW_USER32_API int WINAPI SetScrollPos(HWND hWnd, int nBar, int nPos, BOOL bRedraw);
SXW_USER32_API int WINAPI GetScrollPos(HWND hWnd, int nBar);
SXW_USER32_API BOOL WINAPI SetScrollRange(HWND hWnd, int nBar, int nMinPos, int nMaxPos, BOOL bRedraw);
SXW_USER32_API BOOL WINAPI GetScrollRange(HWND hWnd, int nBar, int *lpMinPos, int *lpMaxPos);
SXW_USER32_API BOOL WINAPI ShowScrollBar(HWND hWnd, int wBar, BOOL bShow);
SXW_USER32_API BOOL WINAPI EnableScrollBar(HWND hWnd, UINT wSBflags, UINT wArrows);
SXW_USER32_API HMENU WINAPI CreateMenu(void);
SXW_USER32_API HMENU WINAPI CreatePopupMenu(void);
SXW_USER32_API BOOL WINAPI DestroyMenu(HMENU hMenu);
SXW_USER32_API BOOL WINAPI IsMenu(HMENU hMenu);
SXW_USER32_API HMENU WINAPI LoadMenuW(HINSTANCE hInstance, LPCWSTR lpMenuName);
SXW_USER32_API BOOL WINAPI AppendMenuW(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem);
SXW_USER32_API BOOL WINAPI InsertMenuW(HMENU hMenu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem,
                                       LPCWSTR lpNewItem);
SXW_USER32_API BOOL WINAPI ModifyMenuW(HMENU hMnu, UINT uPosition, UINT uFlags, UINT_PTR uIDNewItem,
                                       LPCWSTR lpNewItem);
SXW_USER32_API BOOL WINAPI DeleteMenu(HMENU hMenu, UINT uPosition, UINT uFlags);
SXW_USER32_API BOOL WINAPI RemoveMenu(HMENU hMenu, UINT uPosition, UINT uFlags);
SXW_USER32_API DWORD WINAPI CheckMenuItem(HMENU hMenu, UINT uIDCheckItem, UINT uCheck);
SXW_USER32_API UINT WINAPI GetMenuState(HMENU hMenu, UINT uId, UINT uFlags);
SXW_USER32_API int WINAPI GetMenuStringW(HMENU hMenu, UINT uIDItem, LPWSTR lpString, int cchMax, UINT flags);
SXW_USER32_API int WINAPI GetMenuItemCount(HMENU hMenu);
SXW_USER32_API UINT WINAPI GetMenuItemID(HMENU hMenu, int nPos);
SXW_USER32_API HMENU WINAPI GetSubMenu(HMENU hMenu, int nPos);
SXW_USER32_API BOOL WINAPI SetMenu(HWND hWnd, HMENU hMenu);
SXW_USER32_API BOOL WINAPI DrawMenuBar(HWND hWnd);
SXW_USER32_API HMENU WINAPI GetSystemMenu(HWND hWnd, BOOL bRevert);
SXW_USER32_API LONG WINAPI GetMenuCheckMarkDimensions(void);
SXW_USER32_API BOOL WINAPI SetMenuItemBitmaps(HMENU hMenu, UINT uPosition, UINT uFlags, HBITMAP hBitmapUnchecked,
                                              HBITMAP hBitmapChecked);
SXW_USER32_API BOOL WINAPI TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd,
                                          const RECT *prcRect);
SXW_USER32_API HWND WINAPI CreateDialogIndirectParamW(HINSTANCE hInstance, const DLGTEMPLATE *lpTemplate,
                                                      HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
SXW_USER32_API HWND WINAPI CreateDialogParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent,
                                              DLGPROC lpDialogFunc, LPARAM dwInitParam);
SXW_USER32_API INT_PTR WINAPI DialogBoxIndirectParamW(HINSTANCE hInstance, const DLGTEMPLATE *hDialogTemplate,
                                                      HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam);
SXW_USER32_API BOOL WINAPI IsDialogMessageW(HWND hDlg, LPMSG lpMsg);
SXW_USER32_API LRESULT WINAPI DefDlgProcW(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam);
SXW_USER32_API BOOL WINAPI MapDialogRect(HWND hDlg, LPRECT lpRect);
SXW_USER32_API LONG WINAPI GetDialogBaseUnits(void);
SXW_USER32_API HWND WINAPI GetNextDlgTabItem(HWND hDlg, HWND hCtl, BOOL bPrevious);
SXW_USER32_API HWND WINAPI GetNextDlgGroupItem(HWND hDlg, HWND hCtl, BOOL bPrevious);
SXW_USER32_API LRESULT WINAPI SendDlgItemMessageW(HWND hDlg, int nIDDlgItem, UINT Msg, WPARAM wParam,
                                                  LPARAM lParam);
SXW_USER32_API BOOL WINAPI SetDlgItemTextW(HWND hDlg, int nIDDlgItem, LPCWSTR lpString);
SXW_USER32_API UINT WINAPI GetDlgItemTextW(HWND hDlg, int nIDDlgItem, LPWSTR lpString, int cchMax);
SXW_USER32_API BOOL WINAPI SetDlgItemInt(HWND hDlg, int nIDDlgItem, UINT uValue, BOOL bSigned);
SXW_USER32_API UINT WINAPI GetDlgItemInt(HWND hDlg, int nIDDlgItem, BOOL *lpTranslated, BOOL bSigned);
SXW_USER32_API int WINAPI FillRect(HDC hDC, const RECT *lprc, HBRUSH hbr);
SXW_USER32_API BOOL WINAPI DrawEdge(HDC hdc, LPRECT qrc, UINT edge, UINT grfFlags);
SXW_USER32_API BOOL WINAPI DrawFocusRect(HDC hDC, const RECT *lprc);
SXW_USER32_API HBRUSH WINAPI GetSysColorBrush(int nIndex);
SXW_USER32_API BOOL WINAPI OpenClipboard(HWND hWndNewOwner);
SXW_USER32_API BOOL WINAPI CloseClipboard(void);
SXW_USER32_API BOOL WINAPI EmptyClipboard(void);
SXW_USER32_API HANDLE WINAPI SetClipboardData(UINT uFormat, HANDLE hMem);
SXW_USER32_API HANDLE WINAPI GetClipboardData(UINT uFormat);
SXW_USER32_API BOOL WINAPI IsClipboardFormatAvailable(UINT format);
SXW_USER32_API UINT WINAPI EnumClipboardFormats(UINT format);
SXW_USER32_API int WINAPI CountClipboardFormats(void);
SXW_USER32_API UINT WINAPI RegisterClipboardFormatW(LPCWSTR lpszFormat);
SXW_USER32_API int WINAPI GetClipboardFormatNameW(UINT format, LPWSTR lpszFormatName, int cchMaxCount);
SXW_USER32_API DWORD WINAPI GetClipboardSequenceNumber(void);
SXW_USER32_API HWND WINAPI GetClipboardOwner(void);
SXW_USER32_API HWND WINAPI GetOpenClipboardWindow(void);

#ifndef SXW_BUILDING_SHELL32
#define SXW_SHELL32_API DECLSPEC_IMPORT
#else
#define SXW_SHELL32_API
#endif

SXW_SHELL32_API int WINAPI ShellAboutW(HWND hWnd, LPCWSTR szApp, LPCWSTR szOtherStuff, HICON hIcon);
SXW_SHELL32_API void WINAPI DragAcceptFiles(HWND hWnd, BOOL fAccept);
SXW_SHELL32_API UINT WINAPI DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch);
SXW_SHELL32_API BOOL WINAPI DragQueryPoint(HDROP hDrop, LPPOINT ppt);
SXW_SHELL32_API void WINAPI DragFinish(HDROP hDrop);

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
SXW_GDI32_API int WINAPI SetBkMode(HDC hdc, int mode);
SXW_GDI32_API COLORREF WINAPI GetBkColor(HDC hdc);
SXW_GDI32_API COLORREF WINAPI GetTextColor(HDC hdc);
SXW_GDI32_API HFONT WINAPI CreateFontIndirectW(const LOGFONTW *lplf);
SXW_GDI32_API int WINAPI AddFontResourceW(LPCWSTR name);
SXW_GDI32_API int WINAPI GetObjectW(HANDLE h, int c, LPVOID pv);
SXW_GDI32_API BOOL WINAPI ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT *lprect, LPCWSTR lpString,
                                     UINT c, const INT *lpDx);
SXW_GDI32_API BOOL WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT options, const RECT *lprect, LPCSTR lpString,
                                     UINT c, const INT *lpDx);
SXW_GDI32_API BOOL WINAPI GetCharWidthW(HDC hdc, UINT iFirst, UINT iLast, LPINT lpBuffer);
SXW_GDI32_API BOOL WINAPI GetCharWidthA(HDC hdc, UINT iFirst, UINT iLast, LPINT lpBuffer);
SXW_GDI32_API BOOL WINAPI GetTextExtentPoint32A(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl);
SXW_GDI32_API int WINAPI GetTextFaceW(HDC hdc, int c, LPWSTR lpName);
SXW_GDI32_API UINT WINAPI GetOutlineTextMetricsW(HDC hdc, UINT cjCopy, LPOUTLINETEXTMETRICW potm);
SXW_GDI32_API int WINAPI GetTextCharsetInfo(HDC hdc, LPFONTSIGNATURE lpSig, DWORD dwFlags);
SXW_GDI32_API int WINAPI EnumFontFamiliesExW(HDC hdc, LOGFONTW *lpLogfont, FONTENUMPROCW lpProc, LPARAM lParam,
                                            DWORD dwFlags);
SXW_GDI32_API UINT WINAPI SetTextAlign(HDC hdc, UINT align);
SXW_GDI32_API UINT WINAPI GetTextAlign(HDC hdc);
SXW_GDI32_API BOOL WINAPI GetCurrentPositionEx(HDC hdc, LPPOINT lppt);
SXW_GDI32_API BOOL WINAPI Rectangle(HDC hdc, int left, int top, int right, int bottom);
SXW_GDI32_API HPEN WINAPI CreatePen(int iStyle, int cWidth, COLORREF color);
SXW_GDI32_API HBITMAP WINAPI CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount,
                                         const void *lpBits);
SXW_GDI32_API int WINAPI StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight, int xSrc,
                                      int ySrc, int SrcWidth, int SrcHeight, const void *lpBits,
                                      const BITMAPINFO *lpbmi, UINT iUsage, DWORD rop);

#endif
