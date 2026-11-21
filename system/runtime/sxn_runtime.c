#include <stddef.h>
#include <stdint.h>

#include "sxn/sxn.h"

/*
 * The native runtime: what a native program (EI_OSABI 0x53, no POSIX calls)
 * needs on top of the runtime every static program links. It is compiled into
 * every native target -- savanxp_program(... NATIVE ...), which the platform
 * services use through savanxp_system_service, and tools/build-user.sh
 * --native -- and into nothing else.
 *
 * Each piece is a runtime hook whose definition there is weak and POSIX, and
 * whose strong one here does the same with the native calls:
 *
 * - The heap's page source (docs/userland/NATIVE_HEAP.md): the allocator in
 *   runtime/posix.c grows by arenas from sx_heap_pages_map, so a native
 *   program has the one allocator, whole.
 * - Where the shared-library loader's diagnostics go, sx_loader_report.
 * - How the process ends, sx_process_exit: exit() calls it, and so do crt0
 *   when main returns, _exit, abort and __stack_chk_fail.
 * - What the C library's clocks, sleeps, randomness and identity stand on
 *   (sx_clock_*, sx_entropy_fill, sx_process_id, sx_identity_query), so a
 *   library both subsystems share -- OpenSSL -- gets the date, the time since
 *   boot and the kernel's random bytes through time(), gettimeofday() and
 *   getentropy() in a native program too.
 */

/* The contract (savanxp/libc.h): `bytes` (a multiple of 4 KiB) of zeroed,
 * private, read-write memory, or 0. The section's handle is closed once
 * mapped: the view keeps the section alive and costs no descriptor. */
void* sx_heap_pages_map(size_t bytes)
{
    long section = sxn_section_create((uint64_t)bytes, SXN_SECTION_READ | SXN_SECTION_WRITE);
    long view;

    if (section < 0)
    {
        return 0;
    }
    view = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE | SXN_VIEW_PRIVATE);
    (void)sxn_close(section);
    if (view <= 0 && view > -4096)
    {
        return 0;
    }
    return (void*)(uintptr_t)view;
}

/* What one sx_heap_pages_map returned, with the same `bytes`. */
void sx_heap_pages_unmap(void* base, size_t bytes)
{
    (void)bytes;
    (void)sxn_unmap_view((uint64_t)(uintptr_t)base);
}

/* The loader's lines (savanxp/libc.h), in a native program's terms: the
 * system log, always. A program the system starts has no handle 2, and what
 * the loader says -- a library that did not load, or a newer one standing in
 * for the version asked -- must be on record either way; started from a
 * console, the log's line shows there too. */
void sx_loader_report(const char* text, unsigned long length)
{
    char line[SXN_LOG_TEXT_MAX];
    unsigned long used = 0;

    while (used < length && used + 1 < sizeof(line) && text[used] != '\n')
    {
        line[used] = text[used];
        used++;
    }
    line[used] = '\0';
    (void)sxn_log(line);
}

/* The process ends with `code` as its status; every thread goes with it. */
__attribute__((noreturn)) void sx_process_exit(int code)
{
    sxn_exit(code);
}

unsigned long long sx_clock_epoch_seconds(void)
{
    return (unsigned long long)sxn_system_time();
}

unsigned long long sx_clock_monotonic_ns(void)
{
    return (unsigned long long)sxn_clock_ns();
}

/* Waits on a word nobody changes until the timeout ends it. */
int sx_clock_sleep_ms(unsigned long milliseconds)
{
    static volatile uint32_t never_woken;
    long result;

    if (milliseconds == 0)
    {
        return 0;
    }
    if (milliseconds > 0x7fffffffUL)
    {
        milliseconds = 0x7fffffffUL;
    }
    result = sxn_wait_address(&never_woken, 0, sizeof(never_woken), (long)milliseconds);
    return result == 0 || result == -SAVANXP_ETIMEDOUT ? 0 : -1;
}

long sx_entropy_fill(void* buffer, unsigned long length)
{
    return sxn_random_bytes(buffer, length);
}

long sx_process_id(void)
{
    struct sxn_native_info info;
    const long result = sxn_info(&info);

    return result < 0 ? result : (long)info.pid;
}

/* struct savanxp_identity and struct sxn_identity are one layout. */
long sx_identity_query(struct savanxp_identity* out)
{
    return sxn_query_identity((struct sxn_identity*)out);
}
