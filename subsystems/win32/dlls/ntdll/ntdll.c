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
        /* Windows XP ends the other threads before any DLL detaches, and
         * they get no DLL_THREAD_DETACH. */
        SxwService(SXW_SVC_END_OTHER_THREADS, 0);
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

/* ---- Critical sections ---------------------------------------------------------- */
/*
 * Windows XP's: LockCount is -1 when free and counts the owner and every
 * waiter above that; RecursionCount the owner's entries. A thread that finds
 * it taken waits on an auto-reset event made the first time anyone has to
 * (LockSemaphore), which the owner sets on its way out while others wait.
 * kernel32's critical section calls are these.
 */

typedef struct critical_section {
    void *debug_info;
    volatile int lock_count;
    int recursion_count;
    u32 owning_thread;
    volatile u32 lock_semaphore;
    u32 spin_count;
} critical_section;

static u32 thread_id(void) {
    u32 id;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(id) : "i"(SXW_TEB_THREAD_ID));
    return id;
}

/* The section's event, made by whoever needs it first. */
static u32 section_event(critical_section *cs) {
    u32 event = cs->lock_semaphore;
    if (event != 0u) {
        return event;
    }
    u32 out[2] = {0, 0};
    u32 arguments[5] = {SXW_OBJECT_EVENT, 0, 0, 0, (u32)out};
    if (SxwService(SXW_SVC_OBJECT_CREATE, arguments) != 0u) {
        return 0;
    }
    u32 expected = 0;
    if (!__atomic_compare_exchange_n(&cs->lock_semaphore, &expected, out[0], 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        u32 close[1] = {out[0]};
        SxwService(SXW_SVC_CLOSE, close);
        return expected;
    }
    return out[0];
}

__declspec(dllexport) u32 __stdcall RtlInitializeCriticalSection(critical_section *cs) {
    cs->debug_info = 0;
    cs->lock_count = -1;
    cs->recursion_count = 0;
    cs->owning_thread = 0;
    cs->lock_semaphore = 0;
    cs->spin_count = 0;
    return 0;
}

__declspec(dllexport) u32 __stdcall RtlDeleteCriticalSection(critical_section *cs) {
    if (cs->lock_semaphore != 0u) {
        u32 arguments[1] = {cs->lock_semaphore};
        SxwService(SXW_SVC_CLOSE, arguments);
        cs->lock_semaphore = 0;
    }
    return 0;
}

__declspec(dllexport) u32 __stdcall RtlEnterCriticalSection(critical_section *cs) {
    const u32 self = thread_id();
    if (__atomic_add_fetch(&cs->lock_count, 1, __ATOMIC_ACQ_REL) == 0) {
        cs->owning_thread = self;
        cs->recursion_count = 1;
        return 0;
    }
    if (cs->owning_thread == self) {
        ++cs->recursion_count;
        return 0;
    }
    const u32 event = section_event(cs);
    u32 result = 0;
    u32 arguments[5] = {(u32)&event, 1, 0, SXW_INFINITE, (u32)&result};
    SxwService(SXW_SVC_WAIT, arguments);
    cs->owning_thread = self;
    cs->recursion_count = 1;
    return 0;
}

__declspec(dllexport) u32 __stdcall RtlLeaveCriticalSection(critical_section *cs) {
    if (--cs->recursion_count > 0) {
        __atomic_sub_fetch(&cs->lock_count, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    cs->owning_thread = 0;
    if (__atomic_sub_fetch(&cs->lock_count, 1, __ATOMIC_ACQ_REL) >= 0) {
        /* Someone waits: let one in. */
        u32 arguments[4] = {section_event(cs), SXW_SIGNAL_SET, 0, 0};
        SxwService(SXW_SVC_OBJECT_SIGNAL, arguments);
    }
    return 0;
}

__declspec(dllexport) int __stdcall RtlTryEnterCriticalSection(critical_section *cs) {
    const u32 self = thread_id();
    int expected = -1;
    if (__atomic_compare_exchange_n(&cs->lock_count, &expected, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        cs->owning_thread = self;
        cs->recursion_count = 1;
        return 1;
    }
    if (cs->owning_thread == self) {
        __atomic_add_fetch(&cs->lock_count, 1, __ATOMIC_ACQ_REL);
        ++cs->recursion_count;
        return 1;
    }
    return 0;
}

/* The loader lock: DllMain runs under it, as on Windows. */
static critical_section g_loader_lock = {0, -1, 0, 0, 0, 0};

/* ---- Threads ---------------------------------------------------------------------- */

/* Modules that asked for no thread notifications (DisableThreadLibraryCalls). */
static u32 g_quiet[MAX_MODULES];
static u32 g_quiet_count;

/* Called from a DLL's own DllMain, often before its attach is recorded:
 * any module base is taken. */
__declspec(dllexport) int __stdcall SxwDisableThreadCalls(u32 base) {
    if (base == 0u) {
        return 0;
    }
    for (u32 k = 0; k < g_quiet_count; ++k) {
        if (g_quiet[k] == base) {
            return 1;
        }
    }
    if (g_quiet_count < MAX_MODULES) {
        g_quiet[g_quiet_count++] = base;
    }
    return 1;
}

static int quiet(u32 base) {
    for (u32 k = 0; k < g_quiet_count; ++k) {
        if (g_quiet[k] == base) {
            return 1;
        }
    }
    return 0;
}

/* One module's thread notification: its TLS callbacks, then DllMain unless
 * it asked for none. */
static void notify_thread(const sxw_start_module *module, u32 reason) {
    run_tls_callbacks(module, reason);
    if ((module->flags & SXW_START_DLL) != 0u && module->entry != 0 && !quiet(module->base)) {
        ((dll_main_fn)module->entry)(module->base, reason, 0);
    }
}

/* DLL_THREAD_ATTACH or DETACH to every module, as Windows XP orders them:
 * the DLLs in the order they attached (reversed for a detach), the
 * executable's TLS callbacks last either way. */
static void notify_all(u32 reason) {
    RtlEnterCriticalSection(&g_loader_lock);
    const sxw_start_module *exe = 0;
    for (u32 k = 0; k < g_module_count; ++k) {
        const u32 i = reason == SXW_DLL_THREAD_DETACH ? g_module_count - 1u - k : k;
        const sxw_start_module *module = &g_modules[i];
        if ((module->flags & SXW_START_DLL) == 0u) {
            exe = module;
            continue;
        }
        notify_thread(module, reason);
    }
    if (exe != 0) {
        run_tls_callbacks(exe, reason);
    }
    RtlLeaveCriticalSection(&g_loader_lock);
}

/* ExitThread: the detach notifications, then the host ends the thread. The
 * last thread ends the process instead. */
__declspec(dllexport) __declspec(noreturn) void __stdcall SxwExitThread(u32 code) {
    notify_all(SXW_DLL_THREAD_DETACH);
    u32 arguments[1] = {code};
    SxwService(SXW_SVC_THREAD_EXIT, arguments);
    RtlExitUserProcess(code);
}

typedef u32(__stdcall *thread_fn)(u32 parameter);

/* Where a thread SXW_SVC_THREAD_CREATE made enters the guest. */
__declspec(dllexport) __declspec(noreturn) void __stdcall SxwThreadStart(u32 start, u32 parameter) {
    const unsigned short control = 0x27f;
    __asm__ volatile("fninit\n\tfldcw %0" : : "m"(control));
    notify_all(SXW_DLL_THREAD_ATTACH);
    SxwExitThread(((thread_fn)start)(parameter));
}

/* LoadLibrary over the host's loader. 0 with *error set on failure. */
static u32 load_library(const char *name, u32 *error) {
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
static u32 get_proc_address(u32 base, const char *name, u32 *error) {
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

__declspec(dllexport) u32 __stdcall SxwLoadLibrary(const char *name, u32 *error) {
    RtlEnterCriticalSection(&g_loader_lock);
    const u32 base = load_library(name, error);
    RtlLeaveCriticalSection(&g_loader_lock);
    return base;
}

__declspec(dllexport) u32 __stdcall SxwGetProcAddress(u32 base, const char *name, u32 *error) {
    RtlEnterCriticalSection(&g_loader_lock);
    const u32 address = get_proc_address(base, name, error);
    RtlLeaveCriticalSection(&g_loader_lock);
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

/* ---- Heaps -------------------------------------------------------------------- */
/*
 * First fit over arenas the host maps. Each block has a 16-byte header: its
 * total size, a tag, and the size the caller asked for, which is what
 * HeapSize, LocalSize and _msize answer on Windows XP (10 for a request of
 * 10). Free blocks are kept in address order and merged with a free
 * neighbour. Payloads are 8-byte aligned, as Windows' heap gives them.
 *
 * The process heap is one; HeapCreate makes others (RtlCreateHeap), each
 * with arenas of its own that HeapDestroy gives back to the host. A heap
 * created with a maximum size is that size in all, as on Windows. Guest
 * threads take turns (docs/userland/WIN32.md, "Threads"), so a heap needs no
 * lock of its own.
 */

#define HEAP_ARENA 0x40000u
#define HEAP_SMALL_ARENA 0x10000u
#define HEAP_HEADER 16u
#define HEAP_USED 0x5358a110u
#define HEAP_FREE 0x5358f4eeu
#define HEAP_MAGIC 0x53586870u
#define HEAP_MAX_ARENAS 64u

#define HEAP_ZERO_MEMORY 0x00000008u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010u
#define HEAP_GROWABLE 0x00000002u

typedef struct heap_block {
    u32 size; /* the whole block, header included */
    u32 tag;
    union {
        u32 requested;                /* used blocks: the size asked for */
        struct heap_block *next_free; /* free blocks */
    } u;
    u32 reserved;
} heap_block;

typedef struct heap_arena {
    u32 start;
    u32 end;
} heap_arena;

typedef struct sxw_heap {
    u32 magic;
    heap_block *free;
    heap_arena arenas[HEAP_MAX_ARENAS];
    u32 arena_count;
    u32 maximum; /* the bytes it may hold in all, 0 for no limit */
    u32 mapped;
} sxw_heap;

static sxw_heap g_process_heap = {HEAP_MAGIC, 0, {{0, 0}}, 0, 0, 0};

/* The arena an address is in, 1 to arena_count; 0 for none. */
static u32 arena_of(const sxw_heap *heap, u32 address) {
    for (u32 i = 0; i < heap->arena_count; ++i) {
        if (address >= heap->arenas[i].start && address < heap->arenas[i].end) {
            return i + 1u;
        }
    }
    return 0;
}

/* Inserts a free block in address order, merged with free neighbours of
 * the same arena: two arenas the host happened to map next to each other
 * are separate mappings, and a block across them is not one range. */
static void insert_free(sxw_heap *heap, heap_block *block) {
    heap_block **link = &heap->free;
    heap_block *previous = 0;
    while (*link != 0 && *link < block) {
        previous = *link;
        link = &(*link)->u.next_free;
    }
    block->tag = HEAP_FREE;
    block->u.next_free = *link;
    *link = block;
    heap_block *next = block->u.next_free;
    if (next != 0 && (u32)block + block->size == (u32)next &&
        arena_of(heap, (u32)next) == arena_of(heap, (u32)block)) {
        block->size += next->size;
        block->u.next_free = next->u.next_free;
    }
    if (previous != 0 && (u32)previous + previous->size == (u32)block &&
        arena_of(heap, (u32)previous) == arena_of(heap, (u32)block)) {
        previous->size += block->size;
        previous->u.next_free = block->u.next_free;
    }
}

static int grow(sxw_heap *heap, u32 needed) {
    const u32 step = heap == &g_process_heap ? HEAP_ARENA : HEAP_SMALL_ARENA;
    u32 size = needed + HEAP_HEADER > step ? (needed + HEAP_HEADER + 0xffffu) & ~0xffffu : step;
    if (heap->arena_count == HEAP_MAX_ARENAS) {
        return 0;
    }
    if (heap->maximum != 0u) {
        if (heap->mapped >= heap->maximum || needed > heap->maximum - heap->mapped) {
            return 0;
        }
        if (size > heap->maximum - heap->mapped) {
            size = (heap->maximum - heap->mapped + 0xfffu) & ~0xfffu;
        }
    }
    u32 arguments[1] = {size};
    u32 base = SxwService(SXW_SVC_ALLOCATE, arguments);
    if (base == 0) {
        return 0;
    }
    heap->arenas[heap->arena_count].start = base;
    heap->arenas[heap->arena_count].end = base + size;
    ++heap->arena_count;
    heap->mapped += size;
    heap_block *block = (heap_block *)base;
    block->size = size;
    insert_free(heap, block);
    return 1;
}

static sxw_heap *heap_of(void *handle) {
    sxw_heap *heap = handle;
    return heap != 0 && heap->magic == HEAP_MAGIC ? heap : 0;
}

static void *allocate(sxw_heap *heap, u32 size, u32 zero) {
    if (size > 0x7ff00000u) {
        return 0;
    }
    u32 needed = ((size < 8u ? 8u : size) + HEAP_HEADER + 7u) & ~7u;
    for (int attempt = 0; attempt < 2; ++attempt) {
        heap_block **link = &heap->free;
        while (*link != 0) {
            heap_block *block = *link;
            if (block->size >= needed) {
                if (block->size - needed >= HEAP_HEADER + 8u) {
                    heap_block *rest = (heap_block *)((u32)block + needed);
                    rest->size = block->size - needed;
                    rest->tag = HEAP_FREE;
                    rest->u.next_free = block->u.next_free;
                    *link = rest;
                    block->size = needed;
                } else {
                    *link = block->u.next_free;
                }
                block->tag = HEAP_USED;
                block->u.requested = size;
                u8 *payload = (u8 *)block + HEAP_HEADER;
                if (zero) {
                    for (u32 i = 0; i < block->size - HEAP_HEADER; ++i) {
                        payload[i] = 0;
                    }
                }
                return payload;
            }
            link = &block->u.next_free;
        }
        if (attempt == 0 && !grow(heap, needed)) {
            return 0;
        }
    }
    return 0;
}

/* The used block of heap whose payload starts at pointer, or 0. */
static heap_block *used_block(const sxw_heap *heap, const void *pointer) {
    u32 address = (u32)pointer;
    if (address < HEAP_HEADER || (address & 7u) != 0u || !arena_of(heap, address - HEAP_HEADER)) {
        return 0;
    }
    heap_block *block = (heap_block *)(address - HEAP_HEADER);
    return block->tag == HEAP_USED ? block : 0;
}

/* The process heap's blocks, for the other DLLs (msvcrt's malloc, kernel32's
 * global and local memory). */
__declspec(dllexport) void *__stdcall SxwHeapAllocate(u32 size, u32 zero) {
    return allocate(&g_process_heap, size, zero);
}

__declspec(dllexport) int __stdcall SxwHeapFree(void *pointer) {
    heap_block *block = used_block(&g_process_heap, pointer);
    if (block == 0) {
        return 0;
    }
    insert_free(&g_process_heap, block);
    return 1;
}

/* 1 when pointer is a live block of the process heap. */
__declspec(dllexport) int __stdcall SxwHeapIsBlock(const void *pointer) {
    return used_block(&g_process_heap, pointer) != 0;
}

/* The size a block of the process heap was asked for, or 0. */
__declspec(dllexport) u32 __stdcall SxwHeapSize(const void *pointer) {
    const heap_block *block = used_block(&g_process_heap, pointer);
    return block != 0 ? block->u.requested : 0u;
}

/* Gives a block a new size without moving it: 1 when it fits in the space
 * the block already has, 0 otherwise (and nothing changes). Bytes past the
 * old size are left as they are. */
__declspec(dllexport) int __stdcall SxwHeapResize(void *pointer, u32 size) {
    heap_block *block = used_block(&g_process_heap, pointer);
    if (block == 0 || size > block->size - HEAP_HEADER) {
        return 0;
    }
    block->u.requested = size;
    return 1;
}

/* Windows' heap calls, which kernel32's Heap* stand on. */
__declspec(dllexport) void *__stdcall RtlCreateHeap(u32 flags, void *base, u32 reserve, u32 commit, void *lock,
                                                     void *parameters) {
    (void)flags;
    (void)lock;
    (void)parameters;
    (void)commit;
    if (base != 0) {
        return 0; /* a heap in the caller's memory: nobody asks for one */
    }
    sxw_heap *heap = allocate(&g_process_heap, sizeof(sxw_heap), 1);
    if (heap == 0) {
        return 0;
    }
    heap->magic = HEAP_MAGIC;
    heap->maximum = reserve != 0u ? (reserve + 0xfffu) & ~0xfffu : 0u;
    return heap;
}

__declspec(dllexport) void *__stdcall RtlDestroyHeap(void *handle) {
    sxw_heap *heap = heap_of(handle);
    if (heap == 0 || heap == &g_process_heap) {
        return handle;
    }
    for (u32 i = 0; i < heap->arena_count; ++i) {
        u32 arguments[1] = {heap->arenas[i].start};
        SxwService(SXW_SVC_FREE, arguments);
    }
    heap->magic = 0;
    SxwHeapFree(heap);
    return 0;
}

/* A heap that cannot give the memory says so with ERROR_NOT_ENOUGH_MEMORY,
 * as Windows XP's does; the other heap calls leave the last error alone. */
__declspec(dllexport) void *__stdcall RtlAllocateHeap(void *handle, u32 flags, u32 size) {
    sxw_heap *heap = heap_of(handle);
    void *block = heap != 0 ? allocate(heap, size, (flags & HEAP_ZERO_MEMORY) != 0u) : 0;
    if (block == 0) {
        __asm__ volatile("movl %0, %%fs:%c1" : : "r"(8u), "i"(SXW_TEB_LAST_ERROR) : "memory");
    }
    return block;
}

__declspec(dllexport) int __stdcall RtlFreeHeap(void *handle, u32 flags, void *pointer) {
    (void)flags;
    sxw_heap *heap = heap_of(handle);
    if (pointer == 0) {
        return 1;
    }
    heap_block *block = heap != 0 ? used_block(heap, pointer) : 0;
    if (block == 0) {
        return 0;
    }
    insert_free(heap, block);
    return 1;
}

__declspec(dllexport) u32 __stdcall RtlSizeHeap(void *handle, u32 flags, const void *pointer) {
    (void)flags;
    sxw_heap *heap = heap_of(handle);
    const heap_block *block = heap != 0 ? used_block(heap, pointer) : 0;
    return block != 0 ? block->u.requested : 0xffffffffu;
}

/* Grows or shrinks a block, in place when it fits, else by moving it (never
 * with HEAP_REALLOC_IN_PLACE_ONLY); HEAP_ZERO_MEMORY zeroes what it adds. */
__declspec(dllexport) void *__stdcall RtlReAllocateHeap(void *handle, u32 flags, void *pointer, u32 size) {
    sxw_heap *heap = heap_of(handle);
    heap_block *block = heap != 0 ? used_block(heap, pointer) : 0;
    if (block == 0 || size > 0x7ff00000u) {
        return 0;
    }
    const u32 old = block->u.requested;
    u8 *data = pointer;
    if (size <= block->size - HEAP_HEADER) {
        block->u.requested = size;
    } else {
        if ((flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0u) {
            return 0;
        }
        u8 *moved = allocate(heap, size, 0);
        if (moved == 0) {
            return 0;
        }
        for (u32 i = 0; i < old; ++i) {
            moved[i] = data[i];
        }
        insert_free(heap, block);
        data = moved;
    }
    if ((flags & HEAP_ZERO_MEMORY) != 0u) {
        for (u32 i = old; i < size; ++i) {
            data[i] = 0;
        }
    }
    return data;
}

/* The process heap, as PEB.ProcessHeap holds it. */
__declspec(dllexport) void *__stdcall SxwProcessHeap(void) {
    return &g_process_heap;
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
