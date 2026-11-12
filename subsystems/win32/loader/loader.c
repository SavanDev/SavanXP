/*
 * loader.c -- The Win32 process loader. See loader.h and
 * docs/userland/WIN32.md, "The loader".
 */
#include "loader.h"

#include "sxw_abi.h"

#define SXW_GRANULARITY 0x10000u
#define SXW_MAX_FORWARDS 8u
#define SXW_MIN_STACK 0x10000u
#define SXW_MAX_STACK 0x01000000u
#define SXW_MAX_TLS_CALLBACKS 64u

/* ---- Small helpers, no libc --------------------------------------------- */

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint8_t *guest(uint32_t address) {
    return (uint8_t *)(uintptr_t)address;
}

static void put32(uint32_t address, uint32_t value) {
    uint8_t *p = guest(address);
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int same_name(const char *a, const char *b) {
    while (*a != 0 && lower(*a) == lower(*b)) {
        ++a;
        ++b;
    }
    return lower(*a) == lower(*b);
}

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p != 0; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

/* ---- Diagnostics -------------------------------------------------------- */

static void d_reset(sxw_process *p) {
    p->diagnostic[0] = 0;
    p->status = 0;
}

/* The first failure sets the status; the ones that unwind after it keep it. */
static void fail_status(sxw_process *p, uint32_t status) {
    if (p->status == 0u) {
        p->status = status;
    }
}

static void d_put(sxw_process *p, const char *s) {
    size_t n = 0;
    while (p->diagnostic[n] != 0) {
        ++n;
    }
    while (*s != 0 && n + 1u < SXW_DIAGNOSTIC) {
        p->diagnostic[n++] = *s++;
    }
    p->diagnostic[n] = 0;
}

static void d_dec(sxw_process *p, uint32_t value) {
    char digits[11];
    int n = 10;
    digits[n] = 0;
    do {
        digits[--n] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    d_put(p, digits + n);
}

static void fail_status(sxw_process *p, uint32_t status);

/* A refusal by the parser: the image is malformed or unsupported. */
static void d_pe(sxw_process *p, const char *module, const pe_error *error) {
    char text[160];
    fail_status(p, SXW_STATUS_INVALID_IMAGE_FORMAT);
    pe_format_error(error, text, sizeof(text));
    d_put(p, module);
    d_put(p, ": ");
    d_put(p, text);
}

/* ---- Regions ------------------------------------------------------------ */

static int map_at(sxw_process *p, uint32_t base, uint32_t size) {
    if (p->region_count == SXW_MAX_REGIONS) {
        return -1;
    }
    if (p->platform.map(p->platform.context, base, size) != 0) {
        return 1;
    }
    p->regions[p->region_count].base = base;
    p->regions[p->region_count].size = size;
    ++p->region_count;
    return 0;
}

/* Maps size bytes at preferred when it is free, else at the first free
 * 64 KiB-aligned address from low. */
static int place(sxw_process *p, uint32_t size, uint32_t preferred, uint32_t low, uint32_t *base) {
    if (size == 0u || size > SXW_GUEST_HIGH - SXW_GUEST_LOW) {
        return 1;
    }
    if (preferred >= SXW_GUEST_LOW && preferred <= SXW_GUEST_HIGH - size) {
        int r = map_at(p, preferred, size);
        if (r <= 0) {
            *base = preferred;
            return r;
        }
    }
    for (uint32_t at = low; at <= SXW_GUEST_HIGH - size; at += SXW_GRANULARITY) {
        int r = map_at(p, at, size);
        if (r <= 0) {
            *base = at;
            return r;
        }
    }
    return 1;
}

static int map_region(sxw_process *p, uint32_t size, uint32_t *base) {
    if (place(p, align_up(size, SXW_GRANULARITY), 0, SXW_GUEST_LOW, base) != 0) {
        fail_status(p, SXW_STATUS_NO_MEMORY);
        d_put(p, "out of guest address space");
        return -1;
    }
    return 0;
}

/* ---- Modules ------------------------------------------------------------ */

static pe_mapped mapped_of(const sxw_module *m) {
    pe_mapped mapped = {guest(m->base), m->image.size_of_image};
    return mapped;
}

static int find_index(const sxw_process *p, const char *name, uint32_t *index) {
    for (uint32_t i = 0; i < p->module_count; ++i) {
        if (same_name(p->modules[i].name, name)) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

const sxw_module *sxw_find_module(const sxw_process *p, const char *name) {
    uint32_t index;
    return find_index(p, name, &index) ? &p->modules[index] : 0;
}

static int load_module(sxw_process *p, const char *name, const char *requester, uint32_t *index);

typedef struct dll_visit {
    sxw_process *process;
    const char *requester;
} dll_visit;

static int visit_dll(void *context, const char *dll, uint32_t descriptor) {
    (void)descriptor;
    dll_visit *v = (dll_visit *)context;
    uint32_t index;
    return load_module(v->process, dll, v->requester, &index) == 0 ? 0 : 1;
}

/* Maps one image and, depth first, the DLLs it imports from. */
static int load_module(sxw_process *p, const char *name, const char *requester, uint32_t *index) {
    const char *file_name = base_name(name);
    if (find_index(p, file_name, index)) {
        return 0;
    }
    uint32_t length = 0;
    while (file_name[length] != 0) {
        ++length;
    }
    if (length == 0u || length >= SXW_MODULE_NAME) {
        fail_status(p, SXW_STATUS_DLL_NOT_FOUND);
        d_put(p, requester);
        d_put(p, ": invalid DLL name");
        return -1;
    }
    if (p->module_count == SXW_MAX_MODULES) {
        fail_status(p, SXW_STATUS_NO_MEMORY);
        d_put(p, "too many modules");
        return -1;
    }
    sxw_module *m = &p->modules[p->module_count];
    for (uint32_t i = 0; i <= length; ++i) {
        m->name[i] = lower(file_name[i]);
    }
    m->bound = 0;
    m->handle = 0;

    const uint8_t *data;
    size_t size;
    if (p->platform.open_image(p->platform.context, name, &data, &size, &m->handle) != 0) {
        fail_status(p, SXW_STATUS_DLL_NOT_FOUND);
        if (requester != 0) {
            d_put(p, requester);
            d_put(p, ": missing DLL ");
        } else {
            d_put(p, "cannot open ");
        }
        d_put(p, name);
        return -1;
    }
    /* Counted from here, so a failure below closes the file. */
    *index = p->module_count++;

    pe_error error;
    if (pe_parse(&m->image, data, size, &error) != PE_OK) {
        d_pe(p, m->name, &error);
        return -1;
    }
    int is_dll = (m->image.characteristics & PE_FILE_DLL) != 0u;
    if (requester != 0 && !is_dll) {
        d_put(p, requester);
        d_put(p, ": ");
        d_put(p, m->name);
        d_put(p, " is not a DLL");
        return -1;
    }
    if (requester == 0 && is_dll) {
        d_put(p, m->name);
        d_put(p, " is a DLL, not an executable");
        return -1;
    }
    if (place(p, m->image.size_of_image, m->image.image_base, SXW_RELOCATED_LOW, &m->base) != 0) {
        fail_status(p, SXW_STATUS_NO_MEMORY);
        d_put(p, m->name);
        d_put(p, ": no room in the guest address space");
        return -1;
    }
    pe_mapped mapped = mapped_of(m);
    if (pe_map(&m->image, &mapped, &error) != PE_OK ||
        pe_relocate(&m->image, &mapped, m->base, &error) != PE_OK) {
        d_pe(p, m->name, &error);
        return -1;
    }

    dll_visit visit = {p, m->name};
    pe_status status = pe_for_each_imported_dll(&m->image, &mapped, visit_dll, &visit, &error);
    if (status == PE_ERR_CALLBACK) {
        return -1; /* the nested load named the cause */
    }
    if (status != PE_OK) {
        d_pe(p, m->name, &error);
        return -1;
    }
    return 0;
}

/* ---- Binding ------------------------------------------------------------ */

static int resolve(sxw_process *p, const char *requester, const char *dll, const char *name,
                   uint16_t ordinal, uint32_t depth, uint32_t *address, uint32_t *provider) {
    uint32_t index;
    if (load_module(p, dll, requester, &index) != 0) {
        return -1;
    }
    const sxw_module *m = &p->modules[index];
    pe_mapped mapped = mapped_of(m);
    pe_export result;
    pe_error error;
    int found = 0;
    pe_status status = name != 0
                           ? pe_find_export_by_name(&m->image, &mapped, name, &result, &found, &error)
                           : pe_find_export_by_ordinal(&m->image, &mapped, ordinal, &result, &found, &error);
    if (status != PE_OK) {
        d_pe(p, m->name, &error);
        return -1;
    }
    if (!found) {
        fail_status(p, name != 0 ? SXW_STATUS_ENTRYPOINT_NOT_FOUND : SXW_STATUS_ORDINAL_NOT_FOUND);
        d_put(p, requester);
        d_put(p, ": ");
        d_put(p, m->name);
        if (name != 0) {
            d_put(p, "!");
            d_put(p, name);
        } else {
            d_put(p, " ordinal ");
            d_dec(p, ordinal);
        }
        d_put(p, " is not exported");
        return -1;
    }
    if (result.forwarder == 0) {
        *address = m->base + result.rva;
        *provider = index;
        return 0;
    }
    if (depth == SXW_MAX_FORWARDS) {
        d_put(p, m->name);
        d_put(p, ": forwarder chain too long at ");
        d_put(p, result.forwarder);
        return -1;
    }
    /* "DLL.Name" or "DLL.#N": the DLL part takes ".dll". */
    char target[SXW_MODULE_NAME];
    uint32_t n = 0;
    const char *f = result.forwarder;
    while (*f != '.' && n + 5u < SXW_MODULE_NAME) {
        target[n++] = *f++;
    }
    if (*f != '.') {
        d_put(p, m->name);
        d_put(p, ": forwarder name too long");
        return -1;
    }
    const char *suffix = ".dll";
    for (uint32_t i = 0; i <= 4u; ++i) {
        target[n + i] = suffix[i];
    }
    ++f;
    if (*f == '#') {
        uint32_t value = 0;
        for (++f; *f >= '0' && *f <= '9' && value <= 0xffffu; ++f) {
            value = value * 10u + (uint32_t)(*f - '0');
        }
        if (*f != 0 || value == 0u || value > 0xffffu) {
            d_put(p, m->name);
            d_put(p, ": malformed forwarder ");
            d_put(p, result.forwarder);
            return -1;
        }
        return resolve(p, m->name, target, 0, (uint16_t)value, depth + 1u, address, provider);
    }
    return resolve(p, m->name, target, f, 0, depth + 1u, address, provider);
}

typedef struct import_visit {
    sxw_process *process;
    uint32_t module;
} import_visit;

static int visit_import(void *context, const pe_import *import) {
    import_visit *v = (import_visit *)context;
    sxw_process *p = v->process;
    sxw_module *m = &p->modules[v->module];
    uint32_t address;
    uint32_t provider;
    if (resolve(p, m->name, import->dll, import->name, import->ordinal, 0, &address, &provider) != 0) {
        return 1;
    }
    /* resolve() may have loaded a module; m still points into the table. */
    pe_mapped mapped = mapped_of(m);
    pe_write_u32(&mapped, import->iat_rva, address);
    /* Both the DLL the import names and the one that provides the symbol
     * are initialized first, even when one only forwards to the other. */
    uint32_t named;
    if (find_index(p, base_name(import->dll), &named)) {
        p->depends[v->module] |= 1u << named;
    }
    p->depends[v->module] |= 1u << provider;
    return 0;
}

static int bind_all(sxw_process *p) {
    /* A forwarder can load a module mid-loop; it is bound when reached. */
    for (uint32_t i = 0; i < p->module_count; ++i) {
        sxw_module *m = &p->modules[i];
        if (m->bound) {
            continue;
        }
        pe_mapped mapped = mapped_of(m);
        import_visit visit = {p, i};
        pe_error error;
        pe_status status = pe_for_each_import(&m->image, &mapped, visit_import, &visit, &error);
        if (status == PE_ERR_CALLBACK) {
            return -1;
        }
        if (status != PE_OK) {
            d_pe(p, m->name, &error);
            return -1;
        }
        m->bound = 1;
    }
    return 0;
}

/* Depth first, dependencies before their importers. */
static void order_from(sxw_process *p, uint32_t index, uint32_t *visited) {
    if ((*visited & (1u << index)) != 0u) {
        return;
    }
    *visited |= 1u << index;
    for (uint32_t j = 0; j < p->module_count; ++j) {
        if (j != index && (p->depends[index] & (1u << j)) != 0u) {
            order_from(p, j, visited);
        }
    }
    p->order[p->order_count++] = index;
}

/* ---- TLS, stack, TEB, PEB, start block ---------------------------------- */

typedef struct tls_entry {
    uint32_t module;
    pe_tls tls;
} tls_entry;

static int setup_tls(sxw_process *p, uint32_t teb) {
    tls_entry entries[SXW_MAX_MODULES];
    uint32_t count = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < p->module_count; ++i) {
        const sxw_module *m = &p->modules[i];
        pe_mapped mapped = mapped_of(m);
        pe_error error;
        int present = 0;
        if (pe_read_tls(&m->image, &mapped, m->base, &entries[count].tls, &present, &error) != PE_OK) {
            d_pe(p, m->name, &error);
            return -1;
        }
        if (!present) {
            continue;
        }
        for (uint32_t k = 0;; ++k) {
            uint32_t rva;
            if (k == SXW_MAX_TLS_CALLBACKS) {
                d_put(p, m->name);
                d_put(p, ": too many TLS callbacks");
                return -1;
            }
            if (pe_tls_callback(&m->image, &mapped, m->base, &entries[count].tls, k, &rva, &error) !=
                PE_OK) {
                d_pe(p, m->name, &error);
                return -1;
            }
            if (rva == 0u) {
                break;
            }
        }
        entries[count].module = i;
        total += align_up(entries[count].tls.template_size + entries[count].tls.zero_fill, 16u);
        ++count;
    }
    if (count == 0u) {
        return 0;
    }
    uint32_t region;
    if (map_region(p, PE_PAGE_SIZE + total, &region) != 0) {
        return -1;
    }
    /* The pointer array, then each module's block, indexed in load order. */
    uint32_t block = region + PE_PAGE_SIZE;
    for (uint32_t k = 0; k < count; ++k) {
        const sxw_module *m = &p->modules[entries[k].module];
        const pe_tls *tls = &entries[k].tls;
        pe_mapped mapped = mapped_of(m);
        pe_write_u32(&mapped, tls->index_rva, k);
        const uint8_t *source = guest(m->base + tls->template_rva);
        uint8_t *target = guest(block);
        for (uint32_t b = 0; b < tls->template_size; ++b) {
            target[b] = source[b];
        }
        put32(region + 4u * k, block);
        block += align_up(tls->template_size + tls->zero_fill, 16u);
    }
    put32(teb + SXW_TEB_TLS_POINTER, region);
    return 0;
}

static uint32_t tls_callbacks_va(const sxw_module *m) {
    pe_mapped mapped = mapped_of(m);
    pe_tls tls;
    pe_error error;
    int present = 0;
    if (pe_read_tls(&m->image, &mapped, m->base, &tls, &present, &error) != PE_OK || !present ||
        tls.callbacks_rva == 0u) {
        return 0;
    }
    return m->base + tls.callbacks_rva;
}

static int setup_process(sxw_process *p) {
    const sxw_module *exe = &p->modules[0];
    uint32_t stack = exe->image.size_of_stack_reserve;
    stack = stack < SXW_MIN_STACK ? SXW_MIN_STACK : stack > SXW_MAX_STACK ? SXW_MAX_STACK : stack;
    stack = align_up(stack, SXW_GRANULARITY);
    if (map_region(p, stack, &p->stack_low) != 0) {
        return -1;
    }
    p->stack_top = p->stack_low + stack;

    /* TEB, PEB and the start block, one page each. */
    uint32_t block;
    if (map_region(p, 3u * PE_PAGE_SIZE, &block) != 0) {
        return -1;
    }
    p->teb = block;
    p->peb = block + PE_PAGE_SIZE;
    p->start_block = block + 2u * PE_PAGE_SIZE;

    put32(p->teb + SXW_TEB_EXCEPTION_LIST, 0xffffffffu);
    put32(p->teb + SXW_TEB_STACK_BASE, p->stack_top);
    put32(p->teb + SXW_TEB_STACK_LIMIT, p->stack_low);
    put32(p->teb + SXW_TEB_SELF, p->teb);
    put32(p->teb + SXW_TEB_PEB, p->peb);
    put32(p->teb + SXW_TEB_GATE, p->platform.gate);
    put32(p->peb + SXW_PEB_IMAGE_BASE, exe->base);

    if (setup_tls(p, p->teb) != 0) {
        return -1;
    }

    put32(p->start_block + 0u, SXW_ABI_VERSION);
    put32(p->start_block + 4u, p->order_count);
    for (uint32_t k = 0; k < p->order_count; ++k) {
        const sxw_module *m = &p->modules[p->order[k]];
        uint32_t at = p->start_block + 8u + 16u * k;
        int is_dll = (m->image.characteristics & PE_FILE_DLL) != 0u;
        put32(at + 0u, m->base);
        put32(at + 4u, m->image.entry_rva != 0u ? m->base + m->image.entry_rva : 0u);
        put32(at + 8u, tls_callbacks_va(m));
        put32(at + 12u, is_dll ? SXW_START_DLL : 0u);
    }
    return 0;
}

static int protect_images(sxw_process *p) {
    for (uint32_t i = 0; i < p->module_count; ++i) {
        const sxw_module *m = &p->modules[i];
        pe_run runs[PE_MAX_SECTIONS + 2u];
        uint32_t count = 0;
        pe_error error;
        if (pe_protection_runs(&m->image, runs, PE_MAX_SECTIONS + 2u, &count, &error) != PE_OK) {
            d_pe(p, m->name, &error);
            return -1;
        }
        for (uint32_t r = 0; r < count; ++r) {
            if (p->platform.protect(p->platform.context, m->base + runs[r].rva, runs[r].size,
                                    runs[r].protection) != 0) {
                d_put(p, m->name);
                d_put(p, ": cannot set page protection");
                return -1;
            }
        }
    }
    return 0;
}

static void close_files(sxw_process *p) {
    for (uint32_t i = 0; i < p->module_count; ++i) {
        if (p->modules[i].handle != 0) {
            p->platform.close_image(p->platform.context, p->modules[i].handle);
            p->modules[i].handle = 0;
        }
    }
}

void sxw_unload(sxw_process *p) {
    close_files(p);
    while (p->region_count > 0u) {
        --p->region_count;
        p->platform.unmap(p->platform.context, p->regions[p->region_count].base,
                          p->regions[p->region_count].size);
    }
    p->module_count = 0;
    p->order_count = 0;
}

int sxw_load(sxw_process *p, const sxw_platform *platform, const char *exe) {
    p->platform = *platform;
    p->module_count = 0;
    p->order_count = 0;
    p->region_count = 0;
    for (uint32_t i = 0; i < SXW_MAX_MODULES; ++i) {
        p->depends[i] = 0;
    }
    d_reset(p);

    uint32_t exe_index;
    uint32_t ntdll;
    if (load_module(p, exe, 0, &exe_index) != 0 ||
        load_module(p, "ntdll.dll", p->modules[0].name, &ntdll) != 0 || bind_all(p) != 0) {
        fail_status(p, SXW_STATUS_INVALID_IMAGE_FORMAT);
        sxw_unload(p);
        return -1;
    }

    pe_export start;
    pe_error error;
    int found = 0;
    const sxw_module *nt = &p->modules[ntdll];
    pe_mapped nt_mapped = mapped_of(nt);
    if (pe_find_export_by_name(&nt->image, &nt_mapped, "SxwProcessStart", &start, &found, &error) !=
            PE_OK ||
        !found || start.forwarder != 0) {
        fail_status(p, SXW_STATUS_ENTRYPOINT_NOT_FOUND);
        d_put(p, "ntdll.dll does not export SxwProcessStart");
        sxw_unload(p);
        return -1;
    }
    p->start_entry = nt->base + start.rva;

    /* ntdll first, as on Windows; then the executable's graph. */
    uint32_t visited = 0;
    order_from(p, ntdll, &visited);
    order_from(p, exe_index, &visited);

    if (setup_process(p) != 0 || protect_images(p) != 0) {
        fail_status(p, SXW_STATUS_INVALID_IMAGE_FORMAT);
        sxw_unload(p);
        return -1;
    }
    close_files(p);
    return 0;
}
