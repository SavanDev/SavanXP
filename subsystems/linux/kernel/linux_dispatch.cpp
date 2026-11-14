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
 * and the clocks (clock_gettime, nanosleep) (L2).
 * Every other number returns -ENOSYS and is logged once per process, so a
 * fixture's first run lists what it needs; a variant of a call the layer
 * does not offer fails with its Linux error and is logged once the same way.
 * A call never returns success without doing its work.
 */

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "kernel/console.hpp"
#include "kernel/process.hpp"
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

namespace ssl {
namespace {

constexpr uint64_t kPage = 4096;

// A Linux process's descriptors. Linux's own default limit is 1024; the
// kernel's handle table is the smaller bound anyway.
constexpr uint32_t kMaxDescriptors = 256;
constexpr uint32_t kDescriptorCloseOnExec = 1u << 0;

// One descriptor: the kernel handle it names, plus one (0 is a free slot,
// so zeroed state is an empty table), and its Linux flags.
struct Descriptor {
    uint32_t handle_plus_one;
    uint32_t flags;
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
    bool descriptors_ready;
    Descriptor descriptors[kMaxDescriptors];
};

// The per-thread state, made on first use.
struct ThreadState {
    // set_tid_address's pointer. Kept; clearing it when the thread ends
    // matters once a Linux process can have more than one (clone, L7).
    uint64_t clear_child_tid;
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
        state->descriptors[descriptor].handle_plus_one == 0) {
        return false;
    }
    handle = state->descriptors[descriptor].handle_plus_one - 1;
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

// A guest path from user memory, as the host path under /system/linux. A
// relative path is taken from the guest root: the working directory is L3's,
// and until then it is "/".
int64_t guest_path_from_user(process::Process& proc, uint64_t user_path, vfs::PathBuffer& host) {
    vfs::PathBuffer guest;
    if (guest == nullptr || host == nullptr) {
        return -abi::kENOMEM;
    }
    char* const text = guest;
    text[0] = '/';
    const sx_status copied = sx::copy_user_path_from(proc, user_path, text + 1, guest.capacity() - 1);
    if (copied == SX_ENAMETOOLONG) {
        return -abi::kENAMETOOLONG;
    }
    if (copied != sx::kStatusOk) {
        return -abi::kEFAULT;
    }
    if (text[1] == '\0') {
        // An empty path names nothing, on Linux too.
        return -abi::kENOENT;
    }
    // "/" + an absolute path is the same path with one more slash, which
    // linux_guest_path drops.
    if (!process::linux_guest_path(text, host, host.capacity())) {
        return -abi::kENAMETOOLONG;
    }
    return 0;
}

// write(fd, buffer, count).
process::SavedContext* linux_write(process::Thread& self, process::SavedContext* context) {
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
    uint64_t handle = 0;
    return handle_of(proc, context->rdi, handle) ? -abi::kENOTTY : -abi::kEBADF;
}

// read(fd, buffer, count).
process::SavedContext* linux_read(process::Thread& self, process::SavedContext* context) {
    const uint64_t buffer = context->rsi;
    size_t count = static_cast<size_t>(context->rdx);
    uint64_t handle = 0;
    if (!handle_of(*self.process, context->rdi, handle)) {
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

// open(path, flags, mode): a regular file under the guest root. The access
// mode, O_CREAT, O_TRUNC and O_CLOEXEC (kept for exec, L7) are honoured;
// O_LARGEFILE is always true here and O_NOCTTY has no terminal to refuse.
// Any other flag is refused rather than half done: O_APPEND needs every
// write at the end, not only the first; a directory is L3's.
int64_t linux_open(process::Process& proc, const process::SavedContext* context) {
    ProcessState* const state = descriptors_of(proc);
    if (state == nullptr) {
        return -abi::kENOMEM;
    }
    const uint64_t flags = context->rsi;
    const uint64_t honoured = abi::kOpenAccessMode | abi::kOpenCreate | abi::kOpenTruncate |
                              abi::kOpenLargeFile | abi::kOpenNoCtty | abi::kOpenCloseOnExec;
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
    // The mode as given: the umask is L3's.
    object::IoObject* file = nullptr;
    const sx_status opened = sx::open_file(proc.credentials, host, open_flags,
                                           static_cast<uint16_t>(context->rdx & vfs::kModePermissions),
                                           sx::open_accept_file, file);
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
    uint64_t handle = 0;
    if (!handle_of(proc, context->rdi, handle)) {
        return -abi::kEBADF;
    }
    state_of(proc)->descriptors[context->rdi] = Descriptor{0, 0};
    (void)sx::close_handle(proc, handle);
    return 0;
}

// lseek(fd, offset, whence). A pipe, a socket or a device has no offset.
int64_t linux_lseek(process::Process& proc, const process::SavedContext* context) {
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

// stat(path, statbuf). lstat would be the same: the file system has no links.
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
    if ((flags & abi::kMapAnonymous) == 0) {
        // A file mapping is S2b's (L4): the file system cannot map one yet,
        // which is what ENODEV says.
        report_unsupported(proc, abi::kSysMmap, "file mapping");
        return -abi::kENODEV;
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
    const vm::RegionStatus status =
        vm::map_region(proc.address_space, where, size,
                       static_cast<uint8_t>(prot & (abi::kProtRead | abi::kProtWrite | abi::kProtExec)), placement);
    if (status != vm::RegionStatus::ok) {
        return region_error(proc, abi::kSysMmap, status);
    }
    return static_cast<int64_t>(where);
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

process::SavedContext* dispatch_linux_syscall(process::SavedContext* context) {
    process::Thread* const self = smp::this_cpu().current;
    if (self == nullptr) {
        return context;
    }
    self->context = context;
    self->state = process::State::running;
    process::Process& proc = *self->process;

    switch (context->rax) {
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

void initialize_linux_subsystem() {
    subsystem::register_process_state_size(subsystem::Id::linux, sizeof(ssl::ProcessState));
    subsystem::register_thread_state_size(subsystem::Id::linux, sizeof(ssl::ThreadState));
    subsystem::register_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_int80);
    subsystem::register_trap_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_syscall);
}

} // namespace process
