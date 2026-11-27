/*
 * richedit.h -- The RichEdit control's messages, notifications, styles and
 * structures, as msftedit.dll and the fixtures that drive it use them
 * (WIN32_WORDPAD_ROADMAP.md, WP7). The values are those of the Windows SDK;
 * like windows.h, it holds what is used here and nothing else.
 */
#ifndef SAVANXP_WIN32_RICHEDIT_H
#define SAVANXP_WIN32_RICHEDIT_H

#include <windows.h>

#define MSFTEDIT_CLASS L"RICHEDIT50W"

/* ---- Messages ---------------------------------------------------------- */

#define EM_CANPASTE (WM_USER + 50)
#define EM_DISPLAYBAND (WM_USER + 51)
#define EM_EXGETSEL (WM_USER + 52)
#define EM_EXLIMITTEXT (WM_USER + 53)
#define EM_EXLINEFROMCHAR (WM_USER + 54)
#define EM_EXSETSEL (WM_USER + 55)
#define EM_FINDTEXT (WM_USER + 56)
#define EM_FORMATRANGE (WM_USER + 57)
#define EM_GETCHARFORMAT (WM_USER + 58)
#define EM_GETEVENTMASK (WM_USER + 59)
#define EM_GETOLEINTERFACE (WM_USER + 60)
#define EM_GETPARAFORMAT (WM_USER + 61)
#define EM_GETSELTEXT (WM_USER + 62)
#define EM_HIDESELECTION (WM_USER + 63)
#define EM_PASTESPECIAL (WM_USER + 64)
#define EM_REQUESTRESIZE (WM_USER + 65)
#define EM_SELECTIONTYPE (WM_USER + 66)
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#define EM_SETCHARFORMAT (WM_USER + 68)
#define EM_SETEVENTMASK (WM_USER + 69)
#define EM_SETOLECALLBACK (WM_USER + 70)
#define EM_SETPARAFORMAT (WM_USER + 71)
#define EM_SETTARGETDEVICE (WM_USER + 72)
#define EM_STREAMIN (WM_USER + 73)
#define EM_STREAMOUT (WM_USER + 74)
#define EM_GETTEXTRANGE (WM_USER + 75)
#define EM_FINDWORDBREAK (WM_USER + 76)
#define EM_SETOPTIONS (WM_USER + 77)
#define EM_GETOPTIONS (WM_USER + 78)
#define EM_FINDTEXTEX (WM_USER + 79)
#define EM_SETUNDOLIMIT (WM_USER + 82)
#define EM_REDO (WM_USER + 84)
#define EM_CANREDO (WM_USER + 85)
#define EM_GETUNDONAME (WM_USER + 86)
#define EM_GETREDONAME (WM_USER + 87)
#define EM_STOPGROUPTYPING (WM_USER + 88)
#define EM_SETTEXTMODE (WM_USER + 89)
#define EM_GETTEXTMODE (WM_USER + 90)
#define EM_AUTOURLDETECT (WM_USER + 91)
#define EM_GETAUTOURLDETECT (WM_USER + 92)
#define EM_GETTEXTEX (WM_USER + 94)
#define EM_GETTEXTLENGTHEX (WM_USER + 95)
#define EM_SHOWSCROLLBAR (WM_USER + 96)
#define EM_SETTEXTEX (WM_USER + 97)
#define EM_SETWORDWRAPMODE (WM_USER + 102)
#define EM_GETWORDWRAPMODE (WM_USER + 103)
#define EM_SETLANGOPTIONS (WM_USER + 120)
#define EM_GETLANGOPTIONS (WM_USER + 121)
#define EM_FINDTEXTW (WM_USER + 123)
#define EM_FINDTEXTEXW (WM_USER + 124)
#define EM_SETEDITSTYLE (WM_USER + 204)
#define EM_GETEDITSTYLE (WM_USER + 205)
#define EM_GETSCROLLPOS (WM_USER + 221)
#define EM_SETSCROLLPOS (WM_USER + 222)
#define EM_GETZOOM (WM_USER + 224)
#define EM_SETZOOM (WM_USER + 225)

/* ---- Notifications ----------------------------------------------------- */

#define EN_MSGFILTER 0x0700
#define EN_REQUESTRESIZE 0x0701
#define EN_SELCHANGE 0x0702
#define EN_DROPFILES 0x0703
#define EN_PROTECTED 0x0704
#define EN_CORRECTTEXT 0x0705
#define EN_STOPNOUNDO 0x0706
#define EN_IMECHANGE 0x0707
#define EN_SAVECLIPBOARD 0x0708
#define EN_OLEOPFAILED 0x0709
#define EN_OBJECTPOSITIONS 0x070a
#define EN_LINK 0x070b
#define EN_DRAGDROPDONE 0x070c

#define ENM_NONE 0x00000000
#define ENM_CHANGE 0x00000001
#define ENM_UPDATE 0x00000002
#define ENM_SCROLL 0x00000004
#define ENM_SCROLLEVENTS 0x00000008
#define ENM_DRAGDROPDONE 0x00000010
#define ENM_KEYEVENTS 0x00010000
#define ENM_MOUSEEVENTS 0x00020000
#define ENM_REQUESTRESIZE 0x00040000
#define ENM_SELCHANGE 0x00080000
#define ENM_DROPFILES 0x00100000
#define ENM_PROTECTED 0x00200000

/* ---- Styles and options ------------------------------------------------ */

#define ES_NOOLEDRAGDROP 0x00000008
#define ES_DISABLENOSCROLL 0x00002000
#define ES_SUNKEN 0x00004000
#define ES_SAVESEL 0x00008000
#define ES_SELECTIONBAR 0x01000000

#define ECOOP_SET 1
#define ECOOP_OR 2
#define ECOOP_AND 3
#define ECOOP_XOR 4

#define ECO_AUTOWORDSELECTION 0x00000001
#define ECO_AUTOVSCROLL 0x00000040
#define ECO_AUTOHSCROLL 0x00000080
#define ECO_NOHIDESEL 0x00000100
#define ECO_READONLY 0x00000800
#define ECO_WANTRETURN 0x00001000
#define ECO_SAVESEL 0x00008000
#define ECO_SELECTIONBAR 0x01000000

#define TM_PLAINTEXT 1
#define TM_RICHTEXT 2
#define TM_SINGLELEVELUNDO 4
#define TM_MULTILEVELUNDO 8
#define TM_SINGLECODEPAGE 16
#define TM_MULTICODEPAGE 32

#define SEL_EMPTY 0x0000
#define SEL_TEXT 0x0001
#define SEL_OBJECT 0x0002
#define SEL_MULTICHAR 0x0004
#define SEL_MULTIOBJECT 0x0008

#define GTL_DEFAULT 0
#define GTL_USECRLF 1
#define GTL_PRECISE 2
#define GTL_CLOSE 4
#define GTL_NUMCHARS 8
#define GTL_NUMBYTES 16

#define GT_DEFAULT 0
#define GT_USECRLF 1
#define GT_SELECTION 2

#define ST_DEFAULT 0
#define ST_KEEPUNDO 1
#define ST_SELECTION 2

#define WB_CLASSIFY 3
#define WB_MOVEWORDLEFT 4
#define WB_MOVEWORDRIGHT 5
#define WB_LEFTBREAK 6
#define WB_RIGHTBREAK 7

/* ---- Structures -------------------------------------------------------- */

typedef struct _charrange {
    LONG cpMin;
    LONG cpMax;
} CHARRANGE;

typedef struct _textrangew {
    CHARRANGE chrg;
    LPWSTR lpstrText;
} TEXTRANGEW;

typedef struct _gettextex {
    DWORD cb;
    DWORD flags;
    UINT codepage;
    LPCSTR lpDefaultChar;
    BOOL *lpUsedDefChar;
} GETTEXTEX;

typedef struct _gettextlengthex {
    DWORD flags;
    UINT codepage;
} GETTEXTLENGTHEX;

typedef struct _settextex {
    DWORD flags;
    UINT codepage;
} SETTEXTEX;

typedef struct tagNMHDR {
    HWND hwndFrom;
    UINT_PTR idFrom;
    UINT code;
} NMHDR;

typedef struct _selchange {
    NMHDR nmhdr;
    CHARRANGE chrg;
    WORD seltyp;
} SELCHANGE;

typedef struct _msgfilter {
    NMHDR nmhdr;
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
} MSGFILTER;

typedef struct _ensaveclipboard {
    NMHDR nmhdr;
    LONG cObjectCount;
    LONG cch;
} ENSAVECLIPBOARD;

typedef struct _reqresize {
    NMHDR nmhdr;
    RECT rc;
} REQRESIZE;

typedef struct _POINTL {
    LONG x;
    LONG y;
} POINTL;

#endif
