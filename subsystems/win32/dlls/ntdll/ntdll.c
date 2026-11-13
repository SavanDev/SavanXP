/*
 * ntdll.dll -- The bottom of the subsystem's DLL set: the service gate,
 * process start and exit, the module list, and the process heap.
 *
 * Exports with Windows' names keep Windows' meaning: RtlExitUserProcess.
 * The private ones are prefixed Sxw and are how the other DLLs reach the host
 * (SxwService) and the state kept here (modules, heap). See sxw_abi.h.
 */
#include "sxw_abi.h"

typedef unsigned int u32;
typedef unsigned char u8;
typedef u32(__stdcall *sxw_gate_fn)(u32 service, const u32 *arguments);
typedef int(__stdcall *dll_main_fn)(u32 instance, u32 reason, u32 reserved);
typedef void(__stdcall *tls_callback_fn)(u32 instance, u32 reason, u32 reserved);
typedef u32(__stdcall *entry_fn)(void);

#define MAX_MODULES 64u

static sxw_gate_fn gate(void) {
    sxw_gate_fn g;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(g) : "i"(SXW_TEB_GATE));
    return g;
}

__declspec(dllexport) u32 __stdcall SxwService(u32 service, const u32 *arguments) {
    return gate()(service, arguments);
}

static __declspec(noreturn) void terminate(u32 status) {
    u32 arguments[1] = {status};
    SxwService(SXW_SVC_EXIT_PROCESS, arguments);
    for (;;) {
    }
}

/* ---- Modules ---------------------------------------------------------------- */

/* Every module whose attach ran, in the order it ran: the start block's,
 * then those LoadLibrary added. Exit detaches them in reverse. */
static sxw_start_module g_modules[MAX_MODULES];
static u32 g_module_count;
static int g_exiting;

static void run_tls_callbacks(const sxw_start_module *module, u32 reason) {
    if (module->tls_callbacks == 0) {
        return;
    }
    for (const u32 *callback = (const u32 *)module->tls_callbacks; *callback != 0; ++callback) {
        ((tls_callback_fn)*callback)(module->base, reason, 0);
    }
}

/* Runs a DLL's attach and records it. lpReserved is non-NULL for a DLL that
 * comes with the process, NULL for one LoadLibrary brings. 0 when DllMain
 * refused. */
static int attach_dll(const sxw_start_module *module, u32 reserved) {
    run_tls_callbacks(module, SXW_DLL_PROCESS_ATTACH);
    if (module->entry != 0 &&
        !((dll_main_fn)module->entry)(module->base, SXW_DLL_PROCESS_ATTACH, reserved)) {
        return 0;
    }
    if (g_module_count < MAX_MODULES) {
        g_modules[g_module_count++] = *module;
    }
    return 1;
}

/* Detaches every module whose attach ran, then ends the process. The DLLs go
 * in reverse order, each its TLS callbacks then DllMain with a non-NULL
 * lpReserved (the process is ending); the executable's TLS callbacks come
 * last. This is the order Windows XP runs them in. */
__declspec(dllexport) __declspec(noreturn) void __stdcall RtlExitUserProcess(u32 status) {
    if (!g_exiting) {
        g_exiting = 1;
        const sxw_start_module *exe = 0;
        for (u32 i = g_module_count; i-- > 0;) {
            const sxw_start_module *module = &g_modules[i];
            if ((module->flags & SXW_START_DLL) == 0) {
                exe = module;
                continue;
            }
            run_tls_callbacks(module, SXW_DLL_PROCESS_DETACH);
            if (module->entry != 0) {
                ((dll_main_fn)module->entry)(module->base, SXW_DLL_PROCESS_DETACH, 1);
            }
        }
        if (exe != 0) {
            run_tls_callbacks(exe, SXW_DLL_PROCESS_DETACH);
        }
    }
    terminate(status);
}

/* Attaches the modules a load added. 0 when one refused; it stays loaded and
 * is not detached, and the caller fails with ERROR_DLL_INIT_FAILED. */
static int attach_added(const sxw_start_module *added, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (!attach_dll(&added[i], 0)) {
            return 0;
        }
    }
    return 1;
}

/* LoadLibrary over the host's loader. 0 with *error set on failure. */
__declspec(dllexport) u32 __stdcall SxwLoadLibrary(const char *name, u32 *error) {
    static sxw_start_module added[32];
    u32 result = 0;
    u32 arguments[4] = {(u32)name, (u32)added, 32u, (u32)&result};
    u32 base = SxwService(SXW_SVC_LOAD_LIBRARY, arguments);
    if (base == 0) {
        *error = result;
        return 0;
    }
    if (!attach_added(added, result & 0xffffu)) {
        *error = SXW_ERROR_DLL_INIT_FAILED;
        return 0;
    }
    *error = SXW_ERROR_SUCCESS;
    return base;
}

/* GetProcAddress over the host's loader: name, or an ordinal below 0x10000. */
__declspec(dllexport) u32 __stdcall SxwGetProcAddress(u32 base, const char *name, u32 *error) {
    static sxw_start_module added[32];
    u32 result = 0;
    u32 arguments[5] = {base, (u32)name, (u32)added, 32u, (u32)&result};
    u32 address = SxwService(SXW_SVC_GET_PROC, arguments);
    if (address == 0) {
        *error = result;
        return 0;
    }
    if (!attach_added(added, result & 0xffffu)) {
        *error = SXW_ERROR_DLL_INIT_FAILED;
        return 0;
    }
    *error = SXW_ERROR_SUCCESS;
    return address;
}

/* The base of a loaded module by file name, or 0. */
__declspec(dllexport) u32 __stdcall SxwFindModule(const char *name) {
    u32 arguments[1] = {(u32)name};
    return SxwService(SXW_SVC_FIND_MODULE, arguments);
}

/* Seconds since 1970 UTC. */
__declspec(dllexport) u32 __stdcall SxwTime(void) {
    return SxwService(SXW_SVC_TIME, 0);
}

/* ---- The process heap ------------------------------------------------------- */
/*
 * First fit over arenas the host maps. Each block has an 8-byte header (its
 * total size and a tag); free blocks are kept in address order and merged
 * with a free neighbour. Payloads are 8-byte aligned, as Windows' heap gives
 * them.
 */

#define HEAP_ARENA 0x40000u
#define HEAP_USED 0x5358a110u
#define HEAP_FREE 0x5358f4eeu

typedef struct heap_block {
    u32 size; /* the whole block, header included */
    u32 tag;
    struct heap_block *next_free; /* free blocks only: overlaps the payload */
} heap_block;

typedef struct heap_arena {
    u32 start;
    u32 end;
} heap_arena;

static heap_block *g_free;
static heap_arena g_arenas[64];
static u32 g_arena_count;

/* The arena an address is in, 1 to g_arena_count; 0 for none. */
static u32 arena_of(u32 address) {
    for (u32 i = 0; i < g_arena_count; ++i) {
        if (address >= g_arenas[i].start && address < g_arenas[i].end) {
            return i + 1u;
        }
    }
    return 0;
}

static int in_arena(u32 address) {
    return arena_of(address) != 0;
}

/* Inserts a free block in address order, merged with free neighbours of
 * the same arena: two arenas the host happened to map next to each other
 * are separate mappings, and a block across them is not one range. */
static void insert_free(heap_block *block) {
    heap_block **link = &g_free;
    heap_block *previous = 0;
    while (*link != 0 && *link < block) {
        previous = *link;
        link = &(*link)->next_free;
    }
    block->tag = HEAP_FREE;
    block->next_free = *link;
    *link = block;
    heap_block *next = block->next_free;
    if (next != 0 && (u32)block + block->size == (u32)next && arena_of((u32)next) == arena_of((u32)block)) {
        block->size += next->size;
        block->next_free = next->next_free;
    }
    if (previous != 0 && (u32)previous + previous->size == (u32)block &&
        arena_of((u32)previous) == arena_of((u32)block)) {
        previous->size += block->size;
        previous->next_free = block->next_free;
    }
}

static int grow(u32 needed) {
    u32 size = needed + 8u > HEAP_ARENA ? (needed + 8u + 0xffffu) & ~0xffffu : HEAP_ARENA;
    if (g_arena_count == sizeof(g_arenas) / sizeof(g_arenas[0])) {
        return 0;
    }
    u32 arguments[1] = {size};
    u32 base = SxwService(SXW_SVC_ALLOCATE, arguments);
    if (base == 0) {
        return 0;
    }
    g_arenas[g_arena_count].start = base;
    g_arenas[g_arena_count].end = base + size;
    ++g_arena_count;
    heap_block *block = (heap_block *)base;
    block->size = size;
    insert_free(block);
    return 1;
}

/* A block of at least size bytes, zeroed when zero is set; 0 when there is no
 * memory. */
__declspec(dllexport) void *__stdcall SxwHeapAllocate(u32 size, u32 zero) {
    if (size > 0x7ff00000u) {
        return 0;
    }
    u32 needed = ((size < 8u ? 8u : size) + 8u + 7u) & ~7u;
    for (int attempt = 0; attempt < 2; ++attempt) {
        heap_block **link = &g_free;
        while (*link != 0) {
            heap_block *block = *link;
            if (block->size >= needed) {
                if (block->size - needed >= 16u) {
                    heap_block *rest = (heap_block *)((u32)block + needed);
                    rest->size = block->size - needed;
                    rest->tag = HEAP_FREE;
                    rest->next_free = block->next_free;
                    *link = rest;
                    block->size = needed;
                } else {
                    *link = block->next_free;
                }
                block->tag = HEAP_USED;
                u8 *payload = (u8 *)block + 8;
                if (zero) {
                    for (u32 i = 0; i < block->size - 8u; ++i) {
                        payload[i] = 0;
                    }
                }
                return payload;
            }
            link = &block->next_free;
        }
        if (attempt == 0 && !grow(needed)) {
            return 0;
        }
    }
    return 0;
}

/* Frees a block from SxwHeapAllocate. 0 when the pointer is not one. */
__declspec(dllexport) int __stdcall SxwHeapFree(void *pointer) {
    u32 address = (u32)pointer;
    if (address < 8u || (address & 7u) != 0u || !in_arena(address - 8u)) {
        return 0;
    }
    heap_block *block = (heap_block *)(address - 8u);
    if (block->tag != HEAP_USED) {
        return 0;
    }
    insert_free(block);
    return 1;
}

/* The usable size of a block from SxwHeapAllocate, or 0. */
__declspec(dllexport) u32 __stdcall SxwHeapSize(const void *pointer) {
    u32 address = (u32)pointer;
    if (address < 8u || (address & 7u) != 0u || !in_arena(address - 8u)) {
        return 0;
    }
    const heap_block *block = (const heap_block *)(address - 8u);
    return block->tag == HEAP_USED ? block->size - 8u : 0u;
}

/* ---- Process start --------------------------------------------------------- */

__declspec(dllexport) void __stdcall SxwProcessStart(const sxw_start_block *block) {
    if (SxwService(SXW_SVC_VERSION, 0) != SXW_ABI_VERSION || block->version != SXW_ABI_VERSION) {
        terminate(SXW_STATUS_DLL_INIT_FAILED);
    }
    /* A Windows thread starts with 53-bit x87 precision, every exception
     * masked (0x27F), as Windows XP sets it; the host's default is 64-bit. */
    const unsigned short control = 0x27f;
    __asm__ volatile("fninit\n\tfldcw %0" : : "m"(control));

    u32 status = 0;
    for (u32 i = 0; i < block->count; ++i) {
        const sxw_start_module *module = &block->modules[i];
        if ((module->flags & SXW_START_DLL) != 0) {
            /* A DLL whose DllMain fails is not detached. */
            if (!attach_dll(module, 1)) {
                RtlExitUserProcess(SXW_STATUS_DLL_INIT_FAILED);
            }
        } else {
            run_tls_callbacks(module, SXW_DLL_PROCESS_ATTACH);
            if (g_module_count < MAX_MODULES) {
                g_modules[g_module_count++] = *module;
            }
            if (module->entry != 0) {
                status = ((entry_fn)module->entry)();
            }
        }
    }
    /* Returning from the entry point ends the process, as on Windows. */
    RtlExitUserProcess(status);
}
