/*
 * controls.c -- The system's Button and Static classes, the controls a
 * dialog template of the profile holds (docs/userland/WIN32.md, "Dialogs and
 * controls"): push and default push buttons, check boxes, radio buttons,
 * group boxes, owner-drawn buttons, and left-, centre- and right-aligned
 * static text. They look like Windows XP's classic controls; the glyphs and
 * frames were read back from Windows XP pixel by pixel.
 *
 * A control asks its parent for its colours (WM_CTLCOLORBTN for push and
 * owner-drawn buttons, WM_CTLCOLORSTATIC for the others) and tells it what
 * happened with WM_COMMAND, as Windows does.
 */
#include "user32_private.h"

#define ATOM_BUTTON 0x0080
#define ATOM_STATIC 0x0082
#define SS_NOTIFY 0x0100L
#define HTTRANSPARENT (-1)
#define DT_HIDEPREFIX 0x00100000u

/* control_state: the button's BST_ bits, and while a press is being
 * followed, PRESSED. */
#define BUTTON_STATES 0x000fu
#define BUTTON_PRESSED 0x0100u
#define BUTTON_IN_CLICK 0x0200u /* inside BM_CLICK */

/* ---- Drawing ----------------------------------------------------------------------------- */

static void line(HDC dc, int x, int y, int width, int height, COLORREF color) {
    RECT r = {x, y, x + width, y + height};
    fill_rect(dc, &r, color);
}

/* One frame, then another inside it unless its colours are CLR_INVALID;
 * the bottom and right lines are drawn last and run the whole length, as
 * DrawEdge draws them. r becomes the inside. */
void draw_edge(HDC dc, RECT *r, COLORREF top_left_outer, COLORREF bottom_right_outer, COLORREF top_left_inner,
               COLORREF bottom_right_inner) {
    for (int pass = 0; pass < 2; ++pass) {
        const COLORREF tl = pass == 0 ? top_left_outer : top_left_inner;
        const COLORREF br = pass == 0 ? bottom_right_outer : bottom_right_inner;
        if (tl == CLR_INVALID) {
            return;
        }
        const int width = r->right - r->left;
        const int height = r->bottom - r->top;
        if (width <= 0 || height <= 0) {
            return;
        }
        line(dc, r->left, r->top, width, 1, tl);
        line(dc, r->left, r->top, 1, height, tl);
        line(dc, r->left, r->bottom - 1, width, 1, br);
        line(dc, r->right - 1, r->top, 1, height, br);
        r->left += 1;
        r->top += 1;
        r->right -= 1;
        r->bottom -= 1;
    }
}

/* DrawFocusRect: PATINVERT with Windows' checkered monochrome brush, which
 * on a colour device is the background colour where its bits are set (the
 * outline's pixels at odd x + y) and the text colour elsewhere. */
static void focus_pixel(HDC dc, int x, int y, COLORREF dot, COLORREF other) {
    const COLORREF with = ((x + y) & 1) != 0 ? dot : other;
    if (with != 0) {
        SetPixel(dc, x, y, GetPixel(dc, x, y) ^ with);
    }
}

void draw_focus_rect(HDC dc, const RECT *r) {
    const COLORREF dot = SetBkColor(dc, 0);
    SetBkColor(dc, dot);
    const COLORREF other = SetTextColor(dc, 0);
    SetTextColor(dc, other);
    for (int x = r->left; x < r->right; ++x) {
        focus_pixel(dc, x, r->top, dot, other);
        if (r->bottom - 1 > r->top) {
            focus_pixel(dc, x, r->bottom - 1, dot, other);
        }
    }
    for (int y = r->top + 1; y < r->bottom - 1; ++y) {
        focus_pixel(dc, r->left, y, dot, other);
        if (r->right - 1 > r->left) {
            focus_pixel(dc, r->right - 1, y, dot, other);
        }
    }
}

/* Where a button's label starts, top down: a line one pixel taller than the
 * font, centred, as Windows XP places it. */
static int label_top(window *w, HDC dc) {
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    return (client_height(w) - (tm.tmHeight + 1)) / 2;
}

/* DrawTextW in the text colour already selected, or grayed the classic way
 * (an etched copy in the highlight colour, then the shadow colour). The
 * text's width. */
int draw_label(HDC dc, const WCHAR *text, RECT *r, UINT format, int disabled, int hide_prefix) {
    if (hide_prefix) {
        format |= DT_HIDEPREFIX;
    }
    if ((format & DT_CALCRECT) != 0) {
        DrawTextW(dc, text, -1, r, format);
        return r->right - r->left;
    }
    RECT size = *r;
    DrawTextW(dc, text, -1, &size, format | DT_CALCRECT);
    if (disabled) {
        RECT shadow = *r;
        shadow.left += 1;
        shadow.top += 1;
        shadow.right += 1;
        shadow.bottom += 1;
        const COLORREF old = SetTextColor(dc, GetSysColor(COLOR_BTNHIGHLIGHT));
        DrawTextW(dc, text, -1, &shadow, format);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(dc, text, -1, r, format);
        SetTextColor(dc, old);
    } else {
        DrawTextW(dc, text, -1, r, format);
    }
    return size.right - size.left;
}

/* ---- Buttons ------------------------------------------------------------------------------- */

static UINT button_type(const window *w) {
    return (UINT)(w->style & BS_TYPEMASK);
}

static void notify(window *w, WORD code) {
    if (w->parent != 0) {
        send(w->parent, WM_COMMAND, MAKELPARAM(w->id, code), (LPARAM)handle_of(w));
    }
}

static u32 cues(window *w) {
    return top_of(w)->ui_state;
}

/* The font the parent gave, the dialog font, selected into dc. */
static void prepare(window *w, HDC dc) {
    if (w->font != NULL) {
        SelectObject(dc, w->font);
    }
    SxwSetBkMode(dc, TRANSPARENT);
}

static HBRUSH control_brush(window *w, HDC dc, UINT message) {
    HBRUSH brush = (HBRUSH)send(w->parent != 0 ? w->parent : w, message, (WPARAM)dc, (LPARAM)handle_of(w));
    if (brush == NULL) {
        brush = (HBRUSH)default_control_color(message, dc);
    }
    return brush;
}

/* The classic radio button, 13 columns by 12 rows from the top of its box:
 * '8' shadow, '4' dark shadow, 'w' highlight, 'l' light, 'f' the face (the
 * window colour), 'k' the dot of a checked one, '.' left alone. */
static const char k_radio[12][14] = {
    ".....8888....", "...88444488..", "..844ffff44w.", "..84fffffflw.", ".84fffkkffflw", ".84ffkkkkfflw",
    ".84ffkkkkfflw", ".84fffkkffflw", "..84fffffflw.", "..8llffffllw.", "...wwllllww..", ".....wwww....",
};

/* The check mark in a check box, 7 by 7 from (3, 3) of the box. */
static const char k_check[7][8] = {
    "......x", ".....xx", "x...xxx", "xx.xxx.", "xxxxx..", ".xxx...", "..x....",
};

static void draw_glyph(HDC dc, int left, int top, int radio, int checked, int face_gray, int disabled) {
    if (radio) {
        for (int row = 0; row < 12; ++row) {
            for (int col = 0; col < 13; ++col) {
                const char c = k_radio[row][col];
                COLORREF color;
                switch (c) {
                case '8': color = GetSysColor(COLOR_BTNSHADOW); break;
                case '4': color = GetSysColor(COLOR_3DDKSHADOW); break;
                case 'w': color = GetSysColor(COLOR_BTNHIGHLIGHT); break;
                case 'l': color = GetSysColor(COLOR_3DLIGHT); break;
                case 'f': color = GetSysColor(face_gray ? COLOR_BTNFACE : COLOR_WINDOW); break;
                case 'k':
                    color = checked ? GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT)
                                    : GetSysColor(face_gray ? COLOR_BTNFACE : COLOR_WINDOW);
                    break;
                default: continue;
                }
                SetPixel(dc, left + col, top + row, color);
            }
        }
        return;
    }
    RECT box = {left, top, left + 13, top + 13};
    draw_edge(dc, &box, GetSysColor(COLOR_BTNSHADOW), GetSysColor(COLOR_BTNHIGHLIGHT),
              GetSysColor(COLOR_3DDKSHADOW), GetSysColor(COLOR_3DLIGHT));
    fill_rect(dc, &box, GetSysColor(face_gray ? COLOR_BTNFACE : COLOR_WINDOW));
    if (checked) {
        const COLORREF mark = GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 7; ++col) {
                if (k_check[row][col] == 'x') {
                    SetPixel(dc, left + 3 + col, top + 3 + row, mark);
                }
            }
        }
    }
}

static int is_default(const window *w) {
    return button_type(w) == BS_DEFPUSHBUTTON;
}

static void paint_push(window *w, HDC dc) {
    control_brush(w, dc, WM_CTLCOLORBTN);
    RECT r = {0, 0, client_width(w), client_height(w)};
    const int pushed = (w->control_state & BST_PUSHED) != 0;
    if (is_default(w) || pushed) {
        RECT outer = r;
        draw_edge(dc, &outer, GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_WINDOWTEXT), CLR_INVALID,
                  CLR_INVALID);
        r = outer;
    }
    if (pushed) {
        RECT inner = r;
        draw_edge(dc, &inner, GetSysColor(COLOR_BTNSHADOW), GetSysColor(COLOR_BTNSHADOW), CLR_INVALID, CLR_INVALID);
        fill_rect(dc, &inner, GetSysColor(COLOR_BTNFACE));
    } else {
        RECT inner = r;
        draw_edge(dc, &inner, GetSysColor(COLOR_BTNHIGHLIGHT), GetSysColor(COLOR_3DDKSHADOW),
                  GetSysColor(COLOR_3DLIGHT), GetSysColor(COLOR_BTNSHADOW));
        fill_rect(dc, &inner, GetSysColor(COLOR_BTNFACE));
    }
    RECT text = {0, label_top(w, dc), client_width(w), client_height(w)};
    if (pushed) {
        text.left += 1;
        text.top += 1;
        text.right += 1;
        text.bottom += 1;
    }
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    draw_label(dc, w->text, &text, DT_CENTER | DT_TOP | DT_SINGLELINE, (w->style & WS_DISABLED) != 0,
               (cues(w) & UISF_HIDEACCEL) != 0);
    if ((w->control_state & BST_FOCUS) != 0 && (cues(w) & UISF_HIDEFOCUS) == 0) {
        RECT focus = {4, 4, client_width(w) - 4, client_height(w) - 4};
        draw_focus_rect(dc, &focus);
    }
}

static void paint_check(window *w, HDC dc) {
    HBRUSH brush = control_brush(w, dc, WM_CTLCOLORSTATIC);
    RECT all = {0, 0, client_width(w), client_height(w)};
    fill_brush(dc, &all, brush);
    const UINT type = button_type(w);
    const int radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const int disabled = (w->style & WS_DISABLED) != 0;
    const int top = (client_height(w) - 13) / 2;
    draw_glyph(dc, 0, top, radio, (w->control_state & BST_CHECKED) != 0,
               disabled || (w->control_state & BST_PUSHED) != 0, disabled);
    const int line_top = label_top(w, dc);
    RECT text = {18, line_top, client_width(w), client_height(w)};
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    draw_label(dc, w->text, &text, DT_LEFT | DT_TOP | DT_SINGLELINE, disabled, (cues(w) & UISF_HIDEACCEL) != 0);
    if ((w->control_state & BST_FOCUS) != 0 && (cues(w) & UISF_HIDEFOCUS) == 0 && w->text[0] != 0) {
        RECT size = {0, 0, 0, 0};
        DrawTextW(dc, w->text, -1, &size, DT_CALCRECT | DT_SINGLELINE);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        RECT focus = {17, line_top - 1, 18 + size.right + 1, line_top + tm.tmHeight + 2};
        draw_focus_rect(dc, &focus);
    }
}

static void paint_group(window *w, HDC dc) {
    HBRUSH brush = control_brush(w, dc, WM_CTLCOLORSTATIC);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    RECT frame = {0, tm.tmHeight / 2, client_width(w), client_height(w)};
    draw_edge(dc, &frame, GetSysColor(COLOR_BTNSHADOW), GetSysColor(COLOR_BTNHIGHLIGHT),
              GetSysColor(COLOR_BTNHIGHLIGHT), GetSysColor(COLOR_BTNSHADOW));
    if (w->text[0] == 0) {
        return;
    }
    RECT size = {0, 0, 0, 0};
    DrawTextW(dc, w->text, -1, &size, DT_CALCRECT | DT_SINGLELINE);
    RECT label = {7, 0, 7 + size.right + 4, size.bottom};
    fill_brush(dc, &label, brush);
    label.left += 2;
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    draw_label(dc, w->text, &label, DT_LEFT | DT_SINGLELINE, (w->style & WS_DISABLED) != 0,
               (cues(w) & UISF_HIDEACCEL) != 0);
}

static void paint_owner(window *w, HDC dc, UINT action) {
    control_brush(w, dc, WM_CTLCOLORBTN);
    DRAWITEMSTRUCT d;
    zero(&d, sizeof(d));
    d.CtlType = ODT_BUTTON;
    d.CtlID = (UINT)w->id;
    d.itemAction = action;
    d.itemState = ((w->control_state & BST_PUSHED) != 0 ? ODS_SELECTED : 0) |
                  ((w->control_state & BST_FOCUS) != 0 ? ODS_FOCUS : 0) |
                  ((w->style & WS_DISABLED) != 0 ? ODS_DISABLED : 0) |
                  ((cues(w) & UISF_HIDEFOCUS) != 0 ? ODS_NOFOCUSRECT : 0) |
                  ((cues(w) & UISF_HIDEACCEL) != 0 ? ODS_NOACCEL : 0);
    d.hwndItem = handle_of(w);
    d.hDC = dc;
    d.rcItem.right = client_width(w);
    d.rcItem.bottom = client_height(w);
    if (w->parent != 0) {
        send(w->parent, WM_DRAWITEM, (WPARAM)w->id, (LPARAM)&d);
    }
}

static void paint_button(window *w, HDC dc, UINT action) {
    prepare(w, dc);
    switch (button_type(w)) {
    case BS_PUSHBUTTON:
    case BS_DEFPUSHBUTTON:
    case BS_USERBUTTON:
        paint_push(w, dc);
        break;
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
    case BS_RADIOBUTTON:
    case BS_AUTORADIOBUTTON:
    case BS_3STATE:
    case BS_AUTO3STATE:
        paint_check(w, dc);
        break;
    case BS_GROUPBOX:
        paint_group(w, dc);
        break;
    case BS_OWNERDRAW:
        paint_owner(w, dc, action);
        break;
    default:
        break;
    }
}

/* A change shows at once, as Windows paints a button's state changes
 * outside WM_PAINT. */
static void repaint(window *w, UINT action) {
    if (!visible_chain(w)) {
        return;
    }
    HDC dc = window_dc(w, 0);
    paint_button(w, dc, action);
    DeleteObject(dc);
}

static void set_state(window *w, u32 state, UINT action) {
    if ((w->control_state & BUTTON_STATES) != (state & BUTTON_STATES)) {
        w->control_state = (w->control_state & ~BUTTON_STATES) | (state & BUTTON_STATES);
        repaint(w, action);
    }
}

static void release(window *w, int inside) {
    const u32 state = w->control_state;
    if ((state & BUTTON_PRESSED) == 0) {
        return;
    }
    w->control_state &= ~BUTTON_PRESSED;
    if ((state & BST_PUSHED) == 0) {
        ReleaseCapture();
        return;
    }
    send(w, BM_SETSTATE, FALSE, 0);
    if (!inside) {
        ReleaseCapture();
        return;
    }
    switch (button_type(w)) {
    case BS_AUTOCHECKBOX:
        send(w, BM_SETCHECK, (state & BST_CHECKED) == 0, 0);
        break;
    case BS_AUTORADIOBUTTON:
        send(w, BM_SETCHECK, TRUE, 0);
        break;
    case BS_AUTO3STATE:
        send(w, BM_SETCHECK, ((state & 3u) + 1u) % 3u, 0);
        break;
    default:
        break;
    }
    ReleaseCapture();
    notify(w, BN_CLICKED);
}

static LRESULT WINAPI button_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    const UINT type = button_type(w);
    switch (message) {
    case WM_NCHITTEST:
        return type == BS_GROUPBOX ? HTTRANSPARENT : HTCLIENT;
    case WM_GETDLGCODE:
        switch (type) {
        case BS_PUSHBUTTON:
        case BS_USERBUTTON:
            return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        case BS_DEFPUSHBUTTON:
            return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        case BS_RADIOBUTTON:
        case BS_AUTORADIOBUTTON:
            return DLGC_BUTTON | DLGC_RADIOBUTTON;
        case BS_GROUPBOX:
            return DLGC_STATIC;
        default:
            return DLGC_BUTTON;
        }
    case WM_SETFONT:
        w->font = (HFONT)wParam;
        if (lParam != 0) {
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_GETFONT:
        return (LRESULT)w->font;
    case WM_ENABLE:
    case WM_SETTEXT: {
        const LRESULT result = message == WM_SETTEXT ? DefWindowProcW(hwnd, message, wParam, lParam) : 0;
        repaint(w, ODA_DRAWENTIRE);
        return result;
    }
    case WM_ERASEBKGND:
        if (type == BS_OWNERDRAW) {
            HBRUSH brush = control_brush(w, (HDC)wParam, WM_CTLCOLORBTN);
            RECT r = {0, 0, client_width(w), client_height(w)};
            fill_brush((HDC)wParam, &r, brush);
        }
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_button(w, dc, ODA_DRAWENTIRE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON || type == BS_OWNERDRAW || type == BS_USERBUTTON) {
            notify(w, BN_DOUBLECLICKED);
            return 0;
        }
        /* fall through */
    case WM_LBUTTONDOWN:
        if (type == BS_GROUPBOX) {
            return 0;
        }
        SetCapture(hwnd);
        if (g_focus != w) {
            /* A click in a dialog settles its default button first; a
             * mnemonic's BM_CLICK leaves that to the dialog. */
            window *old = g_focus;
            window *dlg = w->parent;
            if ((w->control_state & BUTTON_IN_CLICK) == 0 && dlg != 0 && dlg->dialog != 0) {
                dialog_check_default(dlg, old, w, 1);
            }
            SetFocus(hwnd);
        }
        w->control_state |= BUTTON_PRESSED;
        send(w, BM_SETSTATE, TRUE, 0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE && type != BS_GROUPBOX) {
            send(w, BM_SETSTATE, TRUE, 0);
            w->control_state |= BUTTON_PRESSED;
            SetCapture(hwnd);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if ((w->control_state & BUTTON_PRESSED) != 0 && g_capture == w) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int inside = x >= 0 && y >= 0 && x < client_width(w) && y < client_height(w);
            send(w, BM_SETSTATE, inside, 0);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        release(w, x >= 0 && y >= 0 && x < client_width(w) && y < client_height(w));
        return 0;
    }
    case WM_KEYUP:
        if (wParam == VK_SPACE) {
            release(w, 1);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if ((w->control_state & BUTTON_PRESSED) != 0 && (HWND)lParam != hwnd) {
            w->control_state &= ~BUTTON_PRESSED;
            if ((w->control_state & BST_PUSHED) != 0) {
                send(w, BM_SETSTATE, FALSE, 0);
            }
        }
        return 0;
    case WM_SETFOCUS:
        set_state(w, w->control_state | BST_FOCUS, ODA_FOCUS);
        /* A radio button the keyboard moves to is clicked. */
        if ((type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) && g_capture != w &&
            (w->control_state & BST_CHECKED) == 0) {
            notify(w, BN_CLICKED);
        }
        return 0;
    case WM_KILLFOCUS:
        if ((w->control_state & BUTTON_PRESSED) != 0 && g_capture == w) {
            ReleaseCapture();
        }
        set_state(w, w->control_state & ~(u32)BST_FOCUS, ODA_FOCUS);
        return 0;
    case WM_UPDATEUISTATE: {
        const u32 before = top_of(w)->ui_state;
        DefWindowProcW(hwnd, message, wParam, lParam);
        if (top_of(w)->ui_state != before) {
            repaint(w, ODA_DRAWENTIRE);
        }
        return 0;
    }
    case BM_GETCHECK:
        return w->control_state & 3u;
    case BM_SETCHECK: {
        if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
            /* A checked radio button is where Tab stops in its group. */
            if (wParam != 0) {
                w->style |= WS_TABSTOP;
            } else {
                w->style &= ~(DWORD)WS_TABSTOP;
            }
        }
        const u32 check = type == BS_3STATE || type == BS_AUTO3STATE ? (u32)wParam & 3u : (u32)(wParam != 0);
        set_state(w, (w->control_state & ~3u) | check, ODA_SELECT);
        return 0;
    }
    case BM_GETSTATE:
        return w->control_state & BUTTON_STATES;
    case BM_SETSTATE:
        set_state(w, wParam ? w->control_state | BST_PUSHED : w->control_state & ~(u32)BST_PUSHED, ODA_SELECT);
        return 0;
    case BM_SETSTYLE:
        w->style = (w->style & ~(DWORD)BS_TYPEMASK) | ((DWORD)wParam & BS_TYPEMASK);
        if (lParam != 0) {
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case BM_CLICK:
        if ((w->control_state & BUTTON_IN_CLICK) != 0) {
            return 0;
        }
        w->control_state |= BUTTON_IN_CLICK;
        send(w, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        send(w, WM_LBUTTONUP, 0, 0);
        if (window_of(hwnd) != 0) {
            w->control_state &= ~BUTTON_IN_CLICK;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/* ---- Static text ----------------------------------------------------------------------------- */

static LRESULT WINAPI static_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    window *w = window_of(hwnd);
    if (w == 0) {
        return 0;
    }
    switch (message) {
    case WM_NCHITTEST:
        return (w->style & SS_NOTIFY) != 0 ? HTCLIENT : HTTRANSPARENT;
    case WM_GETDLGCODE:
        return DLGC_STATIC;
    case WM_SETFONT:
        w->font = (HFONT)wParam;
        if (lParam != 0) {
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_GETFONT:
        return (LRESULT)w->font;
    case WM_ENABLE:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_SETTEXT: {
        const LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
        InvalidateRect(hwnd, NULL, TRUE);
        return result;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        prepare(w, dc);
        HBRUSH brush = control_brush(w, dc, WM_CTLCOLORSTATIC);
        RECT r = {0, 0, client_width(w), client_height(w)};
        fill_brush(dc, &r, brush);
        UINT format = DT_WORDBREAK | DT_EXPANDTABS;
        switch (w->style & SS_TYPEMASK) {
        case SS_CENTER:
            format |= DT_CENTER;
            break;
        case SS_RIGHT:
            format |= DT_RIGHT;
            break;
        default:
            format |= DT_LEFT;
            break;
        }
        if ((w->style & SS_NOPREFIX) != 0) {
            format |= DT_NOPREFIX;
        }
        draw_label(dc, w->text, &r, format, (w->style & WS_DISABLED) != 0, (cues(w) & UISF_HIDEACCEL) != 0);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int control_transparent(const window *w) {
    if (w->cls->atom == ATOM_BUTTON) {
        return (w->style & BS_TYPEMASK) == BS_GROUPBOX;
    }
    return w->cls->atom == ATOM_STATIC && (w->style & SS_NOTIFY) == 0;
}

void register_controls(void) {
    static const WCHAR k_button[] = {'B', 'u', 't', 't', 'o', 'n', 0};
    static const WCHAR k_static[] = {'S', 't', 'a', 't', 'i', 'c', 0};
    static const WCHAR k_dialog[] = {'#', '3', '2', '7', '7', '0', 0};
    register_system_class(CS_DBLCLKS | CS_PARENTDC, button_proc, k_button, ATOM_BUTTON, NULL);
    register_system_class(CS_PARENTDC, static_proc, k_static, ATOM_STATIC, NULL);
    register_system_class(CS_DBLCLKS | CS_SAVEBITS, (WNDPROC)DefDlgProcW, k_dialog, 0x8002, NULL);
}
