/*
 * pe.h -- PE32 image parser and mapper for the Win32 subsystem.
 *
 * Portable C with no library dependency: it builds for the build host (host
 * tests and the host harness) and for winhost on SavanXP. It never allocates
 * and never touches anything but the buffers it is given.
 *
 * Two stages:
 *   1. pe_parse() reads the headers of a file in memory and checks them.
 *   2. pe_map() lays the image out in a buffer of size_of_image bytes, the
 *      way it will sit in guest memory. Every later call (relocations,
 *      imports, exports, TLS) works on that mapped buffer by RVA, and checks
 *      every RVA and length it reads against the buffer.
 *
 * Anything the subsystem does not support is rejected by name: other
 * machines, PE32+, low section alignment, writable and executable sections,
 * unknown relocation types, delay-load imports. Nothing is silently skipped.
 *
 * See docs/userland/WIN32.md.
 */
#ifndef SAVANXP_WIN32_PE_H
#define SAVANXP_WIN32_PE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PE_PAGE_SIZE 0x1000u
#define PE_MAX_SECTIONS 96u
#define PE_MAX_NAME 256u

/* Data directory indexes used by the loader. */
enum {
    PE_DIR_EXPORT = 0,
    PE_DIR_IMPORT = 1,
    PE_DIR_RESOURCE = 2,
    PE_DIR_SECURITY = 4,
    PE_DIR_BASERELOC = 5,
    PE_DIR_TLS = 9,
    PE_DIR_BOUND_IMPORT = 11,
    PE_DIR_IAT = 12,
    PE_DIR_DELAY_IMPORT = 13,
    PE_DIR_CLR = 14,
    PE_DIR_COUNT = 16,
};

/* COFF file characteristics. */
#define PE_FILE_RELOCS_STRIPPED 0x0001u
#define PE_FILE_EXECUTABLE 0x0002u
#define PE_FILE_32BIT_MACHINE 0x0100u
#define PE_FILE_DLL 0x2000u

/* Section characteristics. */
#define PE_SCN_CNT_CODE 0x00000020u
#define PE_SCN_CNT_INITIALIZED_DATA 0x00000040u
#define PE_SCN_CNT_UNINITIALIZED_DATA 0x00000080u
#define PE_SCN_MEM_EXECUTE 0x20000000u
#define PE_SCN_MEM_READ 0x40000000u
#define PE_SCN_MEM_WRITE 0x80000000u

typedef enum pe_status {
    PE_OK = 0,
    PE_ERR_TRUNCATED,         /* a header or table runs past the end of the file */
    PE_ERR_NOT_MZ,            /* no DOS header */
    PE_ERR_NOT_PE,            /* no PE signature at e_lfanew */
    PE_ERR_MACHINE,           /* not i386; value = machine */
    PE_ERR_PE32PLUS,          /* a PE32+ (64-bit) image */
    PE_ERR_OPTIONAL_HEADER,   /* bad optional header magic or size */
    PE_ERR_NOT_EXECUTABLE,    /* the executable-image flag is clear */
    PE_ERR_SECTION_COUNT,     /* zero or more than PE_MAX_SECTIONS */
    PE_ERR_ALIGNMENT,         /* section or file alignment unsupported */
    PE_ERR_IMAGE_BASE,        /* base not 64 KiB aligned, or image wraps 4 GiB */
    PE_ERR_IMAGE_SIZE,        /* size of image or headers inconsistent */
    PE_ERR_SECTION_LAYOUT,    /* section unaligned, out of order, overlapping or outside the image; index = section */
    PE_ERR_SECTION_RAW,       /* section file data outside the file; index = section */
    PE_ERR_SECTION_WX,        /* section both writable and executable; index = section */
    PE_ERR_ENTRY_POINT,       /* entry point outside an executable section */
    PE_ERR_DIRECTORY,         /* data directory outside the image; index = directory */
    PE_ERR_UNSUPPORTED_DIRECTORY, /* delay-load or CLR directory; index = directory */
    PE_ERR_BUFFER,            /* the caller's buffer is too small */
    PE_ERR_RELOCS_STRIPPED,   /* the image must move but has no relocations */
    PE_ERR_RELOC_BLOCK,       /* malformed relocation block; value = block RVA */
    PE_ERR_RELOC_TYPE,        /* unsupported relocation type; value = type */
    PE_ERR_RELOC_TARGET,      /* relocation patches outside the image; value = RVA */
    PE_ERR_IMPORT,            /* malformed import descriptor; index = descriptor */
    PE_ERR_IMPORT_BOUND,      /* bound import with no lookup table; index = descriptor */
    PE_ERR_EXPORT,            /* malformed export directory */
    PE_ERR_TLS,               /* malformed TLS directory */
    PE_ERR_STRING,            /* a name is unterminated, empty or too long; value = RVA */
    PE_ERR_CALLBACK,          /* the caller's callback refused; value = its code */
} pe_status;

typedef struct pe_error {
    pe_status status;
    uint32_t index; /* section, directory or descriptor index, when relevant */
    uint32_t value; /* machine, RVA, relocation type, ..., when relevant */
} pe_error;

typedef struct pe_section {
    char name[9];         /* NUL-terminated copy of the 8-byte name */
    uint32_t rva;
    uint32_t virtual_size; /* rounded up to the section alignment */
    uint32_t raw_offset;
    uint32_t raw_size;     /* bytes copied from the file; the rest is zero */
    uint32_t characteristics;
} pe_section;

typedef struct pe_directory {
    uint32_t rva;
    uint32_t size;
} pe_directory;

typedef struct pe_image {
    const uint8_t *file;
    size_t file_size;
    uint16_t characteristics;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint16_t section_count;
    uint32_t image_base;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t entry_rva; /* 0 when a DLL has no entry point */
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint32_t size_of_stack_reserve;
    uint32_t size_of_stack_commit;
    uint32_t size_of_heap_reserve;
    uint32_t size_of_heap_commit;
    pe_directory directories[PE_DIR_COUNT];
    pe_section sections[PE_MAX_SECTIONS];
} pe_image;

/* A mapped image: size bytes laid out by RVA. */
typedef struct pe_mapped {
    uint8_t *base;
    uint32_t size;
} pe_mapped;

/* Page protections of a mapped image, coarsest form the loader maps. */
typedef enum pe_protection {
    PE_PROT_R = 1,
    PE_PROT_RW = 3,
    PE_PROT_RX = 5,
} pe_protection;

typedef struct pe_run {
    uint32_t rva;
    uint32_t size;
    pe_protection protection;
} pe_run;

/* One imported symbol, as the import lookup table describes it. */
typedef struct pe_import {
    const char *dll;        /* inside the mapped image */
    const char *name;       /* NULL when imported by ordinal */
    uint16_t ordinal;       /* valid when name is NULL */
    uint16_t hint;          /* valid when name is not NULL */
    uint32_t iat_rva;       /* the 32-bit slot to write the address to */
    uint32_t descriptor;    /* index of the descriptor, in file order */
} pe_import;

/* The result of an export lookup. */
typedef struct pe_export {
    uint32_t rva;           /* 0 when the export is a forwarder */
    const char *forwarder;  /* "DLL.Name" or "DLL.#N", inside the mapped image */
    uint16_t ordinal;       /* the biased ordinal */
} pe_export;

typedef struct pe_tls {
    uint32_t template_rva;  /* initial data of each thread's block */
    uint32_t template_size;
    uint32_t zero_fill;     /* zero bytes after the template */
    uint32_t index_rva;     /* the 32-bit slot that receives the TLS index */
    uint32_t callbacks_rva; /* NUL-terminated array of VAs, or 0 */
} pe_tls;

/* Reads and checks the headers. The file must outlive the image. */
pe_status pe_parse(pe_image *image, const void *file, size_t file_size, pe_error *error);

/* Lays the image out at mapped->base, which holds image->size_of_image bytes:
 * headers and section data copied, everything else zero. */
pe_status pe_map(const pe_image *image, pe_mapped *mapped, pe_error *error);

/* Applies the base relocations for a move from image->image_base to new_base.
 * Moving an image without relocations fails; not moving it is always fine. */
pe_status pe_relocate(const pe_image *image, const pe_mapped *mapped, uint32_t new_base,
                      pe_error *error);

/* Writes the protection runs of the image (headers, then each section, with
 * neighbours of the same protection merged) into runs[capacity]. *count is
 * the number written. PE_ERR_BUFFER when capacity is too small. */
pe_status pe_protection_runs(const pe_image *image, pe_run *runs, uint32_t capacity,
                             uint32_t *count, pe_error *error);

/* Calls visit(context, import) for every imported symbol, descriptor by
 * descriptor in file order. A nonzero return from visit stops the walk with
 * PE_ERR_CALLBACK and that value. The bound IAT is never read. */
typedef int (*pe_import_visitor)(void *context, const pe_import *import);
pe_status pe_for_each_import(const pe_image *image, const pe_mapped *mapped,
                             pe_import_visitor visit, void *context, pe_error *error);

/* Calls visit(context, dll) once per import descriptor, in file order. */
typedef int (*pe_dll_visitor)(void *context, const char *dll, uint32_t descriptor);
pe_status pe_for_each_imported_dll(const pe_image *image, const pe_mapped *mapped,
                                   pe_dll_visitor visit, void *context, pe_error *error);

/* Export lookups. Return PE_OK and fill *result when found; *found says
 * whether the symbol exists. Malformed directories fail with PE_ERR_EXPORT. */
pe_status pe_find_export_by_name(const pe_image *image, const pe_mapped *mapped,
                                 const char *name, pe_export *result, int *found,
                                 pe_error *error);
pe_status pe_find_export_by_ordinal(const pe_image *image, const pe_mapped *mapped,
                                    uint16_t ordinal, pe_export *result, int *found,
                                    pe_error *error);

/* Reads the TLS directory. *present is 0 when there is none. Addresses in
 * the directory are VAs for current_base (call after pe_relocate). */
pe_status pe_read_tls(const pe_image *image, const pe_mapped *mapped, uint32_t current_base,
                      pe_tls *tls, int *present, pe_error *error);

/* The RVA of TLS callback number i, or 0 past the last one. A callback
 * outside an executable section fails with PE_ERR_TLS. */
pe_status pe_tls_callback(const pe_image *image, const pe_mapped *mapped, uint32_t current_base,
                          const pe_tls *tls, uint32_t i, uint32_t *rva, pe_error *error);

/* Reads and writes a 32-bit little-endian value at an RVA of the mapped
 * image. Fail with PE_ERR_BUFFER when the 4 bytes are not inside it. */
pe_status pe_read_u32(const pe_mapped *mapped, uint32_t rva, uint32_t *value);
pe_status pe_write_u32(const pe_mapped *mapped, uint32_t rva, uint32_t value);

/* Whether rva lies inside a section marked executable. */
int pe_rva_is_executable(const pe_image *image, uint32_t rva);

/* Formats a diagnostic, always NUL-terminated. Returns the length written. */
size_t pe_format_error(const pe_error *error, char *buffer, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
