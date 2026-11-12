/*
 * pe.c -- PE32 image parser and mapper. See pe.h and docs/userland/WIN32.md.
 *
 * Every offset read from the file or the mapped image is checked in 64-bit
 * arithmetic before use, so no field can wrap a bound.
 */
#include "pe.h"

#define PE_DOS_LFANEW 0x3cu
#define PE_COFF_SIZE 20u
#define PE_OPT32_FIXED 96u
#define PE_SECTION_HEADER 40u
#define PE_IMPORT_DESCRIPTOR 20u
#define PE_EXPORT_DIRECTORY 40u
#define PE_TLS_DIRECTORY32 24u
#define PE_MACHINE_I386 0x014cu
#define PE_MAGIC_PE32 0x010bu
#define PE_MAGIC_PE32PLUS 0x020bu
#define PE_MAX_FILE_ALIGNMENT 0x10000u
#define PE_MAX_SECTION_ALIGNMENT 0x100000u
#define PE_MAX_IMAGE_SIZE 0x80000000u
#define PE_RELOC_ABSOLUTE 0u
#define PE_RELOC_HIGHLOW 3u

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int fits(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int is_power_of_two(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        dst[i] = src[i];
    }
}

static void zero_bytes(uint8_t *dst, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        dst[i] = 0;
    }
}

static pe_status fail(pe_error *error, pe_status status, uint32_t index, uint32_t value) {
    if (error != 0) {
        error->status = status;
        error->index = index;
        error->value = value;
    }
    return status;
}

static pe_status ok(pe_error *error) {
    return fail(error, PE_OK, 0, 0);
}

/* A NUL-terminated, non-empty name of at most PE_MAX_NAME bytes at rva. */
static const char *mapped_string(const pe_mapped *mapped, uint32_t rva) {
    if (rva == 0u || rva >= mapped->size) {
        return 0;
    }
    uint32_t limit = mapped->size - rva;
    if (limit > PE_MAX_NAME) {
        limit = PE_MAX_NAME;
    }
    const uint8_t *s = mapped->base + rva;
    for (uint32_t i = 0; i < limit; ++i) {
        if (s[i] == 0) {
            return i == 0 ? 0 : (const char *)s;
        }
    }
    return 0;
}

static int names_equal(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static pe_protection section_protection(uint32_t characteristics) {
    if ((characteristics & PE_SCN_MEM_EXECUTE) != 0u) {
        return PE_PROT_RX;
    }
    if ((characteristics & PE_SCN_MEM_WRITE) != 0u) {
        return PE_PROT_RW;
    }
    return PE_PROT_R;
}

int pe_rva_is_executable(const pe_image *image, uint32_t rva) {
    for (uint32_t i = 0; i < image->section_count; ++i) {
        const pe_section *s = &image->sections[i];
        if (rva >= s->rva && rva - s->rva < s->virtual_size) {
            return (s->characteristics & PE_SCN_MEM_EXECUTE) != 0u;
        }
    }
    return 0;
}

pe_status pe_read_u32(const pe_mapped *mapped, uint32_t rva, uint32_t *value) {
    if (!fits(rva, 4u, mapped->size)) {
        return PE_ERR_BUFFER;
    }
    *value = rd32(mapped->base + rva);
    return PE_OK;
}

pe_status pe_write_u32(const pe_mapped *mapped, uint32_t rva, uint32_t value) {
    if (!fits(rva, 4u, mapped->size)) {
        return PE_ERR_BUFFER;
    }
    wr32(mapped->base + rva, value);
    return PE_OK;
}

/* ---- Headers ------------------------------------------------------------ */

static pe_status parse_sections(pe_image *image, const uint8_t *table, pe_error *error) {
    uint64_t next = align_up(image->size_of_headers, image->section_alignment);
    for (uint32_t i = 0; i < image->section_count; ++i) {
        const uint8_t *h = table + (uint64_t)i * PE_SECTION_HEADER;
        pe_section *s = &image->sections[i];
        for (uint32_t c = 0; c < 8u; ++c) {
            s->name[c] = (char)h[c];
        }
        s->name[8] = 0;
        uint32_t virtual_size = rd32(h + 8);
        uint32_t rva = rd32(h + 12);
        uint32_t raw_size = rd32(h + 16);
        uint32_t raw_offset = rd32(h + 20);
        s->characteristics = rd32(h + 36);

        if (virtual_size == 0u) {
            virtual_size = raw_size;
        }
        uint64_t extent = align_up(virtual_size, image->section_alignment);
        /* Sections follow the headers and each other with no gap, as the
         * Windows loader requires, and stay inside the image. */
        if (virtual_size == 0u || rva != next || !fits(rva, extent, image->size_of_image)) {
            return fail(error, PE_ERR_SECTION_LAYOUT, i, rva);
        }
        if ((s->characteristics & (PE_SCN_MEM_WRITE | PE_SCN_MEM_EXECUTE)) ==
            (PE_SCN_MEM_WRITE | PE_SCN_MEM_EXECUTE)) {
            return fail(error, PE_ERR_SECTION_WX, i, s->characteristics);
        }
        uint32_t copied = raw_size < extent ? raw_size : (uint32_t)extent;
        if (copied != 0u && !fits(raw_offset, copied, image->file_size)) {
            return fail(error, PE_ERR_SECTION_RAW, i, raw_offset);
        }
        s->rva = rva;
        s->virtual_size = (uint32_t)extent;
        s->raw_offset = copied != 0u ? raw_offset : 0u;
        s->raw_size = copied;
        next = rva + extent;
    }
    return ok(error);
}

pe_status pe_parse(pe_image *image, const void *file, size_t file_size, pe_error *error) {
    const uint8_t *f = (const uint8_t *)file;
    zero_bytes((uint8_t *)image, (uint32_t)sizeof(*image));
    image->file = f;
    image->file_size = file_size;

    if (file_size < 0x40u) {
        return fail(error, PE_ERR_TRUNCATED, 0, 0);
    }
    if (f[0] != 'M' || f[1] != 'Z') {
        return fail(error, PE_ERR_NOT_MZ, 0, 0);
    }
    uint32_t lfanew = rd32(f + PE_DOS_LFANEW);
    if (!fits(lfanew, 4u + PE_COFF_SIZE, file_size)) {
        return fail(error, PE_ERR_TRUNCATED, 0, lfanew);
    }
    const uint8_t *pe = f + lfanew;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) {
        return fail(error, PE_ERR_NOT_PE, 0, lfanew);
    }
    const uint8_t *coff = pe + 4;
    uint16_t machine = rd16(coff);
    uint16_t section_count = rd16(coff + 2);
    uint16_t optional_size = rd16(coff + 16);
    image->characteristics = rd16(coff + 18);

    uint64_t optional_offset = (uint64_t)lfanew + 4u + PE_COFF_SIZE;
    if (!fits(optional_offset, 2u, file_size)) {
        return fail(error, PE_ERR_TRUNCATED, 0, 0);
    }
    const uint8_t *opt = f + optional_offset;
    uint16_t magic = rd16(opt);
    if (magic == PE_MAGIC_PE32PLUS) {
        return fail(error, PE_ERR_PE32PLUS, 0, magic);
    }
    if (machine != PE_MACHINE_I386) {
        return fail(error, PE_ERR_MACHINE, 0, machine);
    }
    if (magic != PE_MAGIC_PE32 || optional_size < PE_OPT32_FIXED) {
        return fail(error, PE_ERR_OPTIONAL_HEADER, 0, magic);
    }
    if (!fits(optional_offset, optional_size, file_size)) {
        return fail(error, PE_ERR_TRUNCATED, 0, 0);
    }
    if ((image->characteristics & PE_FILE_EXECUTABLE) == 0u) {
        return fail(error, PE_ERR_NOT_EXECUTABLE, 0, image->characteristics);
    }
    if (section_count == 0u || section_count > PE_MAX_SECTIONS) {
        return fail(error, PE_ERR_SECTION_COUNT, 0, section_count);
    }
    image->section_count = section_count;

    image->entry_rva = rd32(opt + 16);
    image->image_base = rd32(opt + 28);
    image->section_alignment = rd32(opt + 32);
    image->file_alignment = rd32(opt + 36);
    uint32_t size_of_image = rd32(opt + 56);
    image->size_of_headers = rd32(opt + 60);
    image->subsystem = rd16(opt + 68);
    image->dll_characteristics = rd16(opt + 70);
    image->size_of_stack_reserve = rd32(opt + 72);
    image->size_of_stack_commit = rd32(opt + 76);
    image->size_of_heap_reserve = rd32(opt + 80);
    image->size_of_heap_commit = rd32(opt + 84);
    uint32_t directory_count = rd32(opt + 92);
    if (directory_count > PE_DIR_COUNT) {
        directory_count = PE_DIR_COUNT;
    }
    if (PE_OPT32_FIXED + 8u * directory_count > optional_size) {
        return fail(error, PE_ERR_OPTIONAL_HEADER, 0, directory_count);
    }
    for (uint32_t i = 0; i < directory_count; ++i) {
        image->directories[i].rva = rd32(opt + PE_OPT32_FIXED + 8u * i);
        image->directories[i].size = rd32(opt + PE_OPT32_FIXED + 8u * i + 4u);
    }

    /* Only page-sized or larger section alignment: low-alignment images map
     * their headers and sections with one protection, which the loader does
     * not do. */
    uint32_t sa = image->section_alignment;
    uint32_t fa = image->file_alignment;
    if (!is_power_of_two(sa) || sa < PE_PAGE_SIZE || sa > PE_MAX_SECTION_ALIGNMENT ||
        !is_power_of_two(fa) || fa < 0x200u || fa > PE_MAX_FILE_ALIGNMENT || fa > sa) {
        return fail(error, PE_ERR_ALIGNMENT, 0, sa);
    }
    uint64_t rounded_size = align_up(size_of_image, sa);
    if (size_of_image == 0u || rounded_size > PE_MAX_IMAGE_SIZE) {
        return fail(error, PE_ERR_IMAGE_SIZE, 0, size_of_image);
    }
    image->size_of_image = (uint32_t)rounded_size;
    if ((image->image_base & 0xffffu) != 0u ||
        (uint64_t)image->image_base + rounded_size > 0x100000000ull) {
        return fail(error, PE_ERR_IMAGE_BASE, 0, image->image_base);
    }

    uint64_t table_offset = optional_offset + optional_size;
    uint64_t table_size = (uint64_t)section_count * PE_SECTION_HEADER;
    if (!fits(table_offset, table_size, file_size)) {
        return fail(error, PE_ERR_TRUNCATED, 0, (uint32_t)table_offset);
    }
    if (image->size_of_headers < table_offset + table_size ||
        image->size_of_headers > file_size ||
        align_up(image->size_of_headers, sa) > image->size_of_image) {
        return fail(error, PE_ERR_IMAGE_SIZE, 0, image->size_of_headers);
    }

    pe_status status = parse_sections(image, f + table_offset, error);
    if (status != PE_OK) {
        return status;
    }

    if (image->entry_rva != 0u) {
        if (!pe_rva_is_executable(image, image->entry_rva)) {
            return fail(error, PE_ERR_ENTRY_POINT, 0, image->entry_rva);
        }
    } else if ((image->characteristics & PE_FILE_DLL) == 0u) {
        return fail(error, PE_ERR_ENTRY_POINT, 0, 0);
    }

    for (uint32_t i = 0; i < PE_DIR_COUNT; ++i) {
        const pe_directory *d = &image->directories[i];
        if (d->size == 0u || i == PE_DIR_SECURITY) {
            continue; /* the security directory holds a file offset */
        }
        if (i == PE_DIR_DELAY_IMPORT || i == PE_DIR_CLR) {
            return fail(error, PE_ERR_UNSUPPORTED_DIRECTORY, i, d->rva);
        }
        if (d->rva == 0u || !fits(d->rva, d->size, image->size_of_image)) {
            return fail(error, PE_ERR_DIRECTORY, i, d->rva);
        }
    }
    return ok(error);
}

/* ---- Mapping and relocation --------------------------------------------- */

pe_status pe_map(const pe_image *image, pe_mapped *mapped, pe_error *error) {
    if (mapped->size < image->size_of_image) {
        return fail(error, PE_ERR_BUFFER, 0, image->size_of_image);
    }
    zero_bytes(mapped->base, mapped->size);
    copy_bytes(mapped->base, image->file, image->size_of_headers);
    for (uint32_t i = 0; i < image->section_count; ++i) {
        const pe_section *s = &image->sections[i];
        copy_bytes(mapped->base + s->rva, image->file + s->raw_offset, s->raw_size);
    }
    mapped->size = image->size_of_image;
    return ok(error);
}

pe_status pe_relocate(const pe_image *image, const pe_mapped *mapped, uint32_t new_base,
                      pe_error *error) {
    uint32_t delta = new_base - image->image_base;
    if (delta == 0u) {
        return ok(error);
    }
    if ((new_base & 0xffffu) != 0u ||
        (uint64_t)new_base + image->size_of_image > 0x100000000ull) {
        return fail(error, PE_ERR_IMAGE_BASE, 0, new_base);
    }
    const pe_directory *d = &image->directories[PE_DIR_BASERELOC];
    if ((image->characteristics & PE_FILE_RELOCS_STRIPPED) != 0u || d->size == 0u) {
        return fail(error, PE_ERR_RELOCS_STRIPPED, 0, image->image_base);
    }
    uint64_t position = d->rva;
    uint64_t end = (uint64_t)d->rva + d->size;
    while (position < end) {
        if (end - position < 8u) {
            return fail(error, PE_ERR_RELOC_BLOCK, 0, (uint32_t)position);
        }
        const uint8_t *block = mapped->base + position;
        uint32_t page = rd32(block);
        uint32_t block_size = rd32(block + 4);
        if (block_size < 8u || (block_size & 1u) != 0u || block_size > end - position) {
            return fail(error, PE_ERR_RELOC_BLOCK, 0, (uint32_t)position);
        }
        uint32_t entries = (block_size - 8u) / 2u;
        for (uint32_t i = 0; i < entries; ++i) {
            uint16_t entry = rd16(block + 8u + 2u * i);
            uint32_t type = entry >> 12;
            uint64_t target = (uint64_t)page + (entry & 0x0fffu);
            if (type == PE_RELOC_ABSOLUTE) {
                continue;
            }
            if (type != PE_RELOC_HIGHLOW) {
                return fail(error, PE_ERR_RELOC_TYPE, 0, type);
            }
            if (!fits(target, 4u, image->size_of_image)) {
                return fail(error, PE_ERR_RELOC_TARGET, 0, (uint32_t)target);
            }
            uint8_t *slot = mapped->base + target;
            wr32(slot, rd32(slot) + delta);
        }
        position += block_size;
    }
    return ok(error);
}

pe_status pe_protection_runs(const pe_image *image, pe_run *runs, uint32_t capacity,
                             uint32_t *count, pe_error *error) {
    uint32_t n = 0;
    uint32_t headers = (uint32_t)align_up(image->size_of_headers, image->section_alignment);
    pe_run pending[PE_MAX_SECTIONS + 2u];
    pending[n++] = (pe_run){0u, headers, PE_PROT_R};
    uint32_t end = headers;
    for (uint32_t i = 0; i < image->section_count; ++i) {
        const pe_section *s = &image->sections[i];
        pending[n++] = (pe_run){s->rva, s->virtual_size, section_protection(s->characteristics)};
        end = s->rva + s->virtual_size;
    }
    if (end < image->size_of_image) {
        pending[n++] = (pe_run){end, image->size_of_image - end, PE_PROT_R};
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (written != 0u && runs[written - 1u].protection == pending[i].protection) {
            runs[written - 1u].size += pending[i].size;
            continue;
        }
        if (written == capacity) {
            *count = 0;
            return fail(error, PE_ERR_BUFFER, 0, capacity);
        }
        runs[written++] = pending[i];
    }
    *count = written;
    return ok(error);
}

/* ---- Imports ------------------------------------------------------------ */

typedef struct import_descriptor {
    uint32_t lookup;
    uint32_t time_date_stamp;
    uint32_t name;
    uint32_t iat;
} import_descriptor;

/* Reads descriptor i. Returns 0 at the terminator, 1 for a descriptor, and
 * -1 (with *error set) when it is malformed. */
static int read_descriptor(const pe_image *image, const pe_mapped *mapped, uint32_t i,
                           import_descriptor *out, const char **dll, pe_error *error) {
    uint64_t at = (uint64_t)image->directories[PE_DIR_IMPORT].rva +
                  (uint64_t)i * PE_IMPORT_DESCRIPTOR;
    if (!fits(at, PE_IMPORT_DESCRIPTOR, mapped->size)) {
        fail(error, PE_ERR_IMPORT, i, (uint32_t)at);
        return -1;
    }
    const uint8_t *d = mapped->base + at;
    uint32_t original_first_thunk = rd32(d);
    out->time_date_stamp = rd32(d + 4);
    out->name = rd32(d + 12);
    out->iat = rd32(d + 16);
    if (out->name == 0u && out->iat == 0u) {
        return 0;
    }
    *dll = mapped_string(mapped, out->name);
    if (*dll == 0) {
        fail(error, PE_ERR_STRING, i, out->name);
        return -1;
    }
    if (out->iat == 0u) {
        fail(error, PE_ERR_IMPORT, i, (uint32_t)at);
        return -1;
    }
    /* A bound import without its lookup table cannot be resolved again. */
    if (original_first_thunk == 0u && out->time_date_stamp != 0u) {
        fail(error, PE_ERR_IMPORT_BOUND, i, (uint32_t)at);
        return -1;
    }
    out->lookup = original_first_thunk != 0u ? original_first_thunk : out->iat;
    return 1;
}

pe_status pe_for_each_imported_dll(const pe_image *image, const pe_mapped *mapped,
                                   pe_dll_visitor visit, void *context, pe_error *error) {
    if (image->directories[PE_DIR_IMPORT].size == 0u) {
        return ok(error);
    }
    for (uint32_t i = 0;; ++i) {
        import_descriptor d;
        const char *dll = 0;
        int r = read_descriptor(image, mapped, i, &d, &dll, error);
        if (r < 0) {
            return error != 0 ? error->status : PE_ERR_IMPORT;
        }
        if (r == 0) {
            return ok(error);
        }
        int code = visit(context, dll, i);
        if (code != 0) {
            return fail(error, PE_ERR_CALLBACK, i, (uint32_t)code);
        }
    }
}

pe_status pe_for_each_import(const pe_image *image, const pe_mapped *mapped,
                             pe_import_visitor visit, void *context, pe_error *error) {
    if (image->directories[PE_DIR_IMPORT].size == 0u) {
        return ok(error);
    }
    for (uint32_t i = 0;; ++i) {
        import_descriptor d;
        const char *dll = 0;
        int r = read_descriptor(image, mapped, i, &d, &dll, error);
        if (r < 0) {
            return error != 0 ? error->status : PE_ERR_IMPORT;
        }
        if (r == 0) {
            return ok(error);
        }
        for (uint32_t j = 0;; ++j) {
            uint64_t lookup_at = (uint64_t)d.lookup + 4u * (uint64_t)j;
            uint64_t iat_at = (uint64_t)d.iat + 4u * (uint64_t)j;
            if (!fits(lookup_at, 4u, mapped->size)) {
                return fail(error, PE_ERR_IMPORT, i, (uint32_t)lookup_at);
            }
            uint32_t entry = rd32(mapped->base + lookup_at);
            if (entry == 0u) {
                break;
            }
            if (!fits(iat_at, 4u, mapped->size)) {
                return fail(error, PE_ERR_IMPORT, i, (uint32_t)iat_at);
            }
            pe_import import;
            import.dll = dll;
            import.iat_rva = (uint32_t)iat_at;
            import.descriptor = i;
            if ((entry & 0x80000000u) != 0u) {
                if ((entry & 0x7fff0000u) != 0u) {
                    return fail(error, PE_ERR_IMPORT, i, entry);
                }
                import.name = 0;
                import.ordinal = (uint16_t)entry;
                import.hint = 0;
            } else {
                if (!fits(entry, 3u, mapped->size)) {
                    return fail(error, PE_ERR_IMPORT, i, entry);
                }
                import.hint = rd16(mapped->base + entry);
                import.name = mapped_string(mapped, entry + 2u);
                import.ordinal = 0;
                if (import.name == 0) {
                    return fail(error, PE_ERR_STRING, i, entry + 2u);
                }
            }
            int code = visit(context, &import);
            if (code != 0) {
                return fail(error, PE_ERR_CALLBACK, i, (uint32_t)code);
            }
        }
    }
}

/* ---- Exports ------------------------------------------------------------ */

typedef struct export_table {
    uint32_t base;
    uint32_t function_count;
    uint32_t name_count;
    uint32_t functions;
    uint32_t names;
    uint32_t ordinals;
} export_table;

/* Returns 0 when there is no export directory, 1 when *table is valid. */
static int read_exports(const pe_image *image, const pe_mapped *mapped, export_table *table,
                        pe_error *error, pe_status *status) {
    const pe_directory *d = &image->directories[PE_DIR_EXPORT];
    *status = PE_OK;
    if (d->size == 0u) {
        return 0;
    }
    if (d->size < PE_EXPORT_DIRECTORY || !fits(d->rva, PE_EXPORT_DIRECTORY, mapped->size)) {
        *status = fail(error, PE_ERR_EXPORT, 0, d->rva);
        return 0;
    }
    const uint8_t *e = mapped->base + d->rva;
    table->base = rd32(e + 16);
    table->function_count = rd32(e + 20);
    table->name_count = rd32(e + 24);
    table->functions = rd32(e + 28);
    table->names = rd32(e + 32);
    table->ordinals = rd32(e + 36);
    if (table->base > 0xffffu || table->function_count > 0x10000u - table->base ||
        !fits(table->functions, 4ull * table->function_count, mapped->size) ||
        !fits(table->names, 4ull * table->name_count, mapped->size) ||
        !fits(table->ordinals, 2ull * table->name_count, mapped->size)) {
        *status = fail(error, PE_ERR_EXPORT, 0, d->rva);
        return 0;
    }
    return 1;
}

static pe_status resolve_export(const pe_image *image, const pe_mapped *mapped,
                                const export_table *table, uint32_t index, pe_export *result,
                                int *found, pe_error *error) {
    const pe_directory *d = &image->directories[PE_DIR_EXPORT];
    uint32_t rva = rd32(mapped->base + table->functions + 4u * index);
    *found = 0;
    if (rva == 0u) {
        return ok(error);
    }
    result->ordinal = (uint16_t)(table->base + index);
    if (rva >= d->rva && rva - d->rva < d->size) {
        const char *forwarder = mapped_string(mapped, rva);
        const char *dot = forwarder;
        while (dot != 0 && *dot != 0 && *dot != '.') {
            ++dot;
        }
        if (forwarder == 0 || *dot != '.' || dot == forwarder || dot[1] == 0) {
            return fail(error, PE_ERR_EXPORT, index, rva);
        }
        result->rva = 0;
        result->forwarder = forwarder;
    } else {
        if (rva >= image->size_of_image) {
            return fail(error, PE_ERR_EXPORT, index, rva);
        }
        result->rva = rva;
        result->forwarder = 0;
    }
    *found = 1;
    return ok(error);
}

pe_status pe_find_export_by_name(const pe_image *image, const pe_mapped *mapped,
                                 const char *name, pe_export *result, int *found,
                                 pe_error *error) {
    export_table table;
    pe_status status;
    *found = 0;
    if (!read_exports(image, mapped, &table, error, &status)) {
        return status == PE_OK ? ok(error) : status;
    }
    for (uint32_t i = 0; i < table.name_count; ++i) {
        uint32_t name_rva = rd32(mapped->base + table.names + 4u * i);
        const char *candidate = mapped_string(mapped, name_rva);
        if (candidate == 0) {
            return fail(error, PE_ERR_STRING, i, name_rva);
        }
        if (names_equal(candidate, name)) {
            uint32_t index = rd16(mapped->base + table.ordinals + 2u * i);
            if (index >= table.function_count) {
                return fail(error, PE_ERR_EXPORT, i, index);
            }
            return resolve_export(image, mapped, &table, index, result, found, error);
        }
    }
    return ok(error);
}

pe_status pe_find_export_by_ordinal(const pe_image *image, const pe_mapped *mapped,
                                    uint16_t ordinal, pe_export *result, int *found,
                                    pe_error *error) {
    export_table table;
    pe_status status;
    *found = 0;
    if (!read_exports(image, mapped, &table, error, &status)) {
        return status == PE_OK ? ok(error) : status;
    }
    if (ordinal < table.base || ordinal - table.base >= table.function_count) {
        return ok(error);
    }
    return resolve_export(image, mapped, &table, ordinal - table.base, result, found, error);
}

/* ---- TLS ---------------------------------------------------------------- */

static int va_to_rva(uint32_t va, uint32_t base, uint32_t size, uint32_t length, uint32_t *rva) {
    if (va < base || !fits(va - base, length, size)) {
        return 0;
    }
    *rva = va - base;
    return 1;
}

pe_status pe_read_tls(const pe_image *image, const pe_mapped *mapped, uint32_t current_base,
                      pe_tls *tls, int *present, pe_error *error) {
    const pe_directory *d = &image->directories[PE_DIR_TLS];
    *present = 0;
    if (d->size == 0u) {
        return ok(error);
    }
    if (d->size < PE_TLS_DIRECTORY32 || !fits(d->rva, PE_TLS_DIRECTORY32, mapped->size)) {
        return fail(error, PE_ERR_TLS, 0, d->rva);
    }
    const uint8_t *t = mapped->base + d->rva;
    uint32_t start = rd32(t);
    uint32_t end = rd32(t + 4);
    uint32_t index = rd32(t + 8);
    uint32_t callbacks = rd32(t + 12);
    tls->zero_fill = rd32(t + 16);
    tls->template_rva = 0;
    tls->template_size = 0;
    if (start != 0u || end != 0u) {
        if (end < start || !va_to_rva(start, current_base, mapped->size, end - start,
                                      &tls->template_rva)) {
            return fail(error, PE_ERR_TLS, 0, start);
        }
        tls->template_size = end - start;
    }
    if (!va_to_rva(index, current_base, mapped->size, 4u, &tls->index_rva)) {
        return fail(error, PE_ERR_TLS, 0, index);
    }
    tls->callbacks_rva = 0;
    if (callbacks != 0u &&
        !va_to_rva(callbacks, current_base, mapped->size, 4u, &tls->callbacks_rva)) {
        return fail(error, PE_ERR_TLS, 0, callbacks);
    }
    if (tls->zero_fill > 0x01000000u) {
        return fail(error, PE_ERR_TLS, 0, tls->zero_fill);
    }
    *present = 1;
    return ok(error);
}

pe_status pe_tls_callback(const pe_image *image, const pe_mapped *mapped, uint32_t current_base,
                          const pe_tls *tls, uint32_t i, uint32_t *rva, pe_error *error) {
    *rva = 0;
    if (tls->callbacks_rva == 0u) {
        return ok(error);
    }
    uint64_t slot = (uint64_t)tls->callbacks_rva + 4ull * i;
    if (!fits(slot, 4u, mapped->size)) {
        return fail(error, PE_ERR_TLS, i, (uint32_t)slot);
    }
    uint32_t va = rd32(mapped->base + slot);
    if (va == 0u) {
        return ok(error);
    }
    uint32_t callback;
    if (!va_to_rva(va, current_base, mapped->size, 1u, &callback) ||
        !pe_rva_is_executable(image, callback)) {
        return fail(error, PE_ERR_TLS, i, va);
    }
    *rva = callback;
    return ok(error);
}

/* ---- Diagnostics -------------------------------------------------------- */

typedef struct text {
    char *buffer;
    size_t capacity;
    size_t length;
} text;

static void put(text *t, const char *s) {
    while (*s != 0) {
        if (t->length + 1u < t->capacity) {
            t->buffer[t->length] = *s;
        }
        ++t->length;
        ++s;
    }
}

static void put_hex(text *t, uint32_t value) {
    char digits[11];
    digits[0] = '0';
    digits[1] = 'x';
    for (int i = 0; i < 8; ++i) {
        uint32_t nibble = (value >> (28 - 4 * i)) & 0xfu;
        digits[2 + i] = (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u);
    }
    digits[10] = 0;
    put(t, digits);
}

static void put_dec(text *t, uint32_t value) {
    char digits[11];
    int n = 10;
    digits[n] = 0;
    do {
        digits[--n] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    put(t, digits + n);
}

size_t pe_format_error(const pe_error *error, char *buffer, size_t capacity) {
    text t = {buffer, capacity, 0};
    switch (error->status) {
    case PE_OK: put(&t, "no error"); break;
    case PE_ERR_TRUNCATED: put(&t, "file truncated at "); put_hex(&t, error->value); break;
    case PE_ERR_NOT_MZ: put(&t, "not an executable: no MZ header"); break;
    case PE_ERR_NOT_PE: put(&t, "not a PE image: no PE signature at "); put_hex(&t, error->value); break;
    case PE_ERR_MACHINE: put(&t, "unsupported machine "); put_hex(&t, error->value); put(&t, " (only i386)"); break;
    case PE_ERR_PE32PLUS: put(&t, "PE32+ (64-bit) image is not supported"); break;
    case PE_ERR_OPTIONAL_HEADER: put(&t, "malformed optional header"); break;
    case PE_ERR_NOT_EXECUTABLE: put(&t, "image is not marked executable"); break;
    case PE_ERR_SECTION_COUNT: put(&t, "unsupported section count "); put_dec(&t, error->value); break;
    case PE_ERR_ALIGNMENT: put(&t, "unsupported section or file alignment"); break;
    case PE_ERR_IMAGE_BASE: put(&t, "invalid image base "); put_hex(&t, error->value); break;
    case PE_ERR_IMAGE_SIZE: put(&t, "inconsistent image or header size "); put_hex(&t, error->value); break;
    case PE_ERR_SECTION_LAYOUT: put(&t, "section "); put_dec(&t, error->index); put(&t, " is unaligned, overlapping or outside the image"); break;
    case PE_ERR_SECTION_RAW: put(&t, "section "); put_dec(&t, error->index); put(&t, " data lies outside the file"); break;
    case PE_ERR_SECTION_WX: put(&t, "section "); put_dec(&t, error->index); put(&t, " is both writable and executable"); break;
    case PE_ERR_ENTRY_POINT: put(&t, "entry point "); put_hex(&t, error->value); put(&t, " is outside an executable section"); break;
    case PE_ERR_DIRECTORY: put(&t, "data directory "); put_dec(&t, error->index); put(&t, " lies outside the image"); break;
    case PE_ERR_UNSUPPORTED_DIRECTORY: put(&t, error->index == PE_DIR_DELAY_IMPORT ? "delay-load imports are not supported" : "managed (CLR) images are not supported"); break;
    case PE_ERR_BUFFER: put(&t, "buffer too small"); break;
    case PE_ERR_RELOCS_STRIPPED: put(&t, "image must load at "); put_hex(&t, error->value); put(&t, " and has no relocations"); break;
    case PE_ERR_RELOC_BLOCK: put(&t, "malformed relocation block at "); put_hex(&t, error->value); break;
    case PE_ERR_RELOC_TYPE: put(&t, "unsupported relocation type "); put_dec(&t, error->value); break;
    case PE_ERR_RELOC_TARGET: put(&t, "relocation outside the image at "); put_hex(&t, error->value); break;
    case PE_ERR_IMPORT: put(&t, "malformed import descriptor "); put_dec(&t, error->index); break;
    case PE_ERR_IMPORT_BOUND: put(&t, "bound import descriptor "); put_dec(&t, error->index); put(&t, " has no lookup table"); break;
    case PE_ERR_EXPORT: put(&t, "malformed export directory"); break;
    case PE_ERR_TLS: put(&t, "malformed TLS directory"); break;
    case PE_ERR_STRING: put(&t, "invalid name at "); put_hex(&t, error->value); break;
    case PE_ERR_CALLBACK: put(&t, "stopped by the caller with "); put_dec(&t, error->value); break;
    default: put(&t, "unknown error"); break;
    }
    if (capacity != 0u) {
        buffer[t.length < capacity ? t.length : capacity - 1u] = 0;
    }
    return t.length < capacity ? t.length : (capacity != 0u ? capacity - 1u : 0u);
}
