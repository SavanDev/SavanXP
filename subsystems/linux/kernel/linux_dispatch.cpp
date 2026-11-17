/*
 * SSL, the Savan Subsystem for Linux: the kernel half's entry points.
 *
 * A Linux program enters through the `syscall` instruction, which raises #UD
 * because EFER.SCE is clear; the kernel completes the instruction and calls
 * dispatch_linux_syscall, registered as this subsystem's trap dispatcher
 * (docs/roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md, E1). The software interrupt
 * `int 0x80` is the 32-bit Linux ABI, which SSL does not offer: it gets
 * -ENOSYS and nothing else.
 *
 * Implemented: write, exit and exit_group (L1); memory through the anonymous
 * regions of S2a (mmap, munmap, mremap, brk), the thread pointer
 * (arch_prctl), set_tid_address, the descriptor table and the file calls on
 * it (open, read, write, writev, lseek, fstat, stat, close, unlink, ioctl),
 * and the clocks (clock_gettime, nanosleep) (L2); directory enumeration,
 * working directories, creation masks, descriptor flags and sendfile (L3).
 * Every other number returns -ENOSYS and is logged once per process, so a
 * fixture's first run lists what it needs; a variant of a call the layer
 * does not offer fails with its Linux error and is logged once the same way.
 * A call never returns success without doing its work.
 */

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "arch/cpu.hpp"
#include "kernel/elf.hpp"
#include "kernel/console.hpp"
#include "kernel/process.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/region_list.hpp"
#include "kernel/rtc.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/subsystem.hpp"
#include "kernel/sx.hpp"
#include "kernel/timer.hpp"
#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "linux_abi.hpp"
#include "shared/version.h"

namespace ssl {
namespace {

constexpr uint64_t kPage = 4096;

// A Linux process's descriptors. Linux's own default limit is 1024; the
// kernel's handle table is the smaller bound anyway.
constexpr uint32_t kMaxDescriptors = 256;
constexpr uint32_t kDescriptorCloseOnExec = 1u << 0;
constexpr uint32_t kDescriptorSocket = 1u << 1;
constexpr uint32_t kDescriptorNonblock = 1u << 2;
constexpr uint32_t kNoSocketHandle = 0xffffffffu;
// Descriptor::shutdown bits: the directions shutdown() has closed.
constexpr uint8_t kShutdownRead = 1u << 0;
constexpr uint8_t kShutdownWrite = 1u << 1;

// One descriptor: the kernel handle it names, plus one (0 is a free slot,
// so zeroed state is an empty table), and its Linux flags.
struct Descriptor {
    uint32_t handle_plus_one;
    uint32_t flags;
    uint32_t read_handle_plus_one;
    uint8_t display;
    uint8_t shutdown;
};

// The per-process state the kernel reserves for SSL and never reads.
struct ProcessState {
    // One bit per system call number already reported as unimplemented.
    uint64_t reported[abi::kSyscallLimit / 64];
    // A number at or past the limit was reported.
    bool reported_out_of_range;
    // One bit per number whose unsupported variant was already reported.
    uint64_t variant_reported[abi::kSyscallLimit / 64];
    // The program break: where it began, the end of the image, and where it
    // is now. Both 0 until the first brk.
    uint64_t break_start;
    uint64_t break_current;
    // The descriptor table, its own and not the kernel's handle table:
    // numbering and close-on-exec follow Linux's rules. The standard three
    // are the handles the spawn gave, entered on first use.
    bool paths_ready;
    uint16_t mask;
    char cwd[vfs::kMaxPathLength];
    bool descriptors_ready;
    Descriptor descriptors[kMaxDescriptors];
};

// The per-thread state, made on first use.
struct ThreadState {
    // set_tid_address's pointer. Kept; clearing it when the thread ends
    // matters once a Linux process can have more than one (clone, L7).
    uint64_t clear_child_tid;
    uint64_t transfer_output;
    size_t transfer_count;
    bool transfer_wait;
    bool receive_wait;
    uint64_t receive_handle;
    uint64_t receive_syscall;
    bool poll_active;
    uint64_t poll_address;
    uint32_t poll_count;
    abi::Pollfd poll_entries[kMaxDescriptors];
};

ProcessState* state_of(process::Process& proc) {
    return proc.subsystem_state_bytes == sizeof(ProcessState)
        ? reinterpret_cast<ProcessState*>(proc.subsystem_state)
        : nullptr;
}

ThreadState* thread_state_of(process::Thread& thread) {
    uint8_t* const bytes = process::thread_subsystem_state(thread);
    return bytes != nullptr && thread.subsystem_state_bytes == sizeof(ThreadState)
        ? reinterpret_cast<ThreadState*>(bytes)
        : nullptr;
}

// True the first time `number` is seen in `bits`, which it then marks.
bool first_time(uint64_t* bits, uint64_t number) {
    uint64_t& word = bits[number / 64];
    const uint64_t bit = 1ull << (number % 64);
    if ((word & bit) != 0) {
        return false;
    }
    word |= bit;
    return true;
}

void report_unimplemented(process::Process& proc, uint64_t number) {
    ProcessState* const state = state_of(proc);
    if (state != nullptr) {
        if (number < abi::kSyscallLimit) {
            if (!first_time(state->reported, number)) {
                return;
            }
        } else {
            if (state->reported_out_of_range) {
                return;
            }
            state->reported_out_of_range = true;
        }
    }
    console::printf("linux: pid=%u unimplemented syscall %llu\n", proc.pid,
                    static_cast<unsigned long long>(number));
}

// A call SSL has, asked for something it does not offer: `what` names it.
// Logged once per call number per process; the caller returns the error.
void report_unsupported(process::Process& proc, uint64_t number, const char* what) {
    ProcessState* const state = state_of(proc);
    if (state != nullptr && number < abi::kSyscallLimit && !first_time(state->variant_reported, number)) {
        return;
    }
    console::printf("linux: pid=%u syscall %llu: unsupported %s\n", proc.pid,
                    static_cast<unsigned long long>(number), what);
}

void set_result(process::SavedContext* context, int64_t value) {
    context->rax = static_cast<uint64_t>(value);
}

// `length` rounded up to whole pages; false when that wraps.
bool round_to_pages(uint64_t length, uint64_t& rounded) {
    rounded = (length + kPage - 1) & ~(kPage - 1);
    return rounded >= length;
}

// The table, with the standard descriptors entered: 0, 1 and 2 name the
// process's handles of the same numbers when the spawn gave them.
ProcessState* descriptors_of(process::Process& proc) {
    ProcessState* const state = state_of(proc);
    if (state != nullptr && !state->descriptors_ready) {
        for (uint32_t standard = 0; standard <= 2; ++standard) {
            if (process::handle_is_open(proc, standard)) {
                state->descriptors[standard].handle_plus_one = standard + 1;
            }
        }
        state->descriptors_ready = true;
    }
    return state;
}

// The kernel handle a descriptor names. False for a descriptor that is not
// open.
bool handle_of(process::Process& proc, uint64_t descriptor, uint64_t& handle) {
    ProcessState* const state = descriptors_of(proc);
    if (state == nullptr || descriptor >= kMaxDescriptors ||
        state->descriptors[descriptor].handle_plus_one == 0 ||
        state->descriptors[descriptor].handle_plus_one == kNoSocketHandle) {
        return false;
    }
    handle = state->descriptors[descriptor].handle_plus_one - 1;
    return true;
}

Descriptor* descriptor_of(process::Process& proc, uint64_t number) {
    auto* state = descriptors_of(proc);
    if (!state || number >= kMaxDescriptors || !state->descriptors[number].handle_plus_one) return nullptr;
    return &state->descriptors[number];
}

bool read_handle_of(process::Process& proc, uint64_t number, uint64_t& handle) {
    auto* d = descriptor_of(proc, number);
    if (!d) return false;
    if (!(d->flags & kDescriptorSocket)) return handle_of(proc, number, handle);
    if (!d->read_handle_plus_one || d->read_handle_plus_one == kNoSocketHandle) return false;
    handle = d->read_handle_plus_one - 1;
    return true;
}

// The lowest free descriptor, now naming `handle`, as Linux numbers them; -1
// when the table is full.
int64_t allocate_descriptor(ProcessState& state, uint64_t handle, uint32_t flags) {
    for (uint32_t descriptor = 0; descriptor < kMaxDescriptors; ++descriptor) {
        if (state.descriptors[descriptor].handle_plus_one == 0) {
            state.descriptors[descriptor].handle_plus_one = static_cast<uint32_t>(handle + 1);
            state.descriptors[descriptor].flags = flags;
            return descriptor;
        }
    }
    return -1;
}

// Path state belongs to SSL, not to the neutral process.
ProcessState* paths_of(process::Process& proc) {
    ProcessState* state = state_of(proc);
    if (state != nullptr && !state->paths_ready) {
        strcpy(state->cwd, "/");
        state->mask = 0022;
        state->paths_ready = true;
    }
    return state;
}

int64_t guest_path_from_user(process::Process& proc, uint64_t user_path, vfs::PathBuffer& host) {
    vfs::PathBuffer guest;
    vfs::PathBuffer joined;
    ProcessState* state = paths_of(proc);
    if (guest == nullptr || host == nullptr || joined == nullptr || state == nullptr) return -abi::kENOMEM;
    const sx_status copied = sx::copy_user_path_from(proc, user_path, guest, guest.capacity());
    if (copied == SX_ENAMETOOLONG) return -abi::kENAMETOOLONG;
    if (copied != sx::kStatusOk) return -abi::kEFAULT;
    const char* text = guest;
    if (text[0] == '\0') return -abi::kENOENT;
    if (text[0] != '/') {
        const size_t base = strlen(state->cwd);
        if (base + 1 + strlen(text) + 1 > joined.capacity()) return -abi::kENAMETOOLONG;
        strcpy(joined, state->cwd);
        char* out = joined;
        out[base] = '/';
        strcpy(out + base + 1, text);
        text = joined;
    }
    return process::linux_guest_path(text, host, host.capacity()) ? 0 : -abi::kENAMETOOLONG;
}

// write(fd, buffer, count).
process::SavedContext* linux_write(process::Thread& self, process::SavedContext* context) {
    if (auto* d = descriptor_of(*self.process, context->rdi); d && (d->flags & kDescriptorSocket)) {
        if (d->shutdown & kShutdownWrite) { set_result(context, -abi::kEPIPE); return context; }
        if (!d->read_handle_plus_one) { set_result(context, -abi::kENOTCONN); return context; }
    }
    const uint64_t buffer = context->rsi;
    size_t count = static_cast<size_t>(context->rdx);
    uint64_t descriptor = 0;
    if (!handle_of(*self.process, context->rdi, descriptor)) {
        set_result(context, -abi::kEBADF);
        return context;
    }
    if (count > abi::kMaxRwCount) {
        count = abi::kMaxRwCount;
    }
    if (count != 0 && !sx::validate_user_range(buffer, count, false)) {
        set_result(context, -abi::kEFAULT);
        return context;
    }
    const int result = process::native_write_handle(self, descriptor, buffer, count);
    if (result == process::kBlockedResult) {
        return process::choose_next_context(context);
    }
    // A kernel error is a Linux error of the same number (linux_abi.hpp).
    set_result(context, result);
    return context;
}

// writev(fd, iov, iovcnt). Only the first segment with bytes may block: the
// thread's waker completes the call with that segment's count, which is then
// the whole count. A later segment that would block ends the call short, and
// the caller writes the rest, as musl's stdio does.
process::SavedContext* linux_writev(process::Thread& self, process::SavedContext* context) {
    if (auto* d = descriptor_of(*self.process, context->rdi); d && (d->flags & kDescriptorSocket)) {
        if (d->shutdown & kShutdownWrite) { set_result(context, -abi::kEPIPE); return context; }
        if (!d->read_handle_plus_one) { set_result(context, -abi::kENOTCONN); return context; }
    }
    const uint64_t vector = context->rsi;
    const uint64_t count = context->rdx;
    uint64_t descriptor = 0;
    if (!handle_of(*self.process, context->rdi, descriptor)) {
        set_result(context, -abi::kEBADF);
        return context;
    }
    if (count > abi::kIovMax) {
        set_result(context, -abi::kEINVAL);
        return context;
    }
    // The vector is read in batches, twice: first every length, so a bad
    // vector fails before a byte moves, as on Linux.
    constexpr uint64_t kBatch = 32;
    abi::Iovec batch[kBatch];
    for (uint64_t first = 0; first < count; first += kBatch) {
        const uint64_t n = count - first < kBatch ? count - first : kBatch;
        if (!sx::copy_from_user(batch, vector + first * sizeof(abi::Iovec), n * sizeof(abi::Iovec))) {
            set_result(context, -abi::kEFAULT);
            return context;
        }
        for (uint64_t index = 0; index < n; ++index) {
            if (static_cast<int64_t>(batch[index].length) < 0) {
                set_result(context, -abi::kEINVAL);
                return context;
            }
        }
    }
    uint64_t total = 0;
    for (uint64_t first = 0; first < count; first += kBatch) {
        const uint64_t n = count - first < kBatch ? count - first : kBatch;
        if (!sx::copy_from_user(batch, vector + first * sizeof(abi::Iovec), n * sizeof(abi::Iovec))) {
            set_result(context, total != 0 ? static_cast<int64_t>(total) : -abi::kEFAULT);
            return context;
        }
        for (uint64_t index = 0; index < n; ++index) {
            uint64_t length = batch[index].length;
            if (length == 0) {
                continue;
            }
            if (length > abi::kMaxRwCount - total) {
                length = abi::kMaxRwCount - total;
            }
            if (!sx::validate_user_range(batch[index].base, length, false)) {
                set_result(context, total != 0 ? static_cast<int64_t>(total) : -abi::kEFAULT);
                return context;
            }
            const int result = process::native_write_handle(self, descriptor, batch[index].base, length, total == 0);
            if (result == process::kBlockedResult) {
                return process::choose_next_context(context);
            }
            if (result < 0) {
                set_result(context, total != 0 ? static_cast<int64_t>(total) : result);
                return context;
            }
            total += static_cast<uint64_t>(result);
            if (static_cast<uint64_t>(result) < length || total == abi::kMaxRwCount) {
                set_result(context, static_cast<int64_t>(total));
                return context;
            }
        }
    }
    set_result(context, static_cast<int64_t>(total));
    return context;
}

// ioctl(fd, request, argument). No descriptor is a terminal yet: each answers
// -ENOTTY, which is what Linux says of a file or a pipe for TCGETS and
// TIOCGWINSZ, the two requests musl's stdio and isatty make.
int64_t linux_ioctl(process::Process& proc, const process::SavedContext* context) {
    return descriptor_of(proc, context->rdi) ? -abi::kENOTTY : -abi::kEBADF;
}

// read(fd, buffer, count).
process::SavedContext* linux_read(process::Thread& self, process::SavedContext* context) {
    if (auto* d = descriptor_of(*self.process, context->rdi); d && (d->flags & kDescriptorSocket)) {
        if (d->shutdown & kShutdownRead) { set_result(context, 0); return context; }
        if (!d->read_handle_plus_one) { set_result(context, -abi::kENOTCONN); return context; }
    }
    const uint64_t buffer = context->rsi;
    size_t count = static_cast<size_t>(context->rdx);
    uint64_t handle = 0;
    if (!read_handle_of(*self.process, context->rdi, handle)) {
        set_result(context, -abi::kEBADF);
        return context;
    }
    if (count > abi::kMaxRwCount) {
        count = abi::kMaxRwCount;
    }
    if (count != 0 && !sx::validate_user_range(buffer, count, true)) {
        set_result(context, -abi::kEFAULT);
        return context;
    }
    const int result = process::native_read_handle(self, handle, buffer, count);
    if (result == process::kBlockedResult) {
        return process::choose_next_context(context);
    }
    set_result(context, result);
    return context;
}

int64_t vfs_failure();

// open(path, flags, mode): a regular file under the guest root. The access
// mode, O_DIRECTORY, O_CREAT, O_TRUNC and O_CLOEXEC (kept for exec, L7) are honoured;
// O_LARGEFILE is always true here and O_NOCTTY has no terminal to refuse.
// Any other flag is refused rather than half done: O_APPEND needs every
// write at the end, not only the first.
int64_t linux_open(process::Process& proc, const process::SavedContext* context) {
    ProcessState* const state = descriptors_of(proc);
    if (state == nullptr) {
        return -abi::kENOMEM;
    }
    const uint64_t flags = context->rsi;
    const uint64_t honoured = abi::kOpenAccessMode | abi::kOpenCreate | abi::kOpenTruncate |
                              abi::kOpenLargeFile | abi::kOpenNoCtty | abi::kOpenCloseOnExec | abi::kOpenDirectory;
    if ((flags & ~honoured) != 0) {
        report_unsupported(proc, abi::kSysOpen, "open flag");
        return -abi::kEINVAL;
    }
    uint32_t open_flags = 0;
    switch (flags & abi::kOpenAccessMode) {
        case abi::kOpenReadOnly:
            open_flags = SAVANXP_OPEN_READ;
            break;
        case abi::kOpenWriteOnly:
            open_flags = SAVANXP_OPEN_WRITE;
            break;
        case abi::kOpenReadWrite:
            open_flags = SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE;
            break;
        default:
            return -abi::kEINVAL;
    }
    if ((flags & abi::kOpenCreate) != 0) {
        open_flags |= SAVANXP_OPEN_CREATE;
    }
    if ((flags & abi::kOpenTruncate) != 0) {
        open_flags |= SAVANXP_OPEN_TRUNCATE;
    }

    vfs::PathBuffer host;
    const int64_t resolved = guest_path_from_user(proc, context->rdi, host);
    if (resolved != 0) {
        return resolved;
    }
    if ((flags & abi::kOpenDirectory) != 0) {
        const vfs::Vnode* node = vfs::open(proc.credentials, host, 0, 0);
        if (node == nullptr) return vfs_failure();
        if (node->type != vfs::NodeType::directory) return -abi::kENOTDIR;
    }
    object::IoObject* file = nullptr;
    const sx_status opened = sx::open_file(proc.credentials, host, open_flags,
                                           static_cast<uint16_t>(context->rdx & vfs::kModePermissions & ~paths_of(proc)->mask),
                                           sx::open_accept_directory, file);
    if (opened != sx::kStatusOk) {
        return -static_cast<int64_t>(opened);
    }
    // A full handle table is the only way to fail here, which Linux calls
    // EMFILE.
    const int handle = process::install_io_object(proc, file);
    if (handle < 0) {
        return -abi::kEMFILE;
    }
    const int64_t descriptor = allocate_descriptor(
        *state, static_cast<uint64_t>(handle),
        (flags & abi::kOpenCloseOnExec) != 0 ? kDescriptorCloseOnExec : 0u);
    if (descriptor < 0) {
        (void)sx::close_handle(proc, static_cast<uint64_t>(handle));
        return -abi::kEMFILE;
    }
    return descriptor;
}

// close(fd).
int64_t linux_close(process::Process& proc, const process::SavedContext* context) {
    auto* d = descriptor_of(proc, context->rdi);
    if (!d) return -abi::kEBADF;
    const Descriptor old = *d;
    *d = {};
    if (old.handle_plus_one != kNoSocketHandle) (void)sx::close_handle(proc, old.handle_plus_one - 1);
    if ((old.flags & kDescriptorSocket) && old.read_handle_plus_one && old.read_handle_plus_one != kNoSocketHandle)
        (void)sx::close_handle(proc, old.read_handle_plus_one - 1);
    return 0;
}

// lseek(fd, offset, whence). A pipe, a socket or a device has no offset.
int64_t linux_lseek(process::Process& proc, const process::SavedContext* context) {
    if (auto* d = descriptor_of(proc, context->rdi); d && (d->flags & kDescriptorSocket)) return -abi::kESPIPE;
    uint64_t handle = 0;
    if (!handle_of(proc, context->rdi, handle)) {
        return -abi::kEBADF;
    }
    object::IoObject* const file = process::io_object_of(proc, handle);
    if (file == nullptr) {
        return -abi::kEBADF;
    }
    if (file->kind != process::HandleKind::vnode) {
        return -abi::kESPIPE;
    }
    sx::SeekOrigin origin = sx::SeekOrigin::start;
    switch (context->rdx) {
        case abi::kSeekSet:
            break;
        case abi::kSeekCur:
            origin = sx::SeekOrigin::current;
            break;
        case abi::kSeekEnd:
            origin = sx::SeekOrigin::end;
            break;
        default:
            return -abi::kEINVAL;
    }
    uint64_t position = 0;
    const sx_status status = sx::seek_file(*file, static_cast<int64_t>(context->rsi), origin, position);
    if (status != sx::kStatusOk) {
        return -static_cast<int64_t>(status);
    }
    return static_cast<int64_t>(position);
}

// What the VFS said went wrong, as a negative Linux error; never a success,
// even when it left no error behind.
int64_t vfs_failure() {
    const int error = vfs::last_error();
    return error > 0 ? -static_cast<int64_t>(error) : -abi::kENOENT;
}

// struct stat of a node. Times are the modification time, the only one
// recorded.
void stat_of_node(const vfs::Vnode& node, abi::Stat& out) {
    memset(&out, 0, sizeof(out));
    const vfs::Ownership owner = vfs::ownership_of(node);
    uint32_t permissions = owner.mode & vfs::kModePermissions;
    if (node.backend == vfs::Backend::volume && !node.writable) {
        permissions &= ~0222u;
    }
    if (node.backend == vfs::Backend::device) {
        out.mode = abi::kModeCharacter | permissions;
    } else if (node.type == vfs::NodeType::directory) {
        out.mode = abi::kModeDirectory | permissions;
        out.nlink = 2;
    } else {
        out.mode = abi::kModeRegular | permissions;
        out.size = static_cast<int64_t>(node.size);
        out.blocks = static_cast<int64_t>((node.size + 511) / 512);
    }
    if (out.nlink == 0) {
        out.nlink = 1;
    }
    out.dev = 1;
    out.ino = vfs::node_number(node);
    out.uid = owner.uid;
    out.gid = owner.gid;
    out.blksize = static_cast<int64_t>(kPage);
    out.atime_sec = static_cast<int64_t>(node.mtime);
    out.mtime_sec = static_cast<int64_t>(node.mtime);
    out.ctime_sec = static_cast<int64_t>(node.mtime);
}

int64_t copy_stat_out(uint64_t user_out, const abi::Stat& value) {
    if (!sx::validate_user_range(user_out, sizeof(value), true) || !sx::copy_to_user(user_out, &value, sizeof(value))) {
        return -abi::kEFAULT;
    }
    return 0;
}

// fstat(fd, statbuf).
int64_t linux_fstat(process::Process& proc, const process::SavedContext* context) {
    if (auto* d = descriptor_of(proc, context->rdi); d && (d->flags & kDescriptorSocket)) {
        abi::Stat value = {};
        value.nlink = 1; value.dev = 1; value.blksize = kPage;
        value.mode = abi::kModeSocket | 0777u;
        return copy_stat_out(context->rsi, value);
    }
    uint64_t handle = 0;
    if (!handle_of(proc, context->rdi, handle)) {
        return -abi::kEBADF;
    }
    object::IoObject* const file = process::io_object_of(proc, handle);
    if (file == nullptr) {
        return -abi::kEBADF;
    }
    abi::Stat value;
    if (file->kind == process::HandleKind::vnode && file->node != nullptr) {
        stat_of_node(*file->node, value);
    } else {
        // A pipe, a socket or a device the process was handed: its type and
        // nothing more is known.
        memset(&value, 0, sizeof(value));
        value.nlink = 1;
        value.dev = 1;
        value.blksize = static_cast<int64_t>(kPage);
        value.mode = file->kind == process::HandleKind::pipe     ? abi::kModeFifo | 0600u
                     : file->kind == process::HandleKind::socket ? abi::kModeSocket | 0777u
                                                                 : abi::kModeCharacter | 0666u;
    }
    return copy_stat_out(context->rsi, value);
}

// stat(path, statbuf) and lstat: the file system has no links.
int64_t linux_stat(process::Process& proc, const process::SavedContext* context) {
    vfs::PathBuffer host;
    const int64_t resolved = guest_path_from_user(proc, context->rdi, host);
    if (resolved != 0) {
        return resolved;
    }
    // A query open: reaching the node is the whole check, as for stat.
    const vfs::Vnode* node = vfs::open(proc.credentials, host, 0, 0);
    if (node == nullptr) {
        return vfs_failure();
    }
    abi::Stat value;
    stat_of_node(*node, value);
    return copy_stat_out(context->rsi, value);
}

// unlink(path). A directory is EISDIR, as on Linux; rmdir is its call.
int64_t linux_unlink(process::Process& proc, const process::SavedContext* context) {
    vfs::PathBuffer host;
    const int64_t resolved = guest_path_from_user(proc, context->rdi, host);
    if (resolved != 0) {
        return resolved;
    }
    const vfs::Vnode* node = vfs::open(proc.credentials, host, 0, 0);
    if (node == nullptr) {
        return vfs_failure();
    }
    if (node->type == vfs::NodeType::directory) {
        return -abi::kEISDIR;
    }
    if (!vfs::unlink(proc.credentials, host)) {
        return vfs_failure();
    }
    return 0;
}

// getdents64 uses the VFS cursor; an undersized buffer must not consume an entry.
int64_t linux_getdents(process::Process& proc, const process::SavedContext* c) {
    uint64_t handle;
    if (!handle_of(proc, c->rdi, handle)) return -abi::kEBADF;
    auto* file = process::io_object_of(proc, handle);
    if (file == nullptr || file->node == nullptr || file->node->type != vfs::NodeType::directory)
        return -abi::kENOTDIR;
    const size_t capacity = c->rdx < abi::kMaxRwCount ? c->rdx : abi::kMaxRwCount;
    if (!sx::validate_user_range(c->rsi, capacity, true)) return -abi::kEFAULT;
    size_t total = 0;
    for (;;) {
        const int saved = file->cursor.next;
        const auto* node = vfs::next_entry(file->cursor);
        if (node == nullptr) return total ? static_cast<int64_t>(total) : (vfs::last_error() ? vfs_failure() : 0);
        const size_t length = strlen(node->name);
        const size_t record = (abi::kDirentNameOffset + length + 1 + 7) & ~size_t(7);
        if (record > capacity - total) {
            file->cursor.next = saved;
            return total ? static_cast<int64_t>(total) : -abi::kEINVAL;
        }
        char bytes[512]{};
        if (record > sizeof(bytes)) { file->cursor.next = saved; return -abi::kENAMETOOLONG; }
        uint64_t ino = vfs::node_number(*node);
        int64_t offset = static_cast<int64_t>(file->offset + record);
        uint16_t size = static_cast<uint16_t>(record);
        memcpy(bytes, &ino, 8);
        memcpy(bytes + 8, &offset, 8);
        memcpy(bytes + 16, &size, 2);
        bytes[18] = node->type == vfs::NodeType::directory ? abi::kDirentTypeDirectory : abi::kDirentTypeRegular;
        memcpy(bytes + abi::kDirentNameOffset, node->name, length + 1);
        if (!sx::copy_to_user(c->rsi + total, bytes, record)) {
            file->cursor.next = saved;
            return total ? static_cast<int64_t>(total) : -abi::kEFAULT;
        }
        file->offset += record;
        total += record;
    }
}

int64_t linux_chdir(process::Process& proc, const process::SavedContext* c) {
    vfs::PathBuffer host;
    int64_t result = guest_path_from_user(proc, c->rdi, host);
    if (result) return result;
    const auto* node = vfs::open(proc.credentials, host, 0, 0);
    if (!node) return vfs_failure();
    if (node->type != vfs::NodeType::directory) return -abi::kENOTDIR;
    if (!vfs::permits(proc.credentials, *node, vfs::kAccessExecute)) return -abi::kEACCES;
    const char* guest = static_cast<const char*>(host) + strlen("/system/linux");
    strcpy(paths_of(proc)->cwd, *guest ? guest : "/");
    return 0;
}

int64_t linux_getcwd(process::Process& proc, const process::SavedContext* c) {
    const char* cwd = paths_of(proc)->cwd;
    size_t length = strlen(cwd) + 1;
    if (c->rsi < length) return -abi::kERANGE;
    return sx::copy_to_user(c->rdi, cwd, length) ? static_cast<int64_t>(length) : -abi::kEFAULT;
}

int64_t linux_mkdir(process::Process& proc, const process::SavedContext* c) {
    vfs::PathBuffer host;
    int64_t result = guest_path_from_user(proc, c->rdi, host);
    if (result) return result;
    if (vfs::open(proc.credentials, host, 0, 0)) return -abi::kEEXIST;
    if (vfs::last_error() != SX_ENOENT) return vfs_failure();
    return vfs::mkdir(proc.credentials, host, c->rsi & 0777 & ~paths_of(proc)->mask) ? 0 : vfs_failure();
}

int64_t linux_fcntl(process::Process& proc, const process::SavedContext* c) {
    auto* d = descriptor_of(proc, c->rdi);
    if (!d) return -abi::kEBADF;
    if (c->rsi == abi::kFcntlGetFd) return d->flags & kDescriptorCloseOnExec ? abi::kFdCloseOnExec : 0;
    if (c->rsi == abi::kFcntlSetFd) {
        d->flags = (d->flags & ~kDescriptorCloseOnExec) | (c->rdx & abi::kFdCloseOnExec ? kDescriptorCloseOnExec : 0);
        return 0;
    }
    if (c->rsi == abi::kFcntlGetFl) {
        if (d->flags & kDescriptorSocket) return abi::kOpenReadWrite | (d->flags & kDescriptorNonblock ? abi::kOpenNonblock : 0);
        auto* file = process::io_object_of(proc, d->handle_plus_one - 1);
        if (!file) return -abi::kEBADF;
        return (file->open_flags & process::open_write ? (file->open_flags & process::open_read ? abi::kOpenReadWrite : abi::kOpenWriteOnly) : abi::kOpenReadOnly) |
            (file->open_flags & process::open_nonblock ? abi::kOpenNonblock : 0) | abi::kOpenLargeFile;
    }
    if (c->rsi == abi::kFcntlSetFl) {
        if (c->rdx & ~(abi::kOpenAccessMode | abi::kOpenNonblock | abi::kOpenLargeFile)) return -abi::kEINVAL;
        d->flags = (d->flags & ~kDescriptorNonblock) | (c->rdx & abi::kOpenNonblock ? kDescriptorNonblock : 0);
        const uint32_t handles[] = {d->handle_plus_one, d->read_handle_plus_one};
        for (uint32_t handle : handles) {
            if (!handle || handle == kNoSocketHandle) continue;
            auto* file = process::io_object_of(proc, handle - 1);
            if (file) file->open_flags = (file->open_flags & ~process::open_nonblock) |
                (c->rdx & abi::kOpenNonblock ? process::open_nonblock : 0);
        }
        return 0;
    }
    report_unsupported(proc, abi::kSysFcntl, "fcntl command");
    return -abi::kEINVAL;
}

// Transfer one page per call (short transfers are permitted). A full blocking
// pipe parks before any byte moves; the wait hook retries the instruction.
void recheck_transfer(process::Thread& thread, uint64_t, bool) {
    auto* state = thread_state_of(thread);
    if (!state || !state->transfer_wait) return;
    auto* out = process::io_object_of(*thread.process, state->transfer_output);
    if (out && out->pipe && out->pipe->reader_refs &&
        process::detail::kPipeCapacity - out->pipe->size < state->transfer_count) return;
    state->transfer_wait = false;
    thread.context->rip -= 2;
    process::detail::complete_blocked_wait(thread, abi::kSysSendfile);
}
process::SavedContext* linux_sendfile(process::Thread& self, process::SavedContext* c) {
    auto* transfer = thread_state_of(self);
    if (transfer == nullptr) {
        set_result(c, -abi::kENOMEM);
        return c;
    }
    auto& proc = *self.process;
    uint64_t input, output;
    if (!handle_of(proc, c->rsi, input) || !handle_of(proc, c->rdi, output)) {
        set_result(c, -abi::kEBADF); return c;
    }
    auto* file = process::io_object_of(proc, input);
    if (!file || !(file->open_flags & SAVANXP_OPEN_READ)) { set_result(c, -abi::kEBADF); return c; }
    if (!file->node || file->node->type != vfs::NodeType::file) { set_result(c, -abi::kEINVAL); return c; }
    // The measured profile uses the open file offset. Explicit offsets arrive
    // with a consumer, rather than being silently ignored.
    if (c->rdx) { report_unsupported(proc, abi::kSysSendfile, "explicit offset"); set_result(c, -abi::kEINVAL); return c; }
    if (!c->r10) { set_result(c, 0); return c; }
    char bytes[kPage];
    size_t count = vfs::read(*file->node, file->offset, bytes, c->r10 < kPage ? c->r10 : kPage);
    if (!count) { set_result(c, vfs::last_error() ? vfs_failure() : 0); return c; }
    uint64_t page = 0;
    if (vm::map_region(proc.address_space, page, kPage, region::kRead | region::kWrite, vm::RegionPlacement::anywhere) != vm::RegionStatus::ok) {
        set_result(c, -abi::kENOMEM); return c;
    }
    if (!sx::copy_to_user(page, bytes, count)) {
        (void)vm::unmap_regions(proc.address_space, page, kPage);
        set_result(c, -abi::kEFAULT); return c;
    }
    const int result = process::native_write_handle(self, output, page, count, false);
    (void)vm::unmap_regions(proc.address_space, page, kPage);
    auto* out = process::io_object_of(proc, output);
    if (result == -SX_EAGAIN && out && out->kind == process::HandleKind::pipe &&
        !(out->open_flags & process::open_nonblock)) {
        transfer->transfer_output = output;
        transfer->transfer_count = count;
        transfer->transfer_wait = true;
        self.wake_deadline_ms = 0;
        self.wait_reason = process::WaitReason::none;
        self.state = process::State::blocked_wait;
        return process::choose_next_context(c);
    }
    if (result > 0) file->offset += result;
    set_result(c, result);
    return c;
}

// clock_gettime(clock, timespec): what musl calls with no vDSO, for
// clock_gettime itself and for time(). CLOCK_REALTIME is the RTC's date
// carried by the monotonic clock; CLOCK_MONOTONIC is the kernel's.
int64_t linux_clock_gettime(process::Process& proc, const process::SavedContext* context) {
    uint64_t nanoseconds = 0;
    switch (context->rdi) {
        case abi::kClockRealtime:
            nanoseconds = rtc::epoch_ns();
            break;
        case abi::kClockMonotonic:
            nanoseconds = timer::monotonic_ns();
            break;
        default:
            if (context->rdi < abi::kClockLimit) {
                report_unsupported(proc, abi::kSysClockGettime, "clock");
            }
            return -abi::kEINVAL;
    }
    const abi::Timespec value{static_cast<int64_t>(nanoseconds / 1000000000ULL),
                              static_cast<int64_t>(nanoseconds % 1000000000ULL)};
    if (!sx::validate_user_range(context->rsi, sizeof(value), true) ||
        !sx::copy_to_user(context->rsi, &value, sizeof(value))) {
        return -abi::kEFAULT;
    }
    return 0;
}

// nanosleep(request, remaining). The kernel keeps its deadlines in
// milliseconds: the wait is rounded up, plus one, so it is never shorter than
// asked, as Linux promises. Nothing interrupts it (no signals yet), so
// `remaining` is never written.
process::SavedContext* linux_nanosleep(process::Thread& self, process::SavedContext* context) {
    abi::Timespec request{};
    if (!sx::copy_from_user(&request, context->rdi, sizeof(request))) {
        set_result(context, -abi::kEFAULT);
        return context;
    }
    if (request.seconds < 0 || request.nanoseconds < 0 || request.nanoseconds > 999999999) {
        set_result(context, -abi::kEINVAL);
        return context;
    }
    if (request.seconds == 0 && request.nanoseconds == 0) {
        set_result(context, 0);
        return context;
    }
    // A century is as good as forever, and keeps the arithmetic in range.
    constexpr int64_t kLongestSeconds = 100LL * 365 * 24 * 3600;
    const uint64_t seconds = static_cast<uint64_t>(request.seconds < kLongestSeconds ? request.seconds : kLongestSeconds);
    const uint64_t milliseconds =
        seconds * 1000u + (static_cast<uint64_t>(request.nanoseconds) + 999999u) / 1000000u + 1u;
    const int result = process::sleep_via_timer(self, milliseconds);
    if (result == process::kBlockedResult) {
        return process::choose_next_context(context);
    }
    set_result(context, result);
    return context;
}

int64_t region_error(process::Process& proc, uint64_t number, vm::RegionStatus status) {
    switch (status) {
        case vm::RegionStatus::ok:
            return 0;
        case vm::RegionStatus::no_memory:
            return -abi::kENOMEM;
        case vm::RegionStatus::occupied:
            return -abi::kEEXIST;
        case vm::RegionStatus::io_error:
            return -abi::kEIO;
        case vm::RegionStatus::not_mapped:
            return -abi::kEFAULT;
        case vm::RegionStatus::foreign:
            report_unsupported(proc, number, "change to memory that is not an anonymous region");
            return -abi::kEINVAL;
        case vm::RegionStatus::invalid:
        default:
            return -abi::kEINVAL;
    }
}

// mmap(address, length, prot, flags, fd, offset): private anonymous memory.
int64_t linux_mmap(process::Process& proc, const process::SavedContext* context) {
    const uint64_t address = context->rdi;
    const uint64_t length = context->rsi;
    const uint64_t prot = context->rdx;
    const uint64_t flags = context->r10;
    const uint64_t offset = context->r9;
    uint64_t size = 0;
    if (length == 0 || (offset & (kPage - 1)) != 0) {
        return -abi::kEINVAL;
    }
    if (!round_to_pages(length, size)) {
        return -abi::kENOMEM;
    }
    if ((prot & ~(abi::kProtRead | abi::kProtWrite | abi::kProtExec | abi::kProtSem)) != 0) {
        return -abi::kEINVAL;
    }
    const uint64_t type = flags & abi::kMapType;
    if (type == abi::kMapShared || type == abi::kMapSharedValidate) {
        report_unsupported(proc, abi::kSysMmap, "shared mapping");
        return -abi::kEINVAL;
    }
    if (type != abi::kMapPrivate) {
        return -abi::kEINVAL;
    }
    // Flags Linux itself ignores or that change nothing here (no memory is
    // reserved ahead) pass; any other is refused rather than half done.
    const uint64_t known = abi::kMapType | abi::kMapFixed | abi::kMapAnonymous | abi::kMapFixedNoReplace |
                           abi::kMapDenyWrite | abi::kMapExecutable | abi::kMapNoReserve | abi::kMapStack;
    if ((flags & ~known) != 0) {
        report_unsupported(proc, abi::kSysMmap, "mmap flag");
        return -abi::kEINVAL;
    }
    if ((prot & abi::kProtWrite) != 0 && (prot & abi::kProtExec) != 0) {
        // W^X holds for foreign programs too (the roadmap's rule).
        report_unsupported(proc, abi::kSysMmap, "writable and executable memory");
        return -abi::kEACCES;
    }

    vm::RegionPlacement placement = vm::RegionPlacement::anywhere;
    uint64_t where = address;
    if ((flags & abi::kMapFixed) != 0) {
        placement = vm::RegionPlacement::replace;
    } else if ((flags & abi::kMapFixedNoReplace) != 0) {
        placement = vm::RegionPlacement::exact;
    }
    if (placement != vm::RegionPlacement::anywhere) {
        if ((where & (kPage - 1)) != 0) {
            return -abi::kEINVAL;
        }
        // mmap_min_addr: Linux's own floor, the same 64 KiB.
        if (where < vm::kUserFloor) {
            return -abi::kEPERM;
        }
    } else if (!round_to_pages(where, where)) {
        where = 0;
    }
    const uint8_t access = prot & (abi::kProtRead | abi::kProtWrite | abi::kProtExec);
    vm::RegionStatus status;
    if (flags & abi::kMapAnonymous) {
        status = vm::map_region(proc.address_space, where, size, access, placement);
    } else {
        uint64_t handle;
        if (!handle_of(proc, context->r8, handle)) return -abi::kEBADF;
        auto* file = process::io_object_of(proc, handle);
        if (!file || !(file->open_flags & process::open_read)) return -abi::kEACCES;
        if (!file->node || file->node->type != vfs::NodeType::file) return -abi::kENODEV;
        auto read = [](void* node, uint64_t position, void* buffer, size_t count) {
            return vfs::read(*static_cast<vfs::Vnode*>(node), position, buffer, count) == count;
        };
        status = vm::map_private_file(proc.address_space, where, size, access, placement,
                                      read, file->node, file->node->size, offset);
    }
    if (status != vm::RegionStatus::ok) {
        return region_error(proc, abi::kSysMmap, status);
    }
    return static_cast<int64_t>(where);
}

int64_t linux_mprotect(process::Process& proc, const process::SavedContext* c) {
    if ((c->rdi & (kPage - 1)) || (c->rdx & ~(abi::kProtRead | abi::kProtWrite | abi::kProtExec | abi::kProtSem)))
        return -abi::kEINVAL;
    if ((c->rdx & abi::kProtWrite) && (c->rdx & abi::kProtExec)) return -abi::kEACCES;
    uint64_t size;
    if (!round_to_pages(c->rsi, size)) return -abi::kENOMEM;
    if (!size) return 0;
    auto result = vm::protect_regions(proc.address_space, c->rdi, size,
        c->rdx & (abi::kProtRead | abi::kProtWrite | abi::kProtExec));
    if (result == vm::RegionStatus::not_mapped || result == vm::RegionStatus::foreign) return -abi::kENOMEM;
    return region_error(proc, abi::kSysMprotect, result);
}

int64_t linux_readlink(process::Process& proc, const process::SavedContext* c) {
    if (!c->rdx) return -abi::kEINVAL;
    vfs::PathBuffer host;
    int64_t result = guest_path_from_user(proc, c->rdi, host);
    if (result) return result;
    // The VFS has no links: existing paths are EINVAL, missing ones retain
    // their lookup error. Never pretend a resolved regular file is a link.
    return vfs::open(proc.credentials, host, 0, 0) ? -abi::kEINVAL : vfs_failure();
}

int64_t linux_getrandom(process::Process& proc, const process::SavedContext* c) {
    if (c->rdx != 0) {
        report_unsupported(proc, abi::kSysGetrandom, "getrandom flags");
        return -abi::kEINVAL;
    }
    const size_t count = c->rsi < 256 ? c->rsi : 256;
    if (!count) return 0;
    uint64_t bytes[32];
    for (uint64_t& word : bytes) word = arch::random_u64();
    return sx::copy_to_user(c->rdi, bytes, count) ? static_cast<int64_t>(count) : -abi::kEFAULT;
}

// munmap(address, length).
int64_t linux_munmap(process::Process& proc, const process::SavedContext* context) {
    const uint64_t address = context->rdi;
    uint64_t size = 0;
    if ((address & (kPage - 1)) != 0 || context->rsi == 0 || !round_to_pages(context->rsi, size)) {
        return -abi::kEINVAL;
    }
    return region_error(proc, abi::kSysMunmap, vm::unmap_regions(proc.address_space, address, size));
}

// mremap(old_address, old_length, new_length, flags, new_address).
int64_t linux_mremap(process::Process& proc, const process::SavedContext* context) {
    const uint64_t address = context->rdi;
    const uint64_t flags = context->r10;
    if ((flags & ~(abi::kMremapMayMove | abi::kMremapFixed | abi::kMremapDontUnmap)) != 0) {
        return -abi::kEINVAL;
    }
    if ((flags & (abi::kMremapFixed | abi::kMremapDontUnmap)) != 0) {
        report_unsupported(proc, abi::kSysMremap, "MREMAP_FIXED or MREMAP_DONTUNMAP");
        return -abi::kEINVAL;
    }
    uint64_t old_size = 0;
    uint64_t new_size = 0;
    if ((address & (kPage - 1)) != 0 || !round_to_pages(context->rsi, old_size) ||
        !round_to_pages(context->rdx, new_size) || old_size == 0 || new_size == 0) {
        return -abi::kEINVAL;
    }
    uint64_t moved_to = 0;
    const vm::RegionStatus status = vm::remap_region(proc.address_space, address, old_size, new_size,
                                                     (flags & abi::kMremapMayMove) != 0, moved_to);
    if (status != vm::RegionStatus::ok) {
        return region_error(proc, abi::kSysMremap, status);
    }
    return static_cast<int64_t>(moved_to);
}

// brk(address): the break moves to `address` when it can, and the call
// answers where the break is, as Linux's does; a failure is the old value.
// The break starts at the end of the image, and its pages are an anonymous
// region like any other, made and taken as it moves.
int64_t linux_brk(process::Process& proc, const process::SavedContext* context) {
    ProcessState* const state = state_of(proc);
    if (state == nullptr) {
        return 0;
    }
    if (state->break_start == 0) {
        state->break_start = proc.address_space.image_end;
        state->break_current = state->break_start;
    }
    const uint64_t requested = context->rdi;
    const uint64_t current = state->break_current;
    uint64_t old_top = 0;
    uint64_t new_top = 0;
    if (requested < state->break_start || !round_to_pages(current, old_top) ||
        !round_to_pages(requested, new_top)) {
        return static_cast<int64_t>(current);
    }
    if (new_top < old_top) {
        if (vm::unmap_regions(proc.address_space, new_top, old_top - new_top) != vm::RegionStatus::ok) {
            return static_cast<int64_t>(current);
        }
    } else if (new_top > old_top) {
        uint64_t at = old_top;
        if (vm::map_region(proc.address_space, at, new_top - old_top, region::kRead | region::kWrite,
                           vm::RegionPlacement::exact) != vm::RegionStatus::ok) {
            return static_cast<int64_t>(current);
        }
    }
    state->break_current = requested;
    return static_cast<int64_t>(requested);
}

// arch_prctl(code, address): the thread pointer musl's start-up sets.
int64_t linux_arch_prctl(process::Thread& self, const process::SavedContext* context) {
    if (context->rdi != abi::kArchSetFs) {
        report_unsupported(*self.process, abi::kSysArchPrctl, "arch_prctl code");
        return -abi::kEINVAL;
    }
    // Linux's answer to a base outside the user half.
    return process::set_thread_base(self, SXN_THREAD_BASE_FS, context->rsi) == 0 ? 0 : -abi::kEPERM;
}

// set_tid_address(pointer). The answer is the thread's id. A Linux process
// has one thread until clone (L7), and on Linux that one's id is the
// process's.
int64_t linux_set_tid_address(process::Thread& self, const process::SavedContext* context) {
    ThreadState* const state = thread_state_of(self);
    if (state != nullptr) {
        state->clear_child_tid = context->rdi;
    }
    return static_cast<int64_t>(self.process->pid);
}

// AF_UNIX streams are subsystem descriptors over a neutral port's two pipes.
// Only the measured X display paths are registered. Abstract names deliberately
// return ECONNREFUSED, so libxcb retries the pathname as in the Linux profile.
int64_t linux_socket(process::Process& proc, const process::SavedContext* c) {
    if (c->rdi != abi::kAfUnix) { report_unsupported(proc, abi::kSysSocket, "socket family"); return -abi::kEAFNOSUPPORT; }
    if ((c->rsi & ~(abi::kSockCloseOnExec | abi::kSockNonblock)) != abi::kSockStream || c->rdx) return -abi::kEPROTONOSUPPORT;
    auto* state = descriptors_of(proc);
    if (!state) return -abi::kENOMEM;
    const int64_t fd = allocate_descriptor(*state, kNoSocketHandle - 1,
        kDescriptorSocket | (c->rsi & abi::kSockCloseOnExec ? kDescriptorCloseOnExec : 0) |
        (c->rsi & abi::kSockNonblock ? kDescriptorNonblock : 0));
    return fd < 0 ? -abi::kEMFILE : fd;
}

Descriptor* socket_of(process::Process& proc, uint64_t fd, int64_t& error) {
    auto* d = descriptor_of(proc, fd);
    error = d ? -abi::kENOTSOCK : -abi::kEBADF;
    return d && (d->flags & kDescriptorSocket) ? d : nullptr;
}

int64_t linux_connect(process::Process& proc, const process::SavedContext* c) {
    int64_t error;
    auto* d = socket_of(proc, c->rdi, error);
    if (!d) return error;
    if (d->handle_plus_one != kNoSocketHandle || d->shutdown) return -abi::kEISCONN;
    if (c->rdx <= abi::kSockaddrFamilySize || c->rdx > abi::kSockaddrUnSize) return -abi::kEINVAL;
    unsigned char address[abi::kSockaddrUnSize] = {};
    if (!sx::copy_from_user(address, c->rsi, c->rdx)) return -abi::kEFAULT;
    // sun_family is a little-endian 16-bit AF_UNIX.
    if (address[0] != abi::kAfUnix || address[1]) return -abi::kEAFNOSUPPORT;
    if (!address[abi::kSockaddrFamilySize]) return -abi::kECONNREFUSED;
    const char* path = reinterpret_cast<const char*>(address + abi::kSockaddrFamilySize);
    const char prefix[] = "/tmp/.X11-unix/X";
    // The display digit follows the prefix; a longer address must end right after it.
    constexpr uint64_t kDigit = sizeof(prefix) - 1;
    constexpr uint64_t kNamed = abi::kSockaddrFamilySize + kDigit + 1;
    if (c->rdx < kNamed || strncmp(path, prefix, kDigit) || (path[kDigit] != '0' && path[kDigit] != '1') ||
        (c->rdx > kNamed && path[kDigit + 1])) return -abi::kENOENT;
    const uint8_t display = path[kDigit] - '0';
    int32_t handles[2];
    // A desktop service may run as the caller's user, or as the system.
    auto* service = object::find_port(display ? "x11d.1" : "x11d.0");
    if (service && !cred::acts_as_system(service->creator) && service->creator.euid != proc.credentials.euid) return -abi::kEACCES;
    const int result = process::connect_port(proc, display ? "x11d.1" : "x11d.0", handles);
    if (result) return result == -SX_ENOENT ? -abi::kECONNREFUSED : result;
    d->handle_plus_one = handles[0] + 1;
    d->read_handle_plus_one = handles[1] + 1;
    d->display = display;
    if (d->flags & kDescriptorNonblock) {
        process::io_object_of(proc, handles[0])->open_flags |= process::open_nonblock;
        process::io_object_of(proc, handles[1])->open_flags |= process::open_nonblock;
    }
    return 0;
}

int64_t linux_getpeername(process::Process& proc, const process::SavedContext* c) {
    int64_t error;
    auto* d = socket_of(proc, c->rdi, error);
    if (!d) return error;
    if (!d->read_handle_plus_one) return -abi::kENOTCONN;
    uint32_t capacity;
    if (!sx::copy_from_user(&capacity, c->rdx, sizeof(capacity)) ||
        !sx::validate_user_range(c->rdx, sizeof(capacity), true)) return -abi::kEFAULT;
    // The family, the 17-byte display path and its NUL.
    unsigned char address[20] = {abi::kAfUnix, 0};
    memcpy(address + 2, "/tmp/.X11-unix/X0", 17);
    address[18] += d->display;
    const uint32_t length = 20;
    const uint32_t copied = capacity < length ? capacity : length;
    if ((copied && !sx::copy_to_user(c->rsi, address, copied)) ||
        !sx::copy_to_user(c->rdx, &length, sizeof(length))) return -abi::kEFAULT;
    return 0;
}

int64_t linux_getsockopt(process::Process& proc, const process::SavedContext* c) {
    int64_t error;
    if (!socket_of(proc, c->rdi, error)) return error;
    if (c->rsi != abi::kSolSocket ||
        (c->rdx != abi::kSoSndbuf && c->rdx != abi::kSoType && c->rdx != abi::kSoError)) {
        report_unsupported(proc, abi::kSysGetsockopt, "socket option"); return -abi::kENOPROTOOPT;
    }
    uint32_t capacity;
    if (!sx::copy_from_user(&capacity, c->r8, sizeof(capacity)) ||
        !sx::validate_user_range(c->r8, sizeof(capacity), true)) return -abi::kEFAULT;
    const uint32_t length = capacity < 4 ? capacity : 4;
    const int value = c->rdx == abi::kSoSndbuf ? process::detail::kPipeCapacity
        : (c->rdx == abi::kSoType ? static_cast<int>(abi::kSockStream) : 0);
    if ((length && !sx::copy_to_user(c->r10, &value, length)) ||
        !sx::copy_to_user(c->r8, &length, sizeof(length))) return -abi::kEFAULT;
    return 0;
}

int64_t linux_shutdown(process::Process& proc, const process::SavedContext* c) {
    int64_t error;
    auto* d = socket_of(proc, c->rdi, error);
    if (!d) return error;
    if (c->rsi > abi::kShutReadWrite) return -abi::kEINVAL;
    if (!d->read_handle_plus_one) return -abi::kENOTCONN;
    // Publish the new state before releasing: pipe closure may recheck polls.
    const uint32_t old_read = d->read_handle_plus_one;
    const uint32_t old_write = d->handle_plus_one;
    if (c->rsi != abi::kShutWrite) { d->shutdown |= kShutdownRead; d->read_handle_plus_one = kNoSocketHandle; }
    if (c->rsi != abi::kShutRead) { d->shutdown |= kShutdownWrite; d->handle_plus_one = kNoSocketHandle; }
    if (c->rsi != abi::kShutWrite && old_read != kNoSocketHandle) (void)sx::close_handle(proc, old_read - 1);
    if (c->rsi != abi::kShutRead && old_write != kNoSocketHandle) (void)sx::close_handle(proc, old_write - 1);
    return 0;
}

int64_t linux_access(process::Process& proc, const process::SavedContext* c) {
    if (c->rsi & ~7ull) return -abi::kEINVAL;
    vfs::PathBuffer host;
    const int64_t result = guest_path_from_user(proc, c->rdi, host);
    if (result) return result;
    if (!vfs::open(proc.credentials, host, 0, 0)) return vfs_failure();
    return vfs::may_access(proc.credentials, host, c->rsi) ? 0 : -abi::kEACCES;
}

int64_t linux_uname(const process::SavedContext* c) {
    char names[6][65] = {};
    strcpy(names[0], "Linux");
    strcpy(names[1], "savanxp");
    strcpy(names[2], SAVANXP_VERSION_STRING "-ssl");
    strcpy(names[3], "Savan Subsystem for Linux");
    strcpy(names[4], "x86_64");
    strcpy(names[5], "(none)");
    return sx::copy_to_user(c->rdi, names, sizeof(names)) ? 0 : -abi::kEFAULT;
}

int evaluate_poll(process::Thread& self, ThreadState& state) {
    int ready = 0;
    for (uint32_t i = 0; i < state.poll_count; ++i) {
        auto& p = state.poll_entries[i];
        p.revents = 0;
        if (p.fd < 0) continue;
        auto* d = descriptor_of(*self.process, p.fd);
        if (!d) p.revents = abi::kPollNval;
        else if (d->flags & kDescriptorSocket) {
            auto* input = d->read_handle_plus_one && d->read_handle_plus_one != kNoSocketHandle
                ? process::io_object_of(*self.process, d->read_handle_plus_one - 1) : nullptr;
            auto* output = d->handle_plus_one != kNoSocketHandle
                ? process::io_object_of(*self.process, d->handle_plus_one - 1) : nullptr;
            // POLLHUP on the input means the peer will never write again.
            // A missing output reader is POLLERR, not a read hangup.
            if (input) p.revents |= process::detail::poll_open_file(input, p.events & abi::kPollIn);
            if (output) {
                p.revents |= process::detail::poll_open_file(output, p.events & abi::kPollOut) & abi::kPollOut;
                if (!output->pipe->reader_refs) p.revents |= abi::kPollErr;
            }
            if (d->shutdown & kShutdownRead) p.revents |= (p.events & abi::kPollIn);
            if (d->shutdown & kShutdownWrite) p.revents |= (p.events & abi::kPollOut);
            if (d->shutdown == (kShutdownRead | kShutdownWrite)) p.revents |= abi::kPollHup;
            if (!d->read_handle_plus_one) p.revents |= abi::kPollHup;
        } else {
            p.revents = process::detail::poll_open_file(
                process::io_object_of(*self.process, d->handle_plus_one - 1), p.events);
        }
        if (p.revents) ++ready;
    }
    return ready;
}

bool commit_poll(process::Thread& self, ThreadState& state) {
    return !state.poll_count || process::write_to_process_memory(*self.process, state.poll_address,
        state.poll_entries, state.poll_count * sizeof(abi::Pollfd));
}

process::SavedContext* linux_poll(process::Thread& self, process::SavedContext* c) {
    auto* state = thread_state_of(self);
    if (!state) { set_result(c, -abi::kENOMEM); return c; }
    if (c->rsi > kMaxDescriptors) { set_result(c, -abi::kEINVAL); return c; }
    state->poll_count = c->rsi;
    state->poll_address = c->rdi;
    const size_t bytes = state->poll_count * sizeof(abi::Pollfd);
    if (bytes && (!sx::validate_user_range(c->rdi, bytes, true) ||
        !sx::copy_from_user(state->poll_entries, c->rdi, bytes))) {
        set_result(c, -abi::kEFAULT); return c;
    }
    const int ready = evaluate_poll(self, *state);
    const int32_t timeout = c->rdx;
    if (ready || !timeout) { set_result(c, commit_poll(self, *state) ? ready : -abi::kEFAULT); return c; }
    process::clear_wait_state(self);
    state->poll_active = true;
    self.wake_deadline_ms = timeout > 0 ? process::current_uptime_ms() + timeout : 0;
    self.state = process::State::blocked_wait;
    return process::choose_next_context(c);
}

// Receive into a validated vector without passing Linux metadata to the kernel.
// A blocking receive parks before reading, then retries its instruction; a
// partial vector is returned immediately, without waiting for another segment.
process::SavedContext* linux_receive(process::Thread& self, process::SavedContext* c, bool message) {
    auto& proc = *self.process;
    int64_t error;
    auto* d = socket_of(proc, c->rdi, error);
    if (!d) { set_result(c, error); return c; }
    const uint64_t flags = message ? c->rdx : c->r10;
    if (flags & ~abi::kMsgDontWait) { report_unsupported(proc, c->rax, "receive flags"); set_result(c, -abi::kEINVAL); return c; }
    if (!d->read_handle_plus_one) { set_result(c, -abi::kENOTCONN); return c; }
    if (d->shutdown & kShutdownRead) { set_result(c, 0); return c; }
    abi::Message header = {};
    abi::Iovec single = {c->rsi, c->rdx};
    uint64_t count = 1;
    if (message) {
        if (!sx::copy_from_user(&header, c->rsi, sizeof(header)) ||
            !sx::validate_user_range(c->rsi, sizeof(header), true)) { set_result(c, -abi::kEFAULT); return c; }
        count = header.iov_count;
        if (count > abi::kIovMax) { set_result(c, -abi::kEINVAL); return c; }
    } else if (c->r8) {
        uint32_t length;
        if (!sx::copy_from_user(&length, c->r9, 4) || !sx::validate_user_range(c->r9, 4, true) ||
            (length && !sx::validate_user_range(c->r8, length, true))) { set_result(c, -abi::kEFAULT); return c; }
    }
    uint64_t length = 0;
    for (uint64_t i = 0; i < count; ++i) {
        abi::Iovec v = single;
        if (message && !sx::copy_from_user(&v, header.iov + i * sizeof(v), sizeof(v))) { set_result(c, -abi::kEFAULT); return c; }
        if (v.length > 0x7fffffffffffffffull - length) { set_result(c, -abi::kEINVAL); return c; }
        length += v.length;
        if (v.length && !sx::validate_user_range(v.base, v.length, true)) { set_result(c, -abi::kEFAULT); return c; }
    }
    if (!length) { set_result(c, 0); return c; }
    const uint64_t handle = d->read_handle_plus_one - 1;
    auto* input = process::io_object_of(proc, handle);
    if (!input) { set_result(c, -abi::kEBADF); return c; }
    if (!process::detail::poll_open_file(input, abi::kPollIn) &&
        !(flags & abi::kMsgDontWait) && !(d->flags & kDescriptorNonblock)) {
        auto* state = thread_state_of(self);
        if (!state) { set_result(c, -abi::kENOMEM); return c; }
        process::clear_wait_state(self);
        state->receive_handle = handle;
        state->receive_syscall = c->rax;
        state->receive_wait = true;
        self.state = process::State::blocked_wait;
        return process::choose_next_context(c);
    }
    // The neutral read uses user buffers, and its nonblocking bit also
    // prevents parking after a short vector segment.
    const uint32_t old_flags = input->open_flags;
    input->open_flags |= process::open_nonblock;
    int64_t total = 0;
    for (uint64_t i = 0; i < count && total < static_cast<int64_t>(abi::kMaxRwCount); ++i) {
        abi::Iovec v = single;
        if (message && !sx::copy_from_user(&v, header.iov + i * sizeof(v), sizeof(v))) { if (!total) total = -abi::kEFAULT; break; }
        if (!v.length) continue;
        const uint64_t n = v.length < abi::kMaxRwCount - total ? v.length : abi::kMaxRwCount - total;
        const int result = process::native_read_handle(self, handle, v.base, n);
        if (result < 0) { if (!total) total = result; break; }
        total += result;
        if (static_cast<uint64_t>(result) < n) break;
    }
    input->open_flags = old_flags;
    if (total >= 0) {
        if (message) {
            header.name_length = 0;
            header.control_length = 0;
            header.flags = 0;
            if (!sx::copy_to_user(c->rsi, &header, sizeof(header))) total = -abi::kEFAULT;
        } else if (c->r8) {
            const uint32_t zero = 0;
            if (!sx::copy_to_user(c->r9, &zero, 4)) total = -abi::kEFAULT;
        }
    }
    set_result(c, total);
    return c;
}

void recheck_transport(process::Thread& thread, uint64_t now, bool deadlines) {
    auto* state = thread_state_of(thread);
    if (!state) return;
    if (state->poll_active) {
        const int ready = evaluate_poll(thread, *state);
        if (!ready && !(deadlines && thread.wake_deadline_ms && thread.wake_deadline_ms <= now)) return;
        state->poll_active = false;
        process::detail::complete_blocked_wait(thread, commit_poll(thread, *state) ? ready : -abi::kEFAULT);
    } else if (state->receive_wait) {
        auto* input = process::io_object_of(*thread.process, state->receive_handle);
        if (input && !process::detail::poll_open_file(input, abi::kPollIn)) return;
        state->receive_wait = false;
        thread.context->rip -= 2;
        process::detail::complete_blocked_wait(thread, state->receive_syscall);
    } else {
        recheck_transfer(thread, now, deadlines);
    }
}

process::SavedContext* dispatch_linux_syscall(process::SavedContext* context) {
    process::Thread* const self = smp::this_cpu().current;
    if (self == nullptr) {
        return context;
    }
    self->context = context;
    self->state = process::State::running;
    process::Process& proc = *self->process;

    switch (context->rax) {
        case abi::kSysSocket: set_result(context, linux_socket(proc, context)); return context;
        case abi::kSysConnect: set_result(context, linux_connect(proc, context)); return context;
        case abi::kSysGetpeername: set_result(context, linux_getpeername(proc, context)); return context;
        case abi::kSysGetsockopt: set_result(context, linux_getsockopt(proc, context)); return context;
        case abi::kSysShutdown: set_result(context, linux_shutdown(proc, context)); return context;
        case abi::kSysAccess: set_result(context, linux_access(proc, context)); return context;
        case abi::kSysUname: set_result(context, linux_uname(context)); return context;
        case abi::kSysPoll: return linux_poll(*self, context);
        case abi::kSysRecvfrom: return linux_receive(*self, context, false);
        case abi::kSysRecvmsg: return linux_receive(*self, context, true);
        case abi::kSysRead:
            return linux_read(*self, context);
        case abi::kSysWrite:
            return linux_write(*self, context);
        case abi::kSysOpen:
            set_result(context, linux_open(proc, context));
            return context;
        case abi::kSysClose:
            set_result(context, linux_close(proc, context));
            return context;
        case abi::kSysSendfile:
            return linux_sendfile(*self, context);
        case abi::kSysGetdents64:
            set_result(context, linux_getdents(proc, context)); return context;
        case abi::kSysChdir:
            set_result(context, linux_chdir(proc, context)); return context;
        case abi::kSysGetcwd:
            set_result(context, linux_getcwd(proc, context)); return context;
        case abi::kSysMkdir:
            set_result(context, linux_mkdir(proc, context)); return context;
        case abi::kSysFcntl:
            set_result(context, linux_fcntl(proc, context)); return context;
        case abi::kSysGetuid:
            set_result(context, proc.credentials.uid); return context;
        case abi::kSysUmask: {
            auto* state = paths_of(proc);
            set_result(context, state->mask);
            state->mask = context->rdi & 0777;
            return context;
        }
        case abi::kSysLstat:
        case abi::kSysStat:
            set_result(context, linux_stat(proc, context));
            return context;
        case abi::kSysFstat:
            set_result(context, linux_fstat(proc, context));
            return context;
        case abi::kSysLseek:
            set_result(context, linux_lseek(proc, context));
            return context;
        case abi::kSysUnlink:
            set_result(context, linux_unlink(proc, context));
            return context;
        case abi::kSysClockGettime:
            set_result(context, linux_clock_gettime(proc, context));
            return context;
        case abi::kSysNanosleep:
            return linux_nanosleep(*self, context);
        case abi::kSysWritev:
            return linux_writev(*self, context);
        case abi::kSysIoctl:
            set_result(context, linux_ioctl(proc, context));
            return context;
        case abi::kSysMprotect:
            set_result(context, linux_mprotect(proc, context)); return context;
        case abi::kSysReadlink:
            set_result(context, linux_readlink(proc, context)); return context;
        case abi::kSysGetrandom:
            set_result(context, linux_getrandom(proc, context)); return context;
        case abi::kSysMmap:
            set_result(context, linux_mmap(proc, context));
            return context;
        case abi::kSysMunmap:
            set_result(context, linux_munmap(proc, context));
            return context;
        case abi::kSysMremap:
            set_result(context, linux_mremap(proc, context));
            return context;
        case abi::kSysBrk:
            set_result(context, linux_brk(proc, context));
            return context;
        case abi::kSysArchPrctl:
            set_result(context, linux_arch_prctl(*self, context));
            return context;
        case abi::kSysSetTidAddress:
            set_result(context, linux_set_tid_address(*self, context));
            return context;
        case abi::kSysExit:
            // The calling thread; the last one takes the process with it.
            process::exit_current_thread(static_cast<int>(context->rdi & 0xff));
        case abi::kSysExitGroup:
            sx::exit_current(static_cast<int>(context->rdi & 0xff));
            return nullptr;
        default:
            report_unimplemented(proc, context->rax);
            set_result(context, -abi::kENOSYS);
            return context;
    }
}

process::SavedContext* dispatch_linux_int80(process::SavedContext* context) {
    context->rax = static_cast<uint64_t>(-abi::kENOSYS);
    return context;
}

} // namespace
} // namespace ssl

namespace process {

bool linux_guest_path(const char* guest, char* host, size_t capacity) {
    static const char kGuestRoot[] = "/system/linux";
    if (guest == nullptr || guest[0] != '/' || capacity <= sizeof(kGuestRoot)) {
        return false;
    }
    size_t written = sizeof(kGuestRoot) - 1;
    memcpy(host, kGuestRoot, written);
    host[written] = '\0';
    // Each component goes after the root, `.` is dropped and `..` takes off
    // the last one but never the root itself. No alias, no link: the file
    // system has none.
    const char* cursor = guest;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            ++cursor;
        }
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        const size_t length = static_cast<size_t>(cursor - start);
        if (length == 0 || (length == 1 && start[0] == '.')) {
            continue;
        }
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (written > sizeof(kGuestRoot) - 1 && host[written - 1] != '/') {
                --written;
            }
            if (written > sizeof(kGuestRoot) - 1) {
                --written;
            }
            host[written] = '\0';
            continue;
        }
        if (written + 1 + length + 1 > capacity) {
            return false;
        }
        host[written++] = '/';
        memcpy(host + written, start, length);
        written += length;
        host[written] = '\0';
    }
    return true;
}

bool linux_interpreter_source(void* credentials, const char* guest,
                              elf::InterpreterSource& source, elf::LoadFailure& failure) {
    vfs::PathBuffer host;
    if (host == nullptr) { failure = elf::LoadFailure::out_of_memory; return false; }
    if (!linux_guest_path(guest, host, host.capacity())) { failure = elf::LoadFailure::bad_header; return false; }
    const auto& who = *static_cast<const cred::Credentials*>(credentials);
    if (!vfs::may_execute(who, host)) {
        failure = vfs::last_error() == SX_EACCES ? elf::LoadFailure::interpreter_denied
                                               : elf::LoadFailure::missing_interpreter;
        return false;
    }
    auto* node = vfs::open(who, host, SAVANXP_OPEN_READ, 0);
    if (!node || node->type != vfs::NodeType::file) {
        failure = node ? elf::LoadFailure::bad_header : elf::LoadFailure::interpreter_denied;
        return false;
    }
    source.context = node;
    source.size = node->size;
    source.read = [](void* context, uint64_t offset, void* buffer, size_t count) {
        auto& file = *static_cast<vfs::Vnode*>(context);
        return offset <= file.size && count <= file.size - offset && vfs::read(file, offset, buffer, count) == count;
    };
    return true;
}

void initialize_linux_subsystem() {
    subsystem::register_wait_recheck(subsystem::Id::linux, &ssl::recheck_transport);
    subsystem::register_process_state_size(subsystem::Id::linux, sizeof(ssl::ProcessState));
    subsystem::register_thread_state_size(subsystem::Id::linux, sizeof(ssl::ThreadState));
    subsystem::register_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_int80);
    subsystem::register_trap_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_syscall);
}

} // namespace process
