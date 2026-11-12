/*
 * ntdll.dll -- The bottom of the subsystem's DLL set: the service gate and
 * process start. Its only private exports are SxwService, which every other
 * DLL reaches the host through, and SxwProcessStart, where the host enters
 * the guest. RtlExitUserProcess is Windows' own: detach, then terminate.
 * See sxw_abi.h.
 */
#include "sxw_abi.h"

typedef unsigned int u32;
typedef u32(__stdcall *sxw_gate_fn)(u32 service, const u32 *arguments);
typedef int(__stdcall *dll_main_fn)(u32 instance, u32 reason, u32 reserved);
typedef void(__stdcall *tls_callback_fn)(u32 instance, u32 reason, u32 reserved);
typedef u32(__stdcall *entry_fn)(void);

static sxw_gate_fn gate(void) {
    sxw_gate_fn g;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(g) : "i"(SXW_TEB_GATE));
    return g;
}

__declspec(dllexport) u32 __stdcall SxwService(u32 service, const u32 *arguments) {
    return gate()(service, arguments);
}

static const sxw_start_block *g_block;
static u32 g_attached; /* modules whose attach has run, in block order */
static int g_exiting;

static __declspec(noreturn) void terminate(u32 status) {
    u32 arguments[1] = {status};
    SxwService(SXW_SVC_EXIT_PROCESS, arguments);
    for (;;) {
    }
}

static void run_tls_callbacks(const sxw_start_module *module, u32 reason) {
    if (module->tls_callbacks == 0) {
        return;
    }
    for (const u32 *callback = (const u32 *)module->tls_callbacks; *callback != 0; ++callback) {
        ((tls_callback_fn)*callback)(module->base, reason, 0);
    }
}

/* Detach every module whose attach ran, then end the process. */
__declspec(dllexport) __declspec(noreturn) void __stdcall RtlExitUserProcess(u32 status) {
    if (!g_exiting && g_block != 0) {
        g_exiting = 1;
        const sxw_start_module *exe = 0;
        for (u32 i = g_attached; i-- > 0;) {
            const sxw_start_module *module = &g_block->modules[i];
            if ((module->flags & SXW_START_DLL) == 0) {
                exe = module;
                continue;
            }
            run_tls_callbacks(module, SXW_DLL_PROCESS_DETACH);
            if (module->entry != 0) {
                /* lpReserved is non-NULL when the process is ending. */
                ((dll_main_fn)module->entry)(module->base, SXW_DLL_PROCESS_DETACH, 1);
            }
        }
        if (exe != 0) {
            run_tls_callbacks(exe, SXW_DLL_PROCESS_DETACH);
        }
    }
    terminate(status);
}

__declspec(dllexport) void __stdcall SxwProcessStart(const sxw_start_block *block) {
    if (SxwService(SXW_SVC_VERSION, 0) != SXW_ABI_VERSION || block->version != SXW_ABI_VERSION) {
        terminate(SXW_STATUS_DLL_INIT_FAILED);
    }
    g_block = block;
    u32 status = 0;
    for (u32 i = 0; i < block->count; ++i) {
        const sxw_start_module *module = &block->modules[i];
        run_tls_callbacks(module, SXW_DLL_PROCESS_ATTACH);
        if ((module->flags & SXW_START_DLL) != 0) {
            /* lpReserved is non-NULL for a DLL loaded with the process. A
             * DLL whose DllMain fails is not detached. */
            if (module->entry != 0 &&
                !((dll_main_fn)module->entry)(module->base, SXW_DLL_PROCESS_ATTACH, 1)) {
                RtlExitUserProcess(SXW_STATUS_DLL_INIT_FAILED);
            }
            g_attached = i + 1;
        } else {
            g_attached = i + 1;
            if (module->entry != 0) {
                status = ((entry_fn)module->entry)();
            }
        }
    }
    /* Returning from the entry point ends the process, as on Windows. */
    RtlExitUserProcess(status);
}
