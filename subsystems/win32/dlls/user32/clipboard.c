/*
 * clipboard.c -- Win32's clipboard over SavanXP's (docs/userland/WIN32.md,
 * "The clipboard"; docs/desktop/CLIPBOARD.md): OpenClipboard to
 * CloseClipboard, the formats a program sets and gets, and the ones Windows
 * makes from them.
 *
 * While the program that set the clipboard is its owner, its own handles
 * are what GetClipboardData gives back. When it closes the clipboard after
 * changing it, every format that is memory goes to the system's set under a
 * shared name: CF_UNICODETEXT as UTF-8 text with LF line ends, "Rich Text
 * Format" as text/rtf, CF_HDROP as text/uri-list, any other registered
 * format as application/x-win32-NAME and other numbered ones as
 * application/x-win32-cfN. A format set with no data is rendered then
 * (WM_RENDERFORMAT), since the system's clipboard keeps no owner. What
 * another program left is read from the set and kept until it changes.
 */
#include "user32_private.h"

__declspec(dllimport) LCID WINAPI GetUserDefaultLCID(void);

#define MAX_ITEMS 32
#define TEXT_NAME "text/plain;charset=utf-8"
#define RTF_NAME "text/rtf"
#define URI_NAME "text/uri-list"
#define PREFIX "application/x-win32-"
#define DIB_LIKE 1

typedef struct item {
    UINT format;
    HANDLE data;  /* 0 until rendered */
    int made;     /* made here (synthesized or read from the set): freed by us */
} item;

static item g_items[MAX_ITEMS];
static int g_count;
static int g_open;
static HWND g_open_window;
static HWND g_owner;
static int g_changed;     /* emptied or set while open */
static int g_ours;        /* the items are this program's own set */
static u32 g_seen_low;    /* the system's sequence the items match */
static u32 g_seen_high;
static int g_have_seen;
/* What GetClipboardSequenceNumber adds to the system's: Windows XP counts
 * EmptyClipboard, every SetClipboardData and every format it makes at
 * CloseClipboard, where the system's set changes once (fixtures/clip). */
static u32 g_sequence_bias;

static u32 g_rtf;

static UINT rtf_format(void) {
    if (g_rtf == 0) {
        static const WCHAR k_rtf[] = {'R', 'i', 'c', 'h', ' ', 'T', 'e', 'x', 't', ' ', 'F', 'o', 'r', 'm', 'a', 't', 0};
        g_rtf = RegisterClipboardFormatW(k_rtf);
    }
    return g_rtf;
}

static int is_gdi(UINT format) {
    return format == CF_BITMAP || format == CF_METAFILEPICT || format == 9 /* CF_PALETTE */ ||
           format == CF_ENHMETAFILE || (format >= CF_GDIOBJFIRST && format <= 0x3ff) || format == 0x82 ||
           format == 0x83 || format == 0x8e;
}

static int is_text(UINT format) {
    return format == CF_TEXT || format == CF_OEMTEXT || format == CF_UNICODETEXT;
}

static item *find(UINT format) {
    for (int i = 0; i < g_count; ++i) {
        if (g_items[i].format == format) {
            return &g_items[i];
        }
    }
    return 0;
}

static void free_data(UINT format, HANDLE data) {
    if (data == NULL) {
        return;
    }
    if (is_gdi(format)) {
        DeleteObject(data);
    } else {
        GlobalFree(data);
    }
}

static void drop_items(void) {
    for (int i = 0; i < g_count; ++i) {
        free_data(g_items[i].format, g_items[i].data);
    }
    g_count = 0;
}

static item *add(UINT format, HANDLE data, int made) {
    item *it = find(format);
    if (it != 0) {
        if (it->data != data) {
            free_data(it->format, it->data);
        }
    } else if (g_count < MAX_ITEMS) {
        it = &g_items[g_count++];
    } else {
        return 0;
    }
    it->format = format;
    it->data = data;
    it->made = made;
    return it;
}

static void system_info(u32 *low, u32 *high, u32 *count) {
    u32 info[3] = {0, 0, 0};
    u32 arguments[1] = {(u32)info};
    SxwService(SXW_SVC_CLIPBOARD_INFO, arguments);
    *low = info[0];
    *high = info[1];
    if (count != 0) {
        *count = info[2];
    }
}

/* ---- Memory -------------------------------------------------------------------------------- */

static HGLOBAL alloc_bytes(const void *bytes, u32 length, u32 extra_zeros) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (SIZE_T)(length + extra_zeros));
    unsigned char *p = h != NULL ? GlobalLock(h) : 0;
    if (p == 0) {
        if (h != NULL) {
            GlobalFree(h);
        }
        return NULL;
    }
    for (u32 i = 0; i < length; ++i) {
        p[i] = ((const unsigned char *)bytes)[i];
    }
    for (u32 i = 0; i < extra_zeros; ++i) {
        p[length + i] = 0;
    }
    GlobalUnlock(h);
    return h;
}

/* ---- Text --------------------------------------------------------------------------------------- */

/* UTF-16 up to its NUL, CRLF as LF, as UTF-8 in a heap block. */
static char *wide_to_utf8(const WCHAR *text, u32 units, u32 *length) {
    WCHAR *lf = SxwHeapAllocate((units + 1u) * sizeof(WCHAR), 0);
    if (lf == 0) {
        return 0;
    }
    u32 n = 0;
    for (u32 i = 0; i < units && text[i] != 0; ++i) {
        if (text[i] == '\r' && i + 1 < units && text[i + 1] == '\n') {
            continue;
        }
        lf[n++] = text[i];
    }
    const int bytes = n != 0 ? WideCharToMultiByte(CP_UTF8, 0, lf, (int)n, NULL, 0, NULL, NULL) : 0;
    char *out = SxwHeapAllocate((u32)bytes + 1u, 0);
    if (out != 0 && bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, lf, (int)n, out, bytes, NULL, NULL);
    }
    SxwHeapFree(lf);
    *length = (u32)(bytes > 0 ? bytes : 0);
    return out;
}

/* UTF-8 as UTF-16 with LF as CRLF and a NUL, in a global block. */
static HGLOBAL utf8_to_wide(const char *text, u32 length) {
    const int units = length != 0 ? MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0) : 0;
    WCHAR *wide = SxwHeapAllocate((u32)(units + 1) * sizeof(WCHAR), 0);
    if (wide == 0) {
        return NULL;
    }
    if (units > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text, (int)length, wide, units);
    }
    u32 lines = 0;
    for (int i = 0; i < units; ++i) {
        lines += wide[i] == '\n' && (i == 0 || wide[i - 1] != '\r');
    }
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (SIZE_T)(units + (int)lines + 1) * sizeof(WCHAR));
    WCHAR *p = h != NULL ? GlobalLock(h) : 0;
    if (p != 0) {
        int at = 0;
        for (int i = 0; i < units; ++i) {
            if (wide[i] == '\n' && (i == 0 || wide[i - 1] != '\r')) {
                p[at++] = '\r';
            }
            p[at++] = wide[i];
        }
        p[at] = 0;
        GlobalUnlock(h);
    }
    SxwHeapFree(wide);
    return h;
}

/* One text format made from another, as Windows makes them. */
static HGLOBAL convert_text(HANDLE from, UINT from_format, UINT to_format) {
    const void *src = GlobalLock(from);
    if (src == 0) {
        return NULL;
    }
    const u32 size = (u32)GlobalSize(from);
    HGLOBAL out = NULL;
    if (to_format == CF_UNICODETEXT) {
        const UINT cp = from_format == CF_OEMTEXT ? CP_OEMCP : CP_ACP;
        u32 n = 0;
        while (n < size && ((const char *)src)[n] != 0) {
            ++n;
        }
        const int units = MultiByteToWideChar(cp, 0, src, (int)n, NULL, 0);
        out = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (SIZE_T)(units + 1) * sizeof(WCHAR));
        WCHAR *p = out != NULL ? GlobalLock(out) : 0;
        if (p != 0) {
            MultiByteToWideChar(cp, 0, src, (int)n, p, units);
            p[units] = 0;
            GlobalUnlock(out);
        }
    } else {
        const UINT cp = to_format == CF_OEMTEXT ? CP_OEMCP : CP_ACP;
        const WCHAR *w = src;
        HGLOBAL wide = NULL;
        if (from_format != CF_UNICODETEXT) {
            GlobalUnlock(from);
            wide = convert_text(from, from_format, CF_UNICODETEXT);
            if (wide == NULL) {
                return NULL;
            }
            w = GlobalLock(wide);
        }
        int units = 0;
        const u32 wsize = (u32)GlobalSize(wide != NULL ? wide : from) / 2u;
        while ((u32)units < wsize && w[units] != 0) {
            ++units;
        }
        const int bytes = WideCharToMultiByte(cp, 0, w, units, NULL, 0, NULL, NULL);
        out = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (SIZE_T)bytes + 1);
        char *p = out != NULL ? GlobalLock(out) : 0;
        if (p != 0) {
            WideCharToMultiByte(cp, 0, w, units, p, bytes, NULL, NULL);
            p[bytes] = 0;
            GlobalUnlock(out);
        }
        if (wide != NULL) {
            GlobalUnlock(wide);
            GlobalFree(wide);
            return out;
        }
    }
    GlobalUnlock(from);
    return out;
}

/* ---- File lists ----------------------------------------------------------------------------- */

static int hex(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                                         : -1;
}

/* A DROPFILES block as file:// URIs, one per line with CRLF. */
static char *drop_to_uris(HANDLE drop, u32 *length) {
    const unsigned char *p = GlobalLock(drop);
    if (p == 0) {
        return 0;
    }
    const DROPFILES *d = (const DROPFILES *)p;
    const u32 size = (u32)GlobalSize(drop);
    char *out = SxwHeapAllocate(64u * 1024u, 0);
    u32 at = 0;
    u32 offset = d->pFiles;
    while (out != 0 && offset + 2u <= size) {
        WCHAR path[MAX_PATH];
        int n = 0;
        if (d->fWide) {
            const WCHAR *w = (const WCHAR *)(p + offset);
            while (offset + 2u * (u32)n + 2u <= size && w[n] != 0 && n + 1 < MAX_PATH) {
                path[n] = w[n];
                ++n;
            }
            offset += 2u * (u32)(n + 1);
        } else {
            const char *c = (const char *)(p + offset);
            while (offset + (u32)n < size && c[n] != 0 && n + 1 < MAX_PATH) {
                path[n] = (WCHAR)(unsigned char)c[n];
                ++n;
            }
            offset += (u32)(n + 1);
        }
        if (n == 0) {
            break;
        }
        path[n] = 0;
        char host[1024];
        u32 arguments[3] = {(u32)path, (u32)host, sizeof(host)};
        if (SxwService(SXW_SVC_PATH_TO_HOST, arguments) != 0) {
            continue;
        }
        static const char k_scheme[] = "file://";
        for (u32 i = 0; k_scheme[i] != 0 && at < 60u * 1024u; ++i) {
            out[at++] = k_scheme[i];
        }
        for (u32 i = 0; host[i] != 0 && at < 60u * 1024u; ++i) {
            const unsigned char c = (unsigned char)host[i];
            if (c <= ' ' || c == '%' || c == '#' || c == '?' || c >= 0x7f) {
                static const char k_hex[] = "0123456789ABCDEF";
                out[at++] = '%';
                out[at++] = k_hex[c >> 4];
                out[at++] = k_hex[c & 15u];
            } else {
                out[at++] = (char)c;
            }
        }
        out[at++] = '\r';
        out[at++] = '\n';
    }
    GlobalUnlock(drop);
    *length = at;
    return out;
}

/* text/uri-list as a DROPFILES block of wide Windows paths: the file:// URIs
 * of the list, each through the host's mapping. */
HGLOBAL make_drop(const char *uris, u32 length, POINT point, int nonclient) {
    WCHAR *names = SxwHeapAllocate(64u * 1024u, 0);
    if (names == 0) {
        return NULL;
    }
    u32 at = 0;
    u32 i = 0;
    while (i < length) {
        u32 end = i;
        while (end < length && uris[end] != '\r' && uris[end] != '\n') {
            ++end;
        }
        if (end - i > 7u && uris[i] != '#' && uris[i] == 'f' && uris[i + 1] == 'i' && uris[i + 2] == 'l' &&
            uris[i + 3] == 'e' && uris[i + 4] == ':') {
            u32 k = i + 5;
            if (k + 1 < end && uris[k] == '/' && uris[k + 1] == '/') {
                k += 2;
                while (k < end && uris[k] != '/') {
                    ++k; /* a host name: only the local one */
                }
            }
            char host[1024];
            u32 n = 0;
            for (; k < end && n + 1 < sizeof(host); ++k) {
                if (uris[k] == '%' && k + 2 < end && hex(uris[k + 1]) >= 0 && hex(uris[k + 2]) >= 0) {
                    host[n++] = (char)(hex(uris[k + 1]) * 16 + hex(uris[k + 2]));
                    k += 2;
                } else {
                    host[n++] = uris[k];
                }
            }
            host[n] = 0;
            u32 arguments[3] = {(u32)host, (u32)(names + at), (64u * 1024u / 2u) - at - 2u};
            const u32 units = SxwService(SXW_SVC_PATH_FROM_HOST, arguments);
            if (units != 0) {
                at += units + 1u;
            }
        }
        i = end;
        while (i < length && (uris[i] == '\r' || uris[i] == '\n')) {
            ++i;
        }
    }
    names[at++] = 0;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT | GMEM_DDESHARE, sizeof(DROPFILES) + at * sizeof(WCHAR));
    unsigned char *p = h != NULL ? GlobalLock(h) : 0;
    if (p != 0) {
        DROPFILES *d = (DROPFILES *)p;
        d->pFiles = sizeof(DROPFILES);
        d->pt = point;
        d->fNC = nonclient;
        d->fWide = TRUE;
        WCHAR *q = (WCHAR *)(p + sizeof(DROPFILES));
        for (u32 k = 0; k < at; ++k) {
            q[k] = names[k];
        }
        GlobalUnlock(h);
    }
    SxwHeapFree(names);
    return h;
}

/* ---- The system's set --------------------------------------------------------------------------- */

/* The system's name of a format, or 0 when it does not go there. */
static int system_name(UINT format, char *out) {
    static const char k_text[] = TEXT_NAME;
    static const char k_rtf[] = RTF_NAME;
    static const char k_uri[] = URI_NAME;
    const char *fixed = format == CF_UNICODETEXT ? k_text
                        : format == rtf_format() ? k_rtf
                        : format == CF_HDROP     ? k_uri
                                                 : 0;
    int n = 0;
    if (fixed != 0) {
        while (fixed[n] != 0) {
            out[n] = fixed[n];
            ++n;
        }
        out[n] = 0;
        return 1;
    }
    if (is_gdi(format) || is_text(format) || format == CF_LOCALE) {
        return 0;
    }
    static const char k_prefix[] = PREFIX;
    while (k_prefix[n] != 0) {
        out[n] = k_prefix[n];
        ++n;
    }
    WCHAR name[64];
    if (registered_name(format, name, 64) > 0) {
        for (int i = 0; name[i] != 0 && n + 1 < (int)SXW_CLIPBOARD_NAME; ++i) {
            out[n++] = name[i] >= 0x20 && name[i] < 0x7f ? (char)name[i] : '_';
        }
    } else {
        out[n++] = 'c';
        out[n++] = 'f';
        char digits[8];
        int d = 0;
        u32 v = format;
        do {
            digits[d++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v != 0);
        while (d > 0) {
            out[n++] = digits[--d];
        }
    }
    out[n] = 0;
    return 1;
}

/* The format a system name stands for: the ones above, back. */
static int same(const char *a, const char *b) {
    int i = 0;
    while (a[i] != 0 && a[i] == b[i]) {
        ++i;
    }
    return a[i] == b[i];
}

static UINT format_of_name(const char *name) {
    static const char k_prefix[] = PREFIX;
    if (same(TEXT_NAME, name)) {
        return CF_UNICODETEXT;
    }
    if (same(RTF_NAME, name)) {
        return rtf_format();
    }
    if (same(URI_NAME, name)) {
        return CF_HDROP;
    }
    int n = 0;
    while (k_prefix[n] != 0 && name[n] == k_prefix[n]) {
        ++n;
    }
    WCHAR wide[64];
    int w = 0;
    if (k_prefix[n] == 0) {
        if (name[n] == 'c' && name[n + 1] == 'f' && name[n + 2] >= '0' && name[n + 2] <= '9') {
            u32 v = 0;
            for (int i = n + 2; name[i] >= '0' && name[i] <= '9'; ++i) {
                v = v * 10u + (u32)(name[i] - '0');
            }
            return v;
        }
        for (int i = n; name[i] != 0 && w + 1 < 64; ++i) {
            wide[w++] = (WCHAR)(unsigned char)name[i];
        }
    } else {
        for (int i = 0; name[i] != 0 && w + 1 < 64; ++i) {
            wide[w++] = (WCHAR)(unsigned char)name[i];
        }
    }
    wide[w] = 0;
    return RegisterClipboardFormatW(wide);
}

/* What the program left goes to the system's set. */
static void publish(void) {
    static sxw_clip_entry entries[SXW_CLIPBOARD_FORMATS];
    static char names[SXW_CLIPBOARD_FORMATS][SXW_CLIPBOARD_NAME];
    char *made[SXW_CLIPBOARD_FORMATS];
    u32 count = 0;
    /* Text from whichever text format was set; then the others in order. */
    item *text = find(CF_UNICODETEXT);
    HGLOBAL converted = NULL;
    if (text == 0 || text->data == NULL) {
        item *other = find(CF_TEXT) != 0 ? find(CF_TEXT) : find(CF_OEMTEXT);
        if (other != 0 && other->data != NULL) {
            converted = convert_text(other->data, other->format, CF_UNICODETEXT);
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = -1; i < g_count && count < SXW_CLIPBOARD_FORMATS; ++i) {
            UINT format;
            HANDLE data;
            if (i < 0) {
                if (pass != 0) {
                    continue;
                }
                format = CF_UNICODETEXT;
                data = text != 0 && text->data != NULL ? text->data : converted;
            } else {
                format = g_items[i].format;
                data = g_items[i].data;
                if (format == CF_UNICODETEXT || g_items[i].made || data == NULL) {
                    continue;
                }
                const int first = format == rtf_format() || format == CF_HDROP;
                if ((pass == 0) != first) {
                    continue;
                }
            }
            if (data == NULL || !system_name(format, names[count])) {
                continue;
            }
            u32 length = 0;
            char *bytes = 0;
            if (format == CF_UNICODETEXT) {
                const WCHAR *w = GlobalLock(data);
                bytes = w != 0 ? wide_to_utf8(w, (u32)GlobalSize(data) / 2u, &length) : 0;
                GlobalUnlock(data);
            } else if (format == CF_HDROP) {
                bytes = drop_to_uris(data, &length);
            } else {
                const unsigned char *p = GlobalLock(data);
                length = (u32)GlobalSize(data);
                if (format == rtf_format()) {
                    /* RTF is text: it ends at its NUL. */
                    u32 n = 0;
                    while (n < length && p[n] != 0) {
                        ++n;
                    }
                    length = n;
                }
                bytes = SxwHeapAllocate(length + 1u, 0);
                for (u32 k = 0; bytes != 0 && k < length; ++k) {
                    bytes[k] = (char)p[k];
                }
                GlobalUnlock(data);
            }
            if (bytes == 0) {
                continue;
            }
            made[count] = bytes;
            entries[count].name = (u32)names[count];
            entries[count].data = (u32)bytes;
            entries[count].length = length;
            ++count;
        }
    }
    u32 arguments[2] = {(u32)entries, count};
    SxwService(SXW_SVC_CLIPBOARD_SET, arguments);
    for (u32 i = 0; i < count; ++i) {
        SxwHeapFree(made[i]);
    }
    if (converted != NULL) {
        GlobalFree(converted);
    }
    system_info(&g_seen_low, &g_seen_high, 0);
    g_have_seen = 1;
}

/* The items, from the system's set when someone else changed it since. */
static void refresh(void) {
    u32 low;
    u32 high;
    u32 count;
    system_info(&low, &high, &count);
    if (g_have_seen && low == g_seen_low && high == g_seen_high) {
        return;
    }
    drop_items();
    g_ours = 0;
    g_owner = NULL;
    g_seen_low = low;
    g_seen_high = high;
    g_have_seen = 1;
    for (u32 i = 0; i < count; ++i) {
        char name[SXW_CLIPBOARD_NAME];
        u32 length = 0;
        u32 arguments[3] = {i, (u32)name, (u32)&length};
        if (SxwService(SXW_SVC_CLIPBOARD_FORMAT, arguments) != 0) {
            break;
        }
        const UINT format = format_of_name(name);
        if (format != 0 && find(format) == 0) {
            add(format, NULL, 1);
        }
    }
}

/* A format read from the set into a handle of its own kind. */
static HANDLE fetch(UINT format) {
    char name[SXW_CLIPBOARD_NAME];
    if (!system_name(format, name)) {
        return NULL;
    }
    u32 length = 0;
    u32 probe[4] = {(u32)name, 0, 0, (u32)&length};
    if (SxwService(SXW_SVC_CLIPBOARD_GET, probe) == SXW_FAILED) {
        return NULL;
    }
    char *bytes = SxwHeapAllocate(length + 1u, 0);
    if (bytes == 0) {
        return NULL;
    }
    u32 got = length;
    u32 arguments[4] = {(u32)name, (u32)bytes, length, (u32)&got};
    if (length != 0 && SxwService(SXW_SVC_CLIPBOARD_GET, arguments) == SXW_FAILED) {
        SxwHeapFree(bytes);
        return NULL;
    }
    HANDLE h;
    if (format == CF_UNICODETEXT) {
        h = utf8_to_wide(bytes, length);
    } else if (format == CF_HDROP) {
        POINT origin = {0, 0};
        h = make_drop(bytes, length, origin, FALSE);
    } else {
        h = alloc_bytes(bytes, length, format == rtf_format() ? 1u : 0u);
    }
    SxwHeapFree(bytes);
    return h;
}

/* ---- What is there ---------------------------------------------------------------------------------- */

/* The formats there are, in Windows' order: the ones set, then the ones
 * made from them, CF_LOCALE and the other text formats by number. Windows
 * XP makes them when the clipboard closes, so a program that changed it
 * sees only what it set until then. */
static int available(UINT *out, int capacity) {
    int n = 0;
    int text = 0;
    for (int i = 0; i < g_count && n < capacity; ++i) {
        /* One made here from the text keeps its place among the made. */
        if (g_items[i].made && g_ours && (is_text(g_items[i].format) || g_items[i].format == CF_LOCALE)) {
            text = 1;
            continue;
        }
        out[n++] = g_items[i].format;
        text |= is_text(g_items[i].format);
    }
    if (text && !(g_open && g_changed)) {
        static const UINT k_made[4] = {CF_LOCALE, CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT};
        for (int k = 0; k < 4 && n < capacity; ++k) {
            int listed = 0;
            for (int i = 0; i < n; ++i) {
                listed |= out[i] == k_made[k];
            }
            if (!listed) {
                out[n++] = k_made[k];
            }
        }
    }
    return n;
}

static int has(UINT format) {
    UINT list[MAX_ITEMS + 4];
    const int n = available(list, MAX_ITEMS + 4);
    for (int i = 0; i < n; ++i) {
        if (list[i] == format) {
            return 1;
        }
    }
    return 0;
}

/* ---- The calls ---------------------------------------------------------------------------------------- */

BOOL WINAPI OpenClipboard(HWND hWndNewOwner) {
    if (hWndNewOwner != NULL && window_of(hWndNewOwner) == 0) {
        set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    if (g_open) {
        if (g_open_window == hWndNewOwner) {
            return TRUE;
        }
        /* Windows XP refuses without an error code (fixtures/clip). */
        return FALSE;
    }
    g_open = 1;
    g_open_window = hWndNewOwner;
    g_changed = 0;
    refresh();
    return TRUE;
}

BOOL WINAPI CloseClipboard(void) {
    if (!g_open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        return FALSE;
    }
    if (g_changed) {
        /* Formats promised without data are rendered now. */
        for (int i = 0; i < g_count; ++i) {
            if (g_items[i].data == NULL && !g_items[i].made && g_owner != NULL && window_of(g_owner) != 0) {
                send_handle(g_owner, WM_RENDERFORMAT, g_items[i].format, 0);
            }
        }
        /* The formats made from the text count as set; the system's set
         * changing counts once of its own. */
        g_changed = 0;
        UINT list[MAX_ITEMS + 4];
        g_sequence_bias += (u32)(available(list, MAX_ITEMS + 4) - g_count);
        publish();
        g_sequence_bias -= 1u;
    }
    g_open = 0;
    g_open_window = NULL;
    g_changed = 0;
    return TRUE;
}

BOOL WINAPI EmptyClipboard(void) {
    if (!g_open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        return FALSE;
    }
    if (g_ours && g_owner != NULL && window_of(g_owner) != 0) {
        send_handle(g_owner, WM_DESTROYCLIPBOARD, 0, 0);
    }
    drop_items();
    g_owner = g_open_window;
    g_ours = 1;
    g_changed = 1;
    ++g_sequence_bias;
    return TRUE;
}

HANDLE WINAPI SetClipboardData(UINT uFormat, HANDLE hMem) {
    if (!g_open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        return NULL;
    }
    if (uFormat == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!g_ours) {
        /* Setting without emptying first adds to what is there. */
        for (int i = 0; i < g_count; ++i) {
            if (g_items[i].data == NULL) {
                g_items[i].data = fetch(g_items[i].format);
            }
            g_items[i].made = 0;
        }
        g_ours = 1;
    }
    /* A text format set replaces the ones made from the old text. */
    if (is_text(uFormat)) {
        for (int i = 0; i < g_count; ++i) {
            if (g_items[i].made && (is_text(g_items[i].format) || g_items[i].format == CF_LOCALE)) {
                free_data(g_items[i].format, g_items[i].data);
                g_items[i] = g_items[--g_count];
                --i;
            }
        }
    }
    if (add(uFormat, hMem, 0) == 0) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    g_changed = 1;
    ++g_sequence_bias;
    return hMem;
}

HANDLE WINAPI GetClipboardData(UINT uFormat) {
    if (!g_open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        return NULL;
    }
    item *it = find(uFormat);
    if (it != 0 && it->data == NULL && it->made) {
        it->data = fetch(uFormat);
    }
    if (it != 0 && it->data != NULL) {
        return it->data;
    }
    if (!has(uFormat)) {
        return NULL;
    }
    /* A format Windows makes from another. */
    HANDLE made = NULL;
    if (uFormat == CF_LOCALE) {
        const LCID lcid = GetUserDefaultLCID();
        made = alloc_bytes(&lcid, sizeof(lcid), 0);
    } else if (is_text(uFormat)) {
        static const UINT k_sources[3] = {CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT};
        for (int k = 0; k < 3 && made == NULL; ++k) {
            item *source = find(k_sources[k]);
            if (source == 0 || source->format == uFormat) {
                continue;
            }
            if (source->data == NULL && source->made) {
                source->data = fetch(source->format);
            }
            if (source->data != NULL) {
                made = convert_text(source->data, source->format, uFormat);
            }
        }
    }
    if (made != NULL) {
        add(uFormat, made, 1);
    }
    return made;
}

BOOL WINAPI IsClipboardFormatAvailable(UINT format) {
    if (!g_open) {
        refresh();
    }
    return has(format);
}

UINT WINAPI EnumClipboardFormats(UINT format) {
    if (!g_open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    UINT list[MAX_ITEMS + 4];
    const int n = available(list, MAX_ITEMS + 4);
    set_last_error(0);
    if (format == 0) {
        return n > 0 ? list[0] : 0;
    }
    for (int i = 0; i + 1 < n; ++i) {
        if (list[i] == format) {
            return list[i + 1];
        }
    }
    return 0;
}

int WINAPI CountClipboardFormats(void) {
    if (!g_open) {
        refresh();
    }
    UINT list[MAX_ITEMS + 4];
    return available(list, MAX_ITEMS + 4);
}

int WINAPI GetClipboardFormatNameW(UINT format, LPWSTR lpszFormatName, int cchMaxCount) {
    if (lpszFormatName == NULL || cchMaxCount <= 0) {
        return 0;
    }
    const int n = registered_name(format, lpszFormatName, cchMaxCount);
    if (n == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
    }
    return n;
}

DWORD WINAPI GetClipboardSequenceNumber(void) {
    u32 low;
    u32 high;
    system_info(&low, &high, 0);
    return low + g_sequence_bias;
}

HWND WINAPI GetClipboardOwner(void) {
    if (!g_open) {
        refresh();
    }
    return g_ours && g_owner != NULL && window_of(g_owner) != 0 ? g_owner : NULL;
}

HWND WINAPI GetOpenClipboardWindow(void) {
    return g_open ? g_open_window : NULL;
}

/* The program's windows going away: the clipboard it set stays with the
 * system; it only has no owner here any more. */
void clipboard_window_gone(window *w) {
    if (g_owner == handle_of(w)) {
        if (g_ours) {
            /* What was promised and never rendered is asked for now. */
            send(w, WM_RENDERALLFORMATS, 0, 0);
        }
        g_owner = NULL;
    }
    if (g_open && g_open_window == handle_of(w)) {
        g_open_window = NULL;
    }
}
