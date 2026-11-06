#pragma once

// Glue for OpenBSD's sys/dev/hid/hid.c (BSD-2-Clause, Copyright (c) 1998 The
// NetBSD Foundation, Inc.), ported in hid_report.cpp; see
// docs/third-party/THIRD_PARTY_PROVENANCE.md.
//
// What OpenBSD's hid.c takes from <sys/param.h>, <sys/systm.h> and
// <sys/malloc.h>, mapped onto the kernel. Included by hid_report.cpp only:
// the malloc and free macros would break any other file.

#include "hid_types.hpp"
#include "kernel/console.hpp"
#include "kernel/heap.hpp"
#include "kernel/string.hpp"

inline void* hid_compat_allocate_zeroed(size_t size) {
    void* memory = heap::allocate(size);
    if (memory != nullptr) {
        memset(memory, 0, size);
    }
    return memory;
}

#define M_TEMP 0
#define M_WAITOK 0
#define M_ZERO 0
#define malloc(size, type, flags) hid_compat_allocate_zeroed(size)
#define free(pointer, type, size) heap::free(pointer)
#define printf(...) console::printf(__VA_ARGS__)
#define nitems(array) (sizeof(array) / sizeof((array)[0]))
