/*
 * Web Viewer: the native browser (docs/desktop/WEBVIEWER.md), for local
 * files, http:// and validated https://, by name or address.
 *
 * Pages load on a network worker of the window's; HTML and plain text are
 * shown using computed CSS styles and a paragraph renderer. Anything else
 * is a file, saved by download.c on a worker of its own.
 *
 * Images use a placeholder and table cells stack as separate lines.
 */

#include "sxgfx/gfx2d.h"
#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxndns.h"
#include "sxn/sxnhttp.h"
#include "sxn/sxngui.h"
#include "sxn/sxnprofile.h"
#include "download.h"
#include "favorites.h"
#include "engine/webengine.h"
#include "engine/webengine_box.h"
#include "webview.sunres.h"
#include "icons.inc"
#include "../gfx/toolbar_glyphs/icons.inc"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/version.h"

#define WEBVIEW_PATH_CAPACITY SXNHTTP_URL_CAP
#define WEBVIEW_TITLE_CAPACITY 128
#define WEBVIEW_STATUS_CAPACITY 192
#define WEBVIEW_LINE_BUFFER 512

#define WEBVIEW_MENU_OPEN 1
#define WEBVIEW_MENU_RELOAD 2
#define WEBVIEW_MENU_HOME 33
#define WEBVIEW_MENU_EXIT 3
#define WEBVIEW_MENU_TOP 10
#define WEBVIEW_MENU_BOTTOM 11
#define WEBVIEW_MENU_SOURCE 12
#define WEBVIEW_MENU_ABOUT 20
#define WEBVIEW_MENU_BACK 30
#define WEBVIEW_MENU_FORWARD 31
#define WEBVIEW_MENU_STOP 32
#define WEBVIEW_MENU_FAVORITE_ADD 40
#define WEBVIEW_MENU_FAVORITE_REMOVE 41
/* The favorites themselves, one id each from here. */
#define WEBVIEW_MENU_FAVORITE_FIRST 100

#define WEBVIEW_CONTENT_WIDTH 600
#define WEBVIEW_CONTENT_HEIGHT 470

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
#define WEBVIEW_STOP (&g_widgets[6])
#define WEBVIEW_REFRESH (&g_widgets[7])
#define WEBVIEW_HOME (&g_widgets[8])
#define WEBVIEW_OPEN (&g_widgets[9])
#define WEBVIEW_SOURCE (&g_widgets[10])
#define WEBVIEW_SEP_NAV (&g_widgets[11])
#define WEBVIEW_SEP_PAGE (&g_widgets[12])
#define WEBVIEW_RULE_MENU (&g_widgets[13])
#define WEBVIEW_RULE_TOOLBAR (&g_widgets[14])
#define WEBVIEW_ADDR_LABEL (&g_widgets[15])
#define WEBVIEW_PROGRESS (&g_widgets[16])
/* The status bar's security pane: the lock of a page that came over
 * validated HTTPS, drawn by on_paint. */
#define WEBVIEW_ZONE (&g_widgets[17])
#define WEBVIEW_WIDGET_COUNT 18

/* The toolbar, left to right, as Internet Explorer 5 laid it out: history,
 * then the page, then the document. Its glyphs are the shared Back and
 * Forward and Web Viewer's own (system/webview/gen_icons.py). */
#define WEBVIEW_TOOLBAR_BUTTONS 7
#define WEBVIEW_GO_WIDTH 64
#define WEBVIEW_RULE_HEIGHT 4
#define WEBVIEW_SEPARATOR_WIDTH 8
#define WEBVIEW_PROGRESS_WIDTH 120
#define WEBVIEW_ZONE_WIDTH 24
#define WEBVIEW_LOCK_SIZE 16
/* Home, the toolbar button and Go > Home Page. */
#define WEBVIEW_HOME_PAGE "file:///system/samples/welcome.html"

#define WEBVIEW_HISTORY_MAX 32

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

static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[WEBVIEW_WIDGET_COUNT];

struct webview_bytes {
    char *data;
    size_t length;
    size_t capacity;
};

/* A linked stylesheet's element is borrowed from the UI-owned document.
 * The network worker only reads its URL and fills the byte buffer. */
struct webview_sheet {
    dom_element *element;
    char url[WEBVIEW_PATH_CAPACITY];
    struct webview_bytes body;
    struct webview_sheet *next;
};

static struct webview_bytes g_raw;
static dom_document *g_document;
static struct webview_sheet *g_sheets;
static css_stylesheet *g_ua;
static char g_address[WEBVIEW_PATH_CAPACITY];

/* Back/Forward history: plain paths, oldest first. Loading a new page after
 * going back drops the forward branch, like every browser since Mosaic. */
static char g_history[WEBVIEW_HISTORY_MAX][WEBVIEW_PATH_CAPACITY];
static int g_hist_count = 0;
static int g_hist_index = -1;

/* The page on screen as the engine's boxes (docs/desktop/WEBVIEWER_ENGINE.md),
 * built when the page or the view changes and laid out again when the width
 * does. */
static struct webengine_tree *g_tree;
/* View Source (Ctrl+U) shows the page's bytes instead of the page. */
static int g_show_source = 0;
/* The page's area on screen and where its canvas origin is, scrolled, from
 * the last paint: what hit testing reads. */
static struct sx_rect g_page_area;
static int g_page_x;
static int g_page_y;

static char g_path[WEBVIEW_PATH_CAPACITY];
static char g_title[WEBVIEW_TITLE_CAPACITY];
static char g_status[WEBVIEW_STATUS_CAPACITY];
static int g_document_failed;
static int g_has_file = 0;
/* One-shot note from the last fetch (" (connection dropped)"): cleared by
 * every load, appended to the status line while it says something. */
static char g_fetch_note[32];
/* The page on screen came over validated HTTPS; `g_security` names how. */
static int g_page_secure;
static char g_security[64];
/* The lock glyph, wrapped by webview_init_glyphs. */
static struct sx_bitmap g_lock;
/* The page is text/plain: shown line by line, like the source view. */
static int g_plain = 0;

static struct sx_rect g_view;
static int g_total_height = 0;
static int g_layout_width = -1;
static int g_layout_height = -1;

static struct sxgui_dialog g_open_dialog;
static struct sxgui_widget g_open_widgets[4];
static char g_open_path[WEBVIEW_PATH_CAPACITY];

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[5];


/* Vectors grow geometrically and keep their previous storage on failure. */
static void *webview_reserve(void *data, size_t *capacity, size_t count, size_t item)
{
    size_t limit = SIZE_MAX / item;
    size_t grown = *capacity ? *capacity : 16;
    void *replacement;

    if (count <= *capacity)
    {
        return data;
    }
    if (count > limit)
    {
        return NULL;
    }
    while (grown < count)
    {
        grown = grown <= limit / 2 ? grown * 2 : limit;
    }
    replacement = realloc(data, grown * item);
    if (replacement != NULL)
    {
        *capacity = grown;
    }
    return replacement;
}

/* Documents and source views use byte offsets. Reserve one extra byte for
 * the string APIs used by the painter. */
static int webview_append(struct webview_bytes *buffer, const void *bytes, size_t length)
{
    char *data;

    if (length > INT_MAX - 1 - buffer->length)
    {
        return -1;
    }
    data = webview_reserve(buffer->data, &buffer->capacity, buffer->length + length + 1, 1);
    if (data == NULL)
    {
        return -1;
    }
    buffer->data = data;
    memcpy(data + buffer->length, bytes, length);
    buffer->length += length;
    data[buffer->length] = '\0';
    return 0;
}

static void webview_reset_document(void)
{
    webengine_tree_free(g_tree);
    g_tree = NULL;
    g_document_failed = 0;
}

static int webview_html_space(unsigned char value)
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
}

static void webview_set_title(const char *text, size_t length)
{
    size_t used = 0;
    size_t i;

    for (i = 0; i < length && used + 1 < sizeof(g_title); ++i)
    {
        unsigned char value = (unsigned char)text[i];

        if (webview_html_space(value))
        {
            if (used == 0 || g_title[used - 1] == ' ')
            {
                continue;
            }
            value = ' ';
        }
        if (value >= 32)
        {
            g_title[used++] = (char)value;
        }
    }
    /* Do not cut a UTF-8 character at the window-title limit. */
    if (i < length && ((unsigned char)text[i] & 0xc0) == 0x80)
    {
        do
        {
            --used;
        } while (used > 0 && ((unsigned char)g_title[used] & 0xc0) == 0x80);
    }
    if (used > 0 && g_title[used - 1] == ' ')
    {
        --used;
    }
    g_title[used] = '\0';
}

static uint32_t webview_color_over(css_color color, uint32_t background)
{
    unsigned alpha = color >> 24;
    if (alpha == 255)
    {
        return color & 0xffffff;
    }
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
    {
        unsigned from = (color >> shift) & 255;
        unsigned to = (background >> shift) & 255;
        result |= ((from * alpha + to * (255 - alpha) + 127) / 255) << shift;
    }
    return result;
}

/* ---- the engine's fonts ----------------------------------------------------
 *
 * The page is laid out with the painter's faces: the UI face for text under
 * 18 CSS pixels, the title face from there, the console's for monospace.
 * Bold is the bitmap fake bold, a second strike a pixel to the right, so it
 * adds a pixel a character. The painter gives no ascent, so it comes from
 * the baked faces' proportions (system/gfx/gfx_font_*.inc). */

static int webview_font_face(void *user, int size, bool bold, bool monospace)
{
    (void)user;
    (void)bold;
    return monospace ? SX_FONT_MONO : size >= 18 ? SX_FONT_UI_TITLE : SX_FONT_UI;
}

static int webview_font_width(void *user, int face, bool bold, const char *text, size_t length)
{
    struct sx_painter *painter = &g_app.ui.painter;
    char small[256];
    char *copy = length < sizeof(small) ? small : malloc(length + 1);
    int characters = 0;
    int width;
    int previous;

    (void)user;
    for (size_t i = 0; i < length; ++i)
    {
        characters += ((unsigned char)text[i] & 0xc0) != 0x80;
    }
    if (copy == NULL)
    {
        return characters * 8;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    previous = sx_painter_set_font(painter, face);
    width = sx_painter_text_width(painter, copy);
    (void)sx_painter_set_font(painter, previous);
    if (copy != small)
    {
        free(copy);
    }
    return width + (bold ? characters : 0);
}

static void webview_font_metrics(void *user, int face, int *ascent, int *descent)
{
    struct sx_painter *painter = &g_app.ui.painter;
    int previous = sx_painter_set_font(painter, face);
    int height = sx_painter_text_height(painter);

    (void)user;
    (void)sx_painter_set_font(painter, previous);
    *ascent = face == SX_FONT_UI_TITLE ? height * 12 / 17 : face == SX_FONT_MONO ? height * 13 / 16
                                                                             : height * 11 / 13;
    *descent = height - *ascent;
}

static const struct webengine_fonts k_webview_fonts = {
    0, webview_font_face, webview_font_width, webview_font_metrics,
};

static struct webengine_sheet_source webview_link_sheet(void *user, dom_element *element)
{
    struct webview_sheet **next = user;

    if (*next == NULL || (*next)->element != element)
    {
        return (struct webengine_sheet_source){0};
    }
    const struct webview_sheet *sheet = *next;
    *next = sheet->next;
    return (struct webengine_sheet_source){
        .data = (const uint8_t *)sheet->body.data, .length = sheet->body.length, .url = sheet->url,
    };
}

/* The document's <title>, for the window and the status bar. */
static void webview_find_title(dom_document *document)
{
    dom_string *name = NULL;
    dom_nodelist *list = NULL;
    dom_node *title = NULL;
    dom_string *text = NULL;
    uint32_t count = 0;

    if (dom_string_create((const uint8_t *)"title", 5, &name) != DOM_NO_ERR)
    {
        return;
    }
    if (dom_document_get_elements_by_tag_name(document, name, &list) == DOM_NO_ERR && list != NULL &&
        dom_nodelist_get_length(list, &count) == DOM_NO_ERR && count > 0 &&
        dom_nodelist_item(list, 0, &title) == DOM_NO_ERR && title != NULL &&
        dom_node_get_text_content(title, &text) == DOM_NO_ERR && text != NULL)
    {
        webview_set_title(dom_string_data(text), dom_string_byte_length(text));
    }
    dom_string_unref(text);
    dom_node_unref(title);
    if (list != NULL)
    {
        dom_nodelist_unref(list);
    }
    dom_string_unref(name);
}

/* The boxes of `document`, styled with the user-agent sheet, its own sheets
 * and the fetched ones; NULL when that fails. */
static struct webengine_tree *webview_build_tree(dom_document *document, const char *url,
    struct webview_sheet *sheets)
{
    struct webengine_styles styles;
    struct webview_sheet *next_sheet = sheets;
    struct webengine_tree *tree;

    if (document == NULL || g_ua == NULL || webengine_styles_open(
            &styles, document, url, g_ua, webview_link_sheet, &next_sheet) != 0)
    {
        return NULL;
    }
    tree = webengine_tree_build(document, &styles, &k_webview_fonts);
    webengine_styles_close(&styles);
    return tree;
}

static void webview_parse(void)
{
    if (g_document != NULL)
    {
        webview_find_title(g_document);
    }
    g_tree = webview_build_tree(g_document, g_path, g_sheets);
    if (g_tree == NULL)
    {
        g_document_failed = 1;
    }
}

/* ---- document loading ------------------------------------------------------ */

static int g_hovering;

/* A new message takes the status bar back from the link under the pointer;
 * the link shows again when the pointer moves. */
static void webview_status_takes_over(void)
{
    if (g_hovering)
    {
        g_hovering = 0;
        WEBVIEW_STATUS->text = g_status;
    }
}

static void webview_refresh_status(void)
{
    char suffix[96];

    snprintf(suffix, sizeof(suffix), "%s%s%s", g_fetch_note,
        g_show_source ? " (source)" : "", g_page_secure ? g_security : "");
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
    g_layout_width = -1;
    WEBVIEW_SCROLL->value = 0;
    sxngui_app_request_repaint(&g_app);
}

static int webview_open_location(const char *location, int push, int hist_target);
static void webview_navigate(const char *address, int push);

/* The welcome page, shown with no file open: HTML of its own, laid out as
 * any page is. */
static const char k_webview_welcome[] =
    "<h1>Web Viewer</h1>"
    "<p>A minimal browser for local files and HTTP by name or IP, in the spirit of IE 1.1. "
    "It lays pages out with CSS boxes, follows links and redirects, and shows source with "
    "Ctrl+U.</p><hr>"
    "<ul><li>Type a path or an http:// address up there and press Go (or Enter). "
    "File &gt; Open (F3) works too: try /system/samples/welcome.html.</li>"
    "<li>Double-clicking an .html file in Files opens it here.</li></ul>"
    "<p>Pages load in the background: Stop (Esc) drops a slow one. "
    "Not in this version: images.</p>";

static void webview_show_welcome(void)
{
    dom_document *document;

    g_has_file = 0;
    g_path[0] = '\0';
    webview_reset_document();
    free(g_raw.data);
    g_raw = (struct webview_bytes){0};
    g_title[0] = '\0';
    document = webengine_parse_html((const uint8_t *)k_webview_welcome, sizeof(k_webview_welcome) - 1,
        NULL, NULL, 0);
    g_tree = webview_build_tree(document, "", NULL);
    dom_node_unref(document);
    webview_refresh_status();
    webview_invalidate_layout();
}

/* Read a local document or stylesheet into a private buffer. */
static const char *webview_read_file(const char *path, struct webview_bytes *output)
{
    struct sxn_file_info info = {0};
    struct webview_bytes body = {0};
    unsigned char chunk[8192];

    if (sxn_query_path(path, &info) == 0 && info.type == SXN_FILE_TYPE_DIRECTORY)
    {
        return "That path is a directory.";
    }
    long stream = sxn_open_file(path, SXN_FILE_READ);
    if (stream < 0)
    {
        return "Cannot open that file.";
    }
    for (;;)
    {
        long got = sxn_read(stream, chunk, sizeof(chunk));

        if (got == 0)
        {
            break;
        }
        if (got < 0 || webview_append(&body, chunk, (size_t)got) != 0)
        {
            sxn_close(stream);
            free(body.data);
            return got < 0 ? "Cannot read that file." : "Not enough memory for this document.";
        }
    }
    sxn_close(stream);
    *output = body;
    return NULL;
}

static void webview_free_sheets(struct webview_sheet *sheets)
{
    while (sheets != NULL)
    {
        struct webview_sheet *next = sheets->next;
        free(sheets->body.data);
        free(sheets);
        sheets = next;
    }
}

/* The window title names the page, as "<title> - Web Viewer". */
static void webview_update_title(void)
{
    char title[WEBVIEW_TITLE_CAPACITY + 16];

    if (g_has_file && g_title[0] != '\0')
    {
        snprintf(title, sizeof(title), "%s - Web Viewer", g_title);
    }
    else
    {
        snprintf(title, sizeof(title), "Web Viewer");
    }
    (void)sxnwm_set_title(&g_app.wm, title);
}

/* Renders g_raw in the current mode. Both loaders funnel here so source and
 * page views never diverge. */
static void webview_render_current(void)
{
    webview_reset_document();
    g_title[0] = '\0';
    if (g_show_source || g_plain)
    {
        /* View Source and text/plain: the bytes as they came, lines kept. */
        g_tree = webengine_tree_text(g_raw.data != NULL ? g_raw.data : "", g_raw.length, 16,
            &k_webview_fonts);
        g_document_failed = g_tree == NULL;
    }
    else
    {
        webview_parse();
    }
    if (g_document_failed)
    {
        webview_reset_document();
        snprintf(g_status, sizeof(g_status), "Cannot render this document.");
    }
    else
    {
        webview_refresh_status();
    }
    webview_update_title();
    webview_invalidate_layout();
}

/* ---- name resolution, sockets and the network worker ---------------------------
 *
 * Remote loading runs on one network worker thread per window (N3 in
 * docs/roadmaps/open/WEBVIEWER_ROADMAP.md). The UI thread owns every widget,
 * the document and the history; the worker owns the window's resolver
 * (include/sxn/sxndns.h), its sockets and a body buffer of its own, so a load
 * that fails or is stopped never touches the page on screen.
 *
 * They meet in single-slot mailboxes under one lock: the request, the latest
 * progress line and the result. Each navigation has an ID, and whatever
 * carries an ID the UI is no longer waiting for is dropped. A new navigation,
 * Stop and closing the window set the cancel event, which every wait of the
 * worker checks first; the worker hands results back through sxngui_app_wake,
 * and on_wake takes them on the UI thread.
 */

#define WEBVIEW_WORKER_STACK (64 * 1024)

enum webview_load_outcome
{
    WEBVIEW_LOAD_PAGE,      /* the HTML or plain text is in the result */
    WEBVIEW_LOAD_STYLES,    /* the worker has finished the stylesheets */
    WEBVIEW_LOAD_FAILED,    /* `message` says why */
    WEBVIEW_LOAD_CANCELLED, /* the cancel event was set */
    WEBVIEW_LOAD_DOWNLOAD,  /* a file, not a page: `location`, saved as `file_name` */
};

struct webview_request
{
    uint32_t id;
    char url[WEBVIEW_PATH_CAPACITY];
    struct sxn_ipv4_endpoint dns;
    struct webview_sheet *sheets;
    uint64_t deadline;
};

struct webview_result
{
    uint32_t id;
    enum webview_load_outcome outcome;
    char location[WEBVIEW_PATH_CAPACITY]; /* the final URL, after redirects */
    char message[WEBVIEW_STATUS_CAPACITY];
    char note[32];
    int plain; /* text/plain: shown as text, not parsed as HTML */
    int secure; /* https://, validated */
    char security[64];
    struct webview_bytes body;
    struct webview_sheet *sheets;
    char file_name[SXNHTTP_FILENAME_CAP];
};

static void webview_clear_result(struct webview_result *result)
{
    free(result->body.data);
    webview_free_sheets(result->sheets);
    *result = (struct webview_result){0};
}

/* The lock: 0 free, 1 held, 2 held with waiters, a futex mutex over
 * sxn_wait_address. Held only to copy a slot, never across a wait. */
static uint32_t g_mailbox_lock;
/* Under the lock. */
static struct webview_request g_request;
static int g_request_ready;
static int g_worker_quit;
static struct webview_result g_result;
static int g_result_ready;
static uint32_t g_progress_id;
static char g_progress[WEBVIEW_STATUS_CAPACITY];
static int g_progress_percent;

static long g_work_event = -1;   /* auto-reset: a request, or quitting, is waiting */
static long g_cancel_event = -1; /* manual-reset: drop what is being loaded */
static long g_worker = -1;
static unsigned char g_worker_stack[WEBVIEW_WORKER_STACK] __attribute__((aligned(16)));

/* The worker's own. */
static struct sxndns_resolver g_resolver;

/* The UI's own: the DNS server the next request carries (--dns, dns://). The
 * worker hands it to its resolver, which empties its cache when it changes. */
static struct sxn_ipv4_endpoint g_dns_server;

static void webview_lock(void)
{
    uint32_t state = 0;

    if (__atomic_compare_exchange_n(&g_mailbox_lock, &state, 1, 0, __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED))
    {
        return;
    }
    if (state != 2)
    {
        state = __atomic_exchange_n(&g_mailbox_lock, 2, __ATOMIC_ACQUIRE);
    }
    while (state != 0)
    {
        (void)sxn_wait_address(&g_mailbox_lock, 2, sizeof(g_mailbox_lock), -1);
        state = __atomic_exchange_n(&g_mailbox_lock, 2, __ATOMIC_ACQUIRE);
    }
}

static void webview_unlock(void)
{
    if (__atomic_exchange_n(&g_mailbox_lock, 0, __ATOMIC_RELEASE) == 2)
    {
        (void)sxn_wake_address(&g_mailbox_lock, 1);
    }
}

/* The worker tells the UI what it is doing, for the status line, and how far
 * along it thinks it is, for the progress pane. */
static void webview_progress(uint32_t id, const char *text, int percent)
{
    webview_lock();
    g_progress_id = id;
    snprintf(g_progress, sizeof(g_progress), "%s", text);
    g_progress_percent = percent;
    webview_unlock();
    (void)sxngui_app_wake(&g_app);
}

/* ---- fetching ----------------------------------------------------------------
 *
 * On the worker, through SUN's HTTP loader (include/sxn/sxnhttp.h,
 * docs/net/HTTP.md): framing, redirects, budgets and cancellation are its.
 * What is left here is turning its answer into what the window shows.
 */

static void webview_refresh_nav(void);
static void webview_sync_address(void);
static void webview_history_push(const char *path);

/* HTTP and HTTPS requests go to the network worker. */
static int webview_location_is_http(const char *location)
{
    return location != 0 &&
           (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0);
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
        /* RFC 3986 resolution, ports and dot segments included, is the HTTP
         * loader's. */
        int resolved = sxnhttp_resolve(base, clean, out, capacity);

        return resolved == -2 ? -2 : resolved != 0 ? -1 : 0;
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

static struct sxnhttp_client g_http;
/* The worker's own: the host the loader last named, for a DNS failure's
 * message, and the last KiB the progress pane was told about. */
static char g_load_host[SXNHTTP_HOST_CAP];
static size_t g_load_kib;

struct webview_fetch {
    const struct webview_request *request;
    struct webview_bytes *body;
};

static void webview_http_progress(void *user, enum sxnhttp_phase phase, const char *host,
    uint64_t received, uint64_t expected)
{
    const struct webview_fetch *fetch = user;
    const struct webview_request *request = fetch->request;
    char text[WEBVIEW_STATUS_CAPACITY];
    int percent;

    snprintf(g_load_host, sizeof(g_load_host), "%s", host);
    switch (phase)
    {
    case SXNHTTP_PHASE_RESOLVING:
        snprintf(text, sizeof(text), "Resolving %s...", host);
        percent = 10;
        break;
    case SXNHTTP_PHASE_REDIRECTED:
        snprintf(text, sizeof(text), "Redirected to %s...", host);
        percent = 15;
        break;
    case SXNHTTP_PHASE_CONNECTING:
        snprintf(text, sizeof(text), "Connecting to %s...", host);
        percent = 25;
        break;
    case SXNHTTP_PHASE_SECURING:
        snprintf(text, sizeof(text), "Securing the connection to %s...", host);
        percent = 32;
        break;
    case SXNHTTP_PHASE_WAITING:
        g_load_kib = 0;
        snprintf(text, sizeof(text), "Waiting for %s...", host);
        percent = 40;
        break;
    case SXNHTTP_PHASE_RECEIVING:
    default:
        /* For unknown lengths the byte count advances on its own. */
        if (received / 1024 == g_load_kib)
        {
            return;
        }
        g_load_kib = received / 1024;
        snprintf(text, sizeof(text), "Receiving from %s (%u KB)...", host, (unsigned)g_load_kib);
        percent = expected > 0 && received <= expected ? 40 + (int)(55 * received / expected) : 40;
        break;
    }
    webview_progress(request->id, text, percent);
}

/* ASCII, ignoring case: does `type` (a Content-Type value) name the media
 * type `name`, alone or before its parameters? */
static int webview_type_is(const char *type, const char *name)
{
    size_t index = 0;

    for (; name[index] != '\0'; ++index)
    {
        char value = type[index];

        if (value >= 'A' && value <= 'Z')
        {
            value = (char)(value - 'A' + 'a');
        }
        if (value != name[index])
        {
            return 0;
        }
    }
    return type[index] == '\0' || type[index] == ';' || type[index] == ' ' || type[index] == '\t';
}

/* The head of the page asked for: show it or save it
 * (docs/desktop/WEBVIEWER.md#downloads)? HTML and plain text are shown, and
 * so is a response without a type, as before; an attachment, or any other
 * type, is a file: the load stops before its body and the download takes it
 * from there. An error page is always shown. */
static int webview_http_head(void *user, const struct sxnhttp_response *response)
{
    (void)user;
    if (response->status < 200 || response->status > 299)
    {
        return 0;
    }
    if (response->attachment)
    {
        return 1;
    }
    return !(response->content_type[0] == '\0' || webview_type_is(response->content_type, "text/html") ||
             webview_type_is(response->content_type, "text/plain"));
}

static long webview_http_body(void *user, const unsigned char *data, size_t length)
{
    struct webview_fetch *fetch = user;

    return webview_append(fetch->body, data, length);
}

/* On the worker: collects the page in the result's growable buffer. */
static void webview_fetch_http(const struct webview_request *request, struct webview_result *result)
{
    static struct sxnhttp_response response;
    struct sxnhttp_request load;
    struct webview_fetch fetch = { .request = request, .body = &result->body };
    long status;

    memset(&load, 0, sizeof(load));
    load.url = request->url;
    load.cancel = g_cancel_event;
    load.user_agent = "SavanXP-WebViewer/" SAVANXP_VERSION_STRING;
    load.progress = webview_http_progress;
    load.head = webview_http_head;
    load.sink = webview_http_body;
    load.sink_keep_total = 1;
    load.user = &fetch;
    memset(&response, 0, sizeof(response));
    g_load_host[0] = '\0';

    status = sxnhttp_get(&g_http, &load, &response);
    if (status != 0)
    {
        free(result->body.data);
        result->body = (struct webview_bytes){0};
    }
    if (status == SXNHTTP_ERROR_CANCELLED)
    {
        result->outcome = WEBVIEW_LOAD_CANCELLED;
        return;
    }
    result->outcome = WEBVIEW_LOAD_FAILED;
    if (status == SXNHTTP_ERROR_DNS && response.dns_error == SXNDNS_ERROR_NO_SUCH_NAME)
    {
        snprintf(result->message, sizeof(result->message), "No such host: %s.", g_load_host);
        return;
    }
    if (status == SXNHTTP_ERROR_DNS)
    {
        snprintf(result->message, sizeof(result->message), "Cannot resolve %s: %s", g_load_host,
            sxndns_error_text(response.dns_error));
        return;
    }
    if (status != 0 && response.tls_detail[0] != '\0')
    {
        /* A certificate or handshake failure: what, and the verifier's
         * word. There is nothing to click through. */
        snprintf(result->message, sizeof(result->message), "%s (%s: %s)", sxnhttp_error_text(status),
            g_load_host, response.tls_detail);
        return;
    }
    if (status != 0)
    {
        snprintf(result->message, sizeof(result->message), "%s",
            status == SXNHTTP_ERROR_SINK ? "Not enough memory for this document." : sxnhttp_error_text(status));
        return;
    }
    if ((response.flags & SXNHTTP_RESPONSE_HEAD_ONLY) != 0)
    {
        result->outcome = WEBVIEW_LOAD_DOWNLOAD;
        memcpy(result->location, response.url, sizeof(result->location));
        (void)sxnhttp_file_name(response.filename, response.url, result->file_name, sizeof(result->file_name) - 8);
        return;
    }
    result->outcome = WEBVIEW_LOAD_PAGE;
    result->plain = webview_type_is(response.content_type, "text/plain");
    result->note[0] = '\0';
    result->secure = response.tls_protocol[0] != '\0';
    snprintf(result->security, sizeof(result->security), " (secure, %s)", response.tls_protocol);
    if (response.status != 200)
    {
        snprintf(result->note, sizeof(result->note), " (HTTP %d)", response.status);
    }
    memcpy(result->location, response.url, sizeof(result->location));
}

static int webview_css_head(void *user, const struct sxnhttp_response *response)
{
    (void)user;
    return response->status < 200 || response->status >= 300 || response->attachment ||
        (response->content_type[0] != '\0' && !webview_type_is(response->content_type, "text/css"));
}

static void webview_fetch_sheets(const struct webview_request *request, struct webview_result *result)
{
    static struct sxnhttp_response response;

    result->outcome = WEBVIEW_LOAD_STYLES;
    result->sheets = request->sheets;
    for (struct webview_sheet *sheet = result->sheets; sheet != NULL; sheet = sheet->next)
    {
        if (!webview_location_is_http(sheet->url))
        {
            continue;
        }
        uint64_t now = sxn_clock_ns();
        if (now >= request->deadline || sxn_wait_one(g_cancel_event, 0) >= 0)
        {
            break;
        }
        struct webview_fetch fetch = { .request = request, .body = &sheet->body };
        struct sxnhttp_request load = {
            .url = sheet->url,
            .cancel = g_cancel_event,
            .total_ms = (uint32_t)((request->deadline - now + 999999) / 1000000),
            .user_agent = "SavanXP-WebViewer/" SAVANXP_VERSION_STRING,
            .user = &fetch,
            .head = webview_css_head,
            .sink = webview_http_body,
            .sink_keep_total = 1,
        };
        long status = sxnhttp_get(&g_http, &load, &response);
        if (status != 0 || (response.flags & SXNHTTP_RESPONSE_HEAD_ONLY) != 0)
        {
            free(sheet->body.data);
            sheet->body = (struct webview_bytes){0};
            if (status == SXNHTTP_ERROR_CANCELLED)
            {
                break;
            }
        }
        else
        {
            snprintf(sheet->url, sizeof(sheet->url), "%s", response.url);
        }
    }
}

/* The worker thread: one request at a time, each with the cancel event
 * cleared as it is taken, so a cancel set after that point is for it. */
static void webview_worker_main(void *argument)
{
    static struct webview_result result;
    struct webview_request request;

    (void)argument;
    sxnhttp_client_init(&g_http, &g_resolver);
    for (;;)
    {
        int have = 0;

        (void)sxn_wait_one(g_work_event, -1);
        webview_lock();
        if (g_worker_quit)
        {
            webview_unlock();
            sxn_thread_exit(0);
        }
        if (g_request_ready)
        {
            request = g_request;
            g_request.sheets = NULL;
            g_request_ready = 0;
            (void)sxn_event_reset(g_cancel_event);
            have = 1;
        }
        webview_unlock();
        if (!have)
        {
            continue;
        }
        memset(&result, 0, sizeof(result));
        result.id = request.id;
        sxndns_set_server(&g_resolver, &request.dns);
        if (request.sheets != NULL)
        {
            webview_fetch_sheets(&request, &result);
        }
        else
        {
            webview_fetch_http(&request, &result);
        }
        webview_lock();
        webview_clear_result(&g_result);
        g_result = result;
        g_result_ready = 1;
        webview_unlock();
        (void)sxngui_app_wake(&g_app);
    }
}

/* ---- navigation ----------------------------------------------------------------
 *
 * The UI side of a remote load. g_nav is the navigation the window waits for
 * (ID 0: none); what it pushes into the history, or which history entry it
 * moves to, is applied only when its page arrives.
 */

struct webview_navigation
{
    uint32_t id;
    int push;
    int hist_target;
    uint64_t deadline;
};

static struct webview_navigation g_nav;
static uint32_t g_nav_counter = 0;
/* Waiting for the stylesheet batch. Only the UI owns this document. */
static struct webview_result g_pending_page;
static dom_document *g_pending_document;

static void webview_refresh_commands(void);

static void webview_set_loading(int loading)
{
    WEBVIEW_PROGRESS->value = loading ? 5 : 0;
    webview_refresh_commands();
    sxngui_app_request_repaint(&g_app);
}

/* Drops the navigation in flight, if there is one: an unstarted request is
 * withdrawn and the cancel event set. Its result, when it comes, carries an
 * ID nobody waits for. 1 if something was loading. */
static int webview_cancel_navigation(void)
{
    if (g_nav.id == 0)
    {
        return 0;
    }
    webview_lock();
    g_request_ready = 0;
    webview_free_sheets(g_request.sheets);
    g_request.sheets = NULL;
    (void)sxn_event_set(g_cancel_event);
    webview_unlock();
    webview_clear_result(&g_pending_page);
    dom_node_unref(g_pending_document);
    g_pending_document = NULL;
    g_nav.id = 0;
    webview_set_loading(0);
    return 1;
}

/* Stop (the toolbar's Stop, Esc, View > Stop): the page on screen stays. */
static void webview_stop(void)
{
    webview_status_takes_over();
    if (webview_cancel_navigation())
    {
        snprintf(g_status, sizeof(g_status), "Stopped.");
        webview_sync_address();
        webview_refresh_nav();
    }
}

static void webview_begin_navigation(int push, int hist_target)
{
    if (++g_nav_counter == 0)
    {
        ++g_nav_counter;
    }
    g_nav = (struct webview_navigation){
        .id = g_nav_counter, .push = push, .hist_target = hist_target,
        .deadline = sxn_clock_ns() + (uint64_t)SXNHTTP_TOTAL_MS * 1000000,
    };
}

/* Hands the main document request to the worker. */
static void webview_start_remote(const char *url, int push, int hist_target)
{
    if (g_worker < 0)
    {
        snprintf(g_status, sizeof(g_status), "The network worker did not start.");
        webview_sync_address();
        return;
    }
    webview_begin_navigation(push, hist_target);
    webview_lock();
    g_request.id = g_nav.id;
    snprintf(g_request.url, sizeof(g_request.url), "%s", url);
    g_request.dns = g_dns_server;
    g_request_ready = 1;
    (void)sxn_event_set(g_cancel_event); /* whatever runs now is stale */
    webview_unlock();
    (void)sxn_event_set(g_work_event);
    snprintf(g_status, sizeof(g_status), "Opening %s...", url);
    snprintf(g_address, sizeof(g_address), "%s", url);
    WEBVIEW_ADDR->caret = (int)strlen(g_address);
    WEBVIEW_ADDR->scroll = 0;
    webview_set_loading(1);
}

/* A page is in g_raw and g_path names it: record it and show it. */
static void webview_finish_load(int push, int hist_target)
{
    if (push)
    {
        webview_history_push(g_path);
    }
    if (hist_target >= 0)
    {
        g_hist_index = hist_target;
    }
    webview_render_current();
    webview_sync_address();
    webview_refresh_nav();
}

struct webview_sheet_plan {
    const char *base;
    struct webview_sheet *sheets;
    struct webview_sheet **tail;
    int remote;
};

static int webview_plan_sheet(void *user, dom_element *element, const char *href)
{
    struct webview_sheet_plan *plan = user;
    char reference[WEBVIEW_PATH_CAPACITY];
    char url[WEBVIEW_PATH_CAPACITY];
    size_t length;
    int network;

    if (href == NULL)
    {
        return 0;
    }
    while (webview_html_space((unsigned char)*href))
    {
        ++href;
    }
    length = strlen(href);
    while (length > 0 && webview_html_space((unsigned char)href[length - 1]))
    {
        --length;
    }
    if (length == 0 || length >= sizeof(reference) || href[0] == '#')
    {
        return 0;
    }
    memcpy(reference, href, length);
    reference[length] = '\0';
    if (webview_location_is_http(plan->base))
    {
        if (sxnhttp_resolve(plan->base, reference, url, sizeof(url)) != 0 ||
            (strncmp(plan->base, "https://", 8) == 0 && strncmp(url, "https://", 8) != 0))
        {
            return 0;
        }
    }
    else if (webview_resolve_against(plan->base, reference, url, sizeof(url)) != 0)
    {
        return 0;
    }
    network = webview_location_is_http(url);
    const char *path = strncmp(url, "file://", 7) == 0 ? url + 7 : url;
    if (!network && (webview_location_is_http(plan->base) || path[0] != '/' || path[1] == '/'))
    {
        return 0;
    }

    struct webview_sheet *sheet = calloc(1, sizeof(*sheet));
    if (sheet == NULL)
    {
        return -1;
    }
    sheet->element = element;
    snprintf(sheet->url, sizeof(sheet->url), "%s", url);
    if (network)
    {
        plan->remote = 1;
    }
    else if (webview_read_file(path, &sheet->body) != NULL)
    {
        free(sheet);
        return 0;
    }
    *plan->tail = sheet;
    plan->tail = &sheet->next;
    return 0;
}

static void webview_commit_page(struct webview_result *result, dom_document *document,
    struct webview_sheet *sheets)
{
    struct webview_navigation nav = g_nav;

    g_nav.id = 0;
    webview_set_loading(0);
    free(g_raw.data);
    webview_free_sheets(g_sheets);
    dom_node_unref(g_document);
    g_raw = result->body;
    result->body = (struct webview_bytes){0};
    g_document = document;
    g_sheets = sheets;
    g_plain = result->plain;
    memcpy(g_fetch_note, result->note, sizeof(g_fetch_note));
    g_page_secure = result->secure;
    memcpy(g_security, result->security, sizeof(g_security));
    memcpy(g_path, result->location, sizeof(g_path));
    g_has_file = 1;
    webview_finish_load(nav.push, nav.hist_target);
}

/* Parse on the UI thread. Send only the resource buffers to the worker,
 * keeping the candidate document and the displayed page separate. */
static void webview_prepare_page(struct webview_result *result)
{
    dom_document *document = NULL;
    struct webview_sheet_plan plan = { .base = result->location };
    plan.tail = &plan.sheets;

    if (!result->plain)
    {
        const char *html = result->body.data != NULL ? result->body.data : "";
        document = webengine_parse_html((const uint8_t *)html, result->body.length, NULL, NULL, 0);
        if (document == NULL || webengine_visit_sheets(document, webview_plan_sheet, &plan) != 0)
        {
            webview_free_sheets(plan.sheets);
            dom_node_unref(document);
            webview_clear_result(result);
            g_nav.id = 0;
            webview_set_loading(0);
            snprintf(g_status, sizeof(g_status), "Cannot prepare this document.");
            webview_sync_address();
            return;
        }
    }
    if (plan.remote && g_worker >= 0 && sxn_clock_ns() < g_nav.deadline)
    {
        g_pending_page = *result;
        result->body = (struct webview_bytes){0};
        g_pending_document = document;
        webview_lock();
        g_request = (struct webview_request){
            .id = g_nav.id, .dns = g_dns_server, .sheets = plan.sheets, .deadline = g_nav.deadline,
        };
        g_request_ready = 1;
        webview_unlock();
        (void)sxn_event_set(g_work_event);
        snprintf(g_status, sizeof(g_status), "Loading stylesheets...");
        webview_set_loading(1);
        return;
    }
    webview_commit_page(result, document, plan.sheets);
}

/* On the UI thread, after the worker woke the window: the latest progress
 * line and the result, if they belong to the navigation still awaited. */
static void on_wake(struct sxngui_app *app)
{
    static struct webview_result result;
    struct webview_navigation nav = g_nav;
    char progress[WEBVIEW_STATUS_CAPACITY];
    int percent = 0;
    int have_progress = 0;
    int have_result = 0;

    (void)app;
    webview_download_wake();
    webview_lock();
    if (g_progress_id != 0 && g_progress_id == nav.id)
    {
        memcpy(progress, g_progress, sizeof(progress));
        percent = g_progress_percent;
        have_progress = 1;
    }
    g_progress_id = 0;
    if (g_result_ready)
    {
        g_result_ready = 0;
        if (nav.id != 0 && g_result.id == nav.id)
        {
            result = g_result;
            have_result = 1;
        }
        else
        {
            webview_clear_result(&g_result);
        }
        g_result = (struct webview_result){0};
    }
    webview_unlock();

    if (have_progress || have_result)
    {
        webview_status_takes_over();
    }
    if (!have_result)
    {
        if (have_progress)
        {
            memcpy(g_status, progress, sizeof(g_status));
            WEBVIEW_PROGRESS->value = percent;
        }
        return;
    }
    if (result.outcome == WEBVIEW_LOAD_PAGE)
    {
        webview_prepare_page(&result);
        return;
    }
    if (result.outcome == WEBVIEW_LOAD_STYLES)
    {
        webview_commit_page(&g_pending_page, g_pending_document, result.sheets);
        g_pending_document = NULL;
        result.sheets = NULL;
        return;
    }
    g_nav.id = 0;
    webview_set_loading(0);
    switch (result.outcome)
    {
    case WEBVIEW_LOAD_FAILED:
        memcpy(g_status, result.message, sizeof(g_status));
        break;
    case WEBVIEW_LOAD_DOWNLOAD:
        /* A file: the page on screen stays, and Save As asks where. */
        if (webview_download_busy())
        {
            snprintf(g_status, sizeof(g_status), "A download is already running; %s was not saved.",
                result.file_name);
        }
        else if (webview_download_begin(result.location, result.file_name, &g_dns_server) != 0)
        {
            snprintf(g_status, sizeof(g_status), "Cannot ask where to save %s.", result.file_name);
        }
        else
        {
            snprintf(g_status, sizeof(g_status), "Saving %s...", result.file_name);
        }
        break;
    case WEBVIEW_LOAD_CANCELLED:
    default:
        snprintf(g_status, sizeof(g_status), "Stopped.");
        break;
    }
    /* The bar goes back to the page on screen, unless the person is typing
     * in it: a failure arriving in the background must not eat their text. */
    if (g_app.ui.focus_index != WEBVIEW_ADDR_INDEX)
    {
        webview_sync_address();
    }
    webview_refresh_nav();
}

/* Closing the window: the worker drops its load and ends. */
static void webview_stop_worker(void)
{
    if (g_worker < 0)
    {
        return;
    }
    webview_lock();
    g_worker_quit = 1;
    g_request_ready = 0;
    webview_free_sheets(g_request.sheets);
    g_request.sheets = NULL;
    (void)sxn_event_set(g_cancel_event);
    webview_unlock();
    (void)sxn_event_set(g_work_event);
    if (sxn_wait_one(g_worker, 2000) >= 0)
    {
        webview_clear_result(&g_result);
    }
}


/* The single front door for every navigation: address bar, links, history,
 * dialogs, argv. Resolves `location` against the current page. A local file
 * loads here and is shown at once; an http:// URL goes to the network worker
 * and is shown when it arrives (on_wake). Either way, on success the page is
 * recorded in the history (`push`), or the history moves to `hist_target`
 * (>= 0), and any load still in flight is dropped. */
static int webview_open_location(const char *location, int push, int hist_target)
{
    char target[WEBVIEW_PATH_CAPACITY];
    int resolved;

    webview_status_takes_over();
    if (location == 0 || location[0] == '\0')
    {
        return -1;
    }
    if (strncmp(location, "dns://", 6) == 0)
    {
        /* Resolver override, typed not launched: `dns://10.0.2.2:5353`.
         * Config, not a page: it sets the server the next load asks, reports
         * it, reverts the bar and stays out of history. Same parser as --dns. */
        struct sxn_ipv4_endpoint server;

        if (sxndns_parse_server(location + 6, &server) == 0)
        {
            g_dns_server = server;
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
    (void)webview_cancel_navigation();
    if (strstr(location, "://") != 0)
    {
        snprintf(target, sizeof(target), "%s", location);
    }
    else
    {
        resolved = webview_resolve_against(g_has_file ? g_path : "/", location,
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
        webview_start_remote(target, push, hist_target);
        return 0;
    }
    if (strncmp(target, "file://", 7) != 0 && strstr(target, "://") != 0)
    {
        snprintf(g_status, sizeof(g_status), "Only http and file work so far.");
        webview_sync_address();
        webview_refresh_nav();
        return -1;
    }
    struct webview_result result = { .outcome = WEBVIEW_LOAD_PAGE };
    const char *path = strncmp(target, "file://", 7) == 0 ? target + 7 : target;
    const char *error = webview_read_file(path, &result.body);
    if (error != NULL)
    {
        snprintf(g_status, sizeof(g_status), "%s", error);
        webview_sync_address();
        webview_refresh_nav();
        return -1;
    }
    snprintf(result.location, sizeof(result.location), "%s", path);
    webview_begin_navigation(push, hist_target);
    webview_prepare_page(&result);
    return 0;
}


static void webview_toggle_source(void)
{
    if (!g_has_file)
    {
        snprintf(g_status, sizeof(g_status), "Nothing to show yet.");
        return;
    }
    g_show_source = !g_show_source;
    webview_render_current();
}

static void webview_refresh_nav(void)
{
    webview_refresh_commands();
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
    /* The history moves to `target` only once that page is on screen. */
    (void)webview_open_location(g_history[target], 0, target);
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
    (void)webview_open_location(trimmed, push, -1);
}

/* ---- layout and painting ----------------------------------------------------
 *
 * The engine lays the page out (docs/desktop/WEBVIEWER_ENGINE.md) at the
 * width of the page's area, again only when that width changes, so a resize
 * reflows. Painting walks the blocks for their backgrounds and borders, then
 * the line fragments for inline boxes, text and markers, skipping what lies
 * outside the visible area.
 */

static int webview_page_height(void)
{
    int height = g_view.height - 2 * SXGUI_BORDER_SUNKEN;

    return height > 0 ? height : 1;
}

static int webview_scroll_max(void)
{
    int max = g_total_height - webview_page_height();

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
        sxngui_app_request_repaint(&g_app);
    }
}

/* Lays the page out in the page's area, `width` wide, and sizes the scroll
 * bar after it. The area's height is the viewport of fixed boxes. */
static void webview_measure(int width)
{
    g_layout_width = width;
    g_layout_height = webview_page_height();
    g_total_height = 0;
    if (g_tree != NULL)
    {
        if (webengine_layout(g_tree, &k_webview_fonts, width, g_layout_height) == 0)
        {
            g_total_height = g_tree->height;
        }
        else
        {
            webview_reset_document();
            snprintf(g_status, sizeof(g_status), "Not enough memory to lay out this document.");
        }
    }
    WEBVIEW_SCROLL->range_max = webview_scroll_max();
    WEBVIEW_SCROLL->page = webview_page_height();
    if (WEBVIEW_SCROLL->value > WEBVIEW_SCROLL->range_max)
    {
        WEBVIEW_SCROLL->value = WEBVIEW_SCROLL->range_max;
    }
}

/* The canvas behind everything: the page's colour over the field's. */
static uint32_t webview_canvas(void)
{
    return g_tree != NULL ? webview_color_over(g_tree->canvas, SXGUI_COLOR_FIELD) : SXGUI_COLOR_FIELD;
}

/* What is painted behind `box`: the backgrounds of it and its ancestors,
 * composed over the canvas, for colours with an alpha channel. */
static uint32_t webview_backdrop(const struct webengine_box *box)
{
    const struct webengine_box *chain[32];
    int count = 0;
    uint32_t color;

    for (; box != NULL && count < 32; box = box->parent)
    {
        if (box->type != WEBENGINE_BOX_BLOCK && box->type != WEBENGINE_BOX_INLINE)
        {
            continue;
        }
        if ((box->style->background >> 24) != 0 && box != g_tree->canvas_source)
        {
            chain[count++] = box;
            if ((box->style->background >> 24) == 255)
            {
                break;
            }
        }
    }
    color = webview_canvas();
    while (count > 0)
    {
        color = webview_color_over(chain[--count]->style->background, color);
    }
    return color;
}

static uint32_t webview_shade(uint32_t color, int light)
{
    uint32_t result = 0;

    for (unsigned shift = 0; shift < 24; shift += 8)
    {
        unsigned value = (color >> shift) & 255;

        value = light ? value + (255 - value) * 3 / 4 : value;
        result |= value << shift;
    }
    return result;
}

/* One side of a border as a band, dashed or dotted when its style says. */
static void webview_border_band(struct sx_painter *painter, struct sx_rect band, uint8_t style,
    int horizontal, uint32_t color)
{
    int length = horizontal ? band.width : band.height;
    int thickness = horizontal ? band.height : band.width;
    int on = style == CSS_BORDER_STYLE_DOTTED ? thickness : thickness * 3;

    if (style != CSS_BORDER_STYLE_DOTTED && style != CSS_BORDER_STYLE_DASHED)
    {
        if (style == CSS_BORDER_STYLE_DOUBLE && thickness >= 3)
        {
            int line = (thickness + 1) / 3;

            sx_painter_fill_rect(painter, horizontal
                ? sx_rect_make(band.x, band.y, band.width, line) : sx_rect_make(band.x, band.y, line, band.height),
                color);
            sx_painter_fill_rect(painter, horizontal
                ? sx_rect_make(band.x, band.y + thickness - line, band.width, line)
                : sx_rect_make(band.x + thickness - line, band.y, line, band.height), color);
            return;
        }
        sx_painter_fill_rect(painter, band, color);
        return;
    }
    if (on < 1)
    {
        on = 1;
    }
    for (int offset = 0; offset < length; offset += on * 2)
    {
        int piece = offset + on <= length ? on : length - offset;

        sx_painter_fill_rect(painter, horizontal
            ? sx_rect_make(band.x + offset, band.y, piece, band.height)
            : sx_rect_make(band.x, band.y + offset, band.width, piece), color);
    }
}

/* A box's borders around (x, y, width, height), its border box. inset,
 * outset, groove and ridge shade the colour, lighter on one pair of sides. */
static void webview_paint_borders(struct sx_painter *painter, const struct webengine_box_style *style,
    int x, int y, int width, int height, int left_edge, int right_edge, uint32_t backdrop)
{
    const int top = style->border_width[WEBENGINE_TOP];
    const int bottom = style->border_width[WEBENGINE_BOTTOM];
    const int left = left_edge ? style->border_width[WEBENGINE_LEFT] : 0;
    const int right = right_edge ? style->border_width[WEBENGINE_RIGHT] : 0;
    const struct sx_rect bands[4] = {
        sx_rect_make(x, y, width, top),
        sx_rect_make(x + width - right, y + top, right, height - top - bottom),
        sx_rect_make(x, y + height - bottom, width, bottom),
        sx_rect_make(x, y + top, left, height - top - bottom),
    };

    for (int side = 0; side < 4; ++side)
    {
        uint8_t kind = style->border_style[side];
        uint32_t color;
        int light = 0;

        if (bands[side].width <= 0 || bands[side].height <= 0)
        {
            continue;
        }
        color = webview_color_over(style->border_color[side], backdrop);
        if (kind == CSS_BORDER_STYLE_INSET || kind == CSS_BORDER_STYLE_GROOVE)
        {
            light = side == WEBENGINE_BOTTOM || side == WEBENGINE_RIGHT;
        }
        else if (kind == CSS_BORDER_STYLE_OUTSET || kind == CSS_BORDER_STYLE_RIDGE)
        {
            light = side == WEBENGINE_TOP || side == WEBENGINE_LEFT;
        }
        webview_border_band(painter, bands[side], kind, side == WEBENGINE_TOP || side == WEBENGINE_BOTTOM,
            webview_shade(color, light));
    }
}

static int webview_visible(const struct webengine_box_style *style)
{
    return style->visibility != CSS_VISIBILITY_HIDDEN && style->visibility != CSS_VISIBILITY_COLLAPSE;
}

/* A block's background, then its borders. */
static void webview_paint_block(struct sx_painter *painter, const struct webengine_box *box, int top, int bottom)
{
    int y = g_page_y + box->y;

    if (!webview_visible(box->style) || y >= bottom || y + box->height <= top)
    {
        return;
    }
    if ((box->style->background >> 24) != 0 && box != g_tree->canvas_source)
    {
        sx_painter_fill_rect(painter, sx_rect_make(g_page_x + box->x, y, box->width, box->height),
            webview_color_over(box->style->background, webview_backdrop(box->parent)));
    }
    webview_paint_borders(painter, box->style, g_page_x + box->x, y, box->width, box->height, 1, 1,
        webview_backdrop(box));
}

/* A piece of text, drawn as the painter wants it: NUL-terminated. */
static void webview_draw_text(struct sx_painter *painter, int x, int y, const char *text, size_t length,
    uint32_t color, int bold)
{
    char small[512];
    char *copy = length < sizeof(small) ? small : malloc(length + 1);

    if (copy == NULL)
    {
        return;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    sx_painter_draw_text(painter, x, y, copy, color);
    if (bold)
    {
        sx_painter_draw_text(painter, x + 1, y, copy, color);
    }
    if (copy != small)
    {
        free(copy);
    }
}

static void webview_paint_fragment(struct sx_painter *painter, const struct webengine_fragment *fragment)
{
    const struct webengine_box *box = fragment->box;
    const struct webengine_box_style *style = box->style;
    const int x = g_page_x + fragment->x;
    const int y = g_page_y + fragment->y;
    const int baseline = g_page_y + fragment->baseline;
    const uint32_t backdrop = webview_backdrop(box->parent);
    const uint32_t color = webview_color_over(style->color, backdrop);

    if (!webview_visible(style))
    {
        return;
    }
    switch (fragment->type)
    {
    case WEBENGINE_FRAGMENT_INLINE:
        if ((style->background >> 24) != 0)
        {
            sx_painter_fill_rect(painter, sx_rect_make(x, y, fragment->width, fragment->height),
                webview_color_over(style->background, backdrop));
        }
        webview_paint_borders(painter, style, x, y, fragment->width, fragment->height, fragment->left_edge,
            fragment->right_edge, webview_backdrop(box));
        break;
    case WEBENGINE_FRAGMENT_TEXT:
    {
        int ascent = baseline - y;

        (void)sx_painter_set_font(painter, style->face);
        webview_draw_text(painter, x, y, box->text + fragment->start, fragment->length, color, style->bold);
        if (style->decoration & CSS_TEXT_DECORATION_UNDERLINE)
        {
            sx_painter_hline(painter, x, baseline + 1, fragment->width, color);
        }
        if (style->decoration & CSS_TEXT_DECORATION_OVERLINE)
        {
            sx_painter_hline(painter, x, y, fragment->width, color);
        }
        if (style->decoration & CSS_TEXT_DECORATION_LINE_THROUGH)
        {
            sx_painter_hline(painter, x, baseline - ascent / 3, fragment->width, color);
        }
        break;
    }
    case WEBENGINE_FRAGMENT_MARKER:
    {
        struct sx_rect shape = sx_rect_make(x, y, fragment->width, fragment->height);

        if (box->text != NULL)
        {
            (void)sx_painter_set_font(painter, style->face);
            webview_draw_text(painter, x, y, box->text, box->length, color, style->bold);
        }
        else if (style->list_style_type == CSS_LIST_STYLE_TYPE_SQUARE)
        {
            sx_painter_fill_rect(painter, shape, color);
        }
        else if (style->list_style_type == CSS_LIST_STYLE_TYPE_CIRCLE)
        {
            sx_painter_draw_ellipse(painter, shape, color);
        }
        else
        {
            sx_painter_fill_ellipse(painter, shape, color);
        }
        break;
    }
    default:
        break;
    }
}

/* The link under (x, y) in the window, from the last paint's layout: the
 * topmost, so the painting order is walked backwards. */
static int webview_link_at(int x, int y, char *target, size_t capacity)
{
    if (g_tree == NULL || !sx_rect_contains_point(g_page_area, x, y))
    {
        return 0;
    }
    for (size_t i = g_tree->paint_count; i > 0; --i)
    {
        const struct webengine_paint *item = &g_tree->paint[i - 1];
        const struct webengine_box *box = item->box;
        int px = x - g_page_x;
        int py = y - (item->fixed ? g_page_area.y : g_page_area.y - WEBVIEW_SCROLL->value);

        if (item->kind != WEBENGINE_PAINT_LINES)
        {
            continue;
        }
        for (size_t l = box->first_line; l < box->first_line + box->line_count; ++l)
        {
            const struct webengine_line *line = &g_tree->lines[l];

            for (size_t f = line->first_fragment; f < line->first_fragment + line->fragment_count; ++f)
            {
                const struct webengine_fragment *fragment = &g_tree->fragments[f];
                int link = fragment->box->link;

                if (link >= 0 && fragment->type != WEBENGINE_FRAGMENT_MARKER &&
                    px >= fragment->x && px < fragment->x + fragment->width &&
                    py >= fragment->y && py < fragment->y + fragment->height)
                {
                    snprintf(target, capacity, "%s", g_tree->links[link]);
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void on_paint(struct sxngui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    struct sx_rect interior;
    int top;
    int bottom;

    if (g_page_secure && g_has_file)
    {
        const struct sx_rect zone = WEBVIEW_ZONE->rect;

        sx_painter_blit_bitmap(painter, &g_lock, zone.x + (zone.width - WEBVIEW_LOCK_SIZE) / 2,
            zone.y + (zone.height - WEBVIEW_LOCK_SIZE) / 2);
    }
    sxgui_draw_sunken_edge(painter, g_view);
    interior = sx_rect_make(g_view.x + SXGUI_BORDER_SUNKEN, g_view.y + SXGUI_BORDER_SUNKEN,
        g_view.width - SXGUI_BORDER_SUNKEN * 2, g_view.height - SXGUI_BORDER_SUNKEN * 2);
    /* The page keeps the scrollbar strip clear on the right. */
    interior.width -= SXGUI_SCROLLBAR_THICKNESS + SXGUI_GAP;
    if (interior.width < 40)
    {
        interior.width = 40;
    }
    if (interior.width != g_layout_width || webview_page_height() != g_layout_height)
    {
        int before = WEBVIEW_SCROLL->value;

        webview_measure(interior.width);
        if (WEBVIEW_SCROLL->value != before)
        {
            sxngui_app_request_repaint(&g_app);
        }
    }
    sx_painter_fill_rect(painter, sx_rect_make(interior.x, interior.y,
        g_view.width - SXGUI_BORDER_SUNKEN * 2, interior.height), webview_canvas());
    g_page_area = interior;
    g_page_x = interior.x;
    g_page_y = interior.y - WEBVIEW_SCROLL->value;
    if (g_tree == NULL || !sx_painter_push_clip(painter, interior))
    {
        return;
    }
    top = interior.y;
    bottom = interior.y + interior.height;
    /* In the engine's order: backgrounds, floats, then text (CSS 2.1
     * Appendix E). */
    for (size_t i = 0; i < g_tree->paint_count; ++i)
    {
        const struct webengine_paint *item = &g_tree->paint[i];
        const struct webengine_box *box = item->box;

        /* Fixed boxes stay where they are while the page scrolls. */
        g_page_y = item->fixed ? interior.y : interior.y - WEBVIEW_SCROLL->value;
        if (item->kind == WEBENGINE_PAINT_BLOCK)
        {
            webview_paint_block(painter, box, top, bottom);
        }
        else if (item->kind == WEBENGINE_PAINT_MARKER)
        {
            const struct webengine_fragment marker = {
                .type = WEBENGINE_FRAGMENT_MARKER, .box = box, .x = box->x, .y = box->y,
                .width = box->width, .height = box->height, .baseline = box->y + box->height,
                .length = box->length,
            };

            if (g_page_y + box->y < bottom && g_page_y + box->y + box->height > top)
            {
                webview_paint_fragment(painter, &marker);
            }
        }
        else
        {
            for (size_t l = box->first_line; l < box->first_line + box->line_count; ++l)
            {
                const struct webengine_line *line = &g_tree->lines[l];

                if (g_page_y + line->y >= bottom || g_page_y + line->y + line->height <= top)
                {
                    continue;
                }
                for (size_t f = line->first_fragment; f < line->first_fragment + line->fragment_count; ++f)
                {
                    webview_paint_fragment(painter, &g_tree->fragments[f]);
                }
            }
        }
    }
    g_page_y = interior.y - WEBVIEW_SCROLL->value;
    (void)sx_painter_set_font(painter, SX_FONT_UI);
    sx_painter_pop_clip(painter);
}

/* ---- layout ----------------------------------------------------------------- */

/* Toolbar order, separators included: the history, the page, the document. */
static struct sxgui_widget *const k_toolbar_order[] = {
    WEBVIEW_BACK, WEBVIEW_FWD, WEBVIEW_SEP_NAV, WEBVIEW_STOP, WEBVIEW_REFRESH, WEBVIEW_HOME,
    WEBVIEW_SEP_PAGE, WEBVIEW_OPEN, WEBVIEW_SOURCE,
};

/* Bands under the menu, each ruled off by an etched line as in the Explorer
 * (and Files): the toolbar, the address bar, the page, the status bar with
 * its progress pane. */
static void webview_layout(struct sxngui_app *app)
{
    int width = (int)app->info.width;
    int height = (int)app->info.height;
    int toolbar_height = sxgui_toolbar_height();
    int button_width = sxgui_toolbar_button_width("Forward");
    int label_width = gfx_text_width("Address") + SXGUI_GAP;
    int y = sxgui_menubar_height();
    int x = SXGUI_MARGIN;
    int go_x = width - SXGUI_MARGIN - WEBVIEW_GO_WIDTH;
    int field_x = SXGUI_MARGIN + label_width;
    int top;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int content_width = width - SXGUI_MARGIN * 2;
    int view_height;

    WEBVIEW_RULE_MENU->rect = sx_rect_make(0, y, width, WEBVIEW_RULE_HEIGHT);
    y += WEBVIEW_RULE_HEIGHT;
    /* Buttons that do not fit a narrow window are hidden, not squeezed. */
    for (size_t index = 0; index < sizeof(k_toolbar_order) / sizeof(k_toolbar_order[0]); ++index)
    {
        struct sxgui_widget *widget = k_toolbar_order[index];
        int item_width = widget->kind == SXGUI_BUTTON ? button_width : WEBVIEW_SEPARATOR_WIDTH;

        widget->rect = widget->kind == SXGUI_BUTTON
            ? sx_rect_make(x, y, item_width, toolbar_height)
            : sx_rect_make(x + 3, y + 3, 2, toolbar_height - 6);
        if (x + item_width <= width - SXGUI_MARGIN)
        {
            widget->flags |= SXGUI_FLAG_VISIBLE;
        }
        else
        {
            widget->flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        }
        x += item_width;
    }
    y += toolbar_height;
    WEBVIEW_RULE_TOOLBAR->rect = sx_rect_make(0, y, width, WEBVIEW_RULE_HEIGHT);
    y += WEBVIEW_RULE_HEIGHT;

    if (go_x < field_x + 60)
    {
        go_x = field_x + 60;
    }
    WEBVIEW_ADDR_LABEL->rect = sx_rect_make(SXGUI_MARGIN, y, label_width, SXGUI_FIELD_HEIGHT);
    WEBVIEW_ADDR->rect = sx_rect_make(field_x, y, go_x - SXGUI_GAP - field_x, SXGUI_FIELD_HEIGHT);
    WEBVIEW_GO->rect = sx_rect_make(go_x, y, WEBVIEW_GO_WIDTH, SXGUI_FIELD_HEIGHT);
    top = y + SXGUI_FIELD_HEIGHT + SXGUI_GAP;
    view_height = status_y - SXGUI_GAP - top;

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
    WEBVIEW_STATUS->rect = sx_rect_make(SXGUI_MARGIN, status_y,
        content_width - WEBVIEW_PROGRESS_WIDTH - WEBVIEW_ZONE_WIDTH - SXGUI_GAP, SXGUI_STATUS_HEIGHT);
    WEBVIEW_ZONE->rect = sx_rect_make(SXGUI_MARGIN + content_width - WEBVIEW_PROGRESS_WIDTH - WEBVIEW_ZONE_WIDTH -
            SXGUI_GAP / 2,
        status_y, WEBVIEW_ZONE_WIDTH, SXGUI_STATUS_HEIGHT);
    WEBVIEW_PROGRESS->rect = sx_rect_make(SXGUI_MARGIN + content_width - WEBVIEW_PROGRESS_WIDTH,
        status_y, WEBVIEW_PROGRESS_WIDTH, SXGUI_STATUS_HEIGHT);
}

static void on_resize(struct sxngui_app *app)
{
    webview_layout(app);
    g_layout_width = -1;
}

/* ---- interaction -------------------------------------------------------------- */

static void on_scroll_changed(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxngui_app_request_repaint(&g_app);
}

static void on_menu_command(int id, void *user);
static void webview_favorites_change(int add);
static void webview_favorites_open(int index);

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

static void on_toolbar(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget == WEBVIEW_STOP)
    {
        webview_stop();
    }
    else if (widget == WEBVIEW_REFRESH)
    {
        on_menu_command(WEBVIEW_MENU_RELOAD, 0);
    }
    else if (widget == WEBVIEW_HOME)
    {
        on_menu_command(WEBVIEW_MENU_HOME, 0);
    }
    else if (widget == WEBVIEW_OPEN)
    {
        on_menu_command(WEBVIEW_MENU_OPEN, 0);
    }
    else if (widget == WEBVIEW_SOURCE)
    {
        on_menu_command(WEBVIEW_MENU_SOURCE, 0);
    }
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
            (void)webview_open_location(g_path, 0, -1);
        }
        break;
    case WEBVIEW_MENU_BACK:
        webview_history_go(-1);
        break;
    case WEBVIEW_MENU_FORWARD:
        webview_history_go(1);
        break;
    case WEBVIEW_MENU_STOP:
        webview_stop();
        break;
    case WEBVIEW_MENU_HOME:
        (void)webview_open_location(WEBVIEW_HOME_PAGE, 1, -1);
        break;
    case WEBVIEW_MENU_EXIT:
        sxngui_app_quit(&g_app, 0);
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
    case WEBVIEW_MENU_FAVORITE_ADD:
        webview_favorites_change(1);
        break;
    case WEBVIEW_MENU_FAVORITE_REMOVE:
        webview_favorites_change(0);
        break;
    case WEBVIEW_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, WEBVIEW_ABOUT_WIDTH,
            WEBVIEW_ABOUT_HEIGHT);
        break;
    default:
        if (id >= WEBVIEW_MENU_FAVORITE_FIRST && id < WEBVIEW_MENU_FAVORITE_FIRST + WEBVIEW_FAVORITES_MAX)
        {
            webview_favorites_open(id - WEBVIEW_MENU_FAVORITE_FIRST);
        }
        break;
    }
}

/* The viewport is custom-painted, not a widget, so the wheel would hit nothing.
 * This pre-toolkit hook turns wheel ticks over it into scrollbar motion, the
 * same 3-rows-per-tick convention the toolkit uses elsewhere. A left-button
 * press and release inside the text area follows the link under the cursor,
 * if any. While a modal dialog is open the hook stays out of the way. */
static const struct sxgui_menu k_page_menu;
static void webview_refresh_commands(void);

static int g_prev_left = 0;
static int g_prev_right = 0;
static int g_press_in_text = 0;
/* The link under the pointer, shown in the status bar instead of the page's
 * own status while the pointer rests on it, as browsers do. */
static char g_hover_target[WEBVIEW_PATH_CAPACITY];
static int g_hovering = 0;
/* The pointer's last position: a link shows only after the pointer moves. */
static int g_pointer_x = -1;
static int g_pointer_y = -1;

static void webview_hover(int x, int y)
{
    char href[WEBVIEW_PATH_CAPACITY];
    char target[WEBVIEW_PATH_CAPACITY];
    int over = sx_rect_contains_point(g_page_area, x, y) &&
               webview_link_at(x, y, href, sizeof(href));

    /* The link as it would be opened, absolute, not as the page wrote it. */
    if (over && webview_resolve_against(g_has_file ? g_path : "/", href, target, sizeof(target)) != 0)
    {
        memcpy(target, href, sizeof(target));
    }

    if (over && (!g_hovering || strcmp(target, g_hover_target) != 0))
    {
        memcpy(g_hover_target, target, sizeof(g_hover_target));
        WEBVIEW_STATUS->text = g_hover_target;
        g_hovering = 1;
        sxngui_app_request_repaint(&g_app);
    }
    else if (!over && g_hovering)
    {
        WEBVIEW_STATUS->text = g_status;
        g_hovering = 0;
        sxngui_app_request_repaint(&g_app);
    }
}

static int on_pointer(struct sxngui_app *app, const struct savanxp_gui_pointer_event *event)
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
    if (event->x != g_pointer_x || event->y != g_pointer_y)
    {
        g_pointer_x = event->x;
        g_pointer_y = event->y;
        webview_hover(event->x, event->y);
    }
    {
        int right = (event->buttons & SAVANXP_MOUSE_BUTTON_RIGHT) != 0;

        if (!right && g_prev_right && sx_rect_contains_point(g_view, event->x, event->y))
        {
            g_prev_right = 0;
            webview_refresh_commands();
            sxgui_context_menu_open(&g_app.ui, &k_page_menu, event->x, event->y, on_menu_command, 0);
            return 1;
        }
        g_prev_right = right;
    }
    left = (event->buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0;
    if (left && !g_prev_left)
    {
        g_press_in_text = sx_rect_contains_point(g_page_area, event->x, event->y);
    }
    if (!left && g_prev_left)
    {
        int pressed = g_press_in_text;

        g_press_in_text = 0;
        g_prev_left = 0;
        if (pressed && sx_rect_contains_point(g_page_area, event->x, event->y))
        {
            char target[WEBVIEW_PATH_CAPACITY];

            if (webview_link_at(event->x, event->y, target, sizeof(target)))
            {
                webview_open_location(target, 1, -1);
                return 1;
            }
        }
        return 0;
    }
    g_prev_left = left;
    return 0;
}

static int on_key(struct sxngui_app *app, const struct savanxp_input_event *event)
{
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    if (sxgui_dialog_active(&app->ui))
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_ESC && g_nav.id != 0)
    {
        webview_stop(); /* Esc stops a load; otherwise it closes, as ever */
        return 1;
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
            (void)webview_open_location(g_path, 0, -1);
        }
        return 1;
    case SAVANXP_KEY_UP:
        webview_set_scroll(WEBVIEW_SCROLL->value - 18);
        return 1;
    case SAVANXP_KEY_DOWN:
        webview_set_scroll(WEBVIEW_SCROLL->value + 18);
        return 1;
    case SAVANXP_KEY_PAGE_UP:
        webview_set_scroll(WEBVIEW_SCROLL->value - webview_page_height());
        return 1;
    case SAVANXP_KEY_PAGE_DOWN:
        webview_set_scroll(WEBVIEW_SCROLL->value + webview_page_height());
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

/* ---- menus and toolbar glyphs ---------------------------------------------------- */

static struct sx_bitmap g_glyphs[WEBVIEW_TOOLBAR_BUTTONS];
static const struct sx_bitmap *g_glyph_refs[WEBVIEW_TOOLBAR_BUTTONS];

/* Wraps the embedded 20x20 glyphs, in toolbar order, as bitmaps. */
static void webview_init_glyphs(void)
{
    static const uint32_t *const pixels[WEBVIEW_TOOLBAR_BUTTONS] = {
        toolbar_glyph_go_previous, toolbar_glyph_go_next, webview_glyph_process_stop,
        webview_glyph_view_refresh, webview_glyph_go_home, webview_glyph_document_open,
        webview_glyph_text_html,
    };
    struct savanxp_fb_info info = {0};

    info.width = info.height = TOOLBAR_GLYPH_SIZE;
    info.pitch = TOOLBAR_GLYPH_SIZE * 4;
    info.bpp = 32;
    info.buffer_size = TOOLBAR_GLYPH_SIZE * TOOLBAR_GLYPH_SIZE * 4;
    for (int index = 0; index < WEBVIEW_TOOLBAR_BUTTONS; ++index)
    {
        sx_bitmap_wrap(&g_glyphs[index], (uint32_t *)pixels[index], &info, SX_PIXEL_FORMAT_BGRA8888);
        g_glyph_refs[index] = &g_glyphs[index];
    }
    info.width = info.height = WEBVIEW_LOCK_SIZE;
    info.pitch = WEBVIEW_LOCK_SIZE * 4;
    info.buffer_size = WEBVIEW_LOCK_SIZE * WEBVIEW_LOCK_SIZE * 4;
    sx_bitmap_wrap(&g_lock, (uint32_t *)webview_glyph_security_high, &info, SX_PIXEL_FORMAT_BGRA8888);
}

static const struct sxgui_menu_item k_file_items[] = {
    {"Open...", WEBVIEW_MENU_OPEN, 0},
    {0, 0, 0},
    {"Exit", WEBVIEW_MENU_EXIT, 0},
};

/* Enabled and disabled with the toolbar, by webview_refresh_commands. */
static struct sxgui_menu_item k_view_items[] = {
    {"Stop", WEBVIEW_MENU_STOP, 0},
    {"Refresh", WEBVIEW_MENU_RELOAD, 0},
    {0, 0, 0},
    {"Top of page", WEBVIEW_MENU_TOP, 0},
    {"End of page", WEBVIEW_MENU_BOTTOM, 0},
    {0, 0, 0},
    {"View Source", WEBVIEW_MENU_SOURCE, 0},
};

static struct sxgui_menu_item k_go_items[] = {
    {"Back", WEBVIEW_MENU_BACK, 0},
    {"Forward", WEBVIEW_MENU_FORWARD, 0},
    {0, 0, 0},
    {"Home Page", WEBVIEW_MENU_HOME, 0},
};

/* The page's context menu (a right click on it). */
static struct sxgui_menu_item k_page_items[] = {
    {"Back", WEBVIEW_MENU_BACK, 0},
    {"Forward", WEBVIEW_MENU_FORWARD, 0},
    {0, 0, 0},
    {"Refresh", WEBVIEW_MENU_RELOAD, 0},
    {0, 0, 0},
    {"View Source", WEBVIEW_MENU_SOURCE, 0},
};

static const struct sxgui_menu k_page_menu = {
    "Page", k_page_items, (int)(sizeof(k_page_items) / sizeof(k_page_items[0]))};

static void webview_menu_enable(struct sxgui_menu_item *items, int count, int id, int enabled)
{
    for (int index = 0; index < count; ++index)
    {
        if (items[index].id == id)
        {
            items[index].flags = enabled ? 0 : SXGUI_MENU_DISABLED;
        }
    }
}

static void webview_button_enable(struct sxgui_widget *widget, int enabled)
{
    if (enabled)
    {
        widget->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        widget->flags |= SXGUI_FLAG_DISABLED;
    }
}

/* ---- favorites ------------------------------------------------------------------
 *
 * The Favorites menu, as Internet Explorer 5 had it: add or remove the page
 * on screen, then one row per favorite. The list lives in the person's
 * profile (sxn_settings_path, WEBVIEW_FAVORITES_FILE); a profile without the
 * file starts with webview_favorites_defaults. */

static struct webview_favorites g_favorites;
static char g_favorites_text[WEBVIEW_FAVORITES_TEXT_CAP];

#define WEBVIEW_FAVORITES_FIXED 2
/* Its place in k_menus. */
#define WEBVIEW_FAVORITES_MENU 3
static struct sxgui_menu_item k_favorites_items[WEBVIEW_FAVORITES_FIXED + 1 + WEBVIEW_FAVORITES_MAX] = {
    {"Add to Favorites", WEBVIEW_MENU_FAVORITE_ADD, 0},
    {"Remove from Favorites", WEBVIEW_MENU_FAVORITE_REMOVE, 0},
};

static void webview_favorites_load(void)
{
    char path[WEBVIEW_PATH_CAPACITY];
    long handle = -1;
    size_t total = 0;

    if (sxn_settings_path(WEBVIEW_FAVORITES_FILE, 0, path, sizeof(path)) == 0)
    {
        handle = sxn_open_file(path, SXN_FILE_READ);
    }
    if (handle < 0)
    {
        webview_favorites_defaults(&g_favorites);
        return;
    }
    while (total + 1 < sizeof(g_favorites_text))
    {
        long result = sxn_read(handle, g_favorites_text + total,
            (unsigned long)(sizeof(g_favorites_text) - 1 - total));

        if (result <= 0)
        {
            break;
        }
        total += (size_t)result;
    }
    (void)sxn_close(handle);
    webview_favorites_parse(&g_favorites, g_favorites_text, total);
}

/* 0, or the negative status; the status bar says when it fails. */
static long webview_favorites_save(void)
{
    char path[WEBVIEW_PATH_CAPACITY];
    size_t length = webview_favorites_format(&g_favorites, g_favorites_text, sizeof(g_favorites_text));
    long result = length == 0 ? -1 : sxn_settings_path(WEBVIEW_FAVORITES_FILE, 1, path, sizeof(path));

    return result < 0 ? result : sxn_write_file(path, g_favorites_text, (unsigned long)length);
}

static const char *webview_favorites_page_name(void)
{
    return g_title[0] != '\0' ? g_title : g_path;
}

/* Adds or removes the page on screen, saves the list and says so. */
static void webview_favorites_change(int add)
{
    const int index = webview_favorites_find(&g_favorites, g_path);
    char name[WEBVIEW_FAVORITE_NAME_CAP];

    if (!g_has_file || (add ? index >= 0 : index < 0))
    {
        return;
    }
    if (add)
    {
        const int added = webview_favorites_add(&g_favorites, g_path, webview_favorites_page_name());

        if (added < 0)
        {
            webview_status_takes_over();
            snprintf(g_status, sizeof(g_status), "Favorites is full; remove one first.");
            return;
        }
        snprintf(name, sizeof(name), "%s", g_favorites.items[added].name);
    }
    else
    {
        snprintf(name, sizeof(name), "%s", g_favorites.items[index].name);
        (void)webview_favorites_remove(&g_favorites, index);
    }
    webview_status_takes_over();
    if (webview_favorites_save() < 0)
    {
        snprintf(g_status, sizeof(g_status), "Favorites could not be saved.");
    }
    else
    {
        snprintf(g_status, sizeof(g_status), add ? "Added %s to Favorites." : "Removed %s from Favorites.", name);
    }
    webview_refresh_commands();
}

static void webview_favorites_open(int index)
{
    if (index >= 0 && index < g_favorites.count)
    {
        char url[WEBVIEW_FAVORITE_URL_CAP];

        /* Copied: navigating may change the list's rows under it. */
        snprintf(url, sizeof(url), "%s", g_favorites.items[index].url);
        (void)webview_open_location(url, 1, -1);
    }
}

static const struct sxgui_menu_item k_help_items[] = {
    {"About Web Viewer", WEBVIEW_MENU_ABOUT, 0},
};

/* Not const: webview_favorites_refresh_menu sets the Favorites menu's rows. */
static struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Go", k_go_items, (int)(sizeof(k_go_items) / sizeof(k_go_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
    {"Favorites", k_favorites_items, WEBVIEW_FAVORITES_FIXED},
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

/* The Favorites menu after the list and the page on screen: Add for a page
 * not in it, Remove for one that is, which is also checked in the list. */
static void webview_favorites_refresh_menu(void)
{
    const int current = g_has_file ? webview_favorites_find(&g_favorites, g_path) : -1;
    int count = WEBVIEW_FAVORITES_FIXED;

    k_favorites_items[0].flags =
        g_has_file && current < 0 && g_favorites.count < WEBVIEW_FAVORITES_MAX ? 0 : SXGUI_MENU_DISABLED;
    k_favorites_items[1].flags = current >= 0 ? 0 : SXGUI_MENU_DISABLED;
    if (g_favorites.count > 0)
    {
        k_favorites_items[count++] = (struct sxgui_menu_item){0, 0, 0};
    }
    for (int index = 0; index < g_favorites.count; ++index)
    {
        k_favorites_items[count++] = (struct sxgui_menu_item){
            g_favorites.items[index].name, WEBVIEW_MENU_FAVORITE_FIRST + index,
            index == current ? SXGUI_MENU_CHECKED : 0};
    }
    k_menus[WEBVIEW_FAVORITES_MENU].item_count = count;
}

/* One place decides what can be done now, so a grey button and a grey menu
 * item never disagree (as in Files). Back and Forward stay with
 * webview_refresh_nav, which calls this. */
static void webview_refresh_commands(void)
{
    const int loading = g_nav.id != 0;
    const int back = g_hist_index > 0;
    const int forward = g_hist_index >= 0 && g_hist_index + 1 < g_hist_count;
    const int count_view = (int)(sizeof(k_view_items) / sizeof(k_view_items[0]));
    const int count_go = (int)(sizeof(k_go_items) / sizeof(k_go_items[0]));
    const int count_page = (int)(sizeof(k_page_items) / sizeof(k_page_items[0]));

    webview_button_enable(WEBVIEW_STOP, loading);
    webview_button_enable(WEBVIEW_REFRESH, g_has_file);
    webview_button_enable(WEBVIEW_SOURCE, g_has_file);
    webview_menu_enable(k_view_items, count_view, WEBVIEW_MENU_STOP, loading);
    webview_menu_enable(k_view_items, count_view, WEBVIEW_MENU_RELOAD, g_has_file);
    webview_menu_enable(k_view_items, count_view, WEBVIEW_MENU_SOURCE, g_has_file);
    webview_menu_enable(k_go_items, count_go, WEBVIEW_MENU_BACK, back);
    webview_menu_enable(k_go_items, count_go, WEBVIEW_MENU_FORWARD, forward);
    webview_menu_enable(k_page_items, count_page, WEBVIEW_MENU_BACK, back);
    webview_menu_enable(k_page_items, count_page, WEBVIEW_MENU_FORWARD, forward);
    webview_menu_enable(k_page_items, count_page, WEBVIEW_MENU_RELOAD, g_has_file);
    webview_menu_enable(k_page_items, count_page, WEBVIEW_MENU_SOURCE, g_has_file);
    webview_favorites_refresh_menu();
}


int main(int argc, char **argv)
{
    *WEBVIEW_ADDR = sxgui_textfield(sx_rect_make(0, 0, 0, 0), g_address, sizeof(g_address));
    *WEBVIEW_GO = sxgui_button(sx_rect_make(0, 0, 0, 0), "Go", on_go, 0);
    webview_init_glyphs();
    *WEBVIEW_BACK = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Back", &g_glyph_refs[0], on_back, 0);
    *WEBVIEW_FWD = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Forward", &g_glyph_refs[1], on_forward, 0);
    *WEBVIEW_STOP = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Stop", &g_glyph_refs[2], on_toolbar, 0);
    *WEBVIEW_REFRESH = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Refresh", &g_glyph_refs[3], on_toolbar, 0);
    *WEBVIEW_HOME = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Home", &g_glyph_refs[4], on_toolbar, 0);
    *WEBVIEW_OPEN = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Open", &g_glyph_refs[5], on_toolbar, 0);
    *WEBVIEW_SOURCE = sxgui_toolbar_button(sx_rect_make(0, 0, 0, 0), "Source", &g_glyph_refs[6], on_toolbar, 0);
    *WEBVIEW_SEP_NAV = sxgui_separator(sx_rect_make(0, 0, 0, 0));
    *WEBVIEW_SEP_PAGE = sxgui_separator(sx_rect_make(0, 0, 0, 0));
    *WEBVIEW_RULE_MENU = sxgui_separator(sx_rect_make(0, 0, 0, 0));
    *WEBVIEW_RULE_TOOLBAR = sxgui_separator(sx_rect_make(0, 0, 0, 0));
    *WEBVIEW_ADDR_LABEL = sxgui_label(sx_rect_make(0, 0, 0, 0), "Address");
    *WEBVIEW_PROGRESS = sxgui_progress(sx_rect_make(0, 0, 0, 0), 0, 100, 0);
    *WEBVIEW_SCROLL = sxgui_scrollbar(sx_rect_make(0, 0, 0, 0), 0, 0, 10, 0);
    WEBVIEW_SCROLL->on_action = on_scroll_changed;
    *WEBVIEW_STATUS = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status);
    WEBVIEW_STATUS->flags |= SXGUI_FLAG_SUNKEN;
    *WEBVIEW_ZONE = sxgui_label(sx_rect_make(0, 0, 0, 0), "");
    WEBVIEW_ZONE->flags |= SXGUI_FLAG_SUNKEN;

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

    if (sxngui_app_init(&g_app, &sun_resources, g_widgets, WEBVIEW_WIDGET_COUNT) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_paint = on_paint;
    g_app.on_resize = on_resize;
    g_app.on_wake = on_wake;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    (void)sxngui_app_set_content_size(&g_app, WEBVIEW_CONTENT_WIDTH, WEBVIEW_CONTENT_HEIGHT);
    webview_layout(&g_app);

    /* The network worker: remote loads run on it from the first one. Without
     * it the window still opens local files and says why http:// fails. */
    g_work_event = sxn_event_create(0);
    g_cancel_event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    if (g_work_event >= 0 && g_cancel_event >= 0)
    {
        g_worker = sxn_thread_create(webview_worker_main, 0,
            g_worker_stack + sizeof(g_worker_stack), 0);
    }
    webview_download_init(&g_app);
    if (webengine_init() == 0)
    {
        g_ua = webengine_stylesheet("", (const uint8_t *)webengine_ua_css, webengine_ua_css_length, false);
    }

    webview_favorites_load();
    webview_show_welcome();
    /* argv carries an optional resolver override plus the location to open:
     * `webview --dns 10.0.2.2:5353 http://name/page`. Files launches us with
     * a bare path, which still lands below as the initial location. */
    {
        const char *initial = 0;
        int bad_dns = 0;
        int i;

        sxndns_init(&g_resolver);
        g_dns_server = g_resolver.server;
        if (argv != 0)
        {
            for (i = 1; i < argc; ++i)
            {
                if (argv[i] != 0 && strcmp(argv[i], "--dns") == 0 && i + 1 < argc &&
                    argv[i + 1] != 0)
                {
                    struct sxn_ipv4_endpoint server;

                    if (sxndns_parse_server(argv[i + 1], &server) == 0)
                    {
                        g_dns_server = server;
                    }
                    else
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
    {
        int exit_code = sxngui_app_run(&g_app);

        webview_stop_worker();
        webview_download_shutdown();
        webview_clear_result(&g_pending_page);
        dom_node_unref(g_pending_document);
        webview_free_sheets(g_sheets);
        dom_node_unref(g_document);
        if (g_ua != NULL)
        {
            css_stylesheet_destroy(g_ua);
        }
        webview_reset_document();
        webengine_fini();
        free(g_raw.data);
        return exit_code;
    }
}
