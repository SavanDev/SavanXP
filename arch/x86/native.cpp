// Adapt v0.4.0's native dispatcher to protected-mode contexts and user copies.
#include "services.hpp"
#include "process.hpp"
#include "abi/savanxp_native_abi.h"
#include "kernel/console.hpp"
#include "kernel/display.hpp"
#include "kernel/process.hpp"
#include "savanxp/syscall.h"

namespace i386 {
bool native_syscall(ContextFrame& frame) {
    if (frame.eax < SXN_SYS_BASE) return false; // Shared POSIX baseline.
    int result = -SAVANXP_ENOSYS;
    auto* task = process::current();
    if (!task->native_subsystem) { frame.eax = result; return true; }
    static bool announced;
    if (!announced) {
        console::printf("native: dispatcher active (pid=%u, syscall=%u)\n", task->pid, frame.eax);
        announced = true;
    }
    switch (frame.eax) {
        case SXN_SYS_INFO: {
            sxn_native_info info = {};
            info.abi_version = SXN_ABI_VERSION; info.subsystem_id = 1; info.pid = task->pid;
            result = ::process::copy_to_user(frame.edi, &info, sizeof(info)) ? 0 : -SAVANXP_EINVAL;
            break;
        }
        case SXN_SYS_LOG: {
            char message[256] = {};
            bool valid = true;
            for (unsigned i = 0; i < sizeof(message); ++i) {
                if (!::process::copy_from_user(message + i, uint64_t(frame.edi) + i, 1)) { valid = false; break; }
                if (!message[i]) break;
            }
            message[255] = 0;
            if (valid) { console::printf("native[%u]: %s\n", task->pid, message); result = 0; }
            else result = -SAVANXP_EINVAL;
            break;
        }
        case SXN_SYS_GFX_INFO: {
            savanxp_gpu_info gpu = {};
            if (!display::get_info(gpu)) { result = -SAVANXP_ENODEV; break; }
            sxn_gfx_info info = {gpu.width, gpu.height, gpu.pitch, gpu.bpp, gpu.buffer_size, 0};
            result = ::process::copy_to_user(frame.edi, &info, sizeof(info)) ? 0 : -SAVANXP_EINVAL;
            break;
        }
        case SXN_SYS_GFX_ACQUIRE:
            result = display::acquire_session(task->pid) ? 0 : -SAVANXP_EBUSY; break;
        case SXN_SYS_GFX_RELEASE:
            if (!display::owns_session(task->pid)) { result = -SAVANXP_EBUSY; break; }
            display::release_session_resources(); display::release_session(task->pid); result = 0; break;
        case SXN_SYS_GFX_PRESENT: {
            if (!display::owns_session(task->pid)) { result = -SAVANXP_EBUSY; break; }
            sxn_gfx_present region = {};
            const auto& fb = display::framebuffer_info();
            result = -SAVANXP_EINVAL;
            if (!::process::copy_from_user(&region, frame.edi, sizeof(region)) ||
                !region.pixels || !region.source_pitch || !region.width || !region.height ||
                region.x >= fb.width || region.y >= fb.height ||
                region.width > fb.width - region.x || region.height > fb.height - region.y) break;
            const uint64_t bytes = uint64_t(region.y + region.height - 1) * region.source_pitch +
                uint64_t(region.x + region.width) * 4;
            if (bytes > UINT32_MAX || !::process::validate_user_range(region.pixels, bytes, false)) break;
            result = display::present_region(reinterpret_cast<void*>(static_cast<uintptr_t>(region.pixels)),
                region.source_pitch, region.x, region.y, region.width, region.height) ? 0 : -SAVANXP_EIO;
            break;
        }
    }
    frame.eax = static_cast<uint32_t>(result);
    return true;
}
}
