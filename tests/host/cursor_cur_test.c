#include "cursor_cur.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, unsigned v) { put16(p, v); put16(p + 2, v >> 16); }
static struct cursor_image image;
static unsigned char fixture[1200];

/* Four pixels: black, white, transparent, inverted. Two rows exercise DIB
 * orientation; palette depths also exercise DWORD row padding. */
static size_t make_fixture(unsigned bits) {
    unsigned colors = bits <= 8 ? 2 : 0;
    unsigned stride = ((2 * bits + 31) / 32) * 4;
    unsigned start = 22 + 40 + colors * 4;
    unsigned mask = start + 2 * stride;
    memset(fixture, 0, sizeof(fixture));
    put16(fixture + 2, 2); put16(fixture + 4, 1);
    fixture[6] = fixture[7] = 2;
    put16(fixture + 10, 1); put16(fixture + 12, 1);
    put32(fixture + 14, mask + 8 - 22); put32(fixture + 18, 22);
    put32(fixture + 22, 40); put32(fixture + 26, 2); put32(fixture + 30, 4);
    put16(fixture + 34, 1); put16(fixture + 36, bits); put32(fixture + 54, colors);
    if (colors) memset(fixture + 66, 255, 3);
    for (unsigned y = 0; y < 2; ++y) {
        unsigned char *row = fixture + start + y * stride;
        if (bits == 1) row[0] = 0x40;
        else if (bits == 4) row[0] = 0x01;
        else if (bits == 8) row[1] = 1;
        else memset(row + bits / 8, 255, 3);
    }
    fixture[mask] = 0xc0; /* bottom row: transparent, inverted */
    return mask + 8;
}
static void formats(void) {
    static const unsigned depths[] = {1,4,8,24,32};
    for (unsigned d = 0; d < 5; ++d) {
        size_t n = make_fixture(depths[d]);
        assert(!cursor_cur_decode(fixture, n, &image));
        assert(image.width == 2 && image.height == 2 && image.hotspot_x == 1 && image.hotspot_y == 1);
        assert(image.classic && image.needs_background);
        assert(cursor_image_pixel(&image, 0, 0xff123456) == 0xff000000);
        assert(cursor_image_pixel(&image, 1, 0xff123456) == 0xffffffff);
        assert(cursor_image_pixel(&image, 2, 0xff123456) == 0xff123456);
        assert(cursor_image_pixel(&image, 3, 0xff123456) == 0xffedcba9);
        /* Every truncation of the single-image file is rejected. */
        for (size_t k = 0; k < n; ++k) assert(cursor_cur_decode(fixture, k, &image));
    }
    size_t n = make_fixture(32);
    fixture[62 + 8 + 3] = 128; /* top-left half-alpha black */
    assert(!cursor_cur_decode(fixture, n, &image));
    assert(!image.classic && !image.needs_background);
    assert(cursor_image_pixel(&image, 0, 0xffffffff) == 0xff7f7f7f);
    assert(cursor_image_pixel(&image, 1, 0xff123456) == 0xff123456);
    /* A failed decode leaves the previous result intact. */
    image.hotspot_x = 42;
    fixture[2] = 1;
    assert(cursor_cur_decode(fixture, n, &image) && image.hotspot_x == 42);
    n = make_fixture(4); fixture[70] = 0xf0;
    assert(cursor_cur_decode(fixture, n, &image));
    n = make_fixture(1); put32(fixture + 18, 0xfffffff0);
    assert(cursor_cur_decode(fixture, n, &image));
    n = make_fixture(1); fixture[10] = 2;
    assert(cursor_cur_decode(fixture, n, &image));
    n = make_fixture(1); put32(fixture + 38, 1);
    assert(cursor_cur_decode(fixture, n, &image));
    n = make_fixture(1); put16(fixture + 4, 257);
    assert(cursor_cur_decode(fixture, n, &image));
    n = make_fixture(1); put32(fixture + 26, 0xffffffff);
    assert(cursor_cur_decode(fixture, n, &image));
    n = make_fixture(1); memcpy(fixture + 22, "\x89PNG\r\n\x1a\n", 8);
    assert(cursor_cur_decode(fixture, n, &image));
    assert(cursor_cur_decode(0, 0, &image));
    assert(cursor_cur_decode(fixture, CUR_MAX_FILE_SIZE + 1, &image));
}
static void wine(const char *root) {
    static const char *names[] = {"normal","wait","ibeam","sizeall","sizewe","sizens","no","hand","sizenwse","sizenesw"};
    static const int spots[][2] = {{0,0},{15,15},{3,9},{10,10},{10,6},{6,9},{8,8},{7,0},{7,7},{7,7}};
    for (unsigned i = 0; i < 10; ++i) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/assets/desktop/cursors/wine/ocr_%s.cur", root, names[i]);
        FILE *f = fopen(path, "rb"); assert(f);
        unsigned char bytes[40000]; size_t n = fread(bytes, 1, sizeof(bytes), f); fclose(f);
        assert(!cursor_cur_decode(bytes, n, &image));
        assert(image.width == 32 && image.height == 32);
        assert(image.hotspot_x == spots[i][0] && image.hotspot_y == spots[i][1]);
        assert(!image.classic); /* 32-bit candidate wins over monochrome */
        unsigned visible = 0;
        for (unsigned k = 0; k < 1024; ++k) visible += (image.pixels[k] >> 24) != 0;
        assert(visible > 20);
        /* If 32px is absent choose 48px, retaining support for larger CURs. */
        unsigned char larger[40000]; memcpy(larger, bytes, n);
        for (unsigned k = 0; k < larger[4]; ++k) if (larger[6+k*16] == 32) larger[9+k*16] = 1;
        assert(!cursor_cur_decode(larger, n, &image) && image.width == 48);
        for (unsigned k = 0; k < larger[4]; ++k) if (larger[6+k*16] == 48) larger[9+k*16] = 1;
        assert(!cursor_cur_decode(larger, n, &image) && image.width == 64);
        /* Restrict directory to 32px entries: native smaller size, no scaling. */
        for (unsigned k = 0; k < bytes[4]; ++k) if (bytes[6 + k * 16] != 32) bytes[9 + k * 16] = 1;
        assert(!cursor_cur_decode(bytes, n, &image) && image.width == 32);
        /* Keep only monochrome candidates: exercise Wine's classic masks. */
        for (unsigned k = 0; k < bytes[4]; ++k) if (bytes[8 + k * 16] != 2) bytes[9 + k * 16] = 1;
        assert(!cursor_cur_decode(bytes, n, &image) && image.classic);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    formats(); wine(argv[1]);
    puts("cursor-cur-test: PASS");
    return 0;
}
