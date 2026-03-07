#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/vmm.hpp"

namespace elf {

struct LoadResult {
    uint64_t entry_point;
    uint64_t stack_pointer;
};

bool load_user_image(
    const void* image,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result
);

} // namespace elf
