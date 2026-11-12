// The neutral file path (VFS-A step 1, docs/roadmaps/open/VFS_ROADMAP.md):
// opening a node as an I/O object and moving bytes between an open file and a
// process. The native dispatcher and the POSIX translation both come here, so
// a third subsystem does not copy it a third time.

#include "kernel/sx.hpp"

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "kernel/credentials.hpp"
#include "kernel/heap.hpp"
#include "kernel/object.hpp"
#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/vfs.hpp"

namespace sx {

namespace {

// Largest piece of a write handed to a file in one vfs::write.
constexpr size_t kFileWriteChunk = 64u * 1024u;

sx_status last_vfs_error(sx_status fallback) {
    const int error = vfs::last_error();
    return error != 0 ? static_cast<sx_status>(error) : fallback;
}

bool is_open_regular_file(const object::IoObject& file) {
    return file.kind == process::HandleKind::vnode && file.node != nullptr &&
           file.node->type == vfs::NodeType::file;
}

} // namespace

int io_result_value(const IoResult& result) {
    if (result.length != 0) {
        return static_cast<int>(result.length);
    }
    return result.status != kStatusOk ? negative_error(result.status) : 0;
}

sx_status open_file(const cred::Credentials& who, const char* path, uint32_t flags,
                    uint16_t create_permissions, uint32_t accept, object::IoObject*& out) {
    out = nullptr;
    vfs::Vnode* node = vfs::open(who, path, flags, create_permissions);
    if (node == nullptr) {
        return last_vfs_error(SX_ENOENT);
    }
    const bool is_device = node->backend == vfs::Backend::device;
    if (node->type == vfs::NodeType::directory) {
        if ((accept & open_accept_directory) == 0 || (flags & SAVANXP_OPEN_WRITE) != 0) {
            return SX_EISDIR;
        }
    } else if (is_device ? (accept & open_accept_device) == 0 : node->type != vfs::NodeType::file) {
        return SX_EINVAL;
    }

    object::IoObject* file = process::detail::allocate_io_object();
    if (file == nullptr) {
        return SX_ENOMEM;
    }
    file->kind = is_device ? process::HandleKind::device : process::HandleKind::vnode;
    file->open_flags =
        ((flags & SAVANXP_OPEN_READ) != 0 ? process::open_read : 0u) |
        ((flags & SAVANXP_OPEN_WRITE) != 0 ? process::open_write : 0u) |
        ((flags & SAVANXP_OPEN_NONBLOCK) != 0 ? process::open_nonblock : 0u);
    file->node = node;
    file->device = is_device ? static_cast<device::Device*>(node->data) : nullptr;
    file->header.destroy = process::detail::destroy_io_object;
    if ((flags & SAVANXP_OPEN_APPEND) != 0 && node->type == vfs::NodeType::file) {
        file->offset = node->size;
    }
    out = file;
    return kStatusOk;
}

IoResult read_file(process::Process& proc, object::IoObject& file, uint64_t user_buffer, size_t count) {
    IoResult result = {kStatusOk, 0};
    if (!is_open_regular_file(file)) {
        result.status = SX_EBADF;
        return result;
    }
    uint8_t scratch[process::detail::kPipeChunkSize];
    while (result.length < count) {
        const size_t remaining = count - result.length;
        const size_t step = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
        const size_t consumed = vfs::read(*file.node, file.offset, scratch, step);
        if (consumed == 0) {
            // 0 is the end of the file unless the VFS says it was an error.
            result.status = last_vfs_error(kStatusOk);
            break;
        }
        if (!process::write_to_process_memory(proc, user_buffer + result.length, scratch, consumed)) {
            result.status = SX_EINVAL;
            break;
        }
        file.offset += consumed;
        result.length += consumed;
        if (consumed < step) {
            break;
        }
    }
    return result;
}

IoResult write_file(process::Process& proc, object::IoObject& file, uint64_t user_buffer, size_t count) {
    IoResult result = {kStatusOk, 0};
    if (!is_open_regular_file(file)) {
        result.status = SX_EBADF;
        return result;
    }
    // The file sees one vfs::write per chunk, and on SxFS every write that
    // grows a file is a metadata commit: with a 256-byte stack scratch, a
    // 3 KB file was 12 commits. The heap buffer makes it one per
    // kFileWriteChunk; the stack scratch is the fallback when the heap
    // cannot spare one.
    uint8_t stack_scratch[process::detail::kPipeChunkSize];
    const size_t heap_size = count < kFileWriteChunk ? count : kFileWriteChunk;
    uint8_t* heap_scratch = heap_size != 0 ? static_cast<uint8_t*>(heap::allocate(heap_size)) : nullptr;
    uint8_t* scratch = heap_scratch != nullptr ? heap_scratch : stack_scratch;
    const size_t scratch_size = heap_scratch != nullptr ? heap_size : sizeof(stack_scratch);
    while (result.length < count) {
        const size_t remaining = count - result.length;
        const size_t step = remaining < scratch_size ? remaining : scratch_size;
        if (!process::read_from_process_memory(proc, scratch, user_buffer + result.length, step)) {
            result.status = SX_EINVAL;
            break;
        }
        const size_t produced = vfs::write(*file.node, file.offset, scratch, step, false);
        if (produced == 0) {
            result.status = last_vfs_error(SX_EIO);
            break;
        }
        file.offset += produced;
        result.length += produced;
        if (produced < step) {
            break;
        }
    }
    heap::free(heap_scratch);
    return result;
}

sx_status seek_file(object::IoObject& file, int64_t offset, SeekOrigin origin, uint64_t& position) {
    if (file.kind != process::HandleKind::vnode || file.node == nullptr) {
        return SX_EBADF;
    }
    if (file.node->type != vfs::NodeType::file) {
        return SX_EISDIR;
    }
    uint64_t base = 0;
    switch (origin) {
        case SeekOrigin::start:
            break;
        case SeekOrigin::current:
            base = file.offset;
            break;
        case SeekOrigin::end:
            base = file.node->size;
            break;
        default:
            return SX_EINVAL;
    }
    // Checked in unsigned arithmetic: base + offset must land in [0, INT64_MAX].
    constexpr uint64_t kMaxPosition = 0x7fffffffffffffffull;
    if (base > kMaxPosition) {
        return SX_EINVAL;
    }
    uint64_t target = 0;
    if (offset < 0) {
        const uint64_t back = static_cast<uint64_t>(-(offset + 1)) + 1u;
        if (back > base) {
            return SX_EINVAL;
        }
        target = base - back;
    } else {
        if (static_cast<uint64_t>(offset) > kMaxPosition - base) {
            return SX_EINVAL;
        }
        target = base + static_cast<uint64_t>(offset);
    }
    file.offset = static_cast<size_t>(target);
    position = target;
    return kStatusOk;
}

sx_status close_handle(process::Process& proc, uint64_t handle) {
    if (handle >= process::kMaxFileHandles || proc.handles[handle].object == nullptr) {
        return SX_EBADF;
    }
    object::release(proc.handles[handle].object);
    proc.handles[handle].granted_access = object::access_none;
    proc.handles[handle].flags = object::handle_none;
    return kStatusOk;
}

} // namespace sx
