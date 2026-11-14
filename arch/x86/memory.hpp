#pragma once
#include <stdint.h>

namespace i386 {
// Normalize Multiboot2 usable RAM for the reference physical allocator.
bool initialize_memory(uint32_t information);
}
