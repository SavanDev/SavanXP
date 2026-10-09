/*
 * Web Viewer: a minimal browser for local files and HTTP by name or IP, in
 * the spirit of IE 1.1.
 *
 * What this version does: everything before it did, plus a first CSS: tag rules
 * from <style> and style="" over colour, background, alignment, bold,
 * underline and a two-face size (see docs/WEBVIEWER.md for the subset).
 *
 * What it deliberately does NOT do: no TLS, no CSS
 * beyond that subset, no images (an [image] marker), no tables as grids, no
 * scroll-to-fragment. Fetches block the UI: there is no background loading yet.
 */

#include "libc.h"
#include "savanxp/gfx2d.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "shared/version.h"

#define WEBVIEW_RAW_CAPACITY (32 * 1024)
#define WEBVIEW_PARA_MAX 256
#define WEBVIEW_PARA_LENGTH 512
#define WEBVIEW_PATH_CAPACITY 192
#define WEBVIEW_TITLE_CAPACITY 128
#define WEBVIEW_STATUS_CAPACITY 192
#define WEBVIEW_LINE_BUFFER 512
#define WEBVIEW_TRIAL_BUFFER 640

#define WEBVIEW_MENU_OPEN 1
#define WEBVIEW_MENU_RELOAD 2
#define WEBVIEW_MENU_EXIT 3
#define WEBVIEW_MENU_TOP 10
#define WEBVIEW_MENU_BOTTOM 11
#define WEBVIEW_MENU_SOURCE 12
#define WEBVIEW_MENU_ABOUT 20
#define WEBVIEW_MENU_BACK 30
#define WEBVIEW_MENU_FORWARD 31

#define WEBVIEW_CONTENT_WIDTH 560
#define WEBVIEW_CONTENT_HEIGHT 420

/* Tab order: address first, like a browser. The viewport itself is not a
 * widget, so arrows scroll it whenever the focus is not in the address field
 * (see on_key). */
#define WEBVIEW_ADDR_INDEX 0
#define WEBVIEW_ADDR (&g_widgets[0])
#define WEBVIEW_GO (&g_widgets[1])
#define WEBVIEW_BACK (&g_widgets[2])
#define WEBVIEW_FWD (&g_widgets[3])
#define WEBVIEW_SCROLL (&g_widgets[4])
#define WEBVIEW_STATUS (&g_widgets[5])
#define WEBVIEW_WIDGET_COUNT 6

#define WEBVIEW_NAV_WIDTH 34
#define WEBVIEW_GO_WIDTH 64

#define WEBVIEW_HISTORY_MAX 32

#define WEBVIEW_LINK_COLOR SXGUI_RGB(0, 0, 255)

/* Clickable spans, in coordinates of the *unwrapped* paragraph text: a link
 * that wraps still matches line by line at paint and click time. */
#define WEBVIEW_LINK_MAX 64
struct webview_link {
    int para;
    int start;
    int length;
    char target[WEBVIEW_PATH_CAPACITY];
};

/* Style runs: inline formatting over [start, start+length) of a paragraph.
 * Links live here too (link >= 0 indexes g_links); colour, when set, loses to
 * the link colour, like in early browsers. */
#define WEBVIEW_RUN_MAX 256
struct webview_run {
    int para;
    int start;
    int length;
    int hasColor;
    uint32_t color;
    int bold;
    int underline;
    int link;
};

/* Inline style boundaries: pushed whenever the active style changes, consumed
 * into runs at the next flush. Offsets are into g_cur. */
#define WEBVIEW_MARK_MAX 64
struct webview_mark {
    int offset;
    int hasColor;
    uint32_t color;
    int bold;
    int underline;
    int link;
};

/* One parsed declaration block: shared shape for tag rules and style="". */
struct webview_decl {
    int hasColor;
    uint32_t color;
    int hasBg;
    uint32_t bg;
    int align;
    int hasAlign;
    int bold;
    int hasBold;
    int underline;
    int hasUnderline;
    int face;
    int hasFace;
};

/* A tag rule from <style>: later rules win over earlier ones. */
#define WEBVIEW_RULE_MAX 32
struct webview_rule {
    char tag[16];
    struct webview_decl decl;
};

/* Pending block style: the tag that opened plus its inline declarations,
 * applied to the next flushed paragraph. */
struct webview_block {
    char tag[16];
    struct webview_decl inlineDecl;
    int hasInline;
    int gap;
    int heading;
    int indent;
};

/* Painted line geometry, in document coordinates (no scroll applied): what a
 * click maps back through. Capped: a very long document keeps its first
 * WEBVIEW_LINE_MAX lines clickable, the rest still readable. */
#define WEBVIEW_LINE_MAX 1024
struct webview_line_geo {
    int y;
    int height;
    int para;
    int start;
    int end;
    int x;
};

#define WEBVIEW_OPEN_WIDTH 340
#define WEBVIEW_OPEN_HEIGHT 112
#define WEBVIEW_ABOUT_WIDTH 330
#define WEBVIEW_ABOUT_HEIGHT 150

#define WEBVIEW_DLG_MARGIN SXGUI_DIALOG_MARGIN
#define WEBVIEW_DLG_ROW (18 + 4)
#define WEBVIEW_DLG_BUTTON_ROW(height) ((height) - WEBVIEW_DLG_MARGIN - SXGUI_BUTTON_HEIGHT)
#define WEBVIEW_DLG_RIGHT(width, index) \
    ((width) - WEBVIEW_DLG_MARGIN - ((index) + 1) * SXGUI_BUTTON_WIDTH - (index) * SXGUI_GAP)
#define WEBVIEW_DLG_CENTRED(width, count, index) \
    (((width) - (count) * SXGUI_BUTTON_WIDTH - ((count) - 1) * SXGUI_GAP) / 2 + \
     (index) * (SXGUI_BUTTON_WIDTH + SXGUI_GAP))

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[WEBVIEW_WIDGET_COUNT];

static char g_raw[WEBVIEW_RAW_CAPACITY];
static char g_address[WEBVIEW_PATH_CAPACITY];

/* Back/Forward history: plain paths, oldest first. Loading a new page after
 * going back drops the forward branch, like every browser since Mosaic. */
static char g_history[WEBVIEW_HISTORY_MAX][WEBVIEW_PATH_CAPACITY];
static int g_hist_count = 0;
static int g_hist_index = -1;

static struct webview_link g_links[WEBVIEW_LINK_MAX];
static int g_link_count = 0;

static struct webview_run g_runs[WEBVIEW_RUN_MAX];
static int g_run_count = 0;

static struct webview_mark g_marks[WEBVIEW_MARK_MAX];
static int g_mark_count = 0;

static struct webview_rule g_rules[WEBVIEW_RULE_MAX];
static int g_rule_count = 0;

/* Active inline style: colour stack plus bold/underline depths, and the open
 * <a> target. Marks snapshot this; flushes turn marks into runs. */
#define WEBVIEW_STYLE_DEPTH 8
static uint32_t g_color_stack[WEBVIEW_STYLE_DEPTH];
static int g_color_depth = 0;
static int g_bold_depth = 0;
static int g_bold_off = 0;
static int g_ul_depth = 0;
static int g_ul_off = 0;
static char g_link_target[WEBVIEW_PATH_CAPACITY];
static int g_link_index = -1;

/* Generic styled-element frames (<span style>, <font>): what the element set,
 * so its close tag undoes exactly that. */
struct webview_frame {
    int hadColor;
    int bold;
    int boldOff;
    int underline;
    int underlineOff;
    int face;
    int hasFace;
};
static struct webview_frame g_frames[WEBVIEW_STYLE_DEPTH];
static int g_frame_count = 0;

/* Page colours: UA default, <style> body rule, <body> attributes and <style>
 * blocks all funnel here, in that precedence. Reset per document. */
static uint32_t g_page_bg;
static uint32_t g_page_fg;
static uint32_t g_link_color;

/* Raw <style> capture, parsed on </style>. */
#define WEBVIEW_STYLE_CAPACITY 4096
static char g_style[WEBVIEW_STYLE_CAPACITY];
static size_t g_style_length = 0;

static struct webview_line_geo g_lines[WEBVIEW_LINE_MAX];
static int g_line_count = 0;

/* Painted link runs, in window x over a document-coordinate line: clicking
 * maps y to a cached line and x to a run, with no painter needed. Capped like
 * the lines; unrecorded runs stay readable but not clickable. */
#define WEBVIEW_LINK_RUN_MAX 256
struct webview_link_run {
    int line;
    int link;
    int x1;
    int x2;
};

static struct webview_link_run g_link_runs[WEBVIEW_LINK_RUN_MAX];
static int g_link_run_count = 0;

/* Text area of the last paint, in window coordinates: press/release
 * containment for link clicks. */
static struct sx_rect g_text_area;
/* Content origin (first baseline row) of the last paint, in window
 * coordinates: clicks and cached lines both translate through it, so the two
 * agree. Lines are stored in document coordinates (origin here, scroll added);
 * the lookup subtracts the same origin. Mixing surface and content spaces was
 * a real bug: clicks missed by the chrome height. */
static int g_content_top = 0;
static int g_show_source = 0;

struct webview_para {
    char text[WEBVIEW_PARA_LENGTH];
    int heading; /* 0 = body, 1..3 = h1..h3 */
    int rule;    /* 1 = <hr>, text unused */
    int indent;  /* 1 = list item */
    int gap;     /* pixels of air above this paragraph */
    /* Cascade output: tag rule, overridden by the block's style="". */
    int hasColor;
    uint32_t color;
    int baseBold;
    int align; /* 0 = left, 1 = center, 2 = right */
    int hasBg;
    uint32_t bg;
    int face; /* 0 = UI, 1 = title */
};

static struct webview_para g_paras[WEBVIEW_PARA_MAX];
static int g_para_count = 0;

static char g_path[WEBVIEW_PATH_CAPACITY];
static char g_title[WEBVIEW_TITLE_CAPACITY];
static char g_status[WEBVIEW_STATUS_CAPACITY];
static int g_truncated = 0;
static int g_has_file = 0;
/* One-shot note from the last fetch (" (connection dropped)"): cleared by
 * every load, appended to the status line while it says something. */
static char g_fetch_note[32];

static struct sx_rect g_view;
static int g_total_height = 0;
static int g_measured_width = -1;

static struct sxgui_dialog g_open_dialog;
static struct sxgui_widget g_open_widgets[4];
static char g_open_path[WEBVIEW_PATH_CAPACITY];

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[5];

/* ---- tiny HTML subset ------------------------------------------------------
 *
 * One pass over the raw buffer. Pending marks (heading level, list indent,
 * gap) are set by opening block tags and consumed by the next flush; closing
 * tags flush. Inline tags (b, i, a, font, ...) are ignored: their text stays.
 */

static char g_cur[WEBVIEW_PARA_LENGTH];
static int g_cur_length = 0;
static struct webview_block g_pending;

/* Reset every per-document style state: rules, runs, marks, stacks, links,
 * page colours. Welcome and source views come through here too, so stale
 * formatting can never leak from one page into the next. */
static void webview_reset_styles(void)
{
    g_link_count = 0;
    g_run_count = 0;
    g_mark_count = 0;
    g_rule_count = 0;
    g_color_depth = 0;
    g_bold_depth = 0;
    g_bold_off = 0;
    g_ul_depth = 0;
    g_ul_off = 0;
    g_link_target[0] = '\0';
    g_link_index = -1;
    g_frame_count = 0;
    g_style_length = 0;
    g_page_bg = SXGUI_COLOR_FIELD;
    g_page_fg = SXGUI_COLOR_TEXT;
    g_link_color = WEBVIEW_LINK_COLOR;
    memset(&g_pending, 0, sizeof(g_pending));
}

/* Snapshot of the active inline style, taken whenever it changes. Zero-length
 * segments are dropped at flush, so redundant marks are only wasted slots. */
static void webview_push_mark(void)
{
    struct webview_mark *mark;

    if (g_mark_count >= WEBVIEW_MARK_MAX)
    {
        return;
    }
    mark = &g_marks[g_mark_count++];
    mark->offset = g_cur_length;
    mark->hasColor = g_color_depth > 0;
    mark->color = g_color_depth > 0 ? g_color_stack[g_color_depth - 1] : 0;
    mark->bold = g_bold_depth > 0 && g_bold_off == 0;
    mark->underline = g_ul_depth > 0 && g_ul_off == 0;
    mark->link = g_link_index;
}

/* Materialize [0, g_cur_length) into runs from the marks. Called by flush
 * before pushing the paragraph (runs store its upcoming index), then marks
 * restart with the still-active style so unclosed elements span paragraphs. */
static void webview_close_marks(void)
{
    int i;

    for (i = 0; i < g_mark_count; ++i)
    {
        int start = g_marks[i].offset;
        int end = i + 1 < g_mark_count ? g_marks[i + 1].offset : g_cur_length;
        struct webview_run *run;

        if (end <= start || g_run_count >= WEBVIEW_RUN_MAX)
        {
            continue;
        }
        if (!g_marks[i].hasColor && !g_marks[i].bold && !g_marks[i].underline &&
            g_marks[i].link < 0)
        {
            continue;
        }
        run = &g_runs[g_run_count++];
        run->para = g_para_count;
        run->start = start;
        run->length = end - start;
        run->hasColor = g_marks[i].hasColor;
        run->color = g_marks[i].color;
        run->bold = g_marks[i].bold;
        run->underline = g_marks[i].underline;
        run->link = g_marks[i].link;
    }
    g_mark_count = 0;
    if (g_color_depth > 0 || g_bold_depth > 0 || g_ul_depth > 0 || g_link_index >= 0)
    {
        webview_push_mark();
    }
}

/* Begins an <a href>: the target is stored now (marks only carry its index),
 * the run materializes at </a> or at the next flush. A nested <a> is ignored:
 * HTML forbids it, and the outer target wins. */
static void webview_open_anchor(const char *target)
{
    struct webview_link *link;

    if (g_link_target[0] != '\0' || g_link_count >= WEBVIEW_LINK_MAX)
    {
        return;
    }
    link = &g_links[g_link_count];
    link->para = -1;
    link->start = 0;
    link->length = 0;
    snprintf(link->target, sizeof(link->target), "%s", target);
    g_link_count += 1;
    snprintf(g_link_target, sizeof(g_link_target), "%s", target);
    g_link_index = g_link_count - 1;
    webview_push_mark();
}

static void webview_close_anchor(void)
{
    g_link_target[0] = '\0';
    g_link_index = -1;
    webview_push_mark();
}

static void webview_cur_append(unsigned char value)
{
    if (value == '\t' || value == '\n' || value == '\r' || value == ' ')
    {
        if (g_cur_length > 0 && g_cur[g_cur_length - 1] != ' ' &&
            g_cur_length + 1 < (int)sizeof(g_cur))
        {
            g_cur[g_cur_length++] = ' ';
        }
        return;
    }
    if (value < 32)
    {
        return;
    }
    /* Bytes >= 128 pass through untouched: they are UTF-8 the painter decodes.
     * Wrapping only ever splits at ASCII spaces, so multibyte runs stay whole. */
    if (g_cur_length + 1 < (int)sizeof(g_cur))
    {
        g_cur[g_cur_length++] = (char)value;
    }
}

static void webview_cur_append_text(const char *text)
{
    while (*text != '\0')
    {
        webview_cur_append((unsigned char)*text);
        text += 1;
    }
}

static void webview_decode_entities(char *text)
{
    /* In place: every replacement is shorter than its entity, so a read and a
     * write cursor never cross. Unknown entities are kept literally. */
    static const struct {
        const char *entity;
        char value;
    } k_entities[] = {
        {"amp;", '&'},
        {"lt;", '<'},
        {"gt;", '>'},
        {"quot;", '"'},
    };
    size_t read = 0;
    size_t write = 0;
    size_t index;

    while (text[read] != '\0')
    {
        if (text[read] != '&')
        {
            text[write++] = text[read++];
            continue;
        }
        if (strncmp(&text[read + 1], "nbsp;", 5) == 0)
        {
            text[write++] = ' ';
            read += 6;
            continue;
        }
        for (index = 0; index < sizeof(k_entities) / sizeof(k_entities[0]); ++index)
        {
            size_t length = strlen(k_entities[index].entity);
            if (strncmp(&text[read + 1], k_entities[index].entity, length) == 0)
            {
                text[write++] = k_entities[index].value;
                read += 1 + length;
                break;
            }
        }
        if (index < sizeof(k_entities) / sizeof(k_entities[0]))
        {
            continue;
        }
        text[write++] = text[read++];
    }
    text[write] = '\0';
}

static void webview_push_para(const char *text, int heading, int rule, int indent, int gap)
{
    struct webview_para *para;

    if (g_para_count >= WEBVIEW_PARA_MAX)
    {
        return;
    }
    para = &g_paras[g_para_count++];
    snprintf(para->text, sizeof(para->text), "%s", text != 0 ? text : "");
    para->heading = heading;
    para->rule = rule;
    para->indent = indent;
    para->gap = gap;
    /* Cascade output: flush fills the real style right after pushing. */
    para->hasColor = 0;
    para->color = 0;
    para->baseBold = 0;
    para->align = 0;
    para->hasBg = 0;
    para->bg = 0;
    /* Headings start on the title face (the UA sheet); flush recomputes from
     * rules and inline style, so a rule still wins. */
    para->face = (heading >= 1 && heading <= 3) ? 1 : 0;
}

/* Last matching rule wins: later <style> blocks override earlier ones. */
static const struct webview_rule *webview_lookup_rule(const char *tag)
{
    const struct webview_rule *found = 0;
    int i;

    if (tag == 0 || tag[0] == '\0')
    {
        return 0;
    }
    for (i = 0; i < g_rule_count; ++i)
    {
        if (strcmp(g_rules[i].tag, tag) == 0)
        {
            found = &g_rules[i];
        }
    }
    return found;
}

/* Applies the pending block style to the paragraph just pushed: the tag rule
 * first, the block's own style="" over it. h1..h3 keep their title face
 * unless a rule or inline style says otherwise: that face IS the UA sheet. */
static void webview_apply_block_style(struct webview_para *para)
{
    const struct webview_rule *rule = webview_lookup_rule(g_pending.tag);
    int heading = g_pending.heading;

    para->face = (heading >= 1 && heading <= 3) ? 1 : 0;
    if (rule != 0)
    {
        if (rule->decl.hasColor)
        {
            para->hasColor = 1;
            para->color = rule->decl.color;
        }
        if (rule->decl.hasBg)
        {
            para->hasBg = 1;
            para->bg = rule->decl.bg;
        }
        if (rule->decl.hasAlign)
        {
            para->align = rule->decl.align;
        }
        if (rule->decl.hasBold && rule->decl.bold)
        {
            para->baseBold = 1;
        }
        if (rule->decl.hasFace)
        {
            para->face = rule->decl.face;
        }
    }
    if (g_pending.hasInline)
    {
        const struct webview_decl *decl = &g_pending.inlineDecl;

        if (decl->hasColor)
        {
            para->hasColor = 1;
            para->color = decl->color;
        }
        if (decl->hasBg)
        {
            para->hasBg = 1;
            para->bg = decl->bg;
        }
        if (decl->hasAlign)
        {
            para->align = decl->align;
        }
        if (decl->hasBold)
        {
            para->baseBold = decl->bold;
        }
        if (decl->hasFace)
        {
            para->face = decl->face;
        }
    }
}

static void webview_flush_text(void)
{
    while (g_cur_length > 0 && g_cur[g_cur_length - 1] == ' ')
    {
        g_cur_length -= 1;
    }
    g_cur[g_cur_length] = '\0';
    if (g_cur_length > 0)
    {
        webview_decode_entities(g_cur);
        webview_close_marks();
        webview_push_para(g_cur, g_pending.heading, 0, g_pending.indent, g_pending.gap);
        if (g_para_count > 0)
        {
            webview_apply_block_style(&g_paras[g_para_count - 1]);
        }
    }
    g_cur_length = 0;
    memset(&g_pending, 0, sizeof(g_pending));
}

/* Extracts attr="..." (or attr='...' or attr=...) from raw tag text bounded
 * by [text, end), matching the name case-insensitively. Returns 1 with the
 * value in `out`, 0 when there is none. */
static int webview_attr_name_matches(const char *name, size_t length, const char *want)
{
    size_t i;

    for (i = 0; want[i] != '\0'; ++i)
    {
    }
    if (length != i)
    {
        return 0;
    }
    for (i = 0; i < length; ++i)
    {
        char value = name[i];

        if (value >= 'A' && value <= 'Z')
        {
            value = (char)(value - 'A' + 'a');
        }
        if (value != want[i])
        {
            return 0;
        }
    }
    return 1;
}

static int webview_extract_attr(const char *text, const char *end, const char *attr, char *out,
    size_t capacity)
{
    const char *cursor = text;
    size_t length = 0;

    while (cursor < end)
    {
        const char *name = cursor;

        while (cursor < end && *cursor != '=' && *cursor != '>' && *cursor != ' ' &&
               *cursor != '\t' && *cursor != '\n' && *cursor != '\r' && *cursor != '/')
        {
            cursor += 1;
        }
        if (webview_attr_name_matches(name, (size_t)(cursor - name), attr))
        {
            while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
                                    *cursor == '\r'))
            {
                cursor += 1;
            }
            if (cursor < end && *cursor == '=')
            {
                char quote = '\0';

                cursor += 1;
                while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                                        *cursor == '\n' || *cursor == '\r'))
                {
                    cursor += 1;
                }
                if (cursor < end && (*cursor == '"' || *cursor == '\''))
                {
                    quote = *cursor;
                    cursor += 1;
                }
                while (cursor < end && length + 1 < capacity &&
                       (quote != '\0' ? *cursor != quote
                                      : (*cursor != ' ' && *cursor != '\t' && *cursor != '\n' &&
                                         *cursor != '\r' && *cursor != '>')))
                {
                    out[length++] = *cursor;
                    cursor += 1;
                }
                out[length] = '\0';
                return length > 0;
            }
            continue;
        }
        if (cursor < end && *cursor != '=')
        {
            cursor += 1;
            continue;
        }
        while (cursor < end && *cursor != ' ' && *cursor != '\t' && *cursor != '\n' &&
               *cursor != '\r' && *cursor != '>')
        {
            cursor += 1;
        }
    }
    return 0;
}

/* ---- cascade -------------------------------------------------------------
 *
 * Subset on purpose: tag selectors and style="", over color, background,
 * alignment, bold, underline and a two-face font-size. No classes, no ids, no
 * inheritance past the element itself, no shorthand but background-as-colour.
 * What is parsed but has no face to show (italic, line-through, px sizes)
 * stays ignored and noted, instead of half working.
 */

static int webview_hex_value(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

static int webview_parse_color(const char *text, const char *end, uint32_t *out)
{
    static const struct {
        const char *name;
        uint32_t color;
    } k_names[] = {
        {"black", SXGUI_RGB(0, 0, 0)},
        {"silver", SXGUI_RGB(192, 192, 192)},
        {"gray", SXGUI_RGB(128, 128, 128)},
        {"grey", SXGUI_RGB(128, 128, 128)},
        {"white", SXGUI_RGB(255, 255, 255)},
        {"maroon", SXGUI_RGB(128, 0, 0)},
        {"red", SXGUI_RGB(255, 0, 0)},
        {"purple", SXGUI_RGB(128, 0, 128)},
        {"fuchsia", SXGUI_RGB(255, 0, 255)},
        {"green", SXGUI_RGB(0, 128, 0)},
        {"lime", SXGUI_RGB(0, 255, 0)},
        {"olive", SXGUI_RGB(128, 128, 0)},
        {"yellow", SXGUI_RGB(255, 255, 0)},
        {"navy", SXGUI_RGB(0, 0, 128)},
        {"blue", SXGUI_RGB(0, 0, 255)},
        {"teal", SXGUI_RGB(0, 128, 128)},
        {"aqua", SXGUI_RGB(0, 255, 255)},
        {"orange", SXGUI_RGB(255, 165, 0)},
    };
    char lowered[32];
    size_t length = 0;
    size_t i;

    while (text < end && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r'))
    {
        text += 1;
    }
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                          end[-1] == '\r' || end[-1] == ';'))
    {
        end -= 1;
    }
    if (end > text && text[0] == '#')
    {
        int digits[6];
        size_t count = 0;

        text += 1;
        while (text < end && count < 6)
        {
            int digit = webview_hex_value(*text);

            if (digit < 0)
            {
                return 0;
            }
            digits[count++] = digit;
            text += 1;
        }
        if (text != end || (count != 3 && count != 6))
        {
            return 0;
        }
        if (count == 3)
        {
            *out = SXGUI_RGB((unsigned)(digits[0] * 17), (unsigned)(digits[1] * 17),
                (unsigned)(digits[2] * 17));
            return 1;
        }
        *out = SXGUI_RGB((unsigned)(digits[0] * 16 + digits[1]),
            (unsigned)(digits[2] * 16 + digits[3]),
            (unsigned)(digits[4] * 16 + digits[5]));
        return 1;
    }
    while (text < end && length + 1 < sizeof(lowered))
    {
        char value = *text;

        if (value >= 'A' && value <= 'Z')
        {
            value = (char)(value - 'A' + 'a');
        }
        lowered[length++] = value;
        text += 1;
    }
    if (text != end)
    {
        return 0;
    }
    lowered[length] = '\0';
    for (i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i)
    {
        if (strcmp(lowered, k_names[i].name) == 0)
        {
            *out = k_names[i].color;
            return 1;
        }
    }
    return 0;
}

/* Compares a raw value range against a keyword, case-insensitively, trimmed. */
static int webview_value_is(const char *text, const char *end, const char *want)
{
    size_t length = 0;

    while (text < end && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r'))
    {
        text += 1;
    }
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                          end[-1] == '\r' || end[-1] == ';'))
    {
        end -= 1;
    }
    while (want[length] != '\0')
    {
        length += 1;
    }
    if ((size_t)(end - text) != length)
    {
        return 0;
    }
    {
        size_t i;

        for (i = 0; i < length; ++i)
        {
            char value = text[i];

            if (value >= 'A' && value <= 'Z')
            {
                value = (char)(value - 'A' + 'a');
            }
            if (value != want[i])
            {
                return 0;
            }
        }
    }
    return 1;
}

static int webview_parse_font_size(const char *text, const char *end, int *face)
{
    /* Bitmap faces quantize sizes: UI or title, nothing between. px/pt/size
     * numbers have no face to map to and stay ignored. */
    if (webview_value_is(text, end, "xx-small") || webview_value_is(text, end, "x-small") ||
        webview_value_is(text, end, "small") || webview_value_is(text, end, "medium") ||
        webview_value_is(text, end, "smaller"))
    {
        *face = 0;
        return 1;
    }
    if (webview_value_is(text, end, "large") || webview_value_is(text, end, "x-large") ||
        webview_value_is(text, end, "xx-large") || webview_value_is(text, end, "larger"))
    {
        *face = 1;
        return 1;
    }
    return 0;
}

static void webview_apply_decl(struct webview_decl *decl, const char *name, size_t name_length,
    const char *value, const char *value_end)
{
    char prop[24];
    size_t i;

    if (name_length >= sizeof(prop))
    {
        return;
    }
    for (i = 0; i < name_length; ++i)
    {
        char c = name[i];

        prop[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    prop[name_length] = '\0';
    if (strcmp(prop, "color") == 0)
    {
        uint32_t color = 0;

        if (webview_parse_color(value, value_end, &color))
        {
            decl->hasColor = 1;
            decl->color = color;
        }
        return;
    }
    if (strcmp(prop, "background-color") == 0)
    {
        uint32_t color = 0;

        if (webview_parse_color(value, value_end, &color))
        {
            decl->hasBg = 1;
            decl->bg = color;
        }
        return;
    }
    if (strcmp(prop, "background") == 0)
    {
        /* Shorthand with a single colour only: images and repeats are later. */
        uint32_t color = 0;

        if (webview_parse_color(value, value_end, &color))
        {
            decl->hasBg = 1;
            decl->bg = color;
        }
        return;
    }
    if (strcmp(prop, "text-align") == 0)
    {
        if (webview_value_is(value, value_end, "left"))
        {
            decl->hasAlign = 1;
            decl->align = 0;
        }
        else if (webview_value_is(value, value_end, "center"))
        {
            decl->hasAlign = 1;
            decl->align = 1;
        }
        else if (webview_value_is(value, value_end, "right"))
        {
            decl->hasAlign = 1;
            decl->align = 2;
        }
        else if (webview_value_is(value, value_end, "justify"))
        {
            /* No justification engine: left, said plainly. */
            decl->hasAlign = 1;
            decl->align = 0;
        }
        return;
    }
    if (strcmp(prop, "font-weight") == 0)
    {
        if (webview_value_is(value, value_end, "bold"))
        {
            decl->hasBold = 1;
            decl->bold = 1;
        }
        else if (webview_value_is(value, value_end, "normal"))
        {
            decl->hasBold = 1;
            decl->bold = 0;
        }
        else
        {
            /* Bounded digits: value is a range, not a string. */
            const char *digits = value;
            const char *digits_end = value_end;
            unsigned number = 0;
            int ok = 0;

            while (digits < digits_end && (*digits == ' ' || *digits == '\t'))
            {
                digits += 1;
            }
            while (digits_end > digits &&
                   (digits_end[-1] == ' ' || digits_end[-1] == '\t' || digits_end[-1] == ';'))
            {
                digits_end -= 1;
            }
            if (digits_end > digits)
            {
                ok = 1;
                while (digits < digits_end)
                {
                    if (*digits < '0' || *digits > '9')
                    {
                        ok = 0;
                        break;
                    }
                    number = number * 10u + (unsigned)(*digits - '0');
                    digits += 1;
                }
            }
            if (ok && number >= 100 && number <= 900)
            {
                decl->hasBold = 1;
                decl->bold = number >= 700;
            }
        }
        return;
    }
    if (strcmp(prop, "text-decoration") == 0)
    {
        if (webview_value_is(value, value_end, "underline"))
        {
            decl->hasUnderline = 1;
            decl->underline = 1;
        }
        else if (webview_value_is(value, value_end, "none"))
        {
            decl->hasUnderline = 1;
            decl->underline = 0;
        }
        /* blink and line-through: no engine, ignored. */
        return;
    }
    if (strcmp(prop, "font-size") == 0)
    {
        int face = 0;

        if (webview_parse_font_size(value, value_end, &face))
        {
            decl->hasFace = 1;
            decl->face = face;
        }
        return;
    }
    /* font-style and the rest: parsed nowhere, ignored openly. */
}

/* Parses "prop: value; ..." into decl (ORed over the caller's zeroed struct).
 * Values with '(' (url(), rgb()) are out of the subset: skipped whole. */
static void webview_parse_decls(const char *text, const char *end, struct webview_decl *decl)
{
    while (text < end)
    {
        const char *name = text;
        const char *name_end;
        const char *value;
        const char *value_end;

        /* Property names arrive with the block's own spacing ("{ color", ";  margin"):
         * trim both edges, or no property past the first ever matches. */
        while (name < end && (*name == ' ' || *name == '\t' || *name == '\n' || *name == '\r'))
        {
            name += 1;
        }
        text = name;
        while (text < end && *text != ':' && *text != ';' && *text != '}')
        {
            text += 1;
        }
        name_end = text;
        while (name_end > name && (name_end[-1] == ' ' || name_end[-1] == '\t' ||
                                   name_end[-1] == '\n' || name_end[-1] == '\r'))
        {
            name_end -= 1;
        }
        if (text >= end || *text != ':')
        {
            while (text < end && *text != ';' && *text != '}')
            {
                text += 1;
            }
            if (text < end)
            {
                text += 1;
            }
            continue;
        }
        value = text + 1;
        value_end = value;
        while (value_end < end && *value_end != ';' && *value_end != '}')
        {
            value_end += 1;
        }
        {
            const char *scan = value;
            int paren = 0;

            while (scan < value_end)
            {
                if (*scan == '(')
                {
                    paren = 1;
                    break;
                }
                scan += 1;
            }
            if (!paren)
            {
                webview_apply_decl(decl, name, (size_t)(name_end - name), value, value_end);
            }
        }
        text = value_end < end ? value_end + 1 : value_end;
    }
}

static const char *webview_skip_css_space(const char *cursor, const char *end)
{
    while (cursor < end)
    {
        if (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
        {
            cursor += 1;
            continue;
        }
        if (cursor + 1 < end && cursor[0] == '/' && cursor[1] == '*')
        {
            cursor += 2;
            while (cursor + 1 < end && !(cursor[0] == '*' && cursor[1] == '/'))
            {
                cursor += 1;
            }
            if (cursor + 1 < end)
            {
                cursor += 2;
            }
            else
            {
                return end;
            }
            continue;
        }
        break;
    }
    return cursor;
}

/* Adds one rule per plain tag selector; body rules land on the page colours
 * directly (no body paragraphs exist to carry them). Returns how many stored. */
static int webview_store_rule(const char *selector, const char *selector_end,
    const struct webview_decl *decl)
{
    const char *cursor = selector;
    int stored = 0;

    while (cursor < selector_end)
    {
        const char *name = cursor;
        char tag[16];
        size_t length = 0;
        int valid = 1;
        size_t i;

        while (cursor < selector_end && *cursor != ',')
        {
            cursor += 1;
        }
        {
            const char *start = name;
            const char *stop = cursor;

            while (start < stop && (*start == ' ' || *start == '\t' || *start == '\n' ||
                                    *start == '\r'))
            {
                start += 1;
            }
            while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\n' ||
                                     stop[-1] == '\r'))
            {
                stop -= 1;
            }
            while (start < stop && length + 1 < sizeof(tag))
            {
                char value = *start;

                if (value >= 'A' && value <= 'Z')
                {
                    value = (char)(value - 'A' + 'a');
                }
                if ((value < 'a' || value > 'z') && (value < '0' || value > '9'))
                {
                    valid = 0;
                    break;
                }
                tag[length++] = value;
                start += 1;
            }
            if (start != stop)
            {
                valid = 0;
            }
            tag[length] = '\0';
            if (valid && length > 0 && g_rule_count < WEBVIEW_RULE_MAX)
            {
                if (strcmp(tag, "body") == 0)
                {
                    if (decl->hasColor)
                    {
                        g_page_fg = decl->color;
                    }
                    if (decl->hasBg)
                    {
                        g_page_bg = decl->bg;
                    }
                    stored += 1;
                }
                else
                {
                    struct webview_rule *rule = &g_rules[g_rule_count++];

                    for (i = 0; i < sizeof(tag); ++i)
                    {
                        rule->tag[i] = tag[i];
                    }
                    rule->decl = *decl;
                    stored += 1;
                }
            }
        }
        if (cursor < selector_end)
        {
            cursor += 1;
        }
    }
    return stored;
}

/* Parses a <style> body: selector lists with tag names only. Class, id,
 * pseudo and at-rules are skipped block-aware, never half parsed. */
static void webview_parse_css(const char *css)
{
    const char *end = css + strlen(css);
    const char *cursor = css;

    while (cursor < end)
    {
        const char *selector = cursor;
        const char *brace;
        const char *block_end;
        int depth;
        struct webview_decl decl;

        cursor = webview_skip_css_space(cursor, end);
        if (cursor >= end)
        {
            break;
        }
        if (*cursor == '@')
        {
            /* At-rule: skip to ';' or over the balanced block. */
            brace = strchr(cursor, '{');
            {
                const char *semi = strchr(cursor, ';');

                if (brace == 0 || (semi != 0 && semi < brace))
                {
                    cursor = semi != 0 ? semi + 1 : end;
                    continue;
                }
            }
            depth = 0;
            block_end = brace;
            while (block_end < end)
            {
                if (*block_end == '{')
                {
                    depth += 1;
                }
                else if (*block_end == '}')
                {
                    depth -= 1;
                    if (depth == 0)
                    {
                        break;
                    }
                }
                block_end += 1;
            }
            cursor = block_end < end ? block_end + 1 : end;
            continue;
        }
        brace = strchr(cursor, '{');
        if (brace == 0 || brace >= end)
        {
            break;
        }
        memset(&decl, 0, sizeof(decl));
        depth = 1;
        block_end = brace + 1;
        while (block_end < end && depth > 0)
        {
            if (*block_end == '{')
            {
                depth += 1;
            }
            else if (*block_end == '}')
            {
                depth -= 1;
            }
            block_end += 1;
        }
        /* A nested '{' means something at-rule-shaped survived: the decls
         * still parse, the nested chunk parses as garbage declarations that
         * match no property. Harmless. */
        webview_parse_decls(brace + 1, block_end - 1, &decl);
        webview_store_rule(selector, brace, &decl);
        cursor = block_end;
    }
}

static int webview_tag_name(const char *cursor, const char *end, char *out, size_t capacity,
    int *is_close)
{
    size_t length = 0;

    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
                            *cursor == '\r'))
    {
        cursor += 1;
    }
    *is_close = 0;
    if (cursor < end && *cursor == '/')
    {
        *is_close = 1;
        cursor += 1;
    }
    while (cursor < end && length + 1u < capacity)
    {
        char value = *cursor;

        if (value >= 'A' && value <= 'Z')
        {
            value = (char)(value - 'A' + 'a');
        }
        if ((value < 'a' || value > 'z') && (value < '0' || value > '9'))
        {
            break;
        }
        out[length++] = value;
        cursor += 1;
    }
    out[length] = '\0';
    return (int)length;
}

/* Pushes an inline declaration (b/u/font/span): colour onto its stack, bold
 * and underline onto depth counters (with off-counters for explicit normal /
 * none, so <b>a<span style="font-weight:normal">b</span></b> renders right),
 * one frame remembering exactly what to undo, one mark. */
static void webview_inline_push(const struct webview_decl *decl)
{
    struct webview_frame *frame;
    int changed = 0;

    if (decl->hasColor)
    {
        if (g_color_depth < WEBVIEW_STYLE_DEPTH)
        {
            g_color_stack[g_color_depth++] = decl->color;
        }
        changed = 1;
    }
    if (decl->hasBold)
    {
        if (decl->bold)
        {
            g_bold_depth += 1;
        }
        else
        {
            g_bold_off += 1;
        }
        changed = 1;
    }
    if (decl->hasUnderline)
    {
        if (decl->underline)
        {
            g_ul_depth += 1;
        }
        else
        {
            g_ul_off += 1;
        }
        changed = 1;
    }
    if (!changed)
    {
        return;
    }
    if (g_frame_count < WEBVIEW_STYLE_DEPTH)
    {
        frame = &g_frames[g_frame_count++];
        frame->hadColor = decl->hasColor;
        frame->bold = decl->hasBold && decl->bold;
        frame->boldOff = decl->hasBold && !decl->bold;
        frame->underline = decl->hasUnderline && decl->underline;
        frame->underlineOff = decl->hasUnderline && !decl->underline;
        frame->hasFace = 0;
        frame->face = 0;
    }
    webview_push_mark();
}

static void webview_inline_pop(void)
{
    if (g_frame_count > 0)
    {
        struct webview_frame *frame = &g_frames[--g_frame_count];

        if (frame->hadColor && g_color_depth > 0)
        {
            g_color_depth -= 1;
        }
        if (frame->bold && g_bold_depth > 0)
        {
            g_bold_depth -= 1;
        }
        if (frame->boldOff && g_bold_off > 0)
        {
            g_bold_off -= 1;
        }
        if (frame->underline && g_ul_depth > 0)
        {
            g_ul_depth -= 1;
        }
        if (frame->underlineOff && g_ul_off > 0)
        {
            g_ul_off -= 1;
        }
    }
    webview_push_mark();
}

/* Block opener: flush, then stage tag/gaps plus the block's own style="". */
static void webview_pending_tag(const char *tag, int gap, int heading, int indent,
    const char *tag_inner, const char *tag_stop)
{
    char style[256];

    webview_flush_text();
    snprintf(g_pending.tag, sizeof(g_pending.tag), "%s", tag);
    g_pending.gap = gap;
    g_pending.heading = heading;
    g_pending.indent = indent;
    if (tag_inner != 0 &&
        webview_extract_attr(tag_inner, tag_stop, "style", style, sizeof(style)))
    {
        struct webview_decl decl;

        memset(&decl, 0, sizeof(decl));
        webview_parse_decls(style, style + strlen(style), &decl);
        g_pending.inlineDecl = decl;
        g_pending.hasInline = 1;
    }
}

/* Matches "</style" (any case) followed by a tag ending: the raw-text scan
 * for the end of a <style> block. */
static int webview_is_style_close(const char *text, size_t length)
{
    static const char kWant[] = "style";
    size_t i;

    if (length < 8 || text[0] != '<' || text[1] != '/')
    {
        return 0;
    }
    for (i = 0; i < 5; ++i)
    {
        char value = text[2 + i];

        if (value >= 'A' && value <= 'Z')
        {
            value = (char)(value - 'A' + 'a');
        }
        if (value != kWant[i])
        {
            return 0;
        }
    }
    {
        char after = length > 7 ? text[7] : '\0';

        return after == '>' || after == ' ' || after == '\t' || after == '\n' ||
               after == '\r' || after == '/';
    }
}

/* <body bgcolor text link>: page colours straight from the tag. Attributes
 * win over <style> body rules (they come later in the document). */
static void webview_apply_body_attrs(const char *tag_inner, const char *tag_stop)
{
    char value[32];
    uint32_t color = 0;

    if (webview_extract_attr(tag_inner, tag_stop, "bgcolor", value, sizeof(value)) &&
        webview_parse_color(value, value + strlen(value), &color))
    {
        g_page_bg = color;
    }
    if (webview_extract_attr(tag_inner, tag_stop, "text", value, sizeof(value)) &&
        webview_parse_color(value, value + strlen(value), &color))
    {
        g_page_fg = color;
    }
    if (webview_extract_attr(tag_inner, tag_stop, "link", value, sizeof(value)) &&
        webview_parse_color(value, value + strlen(value), &color))
    {
        g_link_color = color;
    }
}

static void webview_parse(void)
{
    const char *text = g_raw;
    size_t length = strlen(text);
    size_t index = 0;
    int in_head = 0;
    int in_title = 0;
    int in_style = 0;
    int skip = 0; /* inside <script> */
    size_t title_length = 0;

    g_para_count = 0;
    g_cur_length = 0;
    webview_reset_styles();
    g_title[0] = '\0';
    title_length = 0;

    while (index < length)
    {
        if (text[index] != '<')
        {
            unsigned char value = (unsigned char)text[index];

            if (in_style)
            {
                if (g_style_length + 1 < sizeof(g_style))
                {
                    g_style[g_style_length++] = (char)value;
                }
                index += 1;
                continue;
            }

            if (skip || (in_head && !in_title))
            {
                index += 1;
                continue;
            }
            if (in_title)
            {
                if (value == '\t' || value == '\n' || value == '\r' || value == ' ')
                {
                    value = ' ';
                }
                if (value < 32 && value != ' ')
                {
                    index += 1;
                    continue;
                }
                if ((value != ' ' || (title_length > 0 &&
                                      g_title[title_length - 1] != ' ')) &&
                    title_length + 1 < sizeof(g_title))
                {
                    g_title[title_length++] = (char)value;
                    g_title[title_length] = '\0';
                }
                index += 1;
                continue;
            }
            webview_cur_append(value);
            index += 1;
            continue;
        }
        /* A tag (or a comment, or a stray '<' with text after it). */
        if (index + 4 <= length && strncmp(&text[index], "<!--", 4) == 0)
        {
            size_t close = index + 4;

            while (close + 3 <= length && strncmp(&text[close], "-->", 3) != 0)
            {
                close += 1;
            }
            index = close + 3 <= length ? close + 3 : length;
            continue;
        }
        if (in_style)
        {
            /* Raw CSS text: only </style ends it, every other '<' is literal
             * (selectors never need it, and url(<) is out of the subset). */
            if (webview_is_style_close(&text[index], length - index))
            {
                size_t tag_end = index + 2;

                while (tag_end < length && text[tag_end] != '>')
                {
                    tag_end += 1;
                }
                index = tag_end < length ? tag_end + 1 : length;
                in_style = 0;
                g_style[g_style_length] = '\0';
                webview_parse_css(g_style);
                continue;
            }
            if (g_style_length + 1 < sizeof(g_style))
            {
                g_style[g_style_length++] = '<';
            }
            index += 1;
            continue;
        }
        {
            size_t tag_end = index + 1;
            char name[16];
            int is_close = 0;

            while (tag_end < length && text[tag_end] != '>')
            {
                tag_end += 1;
            }
            if (tag_end >= length)
            {
                /* Unterminated '<': the rest is text, entities and all. */
                while (index < length)
                {
                    webview_cur_append((unsigned char)text[index]);
                    index += 1;
                }
                break;
            }
            if (webview_tag_name(&text[index + 1], &text[tag_end], name, sizeof(name),
                    &is_close) == 0)
            {
                /* "<>" or "</>": not a tag, keep the '<' as text. */
                webview_cur_append((unsigned char)'<');
                index += 1;
                continue;
            }
            /* Attribute text for the handlers below: from the tag name on to '>'
             * (index still points at '<' here). */
            const char *tag_inner = &text[index + 1];
            const char *tag_stop = &text[tag_end];

            index = tag_end + 1;

            if (strcmp(name, "script") == 0)
            {
                webview_flush_text();
                skip = !is_close;
                continue;
            }
            if (strcmp(name, "style") == 0)
            {
                webview_flush_text();
                if (!is_close)
                {
                    in_style = 1;
                    g_style_length = 0;
                }
                continue;
            }
            if (skip)
            {
                continue;
            }
            if (strcmp(name, "head") == 0)
            {
                webview_flush_text();
                in_head = !is_close;
                continue;
            }
            if (strcmp(name, "title") == 0)
            {
                webview_flush_text();
                in_title = !is_close;
                if (in_title)
                {
                    title_length = 0;
                    g_title[0] = '\0';
                }
                else
                {
                    while (title_length > 0 && g_title[title_length - 1] == ' ')
                    {
                        g_title[--title_length] = '\0';
                    }
                    webview_decode_entities(g_title);
                }
                continue;
            }
            if (in_head)
            {
                continue;
            }
            if (strcmp(name, "h1") == 0 || strcmp(name, "h2") == 0 ||
                strcmp(name, "h3") == 0)
            {
                if (!is_close)
                {
                    webview_pending_tag(name, name[1] == '1' ? 8 : 6, name[1] - '0', 0,
                        tag_inner, tag_stop);
                }
                else
                {
                    webview_flush_text();
                }
                continue;
            }
            if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                strcmp(name, "blockquote") == 0)
            {
                if (!is_close)
                {
                    webview_pending_tag(name, 2, 0, 0, tag_inner, tag_stop);
                }
                else
                {
                    webview_flush_text();
                }
                continue;
            }
            if (strcmp(name, "body") == 0)
            {
                webview_flush_text();
                if (!is_close)
                {
                    webview_apply_body_attrs(tag_inner, tag_stop);
                }
                continue;
            }
            if (strcmp(name, "br") == 0)
            {
                webview_flush_text();
                continue;
            }
            if (strcmp(name, "hr") == 0)
            {
                webview_flush_text();
                webview_push_para("", 0, 1, 0, 4);
                continue;
            }
            if (strcmp(name, "li") == 0)
            {
                if (!is_close)
                {
                    webview_pending_tag(name, 1, 0, 1, tag_inner, tag_stop);
                }
                else
                {
                    webview_flush_text();
                }
                continue;
            }
            if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0 ||
                strcmp(name, "tr") == 0 || strcmp(name, "table") == 0)
            {
                if (!is_close)
                {
                    webview_pending_tag(name, 2, 0, 0, tag_inner, tag_stop);
                }
                else
                {
                    webview_flush_text();
                }
                continue;
            }
            if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)
            {
                /* Cells degrade to stacked lines in v0.1: the flush is what
                 * keeps two cells from running into one word. */
                webview_flush_text();
                continue;
            }
            if (strcmp(name, "img") == 0 && !is_close)
            {
                webview_cur_append_text(" [image] ");
                continue;
            }
            if (strcmp(name, "a") == 0)
            {
                if (is_close)
                {
                    webview_close_anchor();
                    continue;
                }
                if (!skip && !in_head)
                {
                    char href[WEBVIEW_PATH_CAPACITY];

                    if (webview_extract_attr(tag_inner, tag_stop, "href", href,
                            sizeof(href)))
                    {
                        webview_open_anchor(href);
                    }
                }
                continue;
            }
            if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0)
            {
                if (is_close)
                {
                    webview_inline_pop();
                    continue;
                }
                if (!skip && !in_head)
                {
                    struct webview_decl decl;

                    memset(&decl, 0, sizeof(decl));
                    decl.hasBold = 1;
                    decl.bold = 1;
                    webview_inline_push(&decl);
                }
                continue;
            }
            if (strcmp(name, "u") == 0)
            {
                if (is_close)
                {
                    webview_inline_pop();
                    continue;
                }
                if (!skip && !in_head)
                {
                    struct webview_decl decl;

                    memset(&decl, 0, sizeof(decl));
                    decl.hasUnderline = 1;
                    decl.underline = 1;
                    webview_inline_push(&decl);
                }
                continue;
            }
            /* i/em: no italic face exists; the text stays, unstyled, openly. */
            if (strcmp(name, "font") == 0 || strcmp(name, "span") == 0)
            {
                if (is_close)
                {
                    webview_inline_pop();
                    continue;
                }
                if (!skip && !in_head)
                {
                    char style[256];
                    struct webview_decl decl;
                    char color[32];
                    int has_any = 0;

                    memset(&decl, 0, sizeof(decl));
                    if (webview_extract_attr(tag_inner, tag_stop, "style", style,
                            sizeof(style)))
                    {
                        webview_parse_decls(style, style + strlen(style), &decl);
                        has_any = 1;
                    }
                    if (webview_extract_attr(tag_inner, tag_stop, "color", color,
                            sizeof(color)))
                    {
                        uint32_t parsed = 0;

                        if (webview_parse_color(color, color + strlen(color), &parsed))
                        {
                            decl.hasColor = 1;
                            decl.color = parsed;
                            has_any = 1;
                        }
                    }
                    /* size= shapes no face runs carry: ignored, like px sizes. */
                    if (has_any)
                    {
                        webview_inline_push(&decl);
                    }
                }
                continue;
            }
            /* Inline and unknown tags keep their text: nothing to do. */
        }
    }
    webview_flush_text();
    while (title_length > 0 && g_title[title_length - 1] == ' ')
    {
        g_title[--title_length] = '\0';
    }
}

/* ---- document loading ------------------------------------------------------ */

static void webview_refresh_status(void)
{
    char suffix[96];

    snprintf(suffix, sizeof(suffix), "%s%s%s", g_truncated ? " (first 32 KB)" : "",
        g_fetch_note, g_show_source ? " (source)" : "");
    if (!g_has_file)
    {
        snprintf(g_status, sizeof(g_status), "No file loaded. File > Open (F3).");
        return;
    }
    if (g_title[0] != '\0')
    {
        snprintf(g_status, sizeof(g_status), "%s%s - %s", g_title, suffix, g_path);
        return;
    }
    snprintf(g_status, sizeof(g_status), "%s%s", g_path, suffix);
}

static void webview_invalidate_layout(void)
{
    g_measured_width = -1;
    WEBVIEW_SCROLL->value = 0;
    sxgui_app_request_repaint(&g_app);
}

static int webview_load_file(const char *path);
static int webview_fetch_http(const char *url);
static int webview_open_location(const char *location, int push);
static void webview_navigate(const char *address, int push);

static void webview_show_welcome(void)
{
    g_has_file = 0;
    g_path[0] = '\0';
    g_truncated = 0;
    g_para_count = 0;
    webview_reset_styles();
    webview_push_para("Web Viewer", 1, 0, 0, 0);
    webview_push_para(
        "A minimal browser for local files and HTTP by name or IP, in the spirit "
        "of IE 1.1. This version fetches pages, follows links and redirects, "
        "paints a first CSS and shows source with Ctrl+U.",
        0, 0, 0, 2);
    webview_push_para("", 0, 1, 0, 4);
    webview_push_para("Type a path or an http:// address up there "
                      "and press Go (or Enter). File > Open (F3) works too: try "
                      "/disk/welcome.html.",
        0, 0, 1, 2);
    webview_push_para("Double-clicking an .html file in Files opens it here.", 0, 0, 1, 1);
    webview_push_para(
        "Not in this version: images and background loading. "
        "Names resolve through the network's DNS.",
        0, 0, 0, 2);
    webview_refresh_status();
    webview_invalidate_layout();
}

static int webview_load_file(const char *path)
{
    FILE *stream = 0;
    struct stat info = {0};
    size_t read_bytes = 0;

    if (path == 0 || path[0] == '\0')
    {
        return -1;
    }
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
    {
        snprintf(g_status, sizeof(g_status), "That path is a directory.");
        return -1;
    }
    stream = fopen(path, "r");
    if (stream == 0)
    {
        snprintf(g_status, sizeof(g_status), "Cannot open that file.");
        return -1;
    }
    read_bytes = fread(g_raw, 1, sizeof(g_raw) - 1, stream);
    fclose(stream);
    g_raw[read_bytes] = '\0';

    g_truncated = read_bytes == sizeof(g_raw) - 1;
    g_fetch_note[0] = '\0';
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_has_file = 1;
    return 0;
}

/* View Source: the raw buffer as plain lines, no tag stripping and no entity
 * decoding (source must show &amp; as typed). Control bytes are dropped, tabs
 * become two spaces so columns survive. */
static void webview_build_source(void)
{
    const char *cursor = g_raw;

    g_para_count = 0;
    webview_reset_styles();
    g_title[0] = '\0';
    while (*cursor != '\0' && g_para_count < WEBVIEW_PARA_MAX)
    {
        char line[WEBVIEW_PARA_LENGTH];
        int length = 0;

        while (*cursor != '\0' && *cursor != '\n' && length + 3 < (int)sizeof(line))
        {
            unsigned char value = (unsigned char)*cursor;

            cursor += 1;
            if (value == '\r')
            {
                continue;
            }
            if (value == '\t')
            {
                line[length++] = ' ';
                line[length++] = ' ';
                continue;
            }
            if (value < 32 || value == 127)
            {
                continue;
            }
            line[length++] = (char)value;
        }
        while (*cursor != '\0' && *cursor != '\n')
        {
            cursor += 1;
        }
        if (*cursor == '\n')
        {
            cursor += 1;
        }
        line[length] = '\0';
        webview_push_para(line, 0, 0, 0, 0);
    }
}

/* Renders g_raw in the current mode. Both loaders funnel here so source and
 * page views never diverge. */
static void webview_render_current(void)
{
    if (g_show_source)
    {
        webview_build_source();
    }
    else
    {
        webview_parse();
    }
    if (g_para_count == 0)
    {
        webview_push_para("(Empty document)", 0, 0, 0, 0);
    }
    webview_refresh_status();
    webview_invalidate_layout();
}

/* ---- name resolution --------------------------------------------------------
 *
 * Minimal DNS over UDP, A records only. Numeric IPv4 stays a fast path that
 * never touches the network; anything else goes out as one A/IN question to
 * the network's DNS (10.0.2.3 under QEMU slirp, overridable with --dns for
 * tests). No search domains, no AAAA, no TCP fallback on truncation: each gap
 * has its own honest failure below.
 *
 * Two honesty notes. Transaction IDs mix uptime with a counter because the
 * tree has no kernel RNG yet (docs mention arc4random as still missing): fine
 * against accidents on a host bridge, not against an adversary. And answers
 * are trusted past TXID plus the echoed question: no DNSSEC on this road.
 */

#define WEBVIEW_DNS_PORT_DEFAULT 53
#define WEBVIEW_DNS_SERVER_DEFAULT "10.0.2.3"
#define WEBVIEW_DNS_TIMEOUT_MS 2000
#define WEBVIEW_DNS_ATTEMPTS 2
#define WEBVIEW_DNS_MSG_CAP 512
#define WEBVIEW_DNS_CNAME_HOPS 4
#define WEBVIEW_DNS_NAME_CAP 256

static uint32_t g_dns_ipv4 = 0;
static unsigned g_dns_port = WEBVIEW_DNS_PORT_DEFAULT;
static unsigned g_dns_seq = 0;

static int webview_parse_uint(const char *text, unsigned *value);
static int webview_parse_ipv4(const char *text, uint32_t *address);

/* Parses a resolver override, host or host:port. Returns 1 and stores it. */
static int webview_parse_dns_server(const char *text)
{
    char host[WEBVIEW_DNS_NAME_CAP];
    const char *colon;
    size_t host_length;
    unsigned port = WEBVIEW_DNS_PORT_DEFAULT;
    uint32_t ipv4 = 0;

    if (text == 0 || text[0] == '\0')
    {
        return 0;
    }
    colon = strchr(text, ':');
    if (colon != 0 && strchr(colon + 1, ':') != 0)
    {
        return 0; /* IPv6 is not on this road */
    }
    if (colon != 0)
    {
        char port_text[8];
        size_t port_length = strlen(colon + 1);

        if (port_length == 0 || port_length >= sizeof(port_text))
        {
            return 0;
        }
        memcpy(port_text, colon + 1, port_length);
        port_text[port_length] = '\0';
        if (!webview_parse_uint(port_text, &port) || port == 0 || port > 65535u)
        {
            return 0;
        }
    }
    host_length = colon != 0 ? (size_t)(colon - text) : strlen(text);
    if (host_length == 0 || host_length + 1 > sizeof(host))
    {
        return 0;
    }
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    if (!webview_parse_ipv4(host, &ipv4))
    {
        return 0;
    }
    g_dns_ipv4 = ipv4;
    g_dns_port = port;
    return 1;
}

static uint16_t webview_read_u16(const unsigned char *at)
{
    return (uint16_t)(((uint16_t)at[0] << 8) | at[1]);
}

/* Encodes a dotted host into wire labels. Returns 0 with the byte count. */
static int webview_dns_encode_name(const char *host, unsigned char *out, size_t cap,
    size_t *out_length)
{
    size_t pos = 0;
    const char *label = host;

    if (host == 0 || host[0] == '\0')
    {
        return -1;
    }
    for (;;)
    {
        const char *dot = strchr(label, '.');
        size_t length = dot != 0 ? (size_t)(dot - label) : strlen(label);

        if (length == 0 || length > 63 || pos + 1 + length + 1 > cap)
        {
            return -1;
        }
        out[pos++] = (unsigned char)length;
        memcpy(out + pos, label, length);
        pos += length;
        if (dot == 0)
        {
            break;
        }
        label = dot + 1;
        if (*label == '\0')
        {
            break; /* trailing dot is the root, already implied */
        }
    }
    if (pos + 1 > cap)
    {
        return -1;
    }
    out[pos++] = 0;
    *out_length = pos;
    return 0;
}

/* Reads a possibly-compressed name at `off`: dotted text into `out` (or skips
 * it when `out` is 0), and the offset past it into `consumed`. Jump loops and
 * every overrun return -1: these bytes come from the network. */
static int webview_dns_read_name(const unsigned char *msg, size_t msg_length, size_t off,
    char *out, size_t out_cap, size_t *consumed)
{
    size_t out_length = 0;
    size_t jumps = 0;
    size_t jump_end = 0;
    int jumped = 0;
    int first = 1;

    for (;;)
    {
        unsigned char length;

        if (off >= msg_length)
        {
            return -1;
        }
        length = msg[off];
        if ((length & 0xC0) == 0xC0)
        {
            if (off + 1 >= msg_length)
            {
                return -1;
            }
            if (!jumped)
            {
                jump_end = off + 2;
                jumped = 1;
            }
            off = ((size_t)(length & 0x3F) << 8) | msg[off + 1];
            jumps += 1;
            if (jumps > 8 || off >= msg_length)
            {
                return -1;
            }
            continue;
        }
        if (length > 63)
        {
            return -1;
        }
        if (length == 0)
        {
            if (out != 0 && out_cap > 0)
            {
                out[out_length < out_cap ? out_length : out_cap - 1] = '\0';
            }
            *consumed = jumped ? jump_end : off + 1;
            return 0;
        }
        if (off + 1 + length > msg_length)
        {
            return -1;
        }
        if (out != 0)
        {
            if (!first)
            {
                if (out_length + 1 >= out_cap)
                {
                    return -1;
                }
                out[out_length++] = '.';
            }
            if (out_length + length >= out_cap)
            {
                return -1;
            }
            memcpy(out + out_length, msg + off + 1, length);
            out_length += length;
        }
        first = 0;
        off += 1 + length;
    }
}

/* Inspects one response: 0 with the first A record, 1 on NXDOMAIN, 2 with a
 * CNAME target to chase, -1 when the bytes say nothing usable (wrong TXID,
 * truncation, overruns included). */
static int webview_dns_parse(const unsigned char *msg, size_t length, uint16_t txid,
    const unsigned char *question, size_t question_length, uint32_t *ipv4,
    char *cname, size_t cname_cap)
{
    uint16_t qdcount = 0;
    uint16_t ancount = 0;
    size_t off = 0;
    size_t i;
    int have_cname = 0;

    if (length < 12 || webview_read_u16(msg) != txid)
    {
        return -1;
    }
    if ((msg[2] & 0x80) == 0 || ((msg[2] >> 3) & 0x0F) != 0)
    {
        return -1;
    }
    if ((msg[3] & 0x0F) == 3)
    {
        return 1;
    }
    if ((msg[2] & 0x02) != 0)
    {
        return -1; /* truncated: no TCP road yet */
    }
    qdcount = webview_read_u16(msg + 4);
    ancount = webview_read_u16(msg + 6);
    off = 12;
    for (i = 0; i < qdcount && i < 4; ++i)
    {
        size_t consumed = 0;

        if (webview_dns_read_name(msg, length, off, 0, 0, &consumed) != 0 ||
            consumed + 4 > length)
        {
            return -1;
        }
        if (i == 0)
        {
            /* The echoed question must be ours: TXID alone is thin. `have`
             * covers the name; the 4 type/class bytes ride along, in bounds
             * per the check above. */
            size_t have = consumed - off;

            if (have + 4 != question_length || memcmp(msg + off, question, have + 4) != 0)
            {
                return -1;
            }
        }
        off = consumed + 4;
    }
    for (i = 0; i < ancount && i < 16; ++i)
    {
        size_t consumed = 0;
        uint16_t type = 0;
        uint16_t class = 0;
        uint16_t rdlength = 0;

        if (webview_dns_read_name(msg, length, off, 0, 0, &consumed) != 0 ||
            consumed + 10 > length)
        {
            return -1;
        }
        type = webview_read_u16(msg + consumed);
        class = webview_read_u16(msg + consumed + 2);
        rdlength = webview_read_u16(msg + consumed + 8);
        off = consumed + 10;
        if (off + rdlength > length)
        {
            return -1;
        }
        if (type == 1 && class == 1 && rdlength == 4)
        {
            *ipv4 = ((uint32_t)msg[off] << 24) | ((uint32_t)msg[off + 1] << 16) |
                    ((uint32_t)msg[off + 2] << 8) | msg[off + 3];
            return 0;
        }
        if (type == 5 && class == 1 && !have_cname)
        {
            size_t ignored = 0;

            if (webview_dns_read_name(msg, length, off, cname, cname_cap, &ignored) == 0)
            {
                have_cname = 1;
            }
        }
        off += rdlength;
    }
    return have_cname ? 2 : -1;
}

/* One A/IN question, one transaction. Returns 0 with the address, 1 on
 * NXDOMAIN, 2 with a CNAME to chase, -2 on a malformed name, -1 otherwise. */
static int webview_dns_query(const char *host, uint32_t *ipv4, char *cname, size_t cname_cap)
{
    unsigned char query[WEBVIEW_DNS_MSG_CAP];
    unsigned char response[WEBVIEW_DNS_MSG_CAP];
    size_t qname_length = 0;
    size_t question_length = 0;
    struct savanxp_sockaddr_in server;
    struct savanxp_sockaddr_in local;
    long fd = -1;
    int attempt = 0;
    int result = -1;

    if (webview_dns_encode_name(host, query + 12, sizeof(query) - 12 - 4, &qname_length) != 0)
    {
        return -2;
    }
    question_length = qname_length + 4;
    fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (fd < 0)
    {
        return -1;
    }
    memset(&local, 0, sizeof(local));
    if (savanxp_bind((int)fd, &local) < 0)
    {
        savanxp_close((int)fd);
        return -1;
    }
    memset(&server, 0, sizeof(server));
    server.ipv4 = g_dns_ipv4;
    server.port = (uint16_t)g_dns_port;
    for (attempt = 0; attempt < WEBVIEW_DNS_ATTEMPTS; ++attempt)
    {
        uint16_t txid = (uint16_t)(uptime_ms() + (unsigned long)g_dns_seq * 0x9E37ul);
        long sent;
        long got;

        g_dns_seq += 1;
        query[0] = (unsigned char)(txid >> 8);
        query[1] = (unsigned char)(txid & 0xFF);
        query[2] = 0x01;
        query[3] = 0x00;
        query[4] = 0x00;
        query[5] = 0x01;
        query[6] = query[7] = query[8] = query[9] = query[10] = query[11] = 0;
        query[12 + qname_length] = 0x00;
        query[12 + qname_length + 1] = 0x01;
        query[12 + qname_length + 2] = 0x00;
        query[12 + qname_length + 3] = 0x01;
        sent = savanxp_sendto((int)fd, query, 12 + question_length, &server);
        if (sent < 0)
        {
            continue;
        }
        got = savanxp_recvfrom((int)fd, response, sizeof(response), 0, WEBVIEW_DNS_TIMEOUT_MS);
        if (got <= 0)
        {
            continue;
        }
        result = webview_dns_parse(response, (size_t)got, txid, query + 12, question_length,
            ipv4, cname, cname_cap);

        if (result == 0 || result == 1 || result == 2)
        {
            break;
        }
    }
    savanxp_close((int)fd);
    return result;
}

/* Full resolution with CNAME chasing. 0 ok, 1 NXDOMAIN, -2 malformed, -1 fail. */
static int webview_resolve_host(const char *host, uint32_t *ipv4)
{
    char current[WEBVIEW_DNS_NAME_CAP];
    int hops = 0;

    if (host == 0 || host[0] == '\0' || strlen(host) > 253)
    {
        return -2;
    }
    if (webview_parse_ipv4(host, ipv4))
    {
        return 0;
    }
    snprintf(current, sizeof(current), "%s", host);
    for (hops = 0; hops <= WEBVIEW_DNS_CNAME_HOPS; ++hops)
    {
        char cname[WEBVIEW_DNS_NAME_CAP];
        int result;

        cname[0] = '\0';
        result = webview_dns_query(current, ipv4, cname, sizeof(cname));
        if (result == 0 || result == 1 || result == -2)
        {
            return result;
        }
        if (result == 2 && cname[0] != '\0')
        {
            snprintf(current, sizeof(current), "%s", cname);
            continue;
        }
        return -1;
    }
    return -1;
}

/* ---- fetching ---------------------------------------------------------------
 *
 * Plain HTTP/1.0 over a blocking TCP client, the tcpget recipe moved indoors:
 * connect, GET, read to EOF. Hosts resolve through DNS (numeric IPv4 stays a
 * fast path); no TLS, no chunked
 * body (a 1.0 request avoids it), no background loading: the window stands
 * still while the bytes arrive, which on a LAN is milliseconds.
 */

static void webview_refresh_nav(void);
static void webview_sync_address(void);
static void webview_history_push(const char *path);

#define WEBVIEW_HTTP_PORT_DEFAULT 80
#define WEBVIEW_FETCH_REDIRECTS 5
#define WEBVIEW_URL_HOST_CAP 64
#define WEBVIEW_URL_PATH_CAP 512

static int webview_parse_uint(const char *text, unsigned *value)
{
    unsigned result = 0;
    size_t index = 0;

    if (text == 0 || text[0] == '\0')
    {
        return 0;
    }
    while (text[index] != '\0')
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return 0;
        }
        result = result * 10u + (unsigned)(text[index] - '0');
        index += 1;
    }
    *value = result;
    return 1;
}

static int webview_parse_ipv4(const char *text, uint32_t *address)
{
    unsigned parts[4];
    unsigned part = 0;
    unsigned count = 0;
    const char *cursor = text;

    while (*cursor != '\0' && count < 4)
    {
        const char *start = cursor;

        while (*cursor != '\0' && *cursor != '.')
        {
            cursor += 1;
        }
        {
            char chunk[4];
            size_t length = (size_t)(cursor - start);

            if (length == 0 || length >= sizeof(chunk))
            {
                return 0;
            }
            memcpy(chunk, start, length);
            chunk[length] = '\0';
            if (!webview_parse_uint(chunk, &part) || part > 255u)
            {
                return 0;
            }
            parts[count++] = part;
        }
        if (*cursor == '.')
        {
            cursor += 1;
        }
    }
    if (*cursor != '\0' || count != 4)
    {
        return 0;
    }
    *address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

static int webview_location_is_http(const char *location)
{
    return location != 0 && strncmp(location, "http://", 7) == 0;
}

/* Splits an http:// URL into host, port and path. Host keeps no brackets and
 * no port; an absent path becomes "/". */
static int webview_split_http_url(const char *url, char *host, size_t host_cap,
    unsigned *port, char *path, size_t path_cap)
{
    const char *cursor = url + 7;
    const char *slash = strchr(cursor, '/');
    const char *colon;
    size_t host_length;

    if (*cursor == '\0')
    {
        return -1;
    }
    colon = strchr(cursor, ':');
    if (colon != 0 && (slash == 0 || colon < slash))
    {
        char port_text[8];
        size_t port_length = slash != 0 ? (size_t)(slash - colon - 1)
                                        : strlen(colon + 1);

        host_length = (size_t)(colon - cursor);
        if (port_length == 0 || port_length >= sizeof(port_text))
        {
            return -1;
        }
        memcpy(port_text, colon + 1, port_length);
        port_text[port_length] = '\0';
        if (!webview_parse_uint(port_text, port) || *port == 0 || *port > 65535u)
        {
            return -1;
        }
    }
    else
    {
        host_length = slash != 0 ? (size_t)(slash - cursor) : strlen(cursor);
        *port = WEBVIEW_HTTP_PORT_DEFAULT;
    }
    if (host_length == 0 || host_length + 1 > host_cap)
    {
        return -1;
    }
    memcpy(host, cursor, host_length);
    host[host_length] = '\0';
    snprintf(path, path_cap, "%s", slash != 0 ? slash : "/");
    return 0;
}

/* Resolves `ref` (a link target, a Location header, an address) against the
 * current location `base`. Returns 0 with the absolute form in `out`, -1 for
 * an unsupported scheme (mailto:, javascript:, ...), -2 when it does not fit.
 * Fragments are dropped: there is no scroll-to-anchor yet. */
static int webview_resolve_against(const char *base, const char *ref, char *out,
    size_t capacity)
{
    char clean[WEBVIEW_PATH_CAPACITY];
    size_t clean_length = 0;
    const char *colon;
    const char *slash;

    while (*ref != '\0' && clean_length + 1 < sizeof(clean))
    {
        if (*ref == '#')
        {
            break;
        }
        clean[clean_length++] = *ref;
        ref += 1;
    }
    clean[clean_length] = '\0';
    if (clean[0] == '\0')
    {
        snprintf(out, capacity, "%s", base);
        return 0;
    }
    if (strstr(clean, "://") != 0)
    {
        snprintf(out, capacity, "%s", clean);
        return 0;
    }
    /* Scheme without // (mailto:foo): a colon before any slash can only be
     * that. A relative path with a colon later (dir/x:2) falls through. */
    colon = strchr(clean, ':');
    slash = strchr(clean, '/');
    if (colon != 0 && (slash == 0 || colon < slash))
    {
        return -1;
    }
    if (webview_location_is_http(base))
    {
        char host[WEBVIEW_URL_HOST_CAP];
        unsigned port = 0;
        char ignored[WEBVIEW_URL_PATH_CAP];
        const char *base_path;
        const char *last_slash;
        size_t dir_length;

        if (webview_split_http_url(base, host, sizeof(host), &port, ignored,
                sizeof(ignored)) != 0)
        {
            return -2;
        }
        base_path = strchr(base + 7, '/');
        if (base_path == 0)
        {
            base_path = "/";
        }
        if (clean[0] == '/')
        {
            if ((size_t)snprintf(out, capacity, "http://%s%s", host, clean) >= capacity)
            {
                return -2;
            }
            /* Non-default ports have to survive the round trip. */
            if (port != WEBVIEW_HTTP_PORT_DEFAULT)
            {
                char with_port[WEBVIEW_PATH_CAPACITY];

                if ((size_t)snprintf(with_port, sizeof(with_port), "http://%s:%u%s",
                        host, port, clean) >= sizeof(with_port))
                {
                    return -2;
                }
                snprintf(out, capacity, "%s", with_port);
            }
            return 0;
        }
        last_slash = strrchr(base_path, '/');
        dir_length = last_slash != 0 ? (size_t)(last_slash - base_path + 1) : 1;
        if (port == WEBVIEW_HTTP_PORT_DEFAULT)
        {
            if ((size_t)snprintf(out, capacity, "http://%s%.*s%s", host, (int)dir_length,
                    base_path, clean) >= capacity)
            {
                return -2;
            }
            return 0;
        }
        if ((size_t)snprintf(out, capacity, "http://%s:%u%.*s%s", host, port,
                (int)dir_length, base_path, clean) >= capacity)
        {
            return -2;
        }
        return 0;
    }
    if (clean[0] == '/')
    {
        snprintf(out, capacity, "%s", clean);
        return 0;
    }
    {
        const char *last_slash = strrchr(base, '/');
        size_t dir_length = last_slash != 0 ? (size_t)(last_slash - base) : 0;

        if (dir_length + 1 + strlen(clean) + 1 > capacity)
        {
            return -2;
        }
        memcpy(out, base, dir_length);
        out[dir_length] = '/';
        snprintf(out + dir_length + 1, capacity - dir_length - 1, "%s", clean);
        return 0;
    }
}

static int webview_fetch_write_all(int fd, const char *buffer, size_t length)
{
    size_t sent = 0;

    while (sent < length)
    {
        long written = savanxp_write(fd, buffer + sent, length - sent);

        if (written <= 0)
        {
            return -1;
        }
        sent += (size_t)written;
    }
    return 0;
}

/* Fetches an http:// URL into g_raw, following redirects. Sets g_path to the
 * final URL. Returns 0 with the body ready, -1 with the reason in g_status. */
static int webview_fetch_http(const char *url)
{
    char current[WEBVIEW_PATH_CAPACITY];
    int hop;

    snprintf(current, sizeof(current), "%s", url);
    for (hop = 0; hop <= WEBVIEW_FETCH_REDIRECTS; hop += 1)
    {
        char host[WEBVIEW_URL_HOST_CAP];
        char path[WEBVIEW_URL_PATH_CAP];
        unsigned port = 0;
        uint32_t ipv4 = 0;
        struct savanxp_sockaddr_in address;
        long fd;
        char request[1024];
        size_t total = 0;
        int read_failed = 0;
        const char *headers_end = 0;
        size_t header_length = 0;
        const char *body = 0;
        size_t body_length = 0;
        int code = 0;

        if (webview_split_http_url(current, host, sizeof(host), &port, path,
                sizeof(path)) != 0)
        {
            snprintf(g_status, sizeof(g_status), "That URL is malformed.");
            return -1;
        }
        if (!webview_parse_ipv4(host, &ipv4))
        {
            int resolved = webview_resolve_host(host, &ipv4);

            if (resolved == 1)
            {
                snprintf(g_status, sizeof(g_status), "No such host: %s.", host);
                return -1;
            }
            if (resolved == -2)
            {
                snprintf(g_status, sizeof(g_status), "That name is malformed.");
                return -1;
            }
            if (resolved != 0)
            {
                snprintf(g_status, sizeof(g_status), "Cannot resolve %s.", host);
                return -1;
            }
        }
        fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
        if (fd < 0)
        {
            snprintf(g_status, sizeof(g_status), "No network.");
            return -1;
        }
        memset(&address, 0, sizeof(address));
        address.ipv4 = ipv4;
        address.port = (uint16_t)port;
        if (savanxp_connect((int)fd, &address, 5000) < 0)
        {
            snprintf(g_status, sizeof(g_status), "Cannot connect to %s.", host);
            savanxp_close((int)fd);
            return -1;
        }
        snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\n"
                                           "Connection: close\r\n\r\n",
            path, host);
        if (webview_fetch_write_all((int)fd, request, strlen(request)) != 0)
        {
            snprintf(g_status, sizeof(g_status), "The request failed.");
            savanxp_close((int)fd);
            return -1;
        }
        for (;;)
        {
            long got;

            if (total >= sizeof(g_raw) - 1)
            {
                break;
            }
            got = savanxp_read((int)fd, g_raw + total, sizeof(g_raw) - 1 - total);
            if (got == 0)
            {
                break;
            }
            if (got < 0)
            {
                read_failed = 1;
                break;
            }
            total += (size_t)got;
        }
        savanxp_close((int)fd);
        g_raw[total] = '\0';

        headers_end = strstr(g_raw, "\r\n\r\n");
        if (headers_end != 0)
        {
            headers_end += 4;
        }
        else
        {
            headers_end = strstr(g_raw, "\n\n");
            if (headers_end != 0)
            {
                headers_end += 2;
            }
        }
        if (headers_end == 0)
        {
            if (read_failed || total == 0)
            {
                snprintf(g_status, sizeof(g_status), "The connection dropped.");
            }
            else
            {
                snprintf(g_status, sizeof(g_status), "Not an HTTP response.");
            }
            return -1;
        }
        header_length = (size_t)(headers_end - g_raw);
        body = headers_end;
        body_length = total - header_length;
        {
            const char *space = strchr(g_raw, ' ');

            if (strncmp(g_raw, "HTTP/", 5) != 0 || space == 0 || space[1] < '0' ||
                space[1] > '9' || space[2] < '0' || space[2] > '9' || space[3] < '0' ||
                space[3] > '9')
            {
                snprintf(g_status, sizeof(g_status), "Not an HTTP response.");
                return -1;
            }
            code = (space[1] - '0') * 100 + (space[2] - '0') * 10 + (space[3] - '0');
        }
        if (code >= 300 && code < 400)
        {
            /* Redirect: find the Location header, resolve it against this URL
             * (absolute, root-absolute or relative all work) and refetch. The
             * half-read body is discarded; the connection is already closed. */
            const char *line = strchr(g_raw, '\n');
            char location[WEBVIEW_PATH_CAPACITY];
            char next[WEBVIEW_PATH_CAPACITY];
            int resolved = -2;

            location[0] = '\0';
            while (line != 0 && line < headers_end)
            {
                const char *name = line + 1;

                if ((name[0] == 'L' || name[0] == 'l') && (name[1] == 'o' || name[1] == 'O') &&
                    strncmp(name + 2, "cation:", 7) == 0)
                {
                    /* "Location:" is 9 characters: name + 9 is the value. */
                    const char *value = name + 9;
                    size_t value_length = 0;

                    while (*value == ' ' || *value == '\t')
                    {
                        value += 1;
                    }
                    while (value[value_length] != '\0' && value[value_length] != '\r' &&
                           value[value_length] != '\n' &&
                           value_length + 1 < sizeof(location))
                    {
                        value_length += 1;
                    }
                    memcpy(location, value, value_length);
                    location[value_length] = '\0';
                    break;
                }
                line = strchr(line + 1, '\n');
            }
            if (location[0] == '\0')
            {
                snprintf(g_status, sizeof(g_status),
                    "The server redirected without saying where.");
                return -1;
            }
            resolved = webview_resolve_against(current, location, next, sizeof(next));
            if (resolved == -1 || strstr(next, "://") == 0 ||
                (strncmp(next, "http://", 7) != 0 && strncmp(next, "file://", 7) != 0))
            {
                snprintf(g_status, sizeof(g_status),
                    "Cannot follow that redirect.");
                return -1;
            }
            if (resolved == -2)
            {
                snprintf(g_status, sizeof(g_status), "That address is too long.");
                return -1;
            }
            if (strncmp(next, "file://", 7) == 0)
            {
                /* An http URL redirecting to a file: resolve, then fall into
                 * the file loader below through the shared tail. */
                snprintf(current, sizeof(current), "%s", next + 7);
                if (webview_load_file(current) == 0)
                {
                    return 0;
                }
                return -1;
            }
            snprintf(current, sizeof(current), "%s", next);
            continue;
        }
        /* Any other status keeps its body: error pages are pages too. */
        memmove(g_raw, body, body_length);
        g_raw[body_length] = '\0';
        g_truncated = total >= sizeof(g_raw) - 1;
        g_fetch_note[0] = '\0';
        if (read_failed)
        {
            snprintf(g_fetch_note, sizeof(g_fetch_note), " (connection dropped)");
        }
        snprintf(g_path, sizeof(g_path), "%s", current);
        g_has_file = 1;
        return 0;
    }
    snprintf(g_status, sizeof(g_status), "Too many redirects.");
    return -1;
}

/* The single front door for every navigation: address bar, links, history,
 * dialogs, argv. Resolves `location` against the current page, loads it, and
 * on success records history, re-renders and syncs the chrome. */
static int webview_open_location(const char *location, int push)
{
    char target[WEBVIEW_PATH_CAPACITY];
    int resolved;

    if (location == 0 || location[0] == '\0')
    {
        return -1;
    }
    if (strncmp(location, "dns://", 6) == 0)
    {
        /* Resolver override, typed not launched: `dns://10.0.2.2:5353`.
         * Config, not a page: it sets the resolver, reports it, reverts the
         * bar and stays out of history. Same parser as --dns. */
        if (webview_parse_dns_server(location + 6))
        {
            snprintf(g_status, sizeof(g_status), "Resolver: %s.", location + 6);
        }
        else
        {
            snprintf(g_status, sizeof(g_status), "Bad resolver, keeping the old one.");
        }
        webview_sync_address();
        webview_refresh_nav();
        return 0;
    }
    if (strstr(location, "://") != 0)
    {
        snprintf(target, sizeof(target), "%s", location);
    }
    else
    {
        resolved = webview_resolve_against(g_has_file ? g_path : "/disk/", location,
            target, sizeof(target));
        if (resolved == -1)
        {
            snprintf(g_status, sizeof(g_status), "That kind of link is not supported.");
            webview_sync_address();
            return -1;
        }
        if (resolved == -2)
        {
            snprintf(g_status, sizeof(g_status), "That address is too long.");
            webview_sync_address();
            return -1;
        }
    }
    if (webview_location_is_http(target))
    {
        if (webview_fetch_http(target) != 0)
        {
            webview_sync_address();
            webview_refresh_nav();
            return -1;
        }
    }
    else if (strncmp(target, "file://", 7) == 0)
    {
        if (webview_load_file(target + 7) != 0)
        {
            webview_sync_address();
            webview_refresh_nav();
            return -1;
        }
    }
    else if (strstr(target, "://") != 0)
    {
        snprintf(g_status, sizeof(g_status), "Only http and file work so far.");
        webview_sync_address();
        webview_refresh_nav();
        return -1;
    }
    else
    {
        if (webview_load_file(target) != 0)
        {
            webview_sync_address();
            webview_refresh_nav();
            return -1;
        }
    }
    if (push)
    {
        webview_history_push(g_path);
    }
    webview_render_current();
    webview_sync_address();
    webview_refresh_nav();
    return 0;
}

static void webview_toggle_source(void)
{
    if (!g_has_file && g_raw[0] == '\0')
    {
        snprintf(g_status, sizeof(g_status), "Nothing to show yet.");
        return;
    }
    g_show_source = !g_show_source;
    webview_render_current();
}

static void webview_refresh_nav(void)
{
    if (g_hist_index > 0)
    {
        WEBVIEW_BACK->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        WEBVIEW_BACK->flags |= SXGUI_FLAG_DISABLED;
    }
    if (g_hist_index >= 0 && g_hist_index + 1 < g_hist_count)
    {
        WEBVIEW_FWD->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        WEBVIEW_FWD->flags |= SXGUI_FLAG_DISABLED;
    }
}

static void webview_sync_address(void)
{
    snprintf(g_address, sizeof(g_address), "%s", g_has_file ? g_path : "");
    WEBVIEW_ADDR->caret = (int)strlen(g_address);
    WEBVIEW_ADDR->scroll = 0;
}

static void webview_history_push(const char *path)
{
    if (g_hist_index >= 0 && g_hist_count > 0 &&
        strcmp(g_history[g_hist_index], path) == 0)
    {
        /* Same page (Go on the current address, Reload): no duplicate. */
        return;
    }
    if (g_hist_index + 1 < g_hist_count)
    {
        g_hist_count = g_hist_index + 1;
    }
    if (g_hist_count >= WEBVIEW_HISTORY_MAX)
    {
        memmove(&g_history[0][0], &g_history[1][0],
            (size_t)(WEBVIEW_HISTORY_MAX - 1) * WEBVIEW_PATH_CAPACITY);
        g_hist_count = WEBVIEW_HISTORY_MAX - 1;
        g_hist_index = g_hist_count - 1;
    }
    snprintf(g_history[g_hist_count], WEBVIEW_PATH_CAPACITY, "%s", path);
    g_hist_index = g_hist_count;
    g_hist_count += 1;
}

static void webview_history_go(int delta)
{
    int target = g_hist_index + delta;

    if (g_hist_count == 0)
    {
        return;
    }
    if (target < 0)
    {
        target = 0;
    }
    if (target > g_hist_count - 1)
    {
        target = g_hist_count - 1;
    }
    if (target == g_hist_index)
    {
        return;
    }
    if (webview_open_location(g_history[target], 0) == 0)
    {
        g_hist_index = target;
    }
    webview_refresh_nav();
}

/* The address bar takes anything open_location understands: file paths,
 * file:// URLs and http:// URLs. Trims the edges; an empty bar reverts. */
static void webview_navigate(const char *address, int push)
{
    const char *cursor = address;
    const char *end;
    char trimmed[WEBVIEW_PATH_CAPACITY];
    size_t length = 0;

    if (cursor == 0)
    {
        return;
    }
    while (*cursor == ' ' || *cursor == '\t')
    {
        cursor += 1;
    }
    end = cursor + strlen(cursor);
    while (end > cursor && (end[-1] == ' ' || end[-1] == '\t'))
    {
        end -= 1;
    }
    while (cursor < end && length + 1 < sizeof(trimmed))
    {
        trimmed[length++] = *cursor;
        cursor += 1;
    }
    trimmed[length] = '\0';
    if (trimmed[0] == '\0')
    {
        webview_sync_address();
        return;
    }
    (void)webview_open_location(trimmed, push);
}

/* ---- layout and painting ----------------------------------------------------
 *
 * Paragraphs are wrapped against the live viewport width on every paint, so a
 * resize reflows for free. Measuring and drawing share webview_next_line: the
 * measure pass runs first (only when the width changed), then the draw pass
 * reuses it with a scroll offset.
 */

/* Cuts the next visual line of paragraph `para` starting at char `offset`.
 * Writes the line (no trailing spaces) into `out` and returns the offset the
 * next line starts at, or -1 when the paragraph is exhausted. */
static int webview_bold_extra(int para, int start, int end);

static int webview_next_line(struct sx_painter *painter, int para, int offset, int avail,
    char *out, int out_cap)
{
    const char *text = g_paras[para].text;
    int length = (int)strlen(text);
    int accepted = -1;
    int cursor;
    char trial[WEBVIEW_TRIAL_BUFFER];

    while (offset < length && text[offset] == ' ')
    {
        offset += 1;
    }
    if (offset >= length)
    {
        return -1;
    }
    cursor = offset;
    for (;;)
    {
        int word_end = cursor;
        size_t head;
        size_t word;
        size_t total;

        while (word_end < length && text[word_end] != ' ')
        {
            word_end += 1;
        }
        head = accepted < 0 ? 0u : (size_t)(accepted - offset);
        word = (size_t)(word_end - cursor);
        total = head + (head > 0 ? 1u : 0u) + word;
        if (total + 1 >= sizeof(trial))
        {
            break;
        }
        memcpy(trial, &text[offset], head);
        if (head > 0)
        {
            trial[head] = ' ';
        }
        memcpy(trial + head + (head > 0 ? 1u : 0u), &text[cursor], word);
        trial[total] = '\0';
        /* Bold runs strike twice, one pixel over: count their characters in.
         * Whole-line kerning still measures the plain trial at once. */
        if (sx_painter_text_width(painter, trial) +
                webview_bold_extra(para, offset, offset + (int)total) <=
                avail ||
            accepted < 0)
        {
            accepted = word_end;
        }
        else
        {
            break;
        }
        if (word_end >= length)
        {
            break;
        }
        cursor = word_end + 1;
        while (cursor < length && text[cursor] == ' ')
        {
            cursor += 1;
        }
    }
    if (accepted < 0)
    {
        return -1;
    }
    {
        int copy = accepted - offset;

        if (copy >= out_cap)
        {
            copy = out_cap - 1;
        }
        memcpy(out, &text[offset], (size_t)copy);
        out[copy] = '\0';
    }
    return accepted;
}

static int webview_line_height(struct sx_painter *painter, int heading)
{
    return sx_painter_text_height(painter) + (heading ? 6 : 2);
}

static int webview_scroll_max(void)
{
    int max = g_total_height - g_view.height;

    return max > 0 ? max : 0;
}

static void webview_set_scroll(int value)
{
    int max = webview_scroll_max();

    if (value < 0)
    {
        value = 0;
    }
    if (value > max)
    {
        value = max;
    }
    if (value != WEBVIEW_SCROLL->value)
    {
        WEBVIEW_SCROLL->value = value;
        sxgui_app_request_repaint(&g_app);
    }
}

static void webview_measure(struct sx_painter *painter, int avail)
{
    int index;
    int height = 0;
    char line[WEBVIEW_LINE_BUFFER];

    for (index = 0; index < g_para_count; ++index)
    {
        int offset = 0;
        int next;

        if (g_paras[index].rule)
        {
            height += 4 + 10;
            continue;
        }
        (void)sx_painter_set_font(painter, g_paras[index].face ? SX_FONT_UI_TITLE : SX_FONT_UI);
        if (index > 0)
        {
            height += g_paras[index].gap;
        }
        while ((next = webview_next_line(painter, index, offset, avail, line,
                       sizeof(line))) >= 0)
        {
            height += webview_line_height(painter, g_paras[index].heading);
            offset = next;
        }
        height += g_paras[index].heading ? 4 : 2;
    }
    (void)sx_painter_set_font(painter, SX_FONT_UI);
    g_total_height = height;
    g_measured_width = avail;
    WEBVIEW_SCROLL->range_max = webview_scroll_max();
    WEBVIEW_SCROLL->page = g_view.height > 0 ? g_view.height : 10;
    if (WEBVIEW_SCROLL->value > WEBVIEW_SCROLL->range_max)
    {
        WEBVIEW_SCROLL->value = WEBVIEW_SCROLL->range_max;
    }
}

/* Extra width of [start, end): one pixel per bold character for the
 * double-strike. Whole-paragraph bold counts everything. */
static int webview_bold_extra(int para, int start, int end)
{
    int extra = 0;
    int i;

    if (end <= start)
    {
        return 0;
    }
    if (g_paras[para].baseBold)
    {
        return end - start;
    }
    for (i = 0; i < g_run_count; ++i)
    {
        int run_start;
        int run_end;

        if (g_runs[i].para != para || !g_runs[i].bold)
        {
            continue;
        }
        run_start = g_runs[i].start > start ? g_runs[i].start : start;
        run_end = g_runs[i].start + g_runs[i].length < end ? g_runs[i].start + g_runs[i].length
                                                           : end;
        if (run_end > run_start)
        {
            extra += run_end - run_start;
        }
    }
    return extra;
}

/* One text run: double-strike when bold (the bitmap fake-bold). */
static void webview_draw_run_text(struct sx_painter *painter, int x, int y, const char *text,
    uint32_t color, int bold)
{
    sx_painter_draw_text(painter, x, y, text, color);
    if (bold)
    {
        sx_painter_draw_text(painter, x + 1, y, text, color);
    }
}

/* Width of the first `length` bytes of `text` in the painter's active font. */
static int webview_prefix_width(struct sx_painter *painter, const char *text, int length)
{
    char piece[WEBVIEW_LINE_BUFFER];
    int copy = length;

    if (copy < 0)
    {
        copy = 0;
    }
    if (copy >= (int)sizeof(piece))
    {
        copy = (int)sizeof(piece) - 1;
    }
    memcpy(piece, text, (size_t)copy);
    piece[copy] = '\0';
    return sx_painter_text_width(painter, piece);
}

/* Repaints the runs overlapping [start, end) of one laid-out line: run colour
 * (link blue wins), double-strike bold, underline; records link runs for
 * click hit-testing. `line` is the index just recorded in g_lines, `base` the
 * paragraph defaults the runs override. */
static void webview_paint_runs(struct sx_painter *painter, int line, int para, int start,
    int end, int x, int y, int height, uint32_t baseColor, int baseBold)
{
    const char *text = g_paras[para].text;
    int i;

    for (i = 0; i < g_run_count; ++i)
    {
        int run_start;
        int run_end;
        int x1;
        int x2;
        char piece[WEBVIEW_LINE_BUFFER];
        int piece_length;
        uint32_t color;
        int bold;
        int underline;

        if (g_runs[i].para != para)
        {
            continue;
        }
        run_start = g_runs[i].start > start ? g_runs[i].start : start;
        run_end = g_runs[i].start + g_runs[i].length < end ? g_runs[i].start + g_runs[i].length : end;
        if (run_end <= run_start)
        {
            continue;
        }
        color = g_runs[i].hasColor ? g_runs[i].color : baseColor;
        bold = g_runs[i].bold || baseBold;
        underline = g_runs[i].underline;
        if (g_runs[i].link >= 0)
        {
            color = g_link_color;
            underline = 1;
        }
        x1 = x + webview_prefix_width(painter, text + start, run_start - start);
        x2 = x + webview_prefix_width(painter, text + start, run_end - start);
        piece_length = run_end - run_start;
        if (piece_length >= (int)sizeof(piece))
        {
            piece_length = (int)sizeof(piece) - 1;
        }
        memcpy(piece, text + run_start, (size_t)piece_length);
        piece[piece_length] = '\0';
        webview_draw_run_text(painter, x1, y, piece, color, bold);
        if (underline && x2 > x1)
        {
            sx_painter_hline(painter, x1, y + height - 3, x2 - x1, color);
        }
        if (g_runs[i].link >= 0 && g_link_run_count < WEBVIEW_LINK_RUN_MAX)
        {
            struct webview_link_run *run = &g_link_runs[g_link_run_count++];

            run->line = line;
            run->link = g_runs[i].link;
            run->x1 = x1;
            run->x2 = x2;
        }
    }
}

static int webview_record_line(int doc_y, int height, int para, int start, int end, int x)
{
    struct webview_line_geo *slot;

    if (g_line_count >= WEBVIEW_LINE_MAX)
    {
        return -1;
    }
    slot = &g_lines[g_line_count++];
    slot->y = doc_y;
    slot->height = height;
    slot->para = para;
    slot->start = start;
    slot->end = end;
    slot->x = x;
    return g_line_count - 1;
}

/* Finds the link under window point (x, y), if any: y maps to a cached
 * document line, x to a run recorded at paint time. Copies its target. */
static int webview_link_at(int x, int y, char *target, size_t capacity)
{
    int doc_y = y - g_content_top + WEBVIEW_SCROLL->value;
    int line = -1;
    int i;

    for (i = 0; i < g_line_count; ++i)
    {
        if (doc_y >= g_lines[i].y && doc_y < g_lines[i].y + g_lines[i].height)
        {
            line = i;
            break;
        }
    }
    if (line < 0)
    {
        return 0;
    }
    for (i = 0; i < g_link_run_count; ++i)
    {
        if (g_link_runs[i].line == line && x >= g_link_runs[i].x1 && x < g_link_runs[i].x2)
        {
            snprintf(target, capacity, "%s", g_links[g_link_runs[i].link].target);
            return 1;
        }
    }
    return 0;
}

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    struct sx_rect interior;
    int text_left;
    int avail;
    int y;
    int index;
    char line[WEBVIEW_LINE_BUFFER];

    (void)app;
    sxgui_draw_sunken_edge(painter, g_view);
    interior = sx_rect_make(g_view.x + SXGUI_BORDER_SUNKEN, g_view.y + SXGUI_BORDER_SUNKEN,
        g_view.width - SXGUI_BORDER_SUNKEN * 2, g_view.height - SXGUI_BORDER_SUNKEN * 2);
    sx_painter_fill_rect(painter, interior, g_page_bg);
    /* The text area keeps the scrollbar strip clear on the right. */
    interior.width -= SXGUI_SCROLLBAR_THICKNESS + SXGUI_GAP;
    text_left = interior.x + SXGUI_TEXT_PAD + SXGUI_BORDER_SUNKEN;
    avail = interior.width - (text_left - interior.x) - SXGUI_TEXT_PAD;
    if (avail < 40)
    {
        avail = 40;
    }
    if (avail != g_measured_width)
    {
        int before = WEBVIEW_SCROLL->value;

        webview_measure(painter, avail);
        if (WEBVIEW_SCROLL->value != before)
        {
            sxgui_app_request_repaint(&g_app);
        }
    }
    if (!sx_painter_push_clip(painter, interior))
    {
        return;
    }
    g_text_area = sx_rect_make(text_left, interior.y, avail, interior.height);
    g_content_top = interior.y + SXGUI_TEXT_PAD;
    g_line_count = 0;
    g_link_run_count = 0;
    y = interior.y + SXGUI_TEXT_PAD - WEBVIEW_SCROLL->value;
    for (index = 0; index < g_para_count; ++index)
    {
        int offset = 0;
        int next;
        int height;
        int line_x;

        if (g_paras[index].rule)
        {
            y += 4;
            if (y >= interior.y - 8 && y <= interior.y + interior.height + 8)
            {
                sx_painter_hline(painter, text_left, y, avail, SXGUI_COLOR_SHADOW);
                sx_painter_hline(painter, text_left, y + 1, avail, SXGUI_COLOR_LIGHT);
            }
            y += 10;
            continue;
        }
        (void)sx_painter_set_font(painter, g_paras[index].face ? SX_FONT_UI_TITLE : SX_FONT_UI);
        height = webview_line_height(painter, g_paras[index].heading);
        line_x = text_left + (g_paras[index].indent ? 16 : 0);
        if (index > 0)
        {
            y += g_paras[index].gap;
        }
        while ((next = webview_next_line(painter, index, offset, avail, line,
                   sizeof(line))) >= 0)
        {
            int cached;

            /* Document coordinates: surface position relative to the content
             * origin, plus scroll. The click lookup applies the same
             * translation (see webview_link_at). */
            cached = webview_record_line(
                y - interior.y - SXGUI_TEXT_PAD + WEBVIEW_SCROLL->value, height, index,
                offset, next, line_x);
            if (y + height >= interior.y && y <= interior.y + interior.height)
            {
                int width = sx_painter_text_width(painter, line) +
                            webview_bold_extra(index, offset, next);
                int draw_x = line_x;
                uint32_t baseColor = g_paras[index].hasColor ? g_paras[index].color
                                                             : g_page_fg;

                if (g_paras[index].align == 1)
                {
                    draw_x = text_left + (avail - width) / 2;
                }
                else if (g_paras[index].align == 2)
                {
                    draw_x = text_left + avail - width;
                }
                if (draw_x < text_left)
                {
                    draw_x = text_left;
                }
                if (g_paras[index].hasBg)
                {
                    sx_painter_fill_rect(painter,
                        sx_rect_make(text_left, y, avail, height), g_paras[index].bg);
                }
                webview_draw_run_text(painter, draw_x, y, line, baseColor,
                    g_paras[index].baseBold);
                if (cached >= 0)
                {
                    webview_paint_runs(painter, cached, index, offset, next, draw_x, y,
                        height, baseColor, g_paras[index].baseBold);
                }
            }
            y += height;
            offset = next;
        }
        y += g_paras[index].heading ? 4 : 2;
    }
    (void)sx_painter_set_font(painter, SX_FONT_UI);
    sx_painter_pop_clip(painter);
}

/* ---- layout ----------------------------------------------------------------- */

static void webview_layout(struct sxgui_app *app)
{
    int width = (int)app->gfx.info.width;
    int height = (int)app->gfx.info.height;
    int bar_y = sxgui_menubar_height() + SXGUI_MARGIN;
    int field_x = SXGUI_MARGIN;
    int go_x = width - SXGUI_MARGIN - WEBVIEW_GO_WIDTH;
    int top = bar_y + SXGUI_FIELD_HEIGHT + SXGUI_GAP;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int content_width = width - SXGUI_MARGIN * 2;
    int view_height = status_y - SXGUI_GAP - top;

    WEBVIEW_BACK->rect = sx_rect_make(field_x, bar_y, WEBVIEW_NAV_WIDTH, SXGUI_FIELD_HEIGHT);
    field_x += WEBVIEW_NAV_WIDTH + SXGUI_GAP;
    WEBVIEW_FWD->rect = sx_rect_make(field_x, bar_y, WEBVIEW_NAV_WIDTH, SXGUI_FIELD_HEIGHT);
    field_x += WEBVIEW_NAV_WIDTH + SXGUI_GAP;
    if (go_x < field_x + 60)
    {
        go_x = field_x + 60;
    }
    WEBVIEW_GO->rect = sx_rect_make(go_x, bar_y, WEBVIEW_GO_WIDTH, SXGUI_FIELD_HEIGHT);
    WEBVIEW_ADDR->rect = sx_rect_make(field_x, bar_y, go_x - SXGUI_GAP - field_x,
        SXGUI_FIELD_HEIGHT);

    if (view_height < 60)
    {
        view_height = 60;
    }
    if (content_width < 120)
    {
        content_width = 120;
    }
    g_view = sx_rect_make(SXGUI_MARGIN, top, content_width, view_height);
    WEBVIEW_SCROLL->rect = sx_rect_make(g_view.x + g_view.width - SXGUI_SCROLLBAR_THICKNESS,
        g_view.y, SXGUI_SCROLLBAR_THICKNESS, g_view.height);
    WEBVIEW_STATUS->rect = sx_rect_make(SXGUI_MARGIN, status_y, content_width, SXGUI_STATUS_HEIGHT);
}

static void on_resize(struct sxgui_app *app)
{
    webview_layout(app);
    g_measured_width = -1;
}

/* ---- interaction -------------------------------------------------------------- */

static void on_scroll_changed(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_app_request_repaint(&g_app);
}

static void on_back(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    webview_history_go(-1);
}

static void on_forward(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    webview_history_go(1);
}

static void on_go(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    webview_navigate(g_address, 1);
    sxgui_focus(&g_app.ui, -1);
}

static void webview_open_dialog(void)
{
    snprintf(g_open_path, sizeof(g_open_path), "%s", g_path);
    g_open_widgets[1].caret = (int)strlen(g_open_path);
    sxgui_dialog_begin(&g_app.ui, &g_open_dialog, WEBVIEW_OPEN_WIDTH, WEBVIEW_OPEN_HEIGHT);
}

static void on_open_accept(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (g_open_path[0] == '\0')
    {
        return;
    }
    (void)webview_navigate(g_open_path, 1);
}

static void on_open_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_about_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case WEBVIEW_MENU_OPEN:
        webview_open_dialog();
        break;
    case WEBVIEW_MENU_RELOAD:
        if (g_has_file)
        {
            (void)webview_open_location(g_path, 0);
        }
        break;
    case WEBVIEW_MENU_BACK:
        webview_history_go(-1);
        break;
    case WEBVIEW_MENU_FORWARD:
        webview_history_go(1);
        break;
    case WEBVIEW_MENU_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    case WEBVIEW_MENU_TOP:
        webview_set_scroll(0);
        break;
    case WEBVIEW_MENU_BOTTOM:
        webview_set_scroll(webview_scroll_max());
        break;
    case WEBVIEW_MENU_SOURCE:
        webview_toggle_source();
        break;
    case WEBVIEW_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, WEBVIEW_ABOUT_WIDTH,
            WEBVIEW_ABOUT_HEIGHT);
        break;
    default:
        break;
    }
}

/* The viewport is custom-painted, not a widget, so the wheel would hit nothing.
 * This pre-toolkit hook turns wheel ticks over it into scrollbar motion, the
 * same 3-rows-per-tick convention the toolkit uses elsewhere. A left-button
 * press and release inside the text area follows the link under the cursor,
 * if any. While a modal dialog is open the hook stays out of the way. */
static int g_prev_left = 0;
static int g_press_in_text = 0;

static int on_pointer(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event)
{
    int left;

    (void)app;
    if (sxgui_dialog_active(&g_app.ui))
    {
        g_prev_left = 0;
        g_press_in_text = 0;
        return 0;
    }
    if (event->wheel != 0)
    {
        if (!sx_rect_contains_point(g_view, event->x, event->y))
        {
            return 0;
        }
        webview_set_scroll(WEBVIEW_SCROLL->value - event->wheel * 54);
        return 1;
    }
    left = (event->buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0;
    if (left && !g_prev_left)
    {
        g_press_in_text = sx_rect_contains_point(g_text_area, event->x, event->y);
    }
    if (!left && g_prev_left)
    {
        int pressed = g_press_in_text;

        g_press_in_text = 0;
        g_prev_left = 0;
        if (pressed && sx_rect_contains_point(g_text_area, event->x, event->y))
        {
            char target[WEBVIEW_PATH_CAPACITY];

            if (webview_link_at(event->x, event->y, target, sizeof(target)))
            {
                webview_open_location(target, 1);
                return 1;
            }
        }
        return 0;
    }
    g_prev_left = left;
    return 0;
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    if (sxgui_dialog_active(&app->ui))
    {
        return 0;
    }
    if ((event->modifiers & SAVANXP_KEY_MOD_ALT) != 0)
    {
        if (event->key == SAVANXP_KEY_LEFT)
        {
            webview_history_go(-1);
            return 1;
        }
        if (event->key == SAVANXP_KEY_RIGHT)
        {
            webview_history_go(1);
            return 1;
        }
        return 0;
    }
    if ((event->modifiers & SAVANXP_KEY_MOD_CTRL) != 0 &&
        (event->modifiers & SAVANXP_KEY_MOD_ALT_GR) == 0 &&
        (event->ascii == 'u' || event->ascii == 'U'))
    {
        webview_toggle_source();
        return 1;
    }
    if (app->ui.focus_index == WEBVIEW_ADDR_INDEX)
    {
        /* The caret owns every key but Enter and the global shortcuts while
         * the address field has the focus: scrolling the page from there
         * would fight text editing. */
        if (event->key == SAVANXP_KEY_ENTER)
        {
            webview_navigate(g_address, 1);
            sxgui_focus(&app->ui, -1);
            return 1;
        }
        if (event->key == SAVANXP_KEY_F3 || event->key == SAVANXP_KEY_F5)
        {
            /* Fall through to the global shortcuts below. */
        }
        else
        {
            return 0;
        }
    }
    switch (event->key)
    {
    case SAVANXP_KEY_F3:
        webview_open_dialog();
        return 1;
    case SAVANXP_KEY_F5:
        if (g_has_file)
        {
            (void)webview_open_location(g_path, 0);
        }
        return 1;
    case SAVANXP_KEY_UP:
        webview_set_scroll(WEBVIEW_SCROLL->value - 18);
        return 1;
    case SAVANXP_KEY_DOWN:
        webview_set_scroll(WEBVIEW_SCROLL->value + 18);
        return 1;
    case SAVANXP_KEY_PAGE_UP:
        webview_set_scroll(WEBVIEW_SCROLL->value - g_view.height);
        return 1;
    case SAVANXP_KEY_PAGE_DOWN:
        webview_set_scroll(WEBVIEW_SCROLL->value + g_view.height);
        return 1;
    case SAVANXP_KEY_HOME:
        webview_set_scroll(0);
        return 1;
    case SAVANXP_KEY_END:
        webview_set_scroll(webview_scroll_max());
        return 1;
    default:
        return 0;
    }
}

/* ---- menus -------------------------------------------------------------------- */

static const struct sxgui_menu_item k_file_items[] = {
    {"Open...", WEBVIEW_MENU_OPEN, 0},
    {"Reload", WEBVIEW_MENU_RELOAD, 0},
    {0, 0, 0},
    {"Exit", WEBVIEW_MENU_EXIT, 0},
};

static const struct sxgui_menu_item k_view_items[] = {
    {"Top of page", WEBVIEW_MENU_TOP, 0},
    {"End of page", WEBVIEW_MENU_BOTTOM, 0},
    {0, 0, 0},
    {"View Source", WEBVIEW_MENU_SOURCE, 0},
};

static const struct sxgui_menu_item k_go_items[] = {
    {"Back", WEBVIEW_MENU_BACK, 0},
    {"Forward", WEBVIEW_MENU_FORWARD, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Web Viewer", WEBVIEW_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Go", k_go_items, (int)(sizeof(k_go_items) / sizeof(k_go_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menubar g_menubar = {
    k_menus,
    (int)(sizeof(k_menus) / sizeof(k_menus[0])),
    -1,
    -1,
    on_menu_command,
    0
};

int main(int argc, char **argv)
{
    *WEBVIEW_ADDR = sxgui_textfield(sx_rect_make(0, 0, 0, 0), g_address, sizeof(g_address));
    *WEBVIEW_GO = sxgui_button(sx_rect_make(0, 0, 0, 0), "Go", on_go, 0);
    *WEBVIEW_BACK = sxgui_button(sx_rect_make(0, 0, 0, 0), "<", on_back, 0);
    *WEBVIEW_FWD = sxgui_button(sx_rect_make(0, 0, 0, 0), ">", on_forward, 0);
    *WEBVIEW_SCROLL = sxgui_scrollbar(sx_rect_make(0, 0, 0, 0), 0, 0, 10, 0);
    WEBVIEW_SCROLL->on_action = on_scroll_changed;
    *WEBVIEW_STATUS = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status);
    WEBVIEW_STATUS->flags |= SXGUI_FLAG_SUNKEN;

    g_open_widgets[0] = sxgui_label(
        sx_rect_make(WEBVIEW_DLG_MARGIN, WEBVIEW_DLG_MARGIN,
            WEBVIEW_OPEN_WIDTH - WEBVIEW_DLG_MARGIN * 2, 18),
        "File name:");
    g_open_widgets[1] = sxgui_textfield(
        sx_rect_make(WEBVIEW_DLG_MARGIN, WEBVIEW_DLG_MARGIN + WEBVIEW_DLG_ROW,
            WEBVIEW_OPEN_WIDTH - WEBVIEW_DLG_MARGIN * 2, SXGUI_FIELD_HEIGHT),
        g_open_path, sizeof(g_open_path));
    g_open_widgets[2] = sxgui_button(
        sx_rect_make(WEBVIEW_DLG_RIGHT(WEBVIEW_OPEN_WIDTH, 1),
            WEBVIEW_DLG_BUTTON_ROW(WEBVIEW_OPEN_HEIGHT), SXGUI_BUTTON_WIDTH,
            SXGUI_BUTTON_HEIGHT),
        "Open", on_open_accept, 0);
    g_open_widgets[3] = sxgui_button(
        sx_rect_make(WEBVIEW_DLG_RIGHT(WEBVIEW_OPEN_WIDTH, 0),
            WEBVIEW_DLG_BUTTON_ROW(WEBVIEW_OPEN_HEIGHT), SXGUI_BUTTON_WIDTH,
            SXGUI_BUTTON_HEIGHT),
        "Cancel", on_open_cancel, 0);
    g_open_dialog.title = "Open";
    g_open_dialog.widgets = g_open_widgets;
    g_open_dialog.widget_count = 4;
    g_open_dialog.initial_focus = 1;
    g_open_dialog.default_button = 2;

    g_about_widgets[0] = sxgui_label(
        sx_rect_make(WEBVIEW_DLG_MARGIN, WEBVIEW_DLG_MARGIN,
            WEBVIEW_ABOUT_WIDTH - WEBVIEW_DLG_MARGIN * 2, 18),
        "SavanXP Web Viewer");
    g_about_widgets[1] = sxgui_label(
        sx_rect_make(WEBVIEW_DLG_MARGIN, WEBVIEW_DLG_MARGIN + WEBVIEW_DLG_ROW,
            WEBVIEW_ABOUT_WIDTH - WEBVIEW_DLG_MARGIN * 2, 18),
        "Local files, HTTP by name or IP, first CSS.");
    g_about_widgets[2] = sxgui_label(
        sx_rect_make(WEBVIEW_DLG_MARGIN, WEBVIEW_DLG_MARGIN + WEBVIEW_DLG_ROW * 2,
            WEBVIEW_ABOUT_WIDTH - WEBVIEW_DLG_MARGIN * 2, 18),
        "Images arrive later.");
    g_about_widgets[3] = sxgui_label(
        sx_rect_make(WEBVIEW_DLG_MARGIN, WEBVIEW_DLG_MARGIN + WEBVIEW_DLG_ROW * 3,
            WEBVIEW_ABOUT_WIDTH - WEBVIEW_DLG_MARGIN * 2, 18),
        "Version: " SAVANXP_VERSION_STRING);
    g_about_widgets[4] = sxgui_button(
        sx_rect_make(WEBVIEW_DLG_CENTRED(WEBVIEW_ABOUT_WIDTH, 1, 0),
            WEBVIEW_DLG_BUTTON_ROW(WEBVIEW_ABOUT_HEIGHT), SXGUI_BUTTON_WIDTH,
            SXGUI_BUTTON_HEIGHT),
        "OK", on_about_ok, 0);
    g_about_dialog.title = "About Web Viewer";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 5;
    g_about_dialog.default_button = 4;

    if (sxgui_app_init(&g_app, "webview", g_widgets, WEBVIEW_WIDGET_COUNT) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_paint = on_paint;
    g_app.on_resize = on_resize;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    (void)sxgui_app_set_content_size(&g_app, WEBVIEW_CONTENT_WIDTH, WEBVIEW_CONTENT_HEIGHT);
    webview_layout(&g_app);

    webview_show_welcome();
    /* argv carries an optional resolver override plus the location to open:
     * `webview --dns 10.0.2.2:5353 http://name/page`. Files launches us with
     * a bare path, which still lands below as the initial location. */
    {
        const char *initial = 0;
        int bad_dns = 0;
        int i;

        (void)webview_parse_ipv4(WEBVIEW_DNS_SERVER_DEFAULT, &g_dns_ipv4);
        g_dns_port = WEBVIEW_DNS_PORT_DEFAULT;
        if (argv != 0)
        {
            for (i = 1; i < argc; ++i)
            {
                if (argv[i] != 0 && strcmp(argv[i], "--dns") == 0 && i + 1 < argc &&
                    argv[i + 1] != 0)
                {
                    if (!webview_parse_dns_server(argv[i + 1]))
                    {
                        bad_dns = 1;
                    }
                    i += 1;
                    continue;
                }
                if (argv[i] != 0 && argv[i][0] != '\0' && initial == 0)
                {
                    initial = argv[i];
                }
            }
        }
        if (bad_dns)
        {
            snprintf(g_status, sizeof(g_status), "Ignoring a bad --dns value.");
        }
        if (initial != 0)
        {
            (void)webview_navigate(initial, 1);
        }
    }
    webview_refresh_nav();
    sxgui_focus(&g_app.ui, WEBVIEW_ADDR_INDEX);
    return sxgui_app_run(&g_app);
}
