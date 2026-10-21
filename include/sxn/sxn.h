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
 * POSIX -1/errno one. Handles, pipe and device I/O and the clock are what a
 * platform service needs to run without POSIX (docs/desktop/
 * DESKTOP_ARCHITECTURE.md, phase 3).
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

static inline long sxn_close(long handle) {
    return sxn_trap(SXN_SYS_CLOSE, (unsigned long)handle, 0, 0);
}

static inline long sxn_duplicate(long handle) {
    return sxn_trap(SXN_SYS_DUPLICATE, (unsigned long)handle, 0, 0);
}

static inline long sxn_read(long handle, void* buffer, unsigned long count) {
    return sxn_trap(SXN_SYS_READ, (unsigned long)handle, (unsigned long)buffer, count);
}

static inline long sxn_write(long handle, const void* buffer, unsigned long count) {
    return sxn_trap(SXN_SYS_WRITE, (unsigned long)handle, (unsigned long)buffer, count);
}

static inline long sxn_open_device(const char* name, unsigned access) {
    return sxn_trap(SXN_SYS_OPEN_DEVICE, (unsigned long)name, (unsigned long)access, 0);
}

static inline long sxn_device_control(long handle, unsigned long code, unsigned long argument) {
    return sxn_trap(SXN_SYS_DEVICE_CONTROL, (unsigned long)handle, code, argument);
}

static inline uint64_t sxn_clock_ns(void) {
    return (uint64_t)sxn_trap(SXN_SYS_CLOCK, 0, 0, 0);
}

static inline long sxn_port_create(const char* name) {
    return sxn_trap(SXN_SYS_PORT_CREATE, (unsigned long)name, 0, 0);
}

/* handles[0]: write requests to the service; handles[1]: read its replies. */
static inline long sxn_port_connect(const char* name, int32_t handles[2]) {
    return sxn_trap(SXN_SYS_PORT_CONNECT, (unsigned long)name, (unsigned long)handles, 0);
}

static inline long sxn_port_accept(long port, struct sxn_port_connection* out) {
    return sxn_trap(SXN_SYS_PORT_ACCEPT, (unsigned long)port, (unsigned long)out, 0);
}

static inline long sxn_pipe_send_handle(long pipe_write, long handle) {
    return sxn_trap(SXN_SYS_PIPE_SEND_HANDLE, (unsigned long)pipe_write, (unsigned long)handle, 0);
}

static inline long sxn_pipe_receive_handle(long pipe_read) {
    return sxn_trap(SXN_SYS_PIPE_RECEIVE_HANDLE, (unsigned long)pipe_read, 0, 0);
}

/* Waits for any of `count` handles; returns the index of the one signaled. */
static inline long sxn_wait_many(const int32_t* handles, unsigned long count, long timeout_ms) {
    return sxn_trap(SXN_SYS_WAIT_MANY, (unsigned long)handles, count, (unsigned long)timeout_ms);
}

/* What the kernel knows about `pid`: parent, state, name. */
static inline long sxn_process_query(long pid, struct sxn_process_query* out) {
    return sxn_trap(SXN_SYS_PROCESS_QUERY, (unsigned long)pid, (unsigned long)out, 0);
}

/* Spawn whose child gets only stdio, not the caller's other handles. */
static inline long sxn_spawn_stdio(const char* path, char* const* argv, int argc) {
    return sxn_trap(SXN_SYS_SPAWN_STDIO, (unsigned long)path, (unsigned long)argv, (unsigned long)(long)argc);
}

/* Reads and writes on the stream stop blocking (1) or block again (0). */
static inline long sxn_set_nonblocking(long handle, int on) {
    return sxn_trap(SXN_SYS_SET_NONBLOCKING, (unsigned long)handle, (unsigned long)(on != 0), 0);
}

/* Ends a descendant (child, grandchild, ...); its parent's waitpid reports
 * `status`. */
static inline long sxn_terminate(long pid, int status) {
    return sxn_trap(SXN_SYS_TERMINATE, (unsigned long)pid, (unsigned long)(long)status, 0);
}

/* CreatePipe: handles[0] is the read end, handles[1] the write end. */
static inline long sxn_pipe_create(int32_t handles[2]) {
    return sxn_trap(SXN_SYS_PIPE_CREATE, (unsigned long)handles, 0, 0);
}

/* CreateProcess with chosen handles: the child gets request->std_handles as
 * 0, 1, 2 and request->handles as 3, 4, ... and nothing else. Returns the
 * pid. Start from sxn_spawn_request_init so unused fields are right. */
static inline long sxn_spawn_ex(const struct sxn_spawn_request* request) {
    return sxn_trap(SXN_SYS_SPAWN_EX, (unsigned long)request, 0, 0);
}

/* A request for `path` with `argc` arguments, the caller's own stdio and no
 * extra handles. */
static inline void sxn_spawn_request_init(
    struct sxn_spawn_request* request, const char* path, char* const* argv, int argc) {
    request->path = (uint64_t)(unsigned long)path;
    request->argv = (uint64_t)(unsigned long)argv;
    request->argc = argc;
    request->std_handles[0] = -1;
    request->std_handles[1] = -1;
    request->std_handles[2] = -1;
    request->handles[0] = -1;
    request->handles[1] = -1;
    request->handles[2] = -1;
    request->handles[3] = -1;
    request->handle_count = 0;
    request->flags = 0;
}
