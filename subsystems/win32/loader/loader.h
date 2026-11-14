/*
 * loader.h -- The Win32 process loader, above the PE32 parser.
 *
 * Portable C with no library dependency, shared by the host harness and
 * winhost. Memory and files come from a platform table; guest addresses are
 * below 4 GiB and the platform maps them at the same host address, so the
 * loader writes guest memory through plain pointers.
 *
 * sxw_load() maps the executable and every DLL it needs, binds the imports
 * (forwarders included), sets up static TLS, the stack, the TEB, the PEB and
 * the start block, and applies the final page protections. It either
 * succeeds or releases everything it took, names what failed and sets the
 * NT status of the failure.
 */
#ifndef SAVANXP_WIN32_LOADER_H
#define SAVANXP_WIN32_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "pe.h"
#include "sxw_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXW_MAX_MODULES 32u
#define SXW_MAX_REGIONS 64u
#define SXW_MODULE_NAME 64u
#define SXW_DIAGNOSTIC 256u

/* Guest addresses the loader places regions in: from the platform's
 * guest_low (0x10000 in the harness; 0x400000 under winhost, which keeps
 * clear of what lies below although the user floor is 0x10000) up to 2 GiB.
 * A relocated image goes at 0x10000000 or above. */
#define SXW_GUEST_HIGH 0x80000000u
#define SXW_RELOCATED_LOW 0x10000000u

typedef struct sxw_platform {
    void *context;
    /* The lowest guest address the platform can map. */
    uint32_t guest_low;
    /* Maps size zeroed, read-write bytes at exactly base. Nonzero when the
     * range is not free. An image is mapped one protection run per call, and
     * protect() is later called with exactly the base and size of one map(). */
    int (*map)(void *context, uint32_t base, uint32_t size);
    /* Gives a mapped range its final protection, keeping its contents. */
    int (*protect)(void *context, uint32_t base, uint32_t size, pe_protection protection);
    void (*unmap)(void *context, uint32_t base, uint32_t size);
    /* Opens an image by the name an import uses (or the executable's path).
     * Nonzero when there is no such file. */
    int (*open_image)(void *context, const char *name, const uint8_t **data, size_t *size,
                      void **handle);
    void (*close_image)(void *context, void *handle);
    /* The guest address the TEB's gate field points at. */
    uint32_t gate;
    /* The host folder that is the guest's C: drive ("/system/win/drive_c"),
     * or NULL. A path under it is C:\...; any other is Z:\..., Z: being the
     * host's root, as Wine names it. */
    const char *drive_c;
} sxw_platform;

typedef struct sxw_module {
    char name[SXW_MODULE_NAME]; /* lower-case file name */
    pe_image image;
    uint32_t base;
    void *handle;
    int bound;
} sxw_module;

typedef struct sxw_region {
    uint32_t base;
    uint32_t size;
} sxw_region;

typedef struct sxw_process {
    sxw_platform platform;
    sxw_module modules[SXW_MAX_MODULES];
    uint32_t module_count;
    /* Initialization order: dependencies first, the executable last. */
    uint32_t order[SXW_MAX_MODULES];
    uint32_t order_count;
    uint32_t depends[SXW_MAX_MODULES]; /* bit j of depends[i]: i imports from j */
    sxw_region regions[SXW_MAX_REGIONS];
    uint32_t region_count;

    uint32_t stack_low;
    uint32_t stack_top;
    uint32_t teb;
    uint32_t peb;
    uint32_t start_block;
    uint32_t start_entry; /* ntdll!SxwProcessStart */

    char diagnostic[SXW_DIAGNOSTIC];
    /* After a failed load: the NT status Windows XP ends the process with for
     * the same failure (SXW_STATUS_* in sxw_abi.h). */
    uint32_t status;
} sxw_process;

/* Loads the executable named exe and everything it needs. arguments is what
 * follows the program in its command line (UTF-8, already quoted where it
 * needs to be), or NULL. On failure, every region and file is released and
 * diagnostic names the cause. 0 on success. */
int sxw_load(sxw_process *process, const sxw_platform *platform, const char *exe,
             const char *arguments);

/* A new zeroed read-write region of at least size bytes for the guest (the
 * heap's arenas). 0, or -1 when there is no room. */
int sxw_allocate_region(sxw_process *process, uint32_t size, uint32_t *base);

/* LoadLibrary: the DLL named name (ANSI, a path allowed) and what it needs,
 * into a running process. Loaded ones only return their base. The modules
 * this call added are written to added[capacity] in initialization order
 * (the guest runs their DllMain), *count of them. On failure nothing it
 * added stays, and *error is the Win32 error. 0 on success. */
int sxw_load_library(sxw_process *process, const char *name, sxw_start_module *added,
                     uint32_t capacity, uint32_t *count, uint32_t *base, uint32_t *error);

/* GetProcAddress: the export of the module at base, by name (name != NULL)
 * or ordinal. A forwarder may load DLLs, reported as sxw_load_library does. */
int sxw_get_proc(sxw_process *process, uint32_t base, const char *name, uint16_t ordinal,
                 sxw_start_module *added, uint32_t capacity, uint32_t *count, uint32_t *address,
                 uint32_t *error);

/* Releases every region and file of a loaded process. */
void sxw_unload(sxw_process *process);

/* The module with this name (any case), or NULL. */
const sxw_module *sxw_find_module(const sxw_process *process, const char *name);

#ifdef __cplusplus
}
#endif

#endif
