/*
 * resource.c -- Resource lookup in a mapped image (resource.h).
 */
#include "resource.h"

typedef unsigned int u32;
typedef unsigned short u16;

static u32 rd16(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rd32(const unsigned char *p) {
    return rd16(p) | (rd16(p + 2) << 16);
}

/* "#123" as the id 123; any other name as it is. */
static const u16 *id_of_name(const u16 *name) {
    if ((u32)name < 0x10000u || name[0] != '#' || name[1] == 0) {
        return name;
    }
    u32 id = 0;
    for (const u16 *c = name + 1; *c != 0; ++c) {
        if (*c < '0' || *c > '9' || id > 0xffffu) {
            return name;
        }
        id = id * 10u + (u32)(*c - '0');
    }
    return id <= 0xffffu ? (const u16 *)(unsigned long)id : name;
}

/* The entry of a resource directory with this id, or this name (UTF-16, any
 * case); 0 when there is none. *entry is the entry's second word, as stored. */
static int find_entry(const unsigned char *root, const unsigned char *dir, const u16 *name, u32 *entry) {
    const u32 named = rd16(dir + 12);
    const u32 ids = rd16(dir + 14);
    for (u32 i = 0; i < named + ids; ++i) {
        const unsigned char *e = dir + 16 + 8 * i;
        const u32 key = rd32(e);
        int match;
        if ((u32)name < 0x10000u) {
            match = (key & 0x80000000u) == 0 && key == (u32)name;
        } else if ((key & 0x80000000u) != 0) {
            const unsigned char *string = root + (key & 0x7fffffffu);
            const u32 length = rd16(string);
            match = 1;
            u32 k = 0;
            for (; k < length && match; ++k) {
                u16 a = (u16)rd16(string + 2 + 2 * k);
                u16 b = name[k];
                a = a >= 'a' && a <= 'z' ? (u16)(a - 32) : a;
                b = b >= 'a' && b <= 'z' ? (u16)(b - 32) : b;
                match = b != 0 && a == b;
            }
            match = match && name[k] == 0;
        } else {
            match = 0;
        }
        if (match) {
            *entry = rd32(e + 4);
            return 1;
        }
    }
    return 0;
}

const unsigned char *sxw_resource_entry(u32 base, const u16 *type, const u16 *name, u32 language) {
    const unsigned char *image = (const unsigned char *)base;
    if (base == 0 || rd16(image) != 0x5a4du) {
        return 0;
    }
    const unsigned char *nt = image + rd32(image + 0x3c);
    if (rd32(nt) != 0x4550u || rd16(nt + 24) != 0x10bu) {
        return 0;
    }
    const unsigned char *optional = nt + 24;
    if (rd32(optional + 92) <= 2u) {
        return 0;
    }
    const u32 rva = rd32(optional + 96 + 2 * 8);
    if (rva == 0) {
        return 0;
    }
    const unsigned char *root = image + rva;
    u32 entry = 0;
    if (!find_entry(root, root, id_of_name(type), &entry) || (entry & 0x80000000u) == 0) {
        return 0;
    }
    const unsigned char *names = root + (entry & 0x7fffffffu);
    if (!find_entry(root, names, id_of_name(name), &entry) || (entry & 0x80000000u) == 0) {
        return 0;
    }
    const unsigned char *languages = root + (entry & 0x7fffffffu);
    if (rd16(languages + 12) + rd16(languages + 14) == 0u) {
        return 0;
    }
    entry = rd32(languages + 16 + 4);
    if (language != SXW_ANY_LANGUAGE) {
        u32 exact = 0;
        if (find_entry(root, languages, (const u16 *)(unsigned long)language, &exact)) {
            entry = exact;
        }
    }
    if ((entry & 0x80000000u) != 0) {
        return 0;
    }
    return root + entry;
}
