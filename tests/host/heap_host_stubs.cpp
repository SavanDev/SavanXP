// The kernel heap, for host tests that link code allocating from it (the
// VFS's path buffers, vfs::PathBuffer) and do not define it themselves.

#include <stdlib.h>

#include "kernel/heap.hpp"

namespace heap {

void* allocate(size_t size, size_t) {
    return malloc(size);
}

void free(void* address) {
    ::free(address);
}

} // namespace heap
