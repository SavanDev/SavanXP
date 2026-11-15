/* Host adapter for the SUN server's portable protocol engine. Listens on
 * loopback TCP so the unchanged pinned musl/Xlib fixture can exercise it. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../subsystems/linux/x11/x11.h"
struct host { int socket, x, y; struct sx11_client client; };
static int output(void* opaque, const void* bytes, size_t count) {
    struct host* h = opaque; const unsigned char* p = bytes;
    while (count) { ssize_t n = send(h->socket, p, count, 0); if (n < 0 && errno == EINTR) continue; if (n <= 0) return -1; p += n; count -= n; }
    return 0;
}
static int map(void* opaque, struct sx11_window* w) { (void)opaque; printf("MAP %u %s\n", w->id, w->title); fflush(stdout); return 0; }
static void destroy(void* opaque, struct sx11_window* w) { (void)opaque; printf("DESTROY %u\n", w->id); fflush(stdout); }
static int pointer(void* opaque, struct sx11_window* w, int* rx, int* ry, int* wx, int* wy, unsigned* mask) {
    struct host* h = opaque; *rx = h->x; *ry = h->y; *wx = h->x; *wy = h->y; *mask = 0;
    for (struct sx11_window* a = w; a;) {
        *wx -= a->x + a->border; *wy -= a->y + a->border;
        if (a->parent == SX11_ROOT) break;
        struct sx11_window* parent = 0;
        for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (h->client.windows[i].id == a->parent) parent = &h->client.windows[i];
        a = parent;
    }
    return 0;
}
static void screenshot(struct host* h, const char* path) {
    struct sx11_window* w = 0;
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (h->client.windows[i].mapped && h->client.windows[i].parent == SX11_ROOT) { w = &h->client.windows[i]; break; }
    if (!w) { puts("ERROR no mapped window"); fflush(stdout); return; }
    uint32_t* pixels = calloc(w->width * w->height, 4); if (!pixels) exit(2);
    sx11_composite(&h->client, w->id, pixels, w->width, w->width, w->height);
    FILE* file = fopen(path, "wb"); if (!file) exit(2);
    fprintf(file, "P6\n%u %u\n255\n", w->width, w->height);
    for (unsigned y = 0; y < w->height; ++y) for (unsigned x = 0; x < w->width; ++x) {
        uint32_t pixel = pixels[y * w->width + x];
        unsigned char rgb[3] = {pixel >> 16, pixel >> 8, pixel}; fwrite(rgb, 1, 3, file);
    }
    free(pixels); fclose(file); puts("SHOT"); fflush(stdout);
}
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdin, 0, _IONBF, 0);
    int listener = socket(AF_INET, SOCK_STREAM, 0); if (listener < 0) return 2;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) || listen(listener, 1)) return 2;
    socklen_t size = sizeof(address); if (getsockname(listener, (struct sockaddr*)&address, &size)) return 2;
    printf("PORT %u\n", ntohs(address.sin_port)); fflush(stdout);
    struct host* h = calloc(1, sizeof(*h)); if (!h) return 2;
    h->x = 75; h->y = 50; h->socket = accept(listener, 0, 0); close(listener); if (h->socket < 0) return 2;
    const struct sx11_ops ops = {output, map, destroy, 0, pointer}; sx11_init(&h->client, &ops, h, 640, 480);
    int result = 0;
    for (;;) {
        struct pollfd fds[2] = {{h->socket, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        if (poll(fds, 2, 10000) <= 0) { result = 2; break; }
        if (fds[0].revents & POLLIN) {
            unsigned char buffer[4096]; ssize_t n = read(h->socket, buffer, sizeof(buffer));
            if (n <= 0) { result = sx11_finish(&h->client) ? 1 : 0; break; }
            if (sx11_feed(&h->client, buffer, n)) { result = 1; break; }
        } else if (fds[0].revents & (POLLERR | POLLHUP)) break;
        if (fds[1].revents & POLLIN) {
            char line[1024]; if (!fgets(line, sizeof(line), stdin)) break;
            int x, y; char path[900];
            if (sscanf(line, "pointer %d %d", &x, &y) == 2) { h->x = x; h->y = y; puts("POINTER"); fflush(stdout); }
            else if (sscanf(line, "resize %d %d", &x, &y) == 2) {
                for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (h->client.windows[i].mapped && h->client.windows[i].parent == SX11_ROOT)
                    if (sx11_resize(&h->client, h->client.windows[i].id, x, y)) result = 1;
            } else if (sscanf(line, "shot %899s", path) == 1) screenshot(h, path);
            else if (!strcmp(line, "close\n")) {
                for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) if (h->client.windows[i].mapped && h->client.windows[i].parent == SX11_ROOT)
                    if (sx11_close_window(&h->client, h->client.windows[i].id)) result = 1;
            }
        }
    }
    sx11_destroy(&h->client); close(h->socket); free(h);
    puts(result ? "X11 HOST FAIL" : "X11 HOST PASS"); return result;
}
