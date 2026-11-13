#include "sxgfx/pe_version.h"

#include "pe_resource.h"

/* See include/sxgfx/pe_version.h.
 *
 * VS_VERSIONINFO is a tree of blocks, each: wLength (the whole block),
 * wValueLength, wType (1: the value is text, its length in characters),
 * a UTF-16 key ending in 0, padding to 4 bytes, the value, padding to 4
 * bytes, then the children up to wLength. The root "VS_VERSION_INFO" has
 * "StringFileInfo", whose children are string tables (one per language),
 * whose children are the strings. Offsets align from the resource's start. */

#define RT_VERSION 16u

typedef struct block {
    uint32_t start;    /* offset of the block in the resource */
    uint32_t end;      /* start + wLength, within the parent */
    uint32_t key;      /* offset of the key */
    uint32_t value;    /* offset of the value, aligned */
    uint32_t children; /* offset of the first child, aligned */
} block;

static uint32_t align4(uint32_t offset) {
    return (offset + 3u) & ~3u;
}

/* The block at `at`, which must end by `limit`. 0 when it does not fit. */
static int read_block(const uint8_t* data, uint32_t at, uint32_t limit, block* b) {
    if (at > limit || limit - at < 6u) {
        return 0;
    }
    const uint32_t length = pe_get16(data + at);
    if (length < 6u || length > limit - at) {
        return 0;
    }
    b->start = at;
    b->end = at + length;
    b->key = at + 6u;
    uint32_t cursor = b->key;
    for (;;) {
        if (b->end - cursor < 2u) {
            return 0;
        }
        const uint32_t c = pe_get16(data + cursor);
        cursor += 2u;
        if (c == 0u) {
            break;
        }
    }
    b->value = align4(cursor) < b->end ? align4(cursor) : b->end;
    const uint32_t value_length = pe_get16(data + at + 2);
    const uint32_t value_bytes = pe_get16(data + at + 4) == 1u ? value_length * 2u : value_length;
    b->children = value_bytes <= b->end - b->value ? align4(b->value + value_bytes) : b->end;
    if (b->children > b->end) {
        b->children = b->end;
    }
    return 1;
}

static int key_is(const uint8_t* data, const block* b, const char* key) {
    uint32_t at = b->key;
    for (size_t i = 0;; ++i) {
        const uint32_t c = pe_get16(data + at); /* read_block found its 0 */
        if (c != (uint32_t)(uint8_t)key[i]) {
            return 0;
        }
        if (c == 0u) {
            return 1;
        }
        at += 2u;
    }
}

/* The child of `parent` with this key, or its first when key is 0. */
static int find_child(const uint8_t* data, const block* parent, const char* key, block* out) {
    uint32_t at = parent->children;
    while (at < parent->end && read_block(data, at, parent->end, out)) {
        if (key == 0 || key_is(data, out, key)) {
            return 1;
        }
        at = align4(out->end);
    }
    return 0;
}

/* One UTF-16 value, up to its 0 or the block's end, as UTF-8. */
static int to_utf8(const uint8_t* data, uint32_t at, uint32_t end, char* out, size_t capacity) {
    size_t used = 0;
    while (at < end && end - at >= 2u) {
        uint32_t c = pe_get16(data + at);
        at += 2u;
        if (c == 0u) {
            break;
        }
        if (c >= 0xd800u && c < 0xdc00u && end - at >= 2u) {
            const uint32_t low = pe_get16(data + at);
            if (low >= 0xdc00u && low < 0xe000u) {
                c = 0x10000u + ((c - 0xd800u) << 10) + (low - 0xdc00u);
                at += 2u;
            }
        }
        if (c >= 0xd800u && c < 0xe000u) {
            c = 0xfffdu; /* an unpaired surrogate */
        }
        char bytes[4];
        size_t n;
        if (c < 0x80u) {
            bytes[0] = (char)c;
            n = 1;
        } else if (c < 0x800u) {
            bytes[0] = (char)(0xc0u | c >> 6);
            bytes[1] = (char)(0x80u | (c & 0x3fu));
            n = 2;
        } else if (c < 0x10000u) {
            bytes[0] = (char)(0xe0u | c >> 12);
            bytes[1] = (char)(0x80u | ((c >> 6) & 0x3fu));
            bytes[2] = (char)(0x80u | (c & 0x3fu));
            n = 3;
        } else {
            bytes[0] = (char)(0xf0u | c >> 18);
            bytes[1] = (char)(0x80u | ((c >> 12) & 0x3fu));
            bytes[2] = (char)(0x80u | ((c >> 6) & 0x3fu));
            bytes[3] = (char)(0x80u | (c & 0x3fu));
            n = 4;
        }
        if (capacity - 1u - used < n) {
            break;
        }
        for (size_t i = 0; i < n; ++i) {
            out[used++] = bytes[i];
        }
    }
    out[used] = '\0';
    return used > 0;
}

int pe_version_string(const uint8_t* file, size_t size, const char* key, char* out, size_t capacity) {
    pe_resources r;
    uint32_t length = 0;
    block root;
    block strings;
    block table;
    block entry;

    if (out == 0 || capacity == 0u) {
        return 0;
    }
    out[0] = '\0';
    if (key == 0 || !pe_resources_open(&r, file, size)) {
        return 0;
    }
    const uint8_t* data = pe_resources_find(&r, RT_VERSION, 0, &length);
    if (data == 0 || !read_block(data, 0, length, &root) || !key_is(data, &root, "VS_VERSION_INFO") ||
        !find_child(data, &root, "StringFileInfo", &strings) || !find_child(data, &strings, 0, &table) ||
        !find_child(data, &table, key, &entry)) {
        return 0;
    }
    return to_utf8(data, entry.value, entry.end, out, capacity);
}
