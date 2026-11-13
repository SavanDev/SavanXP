#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The resources of a PE32 or PE32+ file already in memory, for the shell's
 * readers of a Windows program (sxgfx/pe_icon.h, sxgfx/pe_version.h). Every
 * offset is checked against the file's size: a damaged file finds nothing,
 * never reads outside it. No system call.
 */

/* Little-endian reads; the caller has checked the bytes are there. */
static inline uint32_t pe_get16(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t pe_get32(const uint8_t* p) {
    return pe_get16(p) | pe_get16(p + 2) << 16;
}

typedef struct pe_resources {
    const uint8_t* file;
    size_t size;
    uint32_t resource_rva;
    const uint8_t* sections;
    uint32_t section_count;
} pe_resources;

/* 1 when the file is a PE with a resource table, 0 otherwise. */
int pe_resources_open(pe_resources* r, const uint8_t* file, size_t size);
/* The data of the resource of this type and id (0: the first), its first
 * language, with its size; 0 when there is none. */
const uint8_t* pe_resources_find(const pe_resources* r, uint32_t type, uint32_t id, uint32_t* size);
