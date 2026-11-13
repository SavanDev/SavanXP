#include "pe_resource.h"

/* See pe_resource.h. */

static int in_file(const pe_resources* v, size_t offset, size_t length) {
    return offset <= v->size && length <= v->size - offset;
}

/* A file offset for length bytes at rva, through the section table; 0 when
 * no section holds all of them in the file. */
static const uint8_t* at_rva(const pe_resources* v, uint32_t rva, uint32_t length) {
    for (uint32_t i = 0; i < v->section_count; ++i) {
        const uint8_t* s = v->sections + 40u * i;
        const uint32_t start = pe_get32(s + 12);
        const uint32_t raw_size = pe_get32(s + 16);
        const uint32_t raw = pe_get32(s + 20);
        if (rva >= start && rva - start <= raw_size && length <= raw_size - (rva - start)) {
            const size_t offset = (size_t)raw + (rva - start);
            return in_file(v, offset, length) ? v->file + offset : 0;
        }
    }
    return 0;
}

/* The entry of a resource directory (at rva, relative to the resource
 * section) with this id, or its first entry when id is 0. Returns the entry's
 * target, an offset from the resource section; the subdirectory bit tells
 * which kind. 0 when there is none. */
static uint32_t find_entry(const pe_resources* v, uint32_t directory, uint32_t id) {
    const uint8_t* d = at_rva(v, v->resource_rva + directory, 16);
    if (d == 0) {
        return 0;
    }
    const uint32_t count = pe_get16(d + 12) + pe_get16(d + 14);
    const uint8_t* entries = at_rva(v, v->resource_rva + directory + 16u, 8u * count);
    if (entries == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t name = pe_get32(entries + 8u * i);
        if (id == 0 || ((name & 0x80000000u) == 0 && name == id)) {
            return pe_get32(entries + 8u * i + 4);
        }
    }
    return 0;
}

const uint8_t* pe_resources_find(const pe_resources* v, uint32_t type, uint32_t id, uint32_t* size) {
    uint32_t target = find_entry(v, 0, type);
    if ((target & 0x80000000u) == 0) {
        return 0;
    }
    target = find_entry(v, target & 0x7fffffffu, id);
    if ((target & 0x80000000u) == 0) {
        return 0;
    }
    target = find_entry(v, target & 0x7fffffffu, 0);
    if (target == 0 || (target & 0x80000000u) != 0) {
        return 0;
    }
    const uint8_t* entry = at_rva(v, v->resource_rva + target, 16);
    if (entry == 0) {
        return 0;
    }
    *size = pe_get32(entry + 4);
    return at_rva(v, pe_get32(entry), *size);
}


int pe_resources_open(pe_resources* v, const uint8_t* file, size_t size) {
    v->file = file;
    v->size = size;
    v->resource_rva = 0;
    v->sections = 0;
    v->section_count = 0;
    if (file == 0 || !in_file(v, 0, 64) || file[0] != 'M' || file[1] != 'Z') {
        return 0;
    }
    const uint32_t pe = pe_get32(file + 60);
    if (!in_file(v, pe, 24) || pe_get32(file + pe) != 0x00004550u) {
        return 0;
    }
    const uint32_t optional_size = pe_get16(file + pe + 20);
    const size_t optional = (size_t)pe + 24u;
    if (!in_file(v, optional, optional_size) || optional_size < 2u) {
        return 0;
    }
    const uint32_t magic = pe_get16(file + optional);
    /* The data directories: after 96 bytes in PE32, 112 in PE32+; the
     * resource table is the third. */
    const uint32_t directories = magic == 0x10bu ? 96u : magic == 0x20bu ? 112u : 0u;
    if (directories == 0u || optional_size < directories + 24u) {
        return 0;
    }
    v->resource_rva = pe_get32(file + optional + directories + 16);
    v->section_count = pe_get16(file + pe + 6);
    if (v->resource_rva == 0u || !in_file(v, optional + optional_size, 40u * (size_t)v->section_count)) {
        return 0;
    }
    v->sections = file + optional + optional_size;
    return 1;
}
