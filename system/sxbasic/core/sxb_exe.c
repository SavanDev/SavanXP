/* Make SXE: a made executable is the stub plus sections
 * (docs/roadmaps/done/SXBASIC_ROADMAP.md#the-runtime-is-a-shared-library-an-executable-is-a-stub-plus-bytecode).
 *
 * This file writes the sections and adds them to a copy of the stub, the way
 * `llvm-objcopy --add-section` does on the build host, and reads them back.
 * The layout of .sxmeta and .sxicon comes from include/sxe/sxe_format.h, as
 * the build's generator (tools/gen_sxe_resources.py) takes it, so the two
 * writers cannot drift apart. */
#include "sxb_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxe/sxe_format.h"

#define ELF_HEADER_BYTES 64u
#define ELF_SHDR_BYTES 64u
#define ELF_SHF_ALLOC 0x2u
#define ELF_SHT_PROGBITS 1u
#define ELF_SHT_NOBITS 8u
#define ELF_OSABI 7u

static uint64_t le(const unsigned char *at, int bytes)
{
    uint64_t value = 0;
    for (int i = 0; i < bytes; i++) value |= (uint64_t)at[i] << (8 * i);
    return value;
}

static void set_le(unsigned char *at, uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; i++) at[i] = (unsigned char)(value >> (8 * i));
}

static int failed(char message[128], const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(message, 128, format, args);
    va_end(args);
    return -1;
}

/* ---- reading an ELF64 file in memory ------------------------------------------- */

struct elf {
    const unsigned char *data;
    size_t size;
    uint64_t shoff;
    unsigned shnum;
    unsigned shstrndx;
    uint64_t strtab_offset;
    uint64_t strtab_size;
};

static int elf_open(struct elf *elf, const void *data, size_t size)
{
    const unsigned char *d = data;
    elf->data = d;
    elf->size = size;
    if (size < ELF_HEADER_BYTES || memcmp(d, "\177ELF", 4) != 0 || d[4] != 2 || d[5] != 1) return 0;
    elf->shoff = le(d + 40, 8);
    elf->shnum = (unsigned)le(d + 60, 2);
    elf->shstrndx = (unsigned)le(d + 62, 2);
    if (le(d + 58, 2) != ELF_SHDR_BYTES || elf->shnum == 0 || elf->shstrndx >= elf->shnum) return 0;
    if (elf->shoff > size || (size - elf->shoff) / ELF_SHDR_BYTES < elf->shnum) return 0;
    const unsigned char *strtab = d + elf->shoff + (uint64_t)elf->shstrndx * ELF_SHDR_BYTES;
    elf->strtab_offset = le(strtab + 24, 8);
    elf->strtab_size = le(strtab + 32, 8);
    return elf->strtab_offset <= size && elf->strtab_size <= size - elf->strtab_offset;
}

static const unsigned char *elf_shdr(const struct elf *elf, unsigned index)
{
    return elf->data + elf->shoff + (uint64_t)index * ELF_SHDR_BYTES;
}

/* The index of the section named `name`, or -1. */
static int elf_find(const struct elf *elf, const char *name)
{
    size_t length = strlen(name);
    for (unsigned i = 0; i < elf->shnum; i++) {
        uint64_t at = le(elf_shdr(elf, i), 4);
        if (at < elf->strtab_size && elf->strtab_size - at > length &&
            memcmp(elf->data + elf->strtab_offset + at, name, length + 1) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int sxb_elf_section(const void *data, size_t size, const char *name, const unsigned char **bytes, size_t *length)
{
    struct elf elf;
    if (!elf_open(&elf, data, size)) return 0;
    int index = elf_find(&elf, name);
    if (index < 0) return 0;
    const unsigned char *shdr = elf_shdr(&elf, (unsigned)index);
    uint64_t offset = le(shdr + 24, 8), section_size = le(shdr + 32, 8);
    if (le(shdr + 4, 4) == ELF_SHT_NOBITS || offset > size || section_size > size - offset) return 0;
    *bytes = elf.data + offset;
    *length = (size_t)section_size;
    return 1;
}

/* ---- adding sections ------------------------------------------------------------- */

int sxb_elf_add_sections(const void *data, size_t size, const struct sxb_elf_add *sections, int count,
                         unsigned char **out, size_t *out_size, char message[128])
{
    struct elf elf;
    if (!elf_open(&elf, data, size)) return failed(message, "the stub is not an ELF64 file");
    int index[8];
    if (count < 0 || count > 8) return failed(message, "too many sections");

    /* The new names go at the end of a copy of the section name table. */
    size_t strtab_size = (size_t)elf.strtab_size;
    unsigned added = 0;
    for (int s = 0; s < count; s++) {
        index[s] = elf_find(&elf, sections[s].name);
        if (index[s] == (int)elf.shstrndx) return failed(message, "%s is the section name table", sections[s].name);
        if (index[s] >= 0 && (le(elf_shdr(&elf, (unsigned)index[s]) + 8, 8) & ELF_SHF_ALLOC)) {
            return failed(message, "the stub's %s is loaded in memory", sections[s].name);
        }
        if (index[s] < 0) {
            strtab_size += strlen(sections[s].name) + 1;
            added++;
        }
    }
    if (elf.shnum + added >= 0xFF00u) return failed(message, "the stub has too many sections");

    /* The stub as it is, then each section's bytes, the name table and the
     * section headers. What the headers no longer point at stays as dead
     * bytes, as it does after llvm-objcopy --update-section. */
    size_t total = size;
    size_t data_at[8];
    for (int s = 0; s < count; s++) {
        total = (total + 7u) & ~(size_t)7u;
        data_at[s] = total;
        total += sections[s].size;
    }
    size_t strtab_at = total;
    total += strtab_size;
    total = (total + 7u) & ~(size_t)7u;
    size_t shdr_at = total;
    unsigned shnum = elf.shnum + added;
    total += (size_t)shnum * ELF_SHDR_BYTES;

    unsigned char *o = calloc(1, total);
    if (o == NULL) return failed(message, "out of memory");
    memcpy(o, data, size);
    for (int s = 0; s < count; s++) {
        if (sections[s].size) memcpy(o + data_at[s], sections[s].data, sections[s].size);
    }
    memcpy(o + strtab_at, elf.data + elf.strtab_offset, (size_t)elf.strtab_size);
    memcpy(o + shdr_at, elf.data + elf.shoff, (size_t)elf.shnum * ELF_SHDR_BYTES);

    size_t name_at = (size_t)elf.strtab_size;
    unsigned next = elf.shnum;
    for (int s = 0; s < count; s++) {
        unsigned char *shdr;
        if (index[s] >= 0) {
            shdr = o + shdr_at + (size_t)index[s] * ELF_SHDR_BYTES;
        } else {
            shdr = o + shdr_at + (size_t)next++ * ELF_SHDR_BYTES;
            size_t length = strlen(sections[s].name) + 1;
            memcpy(o + strtab_at + name_at, sections[s].name, length);
            set_le(shdr + 0, name_at, 4);
            name_at += length;
        }
        set_le(shdr + 4, ELF_SHT_PROGBITS, 4);
        set_le(shdr + 8, 0, 8); /* no SHF_ALLOC: never mapped */
        set_le(shdr + 16, 0, 8);
        set_le(shdr + 24, data_at[s], 8);
        set_le(shdr + 32, sections[s].size, 8);
        set_le(shdr + 40, 0, 4);
        set_le(shdr + 44, 0, 4);
        set_le(shdr + 48, 1, 8);
        set_le(shdr + 56, 0, 8);
    }
    unsigned char *strtab = o + shdr_at + (size_t)elf.shstrndx * ELF_SHDR_BYTES;
    set_le(strtab + 24, strtab_at, 8);
    set_le(strtab + 32, strtab_size, 8);
    set_le(o + 40, shdr_at, 8);
    set_le(o + 60, shnum, 2);

    /* The check the build makes on its own stamping (SXE_FORMAT.md, "The
     * stamping"): an added section with SHF_ALLOC would be mapped into every
     * process that runs the program, silently. */
    struct elf made;
    if (!elf_open(&made, o, total)) {
        free(o);
        return failed(message, "the made file does not read back");
    }
    for (int s = 0; s < count; s++) {
        int at = elf_find(&made, sections[s].name);
        if (at < 0 || (le(elf_shdr(&made, (unsigned)at) + 8, 8) & ELF_SHF_ALLOC)) {
            free(o);
            return failed(message, "%s did not land as a non-allocated section", sections[s].name);
        }
    }
    *out = o;
    *out_size = total;
    return 0;
}

/* ---- .sxmeta ---------------------------------------------------------------------- */

struct blob {
    unsigned char data[SXE_META_MAX_BYTES];
    size_t length;
    uint32_t records;
    int overflow;
};

static void record(struct blob *b, uint16_t tag, const void *payload, size_t length)
{
    size_t padded = (length + SXE_RECORD_ALIGNMENT - 1u) & ~(size_t)(SXE_RECORD_ALIGNMENT - 1u);
    if (b->overflow || SXE_RECORD_HEADER_BYTES + padded > sizeof(b->data) - b->length) {
        b->overflow = 1;
        return;
    }
    unsigned char *at = b->data + b->length;
    set_le(at, tag, 2);
    set_le(at + 2, SXE_RECORD_NONE, 2);
    set_le(at + 4, length, 4);
    memcpy(at + SXE_RECORD_HEADER_BYTES, payload, length);
    memset(at + SXE_RECORD_HEADER_BYTES + length, 0, padded - length);
    b->length += SXE_RECORD_HEADER_BYTES + padded;
    b->records++;
}

static void text_record(struct blob *b, uint16_t tag, const char *text)
{
    if (text[0] != '\0') record(b, tag, text, strlen(text));
}

int sxb_exe_meta(const char *name, const struct sxb_project_info *info, unsigned char **out, size_t *size,
                 char message[128])
{
    struct blob *b = calloc(1, sizeof(*b));
    if (b == NULL) return failed(message, "out of memory");
    b->length = SXE_META_HEADER_BYTES;
    text_record(b, SXE_TAG_NAME, name);
    text_record(b, SXE_TAG_DESCRIPTION, info->description);
    if (info->version[0] != '\0') {
        unsigned char version[SXE_VERSION_BYTES] = {0};
        const char *p = info->version;
        for (unsigned part = 0; part < SXE_VERSION_COMPONENTS && *p; part++) {
            unsigned long value = strtoul(p, (char **)&p, 10);
            set_le(version + 2 * part, value, 2);
            if (*p == '.') p++;
        }
        record(b, SXE_TAG_VERSION, version, sizeof(version));
        text_record(b, SXE_TAG_VERSION_STRING, info->version);
    }
    text_record(b, SXE_TAG_VENDOR, info->vendor);
    text_record(b, SXE_TAG_COPYRIGHT, info->copyright);
    if (info->has_accent) {
        unsigned char accent[4];
        set_le(accent, info->accent & 0xFFFFFFu, 4);
        record(b, SXE_TAG_ACCENT, accent, sizeof(accent));
    }
    text_record(b, SXE_TAG_CATEGORY, info->category);
    if (b->overflow) {
        free(b);
        return failed(message, "the program's description is over %u bytes", SXE_META_MAX_BYTES);
    }
    b->data[0] = SXE_META_MAGIC0;
    b->data[1] = SXE_META_MAGIC1;
    b->data[2] = SXE_META_MAGIC2;
    b->data[3] = SXE_META_MAGIC3;
    set_le(b->data + 4, SXE_META_VERSION, 2);
    set_le(b->data + 6, SXE_META_HEADER_BYTES, 2);
    set_le(b->data + 8, b->length, 4);
    set_le(b->data + 12, b->records, 4);
    *out = malloc(b->length);
    if (*out == NULL) {
        free(b);
        return failed(message, "out of memory");
    }
    memcpy(*out, b->data, b->length);
    *size = b->length;
    free(b);
    return 0;
}

/* The payload of `tag` in a .sxmeta blob, or NULL. */
static const unsigned char *meta_find(const unsigned char *meta, size_t size, uint16_t tag, uint32_t *length)
{
    if (size < SXE_META_HEADER_BYTES || meta[0] != SXE_META_MAGIC0 || meta[1] != SXE_META_MAGIC1 ||
        meta[2] != SXE_META_MAGIC2 || meta[3] != SXE_META_MAGIC3 || le(meta + 4, 2) != SXE_META_VERSION) {
        return NULL;
    }
    uint64_t blob = le(meta + 8, 4), at = le(meta + 6, 2);
    if (blob > size || at < SXE_META_HEADER_BYTES) return NULL;
    while (at + SXE_RECORD_HEADER_BYTES <= blob) {
        uint64_t record_length = le(meta + at + 4, 4);
        uint64_t payload = at + SXE_RECORD_HEADER_BYTES;
        if (record_length > blob - payload) return NULL;
        if (le(meta + at, 2) == tag) {
            *length = (uint32_t)record_length;
            return meta + payload;
        }
        at = payload + ((record_length + SXE_RECORD_ALIGNMENT - 1u) & ~(uint64_t)(SXE_RECORD_ALIGNMENT - 1u));
    }
    return NULL;
}

int sxb_exe_meta_accent(const unsigned char *meta, size_t size, uint32_t *accent)
{
    uint32_t length = 0;
    const unsigned char *payload = meta ? meta_find(meta, size, SXE_TAG_ACCENT, &length) : NULL;
    if (payload == NULL || length != 4) return 0;
    *accent = (uint32_t)le(payload, 4) & 0xFFFFFFu;
    return 1;
}

/* ---- .sxicon ---------------------------------------------------------------------- */

int sxb_exe_icons(const struct sxb_icon_image *images, int count, unsigned char **out, size_t *size,
                  char message[128])
{
    if (count <= 0 || (unsigned)count > SXE_ICON_MAX_IMAGES) return failed(message, "no icon to write");
    size_t total = SXE_ICON_HEADER_BYTES + (size_t)count * SXE_ICON_ENTRY_BYTES;
    for (int i = 0; i < count; i++) {
        if (images[i].side <= 0 || images[i].side > 256) return failed(message, "an icon is 1 to 256 pixels wide");
        total += (size_t)images[i].side * (size_t)images[i].side * SXE_ICON_BYTES_PER_PIXEL;
    }
    if (total > SXE_ICON_MAX_BYTES) return failed(message, "the icons are over %u bytes", SXE_ICON_MAX_BYTES);
    unsigned char *b = calloc(1, total);
    if (b == NULL) return failed(message, "out of memory");
    b[0] = SXE_ICON_MAGIC0;
    b[1] = SXE_ICON_MAGIC1;
    b[2] = SXE_ICON_MAGIC2;
    b[3] = SXE_ICON_MAGIC3;
    set_le(b + 4, SXE_ICON_VERSION, 2);
    set_le(b + 6, SXE_ICON_HEADER_BYTES, 2);
    set_le(b + 8, total, 4);
    set_le(b + 12, (uint64_t)count, 4);
    size_t at = SXE_ICON_HEADER_BYTES + (size_t)count * SXE_ICON_ENTRY_BYTES;
    for (int i = 0; i < count; i++) {
        unsigned char *entry = b + SXE_ICON_HEADER_BYTES + (size_t)i * SXE_ICON_ENTRY_BYTES;
        size_t length = (size_t)images[i].side * (size_t)images[i].side * SXE_ICON_BYTES_PER_PIXEL;
        set_le(entry, (uint64_t)images[i].side, 2);
        set_le(entry + 2, (uint64_t)images[i].side, 2);
        set_le(entry + 4, SXE_ICON_FORMAT_BGRA8888, 4);
        set_le(entry + 8, at, 4);
        set_le(entry + 12, length, 4);
        for (size_t p = 0; p < length / 4u; p++) set_le(b + at + 4u * p, images[i].pixels[p], 4);
        at += length;
    }
    *out = b;
    *size = total;
    return 0;
}

int sxb_exe_icon(const unsigned char *icons, size_t size, int side, uint32_t *pixels)
{
    if (icons == NULL || size < SXE_ICON_HEADER_BYTES || icons[0] != SXE_ICON_MAGIC0 ||
        icons[1] != SXE_ICON_MAGIC1 || icons[2] != SXE_ICON_MAGIC2 || icons[3] != SXE_ICON_MAGIC3 ||
        le(icons + 4, 2) != SXE_ICON_VERSION) {
        return 0;
    }
    uint64_t blob = le(icons + 8, 4), count = le(icons + 12, 4), header = le(icons + 6, 2);
    if (blob > size || header < SXE_ICON_HEADER_BYTES || count > (blob - header) / SXE_ICON_ENTRY_BYTES) {
        return 0;
    }
    for (uint64_t i = 0; i < count; i++) {
        const unsigned char *entry = icons + header + i * SXE_ICON_ENTRY_BYTES;
        uint64_t offset = le(entry + 8, 4), length = le(entry + 12, 4);
        if (le(entry, 2) == (uint64_t)side && le(entry + 2, 2) == (uint64_t)side &&
            le(entry + 4, 4) == SXE_ICON_FORMAT_BGRA8888 && length == (uint64_t)side * (uint64_t)side * 4u &&
            offset <= blob && length <= blob - offset) {
            for (uint64_t p = 0; p < length / 4u; p++) pixels[p] = (uint32_t)le(icons + offset + 4u * p, 4);
            return 1;
        }
    }
    return 0;
}
