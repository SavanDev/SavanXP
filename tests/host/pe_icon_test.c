/* Host test of the shell's readers of a Windows program, with the
 * sanitizers on: sxgfx/pe_icon.h (system/gfx/pe_icon.c), the icon of the
 * resource.exe fixture (its own art, subsystems/win32/fixtures/resource) at
 * two sizes against known checksums, and sxgfx/pe_version.h
 * (system/gfx/pe_version.c), the strings of a version resource in a minimal
 * PE this test writes (not a fixture: those are run on Windows XP, and their
 * hashes are recorded); a program without either, the argument checks, and
 * 20,000 random byte changes and truncations of each file, so a read outside
 * it fails the test. With SAVANXP_WIN32_FIXTURES it also reads the user's
 * sol.exe's description.
 *
 *     savanxp-pe-icon-test resource.exe hello.exe
 */
#include "sxgfx/pe_icon.h"
#include "sxgfx/pe_version.h"

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

/* ---- a minimal PE32 with one RT_VERSION resource ---- */

typedef struct out_buffer {
    uint8_t *bytes;
    size_t used;
} out_buffer;

static void put16(out_buffer *o, uint32_t value) {
    o->bytes[o->used++] = (uint8_t)value;
    o->bytes[o->used++] = (uint8_t)(value >> 8);
}

static void pad4(out_buffer *o) {
    while (o->used % 4u != 0u) {
        o->bytes[o->used++] = 0;
    }
}

/* Latin-1 text as UTF-16, with its 0. */
static void put_text(out_buffer *o, const char *text) {
    for (const unsigned char *c = (const unsigned char *)text; *c != 0; ++c) {
        put16(o, *c);
    }
    put16(o, 0);
}

/* A block's header, key and value; its children follow, then end_block. */
static size_t begin_block(out_buffer *o, const char *key, const char *text, uint32_t binary_length) {
    const size_t start = o->used;
    put16(o, 0);
    put16(o, text != NULL ? (uint32_t)strlen(text) + 1u : binary_length);
    put16(o, text != NULL ? 1u : 0u);
    put_text(o, key);
    pad4(o);
    if (text != NULL) {
        put_text(o, text);
    } else {
        memset(o->bytes + o->used, 0, binary_length);
        o->used += binary_length;
    }
    pad4(o);
    return start;
}

static void end_block(out_buffer *o, size_t start) {
    const size_t length = o->used - start;
    o->bytes[start] = (uint8_t)length;
    o->bytes[start + 1] = (uint8_t)(length >> 8);
}

static void put_string(out_buffer *o, const char *key, const char *value) {
    end_block(o, begin_block(o, key, value, 0));
}

static void set32(uint8_t *p, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

/* The PE, 4 KiB: headers, one section at file 0x400 (RVA 0x1000) holding the
 * resource directory (type 16, id 1, one language) and the version. */
static size_t write_version_pe(uint8_t *file) {
    memset(file, 0, 0x1000);
    file[0] = 'M';
    file[1] = 'Z';
    set32(file + 60, 64);
    memcpy(file + 64, "PE\0\0", 4);
    file[68] = 0x4c;
    file[69] = 0x01;
    file[70] = 1;     /* one section */
    file[84] = 224;   /* PE32 optional header */
    file[88] = 0x0b;  /* magic 0x10b */
    file[89] = 0x01;
    uint8_t *section = file + 88 + 224;
    memcpy(section, ".rsrc", 5);
    set32(section + 8, 0xc00);
    set32(section + 12, 0x1000);
    set32(section + 16, 0xc00);
    set32(section + 20, 0x400);

    uint8_t *res = file + 0x400;
    res[14] = 1;
    set32(res + 16, 16);
    set32(res + 20, 0x80000000u | 0x18u);
    res[0x18 + 14] = 1;
    set32(res + 0x28, 1);
    set32(res + 0x2c, 0x80000000u | 0x30u);
    res[0x30 + 14] = 1;
    set32(res + 0x40, 0x0c0a);
    set32(res + 0x44, 0x48);

    out_buffer o = {res + 0x60, 0};
    const size_t root = begin_block(&o, "VS_VERSION_INFO", NULL, 52);
    const size_t strings = begin_block(&o, "StringFileInfo", NULL, 0);
    const size_t spanish = begin_block(&o, "0c0a04b0", NULL, 0);
    put_string(&o, "CompanyName", "SavanXP");
    put_string(&o, "FileDescription", "Juego de recursos \xf1");
    put_string(&o, "InternalName", "version");
    end_block(&o, spanish);
    const size_t english = begin_block(&o, "040904b0", NULL, 0);
    put_string(&o, "FileDescription", "Resource game");
    end_block(&o, english);
    end_block(&o, strings);
    const size_t variables = begin_block(&o, "VarFileInfo", NULL, 0);
    end_block(&o, begin_block(&o, "Translation", NULL, 8));
    end_block(&o, variables);
    end_block(&o, root);
    set32(res + 0x48, 0x1060);
    set32(res + 0x4c, (uint32_t)o.used);
    set32(file + 88 + 96 + 16, 0x1000);
    set32(file + 88 + 96 + 20, 0x60u + (uint32_t)o.used);
    return 0x1000;
}

static void fuzz(const uint8_t *file, size_t size, int with_icon, uint32_t *pixels) {
    static uint8_t copy[1 << 20];
    char text[64];
    srand(1);
    for (int round = 0; round < 20000; ++round) {
        size_t length = size;
        memcpy(copy, file, size);
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
        if (with_icon) {
            (void)pe_icon_from_file(heap, length, round % 2 ? 32u : 16u, pixels);
        }
        (void)pe_version_string(heap, length, "FileDescription", text, sizeof(text));
        free(heap);
    }
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

    static uint8_t version[0x1000];
    const size_t version_size = write_version_pe(version);
    char text[64];
    expect(pe_version_string(version, version_size, "FileDescription", text, sizeof(text)) == 1 &&
               strcmp(text, "Juego de recursos \xc3\xb1") == 0,
           "FileDescription, from the first table, in UTF-8");
    expect(pe_version_string(version, version_size, "CompanyName", text, sizeof(text)) == 1 && strcmp(text, "SavanXP") == 0,
           "another key of the table");
    expect(pe_version_string(version, version_size, "ProductName", text, sizeof(text)) == 0 && text[0] == '\0',
           "a key the table has not");
    expect(pe_version_string(version, version_size, "FileDescriptio", text, sizeof(text)) == 0, "a key's prefix");
    expect(pe_version_string(version, version_size, "FileDescription", text, 19) == 1 && strcmp(text, "Juego de recursos ") == 0,
           "truncated before a two-byte character, not inside it");
    expect(pe_version_string(version, version_size, "FileDescription", text, 1) == 0 && text[0] == '\0', "no room");
    expect(pe_version_string(other, other_size, "FileDescription", text, sizeof(text)) == 0,
           "hello.exe has no version");
    expect(pe_version_string(image, size, "FileDescription", text, sizeof(text)) == 0, "nor has resource.exe");
    const char *fixtures = getenv("SAVANXP_WIN32_FIXTURES");
    if (fixtures != NULL && fixtures[0] != '\0') {
        static uint8_t sol[1 << 20];
        char path[1024];
        snprintf(path, sizeof(path), "%s/sol.exe", fixtures);
        const size_t sol_size = read_all(path, sol, sizeof(sol));
        expect(pe_version_string(sol, sol_size, "FileDescription", text, sizeof(text)) == 1 &&
                   strcmp(text, "Juego Solitario") == 0,
               "the user's sol.exe is Juego Solitario");
    }

    fuzz(image, size, 1, pixels);
    fuzz(version, version_size, 0, pixels);
    printf(g_failures == 0 ? "pe-icon-test: ok\n" : "pe-icon-test: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
