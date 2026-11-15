/* Exercise the real portable engine, including every stream split and refusal
 * before a host callback can touch an invalid resource. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../subsystems/linux/x11/x11.h"

static unsigned char output[131072];
static size_t used;
static int maps, destroys, presents;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "x11: line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int send_output(void* h, const void* p, size_t n) { (void)h; CHECK(used + n <= sizeof(output)); memcpy(output + used, p, n); used += n; return 0; }
static int map(void* h, struct sx11_window* w) { (void)h; CHECK(w->bitmap.pixels != 0); ++maps; return 0; }
static void destroy(void* h, struct sx11_window* w) { (void)h; (void)w; ++destroys; }
static void present(void* h, struct sx11_window* w) { (void)h; (void)w; ++presents; }
static void w16(unsigned char* p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void w32(unsigned char* p, unsigned v) { w16(p, v); w16(p + 2, v >> 16); }
static struct sx11_ops ops = {send_output, map, destroy, present, 0};
static void start(struct sx11_client* c) {
    sx11_init(c, &ops, 0, 640, 480); used = 0;
    unsigned char setup[12] = {'l', 0, 11};
    for (unsigned i = 0; i < 12; ++i) CHECK(!sx11_feed(c, setup + i, 1));
    CHECK(used == 128 && output[0] == 1 && !sx11_finish(c)); used = 0;
}
static void create(unsigned char p[40], unsigned id) {
    memset(p, 0, 40); p[0] = 1; w16(p + 2, 10); w32(p + 4, id); w32(p + 8, SX11_ROOT);
    w16(p + 16, 200); w16(p + 18, 150); w32(p + 28, 2 | (1u << 11)); w32(p + 32, 0xffffff);
    w32(p + 36, (1u << 15) | (1u << 17));
}
int main(void) {
    struct sx11_client* c = malloc(sizeof(*c)); CHECK(c);
    unsigned char p[40];
    for (unsigned split = 0; split <= 40; ++split) {
        start(c); create(p, 0x200001);
        CHECK(!sx11_feed(c, p, split)); CHECK(!sx11_feed(c, p + split, 40 - split));
        CHECK(c->windows[0].id == 0x200001 && !used && !sx11_finish(c));
        unsigned char request[8] = {8, 0, 2}; w32(request + 4, 0x200001);
        CHECK(!sx11_feed(c, request, 8)); CHECK(used == 64 && output[0] == 19 && output[32] == 12);
        CHECK(!sx11_resize(c, 0x200001, 260, 180)); CHECK(used == 128 && output[64] == 22 && output[96] == 12);
        request[0] = 4; CHECK(!sx11_feed(c, request, 8)); CHECK(!c->windows[0].id);
        sx11_destroy(c);
    }
    CHECK(maps == 41 && destroys == 41);
    start(c);
    // A valid request following BadLength still receives the correct sequence.
    unsigned char malformed[4] = {1, 0, 1, 0}; CHECK(!sx11_feed(c, malformed, 4));
    CHECK(used == 32 && output[1] == 16 && output[2] == 1);
    malformed[0] = 43; CHECK(!sx11_feed(c, malformed, 4)); CHECK(used == 64 && output[32] == 1 && output[34] == 2);
    // Zero length cannot be resynchronized and terminates the connection.
    malformed[2] = 0; CHECK(sx11_feed(c, malformed, 4) == -1); CHECK(sx11_finish(c) == -1); sx11_destroy(c);
    for (unsigned truncated = 1; truncated < 40; ++truncated) {
        start(c); create(p, 0x200001); CHECK(!sx11_feed(c, p, truncated)); CHECK(sx11_finish(c) == -1);
        CHECK(!c->windows[0].id); sx11_destroy(c);
    }
    start(c); create(p, 0x200001); w32(p + 28, 0xffffffff); CHECK(!sx11_feed(c, p, 40)); CHECK(output[1] == 16);
    CHECK(!c->windows[0].id); sx11_destroy(c);
    start(c); create(p, 0x200001); CHECK(!sx11_feed(c, p, 40)); CHECK(!sx11_feed(c, p, 40));
    CHECK(used == 32 && output[1] == 14); sx11_destroy(c);
    start(c); create(p, 0x200001); CHECK(!sx11_feed(c, p, 40));
    unsigned char bad_property[24] = {18, 0, 6}; w32(bad_property + 4, 0x200001); bad_property[16] = 32;
    w32(bad_property + 20, 0xffffffff); CHECK(!sx11_feed(c, bad_property, sizeof(bad_property)));
    CHECK(used == 32 && output[1] == 16); sx11_destroy(c);
    // Capacity failure must preserve every existing resource and release
    // them all on disconnect, even when the client never sent DestroyWindow.
    start(c);
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) {
        create(p, 0x200001 + i); CHECK(!sx11_feed(c, p, 40));
    }
    create(p, 0x200100); CHECK(!sx11_feed(c, p, 40)); CHECK(used == 32 && output[1] == 11);
    sx11_destroy(c);
    for (unsigned i = 0; i < SX11_MAX_WINDOWS; ++i) CHECK(!c->windows[i].id);
    // Sequence numbers on the wire wrap at 16 bits.
    start(c);
    unsigned char noop[4] = {127, 0, 1, 0};
    for (unsigned i = 0; i < 65536; ++i) CHECK(!sx11_feed(c, noop, 4));
    CHECK(c->sequence == 0 && !used);
    noop[0] = 43; CHECK(!sx11_feed(c, noop, 4)); CHECK(output[2] == 1 && output[3] == 0);
    sx11_destroy(c);
    // A truncated setup cannot be mistaken for a usable connection.
    for (unsigned i = 0; i < 12; ++i) {
        unsigned char prefix[12] = {'l', 0, 11};
        sx11_init(c, &ops, 0, 640, 480); CHECK(!sx11_feed(c, prefix, i)); CHECK(sx11_finish(c) == -1);
        sx11_destroy(c);
    }
    free(c); puts("X11 PROTOCOL PASS: fragments, truncation, lengths, IDs, resources, resize and teardown"); return 0;
}
