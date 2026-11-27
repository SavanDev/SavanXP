/*
 * msftedit_private.h -- What the parts of msftedit.dll share: the state of
 * one RICHEDIT50W control, its text (text.c), its lines (layout.c) and its
 * undo (undo.c). Nothing here is exported.
 *
 * The text is kept as Windows XP's RichEdit keeps it: paragraphs end in a
 * single CR, and the document ends in one more paragraph mark that is not
 * part of the text but can be selected (a selection of everything runs to
 * length + 1). See docs/userland/WIN32.md, "RichEdit".
 */
#ifndef SAVANXP_WIN32_MSFTEDIT_PRIVATE_H
#define SAVANXP_WIN32_MSFTEDIT_PRIVATE_H

#include <windows.h>
#include <richedit.h>

typedef unsigned int u32;

__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

/* The default limit, and the one a limit of 0 sets (EM_EXLIMITTEXT). */
#define DEFAULT_LIMIT 32767
#define ZERO_LIMIT 65536
#define DEFAULT_UNDO_LIMIT 100
/* The selection bar's width (ES_SELECTIONBAR), and the formatting
 * rectangle's default inset on the left and right. */
#define SELECTION_BAR 8
#define DEFAULT_INSET 1
/* A tab stop every half inch at 96 dpi. */
#define TAB_WIDTH 48

/* What an undo record undid, as EM_GETUNDONAME names it. */
enum undo_kind { UNDO_UNKNOWN = 0, UNDO_TYPING = 1, UNDO_DELETE = 2, UNDO_DRAGDROP = 3, UNDO_CUT = 4, UNDO_PASTE = 5 };
/* A deletion by Backspace: EM_GETUNDONAME calls it a deletion, and undoing it
 * leaves the caret after what comes back instead of selecting it. */
#define UNDO_BACKSPACE (0x100 | UNDO_DELETE)
#define UNDO_NAME(kind) ((kind) & 0xff)

/* One replacement: at cp, `removed` was taken out and `inserted` put in. */
typedef struct undo_record {
    int kind;
    int group; /* records of one action (a drag that moves text) share it */
    int cp;
    WCHAR *removed;
    int removed_length;
    WCHAR *inserted;
    int inserted_length;
} undo_record;

typedef struct undo_stack {
    undo_record *records;
    int count;
    int capacity;
} undo_stack;

/* A line on screen: `length` characters from `start`, trailing spaces
 * included and a paragraph's CR not; `paragraph_end` when a CR or the
 * document's end follows. */
typedef struct line_info {
    int start;
    int length;
    int paragraph_end;
    int y;
    int width;
} line_info;

typedef struct rich {
    HWND hwnd;
    DWORD style;
    int multiline;
    int wants_vscroll; /* WS_VSCROLL was asked for; it is shown only when needed */

    WCHAR *text;
    int length;
    int capacity;
    int limit;
    int modified;

    int anchor; /* the selection runs from anchor to caret */
    int caret;
    int hidden_selection;

    DWORD event_mask;
    DWORD options;
    DWORD edit_style;
    DWORD lang_options;
    void *ole_callback;

    HFONT font;
    int line_height;
    int ascent;
    int widths[256];

    /* The formatting rectangle: insets from the client area's edges, so it
     * follows the client area as a scroll bar comes and goes. */
    RECT insets;
    int no_wrap;
    line_info *lines;
    int line_count;
    int line_capacity;
    int content_height;
    int content_width;
    int scroll_y;
    int scroll_x;
    int vbar_shown;
    int in_bar_change; /* the bar is coming or going: WM_SIZE is ours */

    undo_stack undo;
    undo_stack redo;
    int undo_limit;
    int typing_open; /* the last undo record is typing that can grow */
    int undo_group;  /* the group new records join, 0 for none */
    int undo_groups;

    int focused;
    int dragging;
    int down_x;
    int down_y;
    int caret_made;
    int caret_shown;
    DWORD own_clipboard; /* the clipboard's sequence number after our copy */
    int clipboard_length;
} rich;

/* text.c */
void *rich_alloc(u32 size);
void rich_free(void *p);
int rich_reserve(rich *r, int length);
int rich_count_crs(const WCHAR *text, int length);
int rich_convert_in(const WCHAR *in, int n, WCHAR *out, int multiline);
WCHAR *rich_import(const WCHAR *in, int n, int multiline, int *out_length);
int rich_crlf_copy(const WCHAR *text, int length, WCHAR *out, int capacity);
int rich_word_class(const rich *r, int cp);
int rich_find_word_break(const rich *r, int code, int cp);
void rich_splice(rich *r, int cp, int remove, const WCHAR *insert, int insert_length);

/* layout.c */
void rich_measure_font(rich *r);
void rich_layout(rich *r);
void rich_format_rect(const rich *r, RECT *out);
int rich_line_of(const rich *r, int cp);
void rich_pos_of(const rich *r, int cp, POINT *out);
int rich_char_at(const rich *r, int x, int y);
int rich_text_x(const rich *r, const line_info *l, int count);
int rich_left(const rich *r);

/* undo.c */
void rich_undo_clear(undo_stack *s);
void rich_undo_push(rich *r, int kind, int cp, const WCHAR *removed, int removed_length, const WCHAR *inserted,
                    int inserted_length);
void rich_undo_typing(rich *r, int cp, const WCHAR *removed, int removed_length, WCHAR c);
int rich_undo(rich *r, int redo);

#endif
