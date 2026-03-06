#pragma once

#include <stddef.h>
#include <stdint.h>

namespace heap {

void initialize();
bool ready();
void* allocate(size_t size, size_t alignment = 16);
void free(void* address);
uint64_t total_bytes();
uint64_t used_bytes();
uint64_t free_bytes();

} // namespace heap
