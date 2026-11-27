/*
 * layout.c -- The RichEdit control's lines: the font's widths from gdi32,
 * paragraphs broken into lines as wide as the formatting rectangle, and
 * positions both ways, as Windows XP's msftedit.dll answered
 * fixtures/richedit.
 *
 * A line breaks after a run of spaces or a hyphen, the spaces staying at
 * its end however wide they are; a word wider than the line is broken
 * where it stops fitting. There is one font (WP7a), so every line is as
 * high as it.
 */
#include "msftedit_private.h"

void rich_measure_font(rich *r) {
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, r->font != NULL ? (HGDIOBJ)r->font : GetStockObject(SYSTEM_FONT));
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    r->line_height = tm.tmHeight > 0 ? (int)tm.tmHeight : 1;
    r->ascent = (int)tm.tmAscent;
    if (!GetCharWidthW(dc, 0, 255, r->widths)) {
        for (int i = 0; i < 256; ++i) {
            r->widths[i] = (int)tm.tmAveCharWidth;
        }
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
}

static int char_width(const rich *r, WCHAR c) {
    if (c < 256) {
        return r->widths[c];
    }
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, r->font != NULL ? (HGDIOBJ)r->font : GetStockObject(SYSTEM_FONT));
    int w = 0;
    if (!GetCharWidthW(dc, c, c, &w)) {
        w = r->widths['?'];
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return w;
}

/* The advance of c drawn at x, from the line's start: a tab goes to the
 * next stop. */
static int advance(const rich *r, WCHAR c, int x) {
    if (c == '\t') {
        return TAB_WIDTH - x % TAB_WIDTH;
    }
    return char_width(r, c);
}

void rich_format_rect(const rich *r, RECT *out) {
    RECT client;
    GetClientRect(r->hwnd, &client);
    out->left = client.left + r->insets.left;
    out->top = client.top + r->insets.top;
    out->right = client.right - r->insets.right;
    out->bottom = client.bottom - r->insets.bottom;
    if (out->right < out->left) {
        out->right = out->left;
    }
    if (out->bottom < out->top) {
        out->bottom = out->top;
    }
}

/* Where the text starts on screen, left of the horizontal scroll. */
int rich_left(const rich *r) {
    RECT f;
    rich_format_rect(r, &f);
    return f.left + ((r->options & ECO_SELECTIONBAR) != 0 ? SELECTION_BAR : 0) - r->scroll_x;
}

static int add_line(rich *r, int start, int length, int paragraph_end, int width) {
    if (r->line_count == r->line_capacity) {
        const int capacity = r->line_capacity == 0 ? 32 : r->line_capacity * 2;
        line_info *grown = rich_alloc((u32)capacity * sizeof(line_info));
        if (grown == 0) {
            return 0;
        }
        for (int i = 0; i < r->line_count; ++i) {
            grown[i] = r->lines[i];
        }
        rich_free(r->lines);
        r->lines = grown;
        r->line_capacity = capacity;
    }
    line_info *l = &r->lines[r->line_count];
    l->start = start;
    l->length = length;
    l->paragraph_end = paragraph_end;
    l->y = r->line_count * r->line_height;
    l->width = width;
    ++r->line_count;
    return 1;
}

/* The paragraph [p, q) as lines no wider than wrap (no limit when no_wrap). */
static void layout_paragraph(rich *r, int p, int q, int wrap) {
    if (p == q) {
        add_line(r, p, 0, 1, 0);
        return;
    }
    int pos = p;
    while (pos < q) {
        int x = 0;
        int i = pos;
        int last_break = -1; /* where the next line would start */
        int width_at_break = 0;
        while (i < q) {
            const WCHAR c = r->text[i];
            const int w = advance(r, c, x);
            if (c == ' ' || c == '\t') {
                x += w;
                ++i;
                last_break = i;
                width_at_break = x;
                continue;
            }
            if (!r->no_wrap && x + w > wrap && i > pos) {
                break;
            }
            x += w;
            ++i;
            if (c == '-') {
                last_break = i;
                width_at_break = x;
            }
        }
        if (i >= q) {
            add_line(r, pos, q - pos, 1, x);
            return;
        }
        int end = i;
        int width = x;
        if (last_break > pos) {
            end = last_break;
            width = width_at_break;
        }
        add_line(r, pos, end - pos, 0, width);
        pos = end;
    }
}

void rich_layout(rich *r) {
    RECT f;
    rich_format_rect(r, &f);
    int wrap = f.right - f.left - ((r->options & ECO_SELECTIONBAR) != 0 ? SELECTION_BAR : 0);
    if (wrap < 1) {
        wrap = 1;
    }
    r->line_count = 0;
    int p = 0;
    for (;;) {
        int q = p;
        while (q < r->length && r->text[q] != '\r') {
            ++q;
        }
        layout_paragraph(r, p, q, wrap);
        if (q >= r->length) {
            break;
        }
        p = q + 1;
        if (p == r->length) {
            /* A CR at the very end: the document's own paragraph follows. */
            add_line(r, p, 0, 1, 0);
            break;
        }
    }
    if (r->line_count == 0) {
        add_line(r, 0, 0, 1, 0);
    }
    r->content_height = r->line_count * r->line_height;
    r->content_width = 0;
    for (int i = 0; i < r->line_count; ++i) {
        if (r->lines[i].width > r->content_width) {
            r->content_width = r->lines[i].width;
        }
    }
}

/* The line that holds cp: a position where a line wraps belongs to the
 * line it starts. */
int rich_line_of(const rich *r, int cp) {
    if (cp < 0) {
        cp = 0;
    }
    int lo = 0;
    int hi = r->line_count - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (r->lines[mid].start <= cp) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

/* The x of the boundary after `count` characters of line l, from the
 * text's left. */
int rich_text_x(const rich *r, const line_info *l, int count) {
    int x = 0;
    for (int i = 0; i < count && i < l->length; ++i) {
        x += advance(r, r->text[l->start + i], x);
    }
    return x;
}

void rich_pos_of(const rich *r, int cp, POINT *out) {
    if (cp < 0) {
        cp = 0;
    }
    if (cp > r->length) {
        cp = r->length;
    }
    RECT f;
    rich_format_rect(r, &f);
    const line_info *l = &r->lines[rich_line_of(r, cp)];
    out->x = rich_left(r) + rich_text_x(r, l, cp - l->start);
    out->y = f.top + l->y - r->scroll_y;
}

/* The character position nearest the point: the line under y (the first or
 * last when outside), then the boundary nearest x in it. */
int rich_char_at(const rich *r, int x, int y) {
    RECT f;
    rich_format_rect(r, &f);
    const int ly = y - f.top + r->scroll_y;
    int index = ly < 0 ? 0 : ly / r->line_height;
    if (index >= r->line_count) {
        index = r->line_count - 1;
    }
    const line_info *l = &r->lines[index];
    const int xx = x - rich_left(r);
    int at = 0;
    for (int i = 0; i < l->length; ++i) {
        const int w = advance(r, r->text[l->start + i], at);
        if (xx < at + w / 2 + (w & 1)) {
            return l->start + i;
        }
        at += w;
    }
    return l->start + l->length;
}
