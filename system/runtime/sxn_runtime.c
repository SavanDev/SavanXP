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
 * Today it is the heap's page source (docs/userland/NATIVE_HEAP.md): the
 * allocator in runtime/posix.c grows by arenas from sx_heap_pages_map, whose
 * runtime definitions are weak and POSIX. These strong ones do the same with
 * the native calls, so a native program has the one allocator, whole.
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
