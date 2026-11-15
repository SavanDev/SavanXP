#pragma once

#include "kernel/object.hpp"

// Architecture-independent I/O contracts from the v0.4.0 process model.
namespace process {

enum class HandleKind : uint8_t {
    none = 0,
    tty = 1,
    vnode = 2,
    pipe = 3,
    device = 4,
    socket = 5,
};

enum OpenFlags : uint32_t {
    open_read = 1u << 0,
    open_write = 1u << 1,
    open_nonblock = 1u << 2,
};

// Reference handle-transfer queue, separate from the byte stream. Each entry
// retains its object until received. The i386 port uses the same queue ownership.
constexpr size_t kPipeHandleQueueCapacity = 4;

struct Pipe {
    bool in_use;
    uint32_t reader_refs;
    uint32_t writer_refs;
    size_t read_pos;
    size_t write_pos;
    size_t size;
    uint8_t* buffer;
    object::Header* handles[kPipeHandleQueueCapacity];
    uint32_t handle_access[kPipeHandleQueueCapacity];
    uint32_t handle_head;
    uint32_t handle_count;
};

struct HandleEntry {
    object::Header* object;
    uint32_t granted_access;
    uint32_t flags;
};

} // namespace process
