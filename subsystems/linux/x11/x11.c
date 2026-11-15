/* X11 protocol encoding follows the X Consortium core protocol, X11R7.7.
 * Only the L5 inventory is implemented; other requests return BadRequest.
 * Malformed requests never reach a host callback or a drawing primitive. */
#include "x11.h"
#include <string.h>

static uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const unsigned char* p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static void w16(unsigned char* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(unsigned char* p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static size_t padded(size_t n) { return (n + 3) & ~(size_t)3; }
static unsigned bits(uint32_t value) { return __builtin_popcount(value); }
static int send_bytes(struct sx11_client* c, const void* p, size_t n) {
    if (!c->ops.send || c->ops.send(c->host, p, n)) { c->failed = 1; return -1; }
    return 0;
}
static void reply(struct sx11_client* c, unsigned char r[32]) {
    r[0] = 1; w16(r + 2, c->sequence); send_bytes(c, r, 32);
}
static int error(struct sx11_client* c, uint8_t code, uint32_t value, uint8_t op) {
    unsigned char r[32] = {0};
    r[1] = code; w16(r + 2, c->sequence); w32(r + 4, value); r[10] = op;
    return send_bytes(c, r, sizeof(r));
}
static struct sx11_window* window(struct sx11_client* c, uint32_t id) {
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (id && c->windows[i].id == id) return &c->windows[i];
    return 0;
}
static struct sx11_gc* gc(struct sx11_client* c, uint32_t id) {
    for (unsigned i = 0; i < SX11_MAX_GCS; ++i) if (id && c->gcs[i].id == id) return &c->gcs[i];
    return 0;
}
static int valid_id(struct sx11_client* c, uint32_t id) {
    return (id & ~0x1fffffu) == 0x200000u && !window(c, id) && !gc(c, id);
}
static uint32_t atom(struct sx11_client* c, const unsigned char* name, size_t length, int create) {
    static const struct { const char* name; uint32_t id; } predefined[] = {
        {"ATOM", 4}, {"STRING", 31}, {"WM_NAME", 39}, {"WM_ICON_NAME", 37},
        {"RESOURCE_MANAGER", 23}, {"WM_CLASS", 67}, {"WM_NORMAL_HINTS", 40}, {"WM_HINTS", 35}
    };
    for (unsigned i = 0; i < sizeof(predefined) / sizeof(predefined[0]); ++i)
        if (strlen(predefined[i].name) == length && !memcmp(name, predefined[i].name, length)) return predefined[i].id;
    for (unsigned i = 0; i < SX11_MAX_ATOMS; ++i)
        if (c->atoms[i].id && strlen(c->atoms[i].name) == length && !memcmp(name, c->atoms[i].name, length)) return c->atoms[i].id;
    if (create && length < sizeof(c->atoms[0].name)) for (unsigned i = 0; i < SX11_MAX_ATOMS; ++i) if (!c->atoms[i].id) {
        memcpy(c->atoms[i].name, name, length); c->atoms[i].name[length] = 0;
        return c->atoms[i].id = 69 + i;
    }
    return 0;
}
static void event(struct sx11_client* c, unsigned char r[32], uint8_t type) {
    r[0] = type; w16(r + 2, c->sequence); send_bytes(c, r, 32);
}
static void expose(struct sx11_client* c, struct sx11_window* w) {
    if (!(w->event_mask & (1u << 15))) return;
    unsigned char r[32] = {0}; w32(r + 4, w->id); w16(r + 12, w->width); w16(r + 14, w->height);
    event(c, r, 12);
}
static int setup(struct sx11_client* c) {
    const unsigned char* p = c->input;
    if (p[0] != 'l' || u16(p + 2) != 11 || u16(p + 4) || u16(p + 6) || u16(p + 8)) return -1;
    unsigned char r[128] = {0};
    r[0] = 1; w16(r + 2, 11); w16(r + 6, (sizeof(r) - 8) / 4);
    w32(r + 8, 1); w32(r + 12, 0x200000); w32(r + 16, 0x1fffff);
    w16(r + 24, 8); w16(r + 26, SX11_MAX_REQUEST / 4);
    r[28] = 1; r[29] = 1; r[32] = 32; r[33] = 32; r[34] = 8; r[35] = 255;
    memcpy(r + 40, "SavanXP ", 8);
    r[48] = 24; r[49] = 32; r[50] = 32;
    w32(r + 56, SX11_ROOT); w32(r + 60, 0x101); w32(r + 64, 0xffffff);
    w16(r + 76, c->screen_width); w16(r + 78, c->screen_height);
    w16(r + 80, c->screen_width / 4); w16(r + 82, c->screen_height / 4);
    w16(r + 84, 1); w16(r + 86, 1); w32(r + 88, 0x102); r[94] = 24; r[95] = 1;
    r[96] = 24; w16(r + 98, 1); w32(r + 104, 0x102); r[108] = 4; r[109] = 8; w16(r + 110, 256);
    w32(r + 112, 0xff0000); w32(r + 116, 0xff00); w32(r + 120, 0xff);
    c->setup = 1;
    return send_bytes(c, r, sizeof(r));
}
static void clear(struct sx11_window* w) {
    struct sx_painter painter; sx_painter_init(&painter, &w->bitmap);
    sx_painter_fill_rect(&painter, sx_rect_make(0, 0, w->width, w->height), w->background);
}
// X11 includes the left edge of a filled ellipse and excludes its right
// edge on exact boundary ties (except the single top/bottom pixel). The
// platform ellipse primitive uses a different pixel-center convention, so
// generate the X11 spans here and let the platform painter clip and fill.
static void fill_arc(struct sx_painter* painter, struct sx_rect rect, uint32_t colour) {
    if (!rect.width || !rect.height) return;
    const int first_y = rect.y < 0 ? 0 : rect.y;
    const int end_y = rect.y + rect.height < (int)painter->target->info.height ?
        rect.y + rect.height : (int)painter->target->info.height;
    const int first_x = rect.x < 0 ? 0 : rect.x;
    const int end_x = rect.x + rect.width < (int)painter->target->info.width ?
        rect.x + rect.width : (int)painter->target->info.width;
    const uint64_t a = (uint64_t)rect.width * rect.width, b = (uint64_t)rect.height * rect.height;
    const unsigned __int128 limit = (unsigned __int128)a * b;
    for (int y = first_y; y < end_y; ++y) {
        int left = -1, right = -1;
        const int64_t dy = 2ll * (y - rect.y) - rect.height;
        for (int x = first_x; x < end_x; ++x) {
            const int64_t dx = 2ll * (x - rect.x) - rect.width;
            const unsigned __int128 value = (unsigned __int128)(dx * dx) * b + (unsigned __int128)(dy * dy) * a;
            if (value < limit || (value == limit && dx <= 0)) {
                if (left < 0) left = x;
                right = x;
            }
        }
        if (left >= 0) sx_painter_fill_rect(painter, sx_rect_make(left, y, right - left + 1, 1), colour);
    }
}

static void destroy_window(struct sx11_client* c, struct sx11_window* w) {
    if (!w) return;
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i)
        if (c->windows[i].id && c->windows[i].parent == w->id) destroy_window(c, &c->windows[i]);
    if (w->mapped && c->ops.destroy) c->ops.destroy(c->host, w);
    if (w->event_mask & (1u << 17)) {
        unsigned char r[32] = {0}; w32(r + 4, w->id); w32(r + 8, w->id);
        if (w->mapped) event(c, r, 18);
        event(c, r, 17);
    }
    sx_bitmap_destroy(&w->bitmap); memset(w, 0, sizeof(*w));
}
static int request(struct sx11_client* c, const unsigned char* p, size_t n) {
    const uint8_t op = p[0]; const uint32_t id = n >= 8 ? u32(p + 4) : 0;
    unsigned char r[32] = {0}; struct sx11_window* w = window(c, id);
#define LENGTH(x) do { if (n != (x)) return error(c, 16, 0, op); } while (0)
#define MINIMUM(x) do { if (n < (x)) return error(c, 16, 0, op); } while (0)
#define WINDOW() do { if (!w) return error(c, 3, id, op); } while (0)
    switch (op) {
    case 98: // QueryExtension: every extension is absent.
        MINIMUM(8); LENGTH(8 + padded(u16(p + 4))); reply(c, r); break;
    case 16: { // InternAtom
        MINIMUM(8); const size_t len = u16(p + 4); LENGTH(8 + padded(len));
        if (p[1] > 1 || !len) return error(c, 2, p[1], op);
        const uint32_t a = atom(c, p + 8, len, !p[1]);
        if (!a && !p[1]) return error(c, 11, 0, op);
        w32(r + 8, a); reply(c, r); break;
    }
    case 20: // GetProperty: only absent root RESOURCE_MANAGER is in L5.
        LENGTH(24);
        if (id != SX11_ROOT && !w) return error(c, 3, id, op);
        if (u32(p + 8) != 23 || id != SX11_ROOT) return error(c, 17, u32(p + 8), op);
        reply(c, r); break;
    case 1: { // CreateWindow
        MINIMUM(32); const uint32_t mask = u32(p + 28); LENGTH(32 + 4 * bits(mask));
        if (!valid_id(c, id)) return error(c, 14, id, op);
        if (u32(p + 8) != SX11_ROOT && !window(c, u32(p + 8))) return error(c, 3, u32(p + 8), op);
        if ((p[1] && p[1] != 24) || (u16(p + 22) && u16(p + 22) != 1) ||
            (u32(p + 24) && u32(p + 24) != 0x102)) return error(c, 8, 0, op);
        if (!u16(p + 16) || !u16(p + 18) || u16(p + 16) > 4096 || u16(p + 18) > 4096 || u16(p + 20))
            return error(c, 2, 0, op);
        if (mask & ~((1u << 1) | (1u << 3) | (1u << 11))) return error(c, 2, mask, op);
        for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (!c->windows[i].id) { w = &c->windows[i]; break; }
        if (!w) return error(c, 11, 0, op);
        w->id = id; w->parent = u32(p + 8); w->x = u16(p + 12); w->y = u16(p + 14);
        w->width = u16(p + 16); w->height = u16(p + 18);
        unsigned offset = 32;
        for (unsigned bit = 0; bit < 32; ++bit) if (mask & (1u << bit)) {
            if (bit == 1) w->background = u32(p + offset);
            if (bit == 11) w->event_mask = u32(p + offset);
            offset += 4;
        }
        if (!sx_bitmap_create(&w->bitmap, w->width, w->height, SX_PIXEL_FORMAT_BGRA8888)) {
            memset(w, 0, sizeof(*w)); return error(c, 11, 0, op);
        }
        clear(w); break;
    }
    case 2: { // ChangeWindowAttributes
        MINIMUM(12); WINDOW(); const uint32_t mask = u32(p + 8); LENGTH(12 + 4 * bits(mask));
        if (mask & ~(1u << 11)) return error(c, 2, mask, op);
        if (mask) w->event_mask = u32(p + 12);
        break;
    }
    case 18: { // ChangeProperty: title and WM_PROTOCOLS drive the host window.
        MINIMUM(24); WINDOW();
        const unsigned format = p[16];
        if (format != 8 && format != 16 && format != 32) return error(c, 2, format, op);
        const uint64_t bytes = (uint64_t)u32(p + 20) * (format / 8);
        if (bytes > SX11_MAX_REQUEST - 24) return error(c, 16, 0, op);
        LENGTH(24 + padded(bytes));
        if (p[1]) return error(c, 2, p[1], op); // Replace only in the profile.
        const uint32_t property = u32(p + 8);
        if (property == 39 && format == 8 && u32(p + 12) == 31) {
            size_t len = bytes < sizeof(w->title) - 1 ? bytes : sizeof(w->title) - 1;
            memcpy(w->title, p + 24, len); w->title[len] = 0;
        } else if (property == atom(c, (const unsigned char*)"WM_PROTOCOLS", 12, 0) &&
            format == 32 && u32(p + 12) == 4) {
            w->delete_protocol = 0;
            const uint32_t deletion = atom(c, (const unsigned char*)"WM_DELETE_WINDOW", 16, 0);
            for (size_t i = 0; i < bytes; i += 4) if (deletion && u32(p + 24 + i) == deletion) w->delete_protocol = 1;
        } else return error(c, 17, property, op);
        if (w->event_mask & (1u << 22)) {
            w32(r + 4, id); w32(r + 8, property); event(c, r, 28);
        }
        break;
    }
    case 55: { // CreateGC
        MINIMUM(16); const uint32_t mask = u32(p + 12); LENGTH(16 + 4 * bits(mask));
        if (!valid_id(c, id)) return error(c, 14, id, op);
        if (u32(p + 8) != SX11_ROOT && !window(c, u32(p + 8))) return error(c, 9, u32(p + 8), op);
        if (mask & ~((1u << 2) | (1u << 3) | (1u << 16))) return error(c, 2, mask, op);
        struct sx11_gc* g = 0;
        for (unsigned i = 0; i < SX11_MAX_GCS; ++i) if (!c->gcs[i].id) { g = &c->gcs[i]; break; }
        if (!g) return error(c, 11, 0, op);
        g->id = id; g->background = 1;
        unsigned offset = 16;
        for (unsigned bit = 0; bit < 32; ++bit) if (mask & (1u << bit)) {
            if (bit == 2) g->foreground = u32(p + offset);
            if (bit == 3) g->background = u32(p + offset);
            offset += 4;
        }
        break;
    }
    case 60: { LENGTH(8); struct sx11_gc* g = gc(c, id); if (!g) return error(c, 13, id, op); memset(g, 0, sizeof(*g)); break; }
    case 8: // MapWindow
        LENGTH(8); WINDOW(); if (w->mapped) break;
        if (c->ops.map && c->ops.map(c->host, w)) return error(c, 11, 0, op);
        w->mapped = 1;
        if (w->event_mask & (1u << 17)) { w32(r + 4, id); w32(r + 8, id); event(c, r, 19); }
        expose(c, w); if (c->ops.present) c->ops.present(c->host, w); break;
    case 4: LENGTH(8); WINDOW(); destroy_window(c, w); break;
    case 70: case 71: { // Filled rectangles and full ellipses.
        MINIMUM(12); WINDOW(); struct sx11_gc* g = gc(c, u32(p + 8));
        if (!g) return error(c, 13, u32(p + 8), op);
        const size_t stride = op == 70 ? 8 : 12;
        if ((n - 12) % stride) return error(c, 16, 0, op);
        if (op == 71) for (size_t i = 12; i < n; i += stride)
            if (u16(p + i + 8) || (int16_t)u16(p + i + 10) < 360 * 64) return error(c, 2, 0, op);
        struct sx_painter painter; sx_painter_init(&painter, &w->bitmap);
        for (size_t i = 12; i < n; i += stride) {
            struct sx_rect rect = sx_rect_make((int16_t)u16(p + i), (int16_t)u16(p + i + 2), u16(p + i + 4), u16(p + i + 6));
            if (op == 70) sx_painter_fill_rect(&painter, rect, g->foreground);
            else fill_arc(&painter, rect, g->foreground);
        }
        if (c->ops.present) c->ops.present(c->host, w); break;
    }
    case 38: { // QueryPointer
        LENGTH(8); if (id != SX11_ROOT && !w) return error(c, 3, id, op);
        int rx = 0, ry = 0, wx = 0, wy = 0; unsigned mask = 0;
        if (c->ops.pointer && c->ops.pointer(c->host, w, &rx, &ry, &wx, &wy, &mask)) { c->failed = 1; return -1; }
        r[1] = 1; w32(r + 8, SX11_ROOT); w16(r + 16, rx); w16(r + 18, ry);
        w16(r + 20, wx); w16(r + 22, wy); w16(r + 24, mask); reply(c, r); break;
    }
    case 43: LENGTH(4); r[1] = 0; w32(r + 8, SX11_ROOT); reply(c, r); break;
    case 127: break; // NoOperation is defined to have arbitrary padding.
    default: return error(c, 1, 0, op);
    }
    return c->failed ? -1 : 0;
#undef LENGTH
#undef MINIMUM
#undef WINDOW
}

void sx11_init(struct sx11_client* c, const struct sx11_ops* ops, void* host, uint16_t width, uint16_t height) {
    memset(c, 0, sizeof(*c)); c->ops = *ops; c->host = host; c->screen_width = width; c->screen_height = height;
}
int sx11_feed(struct sx11_client* c, const void* data, size_t count) {
    const unsigned char* p = data;
    if (c->failed) return -1;
    for (;;) {
        const size_t header = c->setup ? 4 : 12;
        if (c->used < header) {
            if (!count) break;
            size_t n = header - c->used; if (n > count) n = count;
            memcpy(c->input + c->used, p, n); c->used += n; p += n; count -= n;
            if (c->used < header) break;
        }
        const size_t needed = c->setup ? (size_t)u16(c->input + 2) * 4 :
            12 + padded(u16(c->input + 6)) + padded(u16(c->input + 8));
        if (needed < header || needed > SX11_MAX_REQUEST) { c->failed = 1; return -1; }
        if (c->used < needed) {
            size_t n = needed - c->used; if (n > count) n = count;
            if (n) { memcpy(c->input + c->used, p, n); c->used += n; p += n; count -= n; }
            if (c->used < needed) break;
        }
        if (c->setup) { ++c->sequence; if (request(c, c->input, needed)) return -1; }
        else if (setup(c)) { c->failed = 1; return -1; }
        c->used = 0;
    }
    return c->failed ? -1 : 0;
}
int sx11_finish(struct sx11_client* c) { return c->failed || c->used || !c->setup ? -1 : 0; }
void sx11_destroy(struct sx11_client* c) {
    // Teardown cannot send events on an already closed transport.
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) c->windows[i].event_mask = 0;
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (c->windows[i].id) destroy_window(c, &c->windows[i]);
}
int sx11_resize(struct sx11_client* c, uint32_t id, uint16_t width, uint16_t height) {
    struct sx11_window* w = window(c, id);
    if (!w || !width || !height || width > 4096 || height > 4096) return -1;
    if (width == w->width && height == w->height) return 0;
    struct sx_bitmap replacement;
    if (!sx_bitmap_create(&replacement, width, height, SX_PIXEL_FORMAT_BGRA8888)) return -1;
    sx_bitmap_destroy(&w->bitmap); w->bitmap = replacement; w->width = width; w->height = height; clear(w);
    if (w->event_mask & (1u << 17)) {
        unsigned char r[32] = {0}; w32(r + 4, id); w32(r + 8, id); w16(r + 20, width); w16(r + 22, height);
        event(c, r, 22);
    }
    expose(c, w); return c->failed ? -1 : 0;
}
int sx11_close_window(struct sx11_client* c, uint32_t id) {
    struct sx11_window* w = window(c, id);
    if (!w || w->close_sent) return 0;
    if (!w->delete_protocol) return -1;
    unsigned char r[32] = {0}; r[1] = 32; w32(r + 4, id);
    w32(r + 8, atom(c, (const unsigned char*)"WM_PROTOCOLS", 12, 0));
    w32(r + 12, atom(c, (const unsigned char*)"WM_DELETE_WINDOW", 16, 0));
    event(c, r, 33); w->close_sent = 1; return c->failed ? -1 : 0;
}
