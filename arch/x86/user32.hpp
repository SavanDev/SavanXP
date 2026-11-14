#pragma once
#include <stdint.h>
#include "context.hpp"

namespace i386::user32 {
// Load an ELF32 into private physical backing memory without changing CR3.
int load(const char* path, uint8_t* memory, ContextFrame& frame, uint32_t argc,
         const char* const* argv);
// Start the initial user process, or the idle task when no root is mounted.
[[noreturn]] void run(const char* path);
}
