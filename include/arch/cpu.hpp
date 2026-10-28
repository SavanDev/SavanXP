#pragma once

// The CPU contract the kernel is written against (docs/kernel/ARCH.md): the
// functions in namespace arch, implemented by the architecture being built.
// Code outside arch/ includes this header, never an architecture's own.
#if defined(__x86_64__)
#include "arch/x86_64/cpu.hpp"
#else
#error "SavanXP: no arch/ implementation for this target"
#endif
