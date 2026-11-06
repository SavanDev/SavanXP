#pragma once

// Glue for OpenBSD's sys/dev/hid/hid.c (BSD-2-Clause, Copyright (c) 1998 The
// NetBSD Foundation, Inc.), ported in hid_report.cpp; see
// docs/third-party/THIRD_PARTY_PROVENANCE.md.
//
// The integer types and the _KERNEL switch OpenBSD's hid.h expects. Safe to
// include anywhere, unlike hid_compat.hpp.

#include <stddef.h>
#include <stdint.h>

#ifndef _KERNEL
#define _KERNEL 1
#endif

typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef unsigned int u_int;
