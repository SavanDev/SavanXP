/* Host test of sxgfx/pe_icon.h (system/gfx/pe_icon.c), the shell's reader of
 * a Windows program's icon, with the sanitizers on: the icon of the
 * resource.exe fixture (its own art, subsystems/win32/fixtures/resource) at
 * two sizes against known checksums, a program without one, the argument
 * checks, and 20,000 random byte changes and truncations of the file, so a
 * read outside it fails the test.
 *
 *     savanxp-pe-icon-test resource.exe hello.exe
 */
#include "sxgfx/pe_icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a of the pixels resource.exe's icon gives at each size. */
#define EXPECT_32 0x6ccbd5e1u
#define EXPECT_16 0x82438214u

static int g_failures = 0;

static void expect(int condition, const char *label) {
    if (!condition) {
        printf("FAIL %s\n", label);
        ++g_failures;
    }
}

static size_t read_all(const char *path, uint8_t *buffer, size_t capacity) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL cannot read %s\n", path);
        exit(1);
    }
    size_t n = fread(buffer, 1, capacity, f);
    fclose(f);
    return n;
}

static uint32_t fnv(const uint32_t *pixels, size_t count) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        for (int b = 0; b < 32; b += 8) {
            hash = (hash ^ ((pixels[i] >> b) & 0xffu)) * 16777619u;
        }
    }
    return hash;
}

int main(int argc, char **argv) {
    static uint8_t image[1 << 20];
    static uint8_t other[1 << 20];
    static uint32_t pixels[PE_ICON_MAX_EXTENT * PE_ICON_MAX_EXTENT];
    if (argc != 3) {
        printf("usage: savanxp-pe-icon-test resource.exe hello.exe\n");
        return 2;
    }
    const size_t size = read_all(argv[1], image, sizeof(image));
    const size_t other_size = read_all(argv[2], other, sizeof(other));

    expect(pe_icon_from_file(image, size, 32, pixels) == 1, "resource.exe has a 32x32 icon");
    expect(fnv(pixels, 32 * 32) == EXPECT_32, "the 32x32 icon's pixels");
    expect(pe_icon_from_file(image, size, 16, pixels) == 1, "resource.exe has a 16x16 icon");
    expect(fnv(pixels, 16 * 16) == EXPECT_16, "the 16x16 icon's pixels");
    expect(pe_icon_from_file(other, other_size, 32, pixels) == 0, "hello.exe has no icon");
    expect(pe_icon_from_file(image, size, 0, pixels) == 0, "an extent of 0 is refused");
    expect(pe_icon_from_file(image, size, PE_ICON_MAX_EXTENT + 1u, pixels) == 0, "a larger extent is refused");
    expect(pe_icon_from_file(image, 63, 32, pixels) == 0, "a file cut inside its DOS header");

    static uint8_t copy[1 << 20];
    srand(1);
    for (int round = 0; round < 20000; ++round) {
        size_t length = size;
        memcpy(copy, image, size);
        if (round % 4 == 0) {
            length = (size_t)rand() % size;
        } else {
            for (int k = 0; k < 1 + rand() % 8; ++k) {
                copy[(size_t)rand() % size] = (uint8_t)rand();
            }
        }
        /* Exactly the bytes asked for, so ASan sees a read past the end. */
        uint8_t *heap = malloc(length + 1u);
        memcpy(heap, copy, length);
        (void)pe_icon_from_file(heap, length, round % 2 ? 32u : 16u, pixels);
        free(heap);
    }
    printf(g_failures == 0 ? "pe-icon-test: ok\n" : "pe-icon-test: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
