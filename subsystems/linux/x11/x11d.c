/* SUN adapter for the portable X11 engine. The service accepts named-port
 * connections and runs one SUN worker per X client: each worker owns its
 * windowd connection and all mappings disappear with it. No Linux or POSIX
 * runtime API is used by this server. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sxn/sxn.h"
#include "sxn/sxnwm.h"
#include "x11.h"

struct desktop {
    struct sxnwm_client wm;
    struct sx11_client protocol;
    uint32_t mapped;
    char title[128];
    int dirty;
};
static int send_reply(void* opaque, const void* bytes, size_t count) {
    (void)opaque;
    const unsigned char* p = bytes;
    while (count) { long n = sxn_write(1, p, count); if (n <= 0) return -1; p += n; count -= n; }
    return 0;
}
static int map_window(void* opaque, struct sx11_window* w) {
    struct desktop* d = opaque;
    // The first profile has one top-level window per X client. Refuse a
    // second rather than aliasing it onto an existing desktop surface.
    if (d->mapped || w->parent != SX11_ROOT) return -1;
    struct sxnwm_presentation_source presentation = {0}; presentation.title = w->title;
    if (sxnwm_set_presentation(&d->wm, &presentation) || sxnwm_request_size(&d->wm, w->width, w->height)) return -1;
    const uint64_t deadline = sxn_clock_ns() + 3000000000ull;
    while (d->wm.width != w->width || d->wm.height != w->height) {
        (void)sxn_event_reset(d->wm.wake_fd);
        (void)sxnwm_check_resize(&d->wm);
        if (d->wm.width == w->width && d->wm.height == w->height) break;
        if (sxn_clock_ns() >= deadline || sxnwm_close_requested(&d->wm)) return -1;
        (void)sxn_wait_one(d->wm.wake_fd, 10);
    }
    strcpy(d->title, w->title);
    d->mapped = w->id; w->host = d; d->dirty = 1; return 0;
}
static void destroy_window(void* opaque, struct sx11_window* w) {
    struct desktop* d = opaque; if (d->mapped == w->id) { d->mapped = 0; d->dirty = 0; }
}
static void present(void* opaque, struct sx11_window* w) {
    struct desktop* d = opaque;
    if (d->mapped == w->id) {
        if (strcmp(d->title, w->title)) {
            struct sxnwm_presentation_source presentation = {0}; presentation.title = w->title;
            if (sxnwm_set_presentation(&d->wm, &presentation)) d->protocol.failed = 1;
            strcpy(d->title, w->title);
        }
        d->dirty = 1;
    }
}
static int pointer(void* opaque, struct sx11_window* w, int* rx, int* ry, int* wx, int* wy, unsigned* mask) {
    struct desktop* d = opaque;
    struct savanxp_wm_desktop_pointer p;
    long result;
    for (unsigned i = 0; i < 8; ++i) { result = sxnwm_query_pointer(&d->wm, &p); if (!result) break; (void)sxn_wait_one(d->wm.wake_fd, 1); }
    if (result) return -1;
    *rx = p.x; *ry = p.y;
    *wx = w ? p.x - p.origin_x : p.x; *wy = w ? p.y - p.origin_y : p.y;
    *mask = (p.buttons & SAVANXP_MOUSE_BUTTON_LEFT ? 1u << 8 : 0) |
        (p.buttons & SAVANXP_MOUSE_BUTTON_MIDDLE ? 1u << 9 : 0) |
        (p.buttons & SAVANXP_MOUSE_BUTTON_RIGHT ? 1u << 10 : 0);
    return 0;
}
static void paint(struct desktop* d) {
    if (!d->dirty || !d->mapped || !sxnwm_idle(&d->wm)) return;
    struct sx11_window* w = 0;
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (d->protocol.windows[i].id == d->mapped) { w = &d->protocol.windows[i]; break; }
    if (!w) return;
    const unsigned width = w->width < d->wm.width ? w->width : d->wm.width;
    const unsigned height = w->height < d->wm.height ? w->height : d->wm.height;
    for (unsigned y = 0; y < height; ++y)
        memcpy(d->wm.pixels + y * d->wm.stride, w->bitmap.pixels + y * (w->bitmap.info.pitch / 4), width * 4);
    long sequence = sxnwm_submit_full(&d->wm);
    if (sequence > 0) d->dirty = 0;
}
static int worker(void) {
    unsigned char bytes[4096];
    // A probe that closes without an X setup never gets a window.
    const int32_t input = 0;
    if (sxn_wait_many(&input, 1, 5000) < 0) return 0;
    long n = sxn_read(0, bytes, sizeof(bytes)); if (n <= 0) return 0;
    // A SUN launcher holds a lease while loading/running its Linux client.
    // 'L' is not an X11 byte-order marker. A confirmed lease prevents an
    // idle listener from disappearing between readiness and XOpenDisplay.
    if (n == 1 && bytes[0] == 'L') {
        const unsigned char ready = 1;
        if (sxn_write(1, &ready, 1) != 1) return 1;
        while ((n = sxn_read(0, bytes, sizeof(bytes))) > 0) { }
        return n ? 1 : 0;
    }
    struct desktop* d = calloc(1, sizeof(*d)); if (!d) return 1;
    if (sxnwm_connect(&d->wm)) { free(d); return 1; }
    struct savanxp_wm_desktop_pointer snapshot;
    const uint64_t deadline = sxn_clock_ns() + 2000000000ull;
    while (sxnwm_query_pointer(&d->wm, &snapshot)) {
        if (sxn_clock_ns() >= deadline) { free(d); return 1; }
        (void)sxn_wait_one(d->wm.wake_fd, 1);
    }
    const struct sx11_ops ops = {send_reply, map_window, destroy_window, present, pointer};
    sx11_init(&d->protocol, &ops, d, snapshot.width, snapshot.height);
    int result = sx11_feed(&d->protocol, bytes, n);
    (void)sxn_set_nonblocking(0, 1);
    (void)sxn_set_nonblocking(d->wm.events_fd, 1);
    while (!result) {
        (void)sxn_event_reset(d->wm.wake_fd);
        struct savanxp_wm_event connection_event;
        if (sxn_read(d->wm.events_fd, &connection_event, sizeof(connection_event)) == 0) { result = -1; break; }
        if (d->mapped && sxnwm_check_resize(&d->wm)) {
            if (sx11_resize(&d->protocol, d->mapped, d->wm.width, d->wm.height)) break;
            d->dirty = 1;
        }
        if (d->mapped && sxnwm_close_requested(&d->wm) && sx11_close_window(&d->protocol, d->mapped)) break;
        struct savanxp_wm_event event;
        while (sxnwm_poll_input(&d->wm, &event) == 0) { /* Xclient polls the pointer. */ }
        while ((n = sxn_read(0, bytes, sizeof(bytes))) > 0)
            if ((result = sx11_feed(&d->protocol, bytes, n)) != 0) break;
        if (!n) { result = sx11_finish(&d->protocol); break; }
        if (n < 0 && n != -SAVANXP_EAGAIN) { result = -1; break; }
        paint(d);
        const int32_t handles[2] = {0, d->wm.wake_fd};
        (void)sxn_wait_many(handles, 2, 10);
    }
    sx11_destroy(&d->protocol); free(d);
    return result ? 1 : 0;
}
static int server(int display) {
    const char* name = display ? "x11d.1" : "x11d.0";
    long port = sxn_port_create(name);
    const unsigned char ready = port >= 0 || port == -SAVANXP_EEXIST ? 1 : 0;
    (void)sxn_write(1, &ready, 1);
    if (port < 0) return ready ? 0 : 1;
    long children[8] = {0};
    uint64_t idle_deadline = sxn_clock_ns() + 5000000000ull;
    for (;;) {
        unsigned active = 0;
        for (unsigned i = 0; i < 8; ++i) if (children[i]) {
            struct sxn_process_query query;
            if (sxn_process_query(children[i], &query) || query.state == SXN_PROCESS_ENDED) {
                int status; (void)sxn_waitpid(children[i], &status); children[i] = 0;
                idle_deadline = sxn_clock_ns() + 500000000ull;
            } else ++active;
        }
        (void)active;
        (void)sxn_wait_one(port, 10);
        struct sxn_port_connection connection;
        while (sxn_port_accept(port, &connection) == 0) {
            unsigned slot;
            for (slot = 0; slot < 8; ++slot) if (!children[slot]) break;
            if (slot < 8) {
                char* argv[] = {"/system/x11d.sxe", "--client"};
                struct sxn_spawn_request request; sxn_spawn_request_init(&request, argv[0], argv, 2);
                request.std_handles[0] = connection.read_handle; request.std_handles[1] = connection.write_handle;
                const long child = sxn_spawn_ex(&request);
                if (child > 0) children[slot] = child;
            }
            (void)sxn_close(connection.read_handle); (void)sxn_close(connection.write_handle);
        }
        int any = 0;
        for (unsigned i = 0; i < 8; ++i) if (children[i]) any = 1;
        if (!any && sxn_clock_ns() >= idle_deadline) break;
    }
    (void)sxn_close(port);
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--client")) return worker();
    if (argc == 2 && (!strcmp(argv[1], ":0") || !strcmp(argv[1], ":1"))) return server(argv[1][1] - '0');
    (void)sxn_log("x11d: expected :0, :1 or --client"); return 1;
}
