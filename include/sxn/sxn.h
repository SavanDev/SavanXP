#pragma once

/* SavanXP native userland surface (sxn): thin typed wrappers over the SXN
 * syscall numbers in abi/savanxp_native_abi.h, beside POSIX.
 *
 * This is the userland half of the native path: the same shape as the
 * kernel sx:: surface, one static-inline call per syscall, header-only so
 * no tool or link change is needed to use it (savanxp/syscall.h only lends
 * the shared errno space and the POSIX numbers the negative checks use).
 * Returns follow the kernel convention: 0 or a positive result on success,
 * a negative -errno on failure -- the savanxp_* raw layer style, not the
 * POSIX -1/errno one. No toolkit, no IPC yet: windows and the compositor
 * still need native IPC first (see docs/KERNEL_NATIVE_API.md).
 */

#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "savanxp/syscall.h"

static inline long sxn_trap(unsigned long number, unsigned long a, unsigned long b, unsigned long c) {
    long result;

    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "cc", "memory");
    return result;
}

static inline long sxn_info(struct sxn_native_info* out) {
    return sxn_trap(SXN_SYS_INFO, (unsigned long)out, 0, 0);
}

static inline long sxn_log(const char* message) {
    return sxn_trap(SXN_SYS_LOG, (unsigned long)message, 0, 0);
}

static inline void sxn_exit(int code) __attribute__((noreturn));
static inline void sxn_exit(int code) {
    sxn_trap(SXN_SYS_EXIT, (unsigned long)(long)code, 0, 0);
    __builtin_unreachable();
}

static inline long sxn_spawn(const char* path, char* const* argv, int argc) {
    return sxn_trap(SXN_SYS_SPAWN, (unsigned long)path, (unsigned long)argv, (unsigned long)(long)argc);
}

static inline long sxn_waitpid(long pid, int* status) {
    return sxn_trap(SXN_SYS_WAITPID, (unsigned long)pid, (unsigned long)status, 0);
}

static inline long sxn_event_create(unsigned flags) {
    return sxn_trap(SXN_SYS_EVENT_CREATE, (unsigned long)flags, 0, 0);
}

static inline long sxn_event_set(long handle) {
    return sxn_trap(SXN_SYS_EVENT_SET, (unsigned long)handle, 0, 0);
}

static inline long sxn_event_reset(long handle) {
    return sxn_trap(SXN_SYS_EVENT_RESET, (unsigned long)handle, 0, 0);
}

static inline long sxn_wait_one(long handle, long timeout_ms) {
    return sxn_trap(SXN_SYS_WAIT_ONE, (unsigned long)handle, (unsigned long)timeout_ms, 0);
}

static inline long sxn_section_create(uint64_t size_bytes, unsigned flags) {
    return sxn_trap(SXN_SYS_SECTION_CREATE, (unsigned long)size_bytes, (unsigned long)flags, 0);
}

static inline long sxn_map_view(long handle, unsigned flags) {
    return sxn_trap(SXN_SYS_MAP_VIEW, (unsigned long)handle, (unsigned long)flags, 0);
}

static inline long sxn_unmap_view(uint64_t base_address) {
    return sxn_trap(SXN_SYS_UNMAP_VIEW, (unsigned long)base_address, 0, 0);
}

static inline long sxn_gfx_info(struct sxn_gfx_info* out) {
    return sxn_trap(SXN_SYS_GFX_INFO, (unsigned long)out, 0, 0);
}
