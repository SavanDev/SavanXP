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
 * Implemented: write on the standard descriptors, exit and exit_group (L1);
 * memory through the anonymous regions of S2a (mmap, munmap, mremap, brk),
 * the thread pointer (arch_prctl), set_tid_address, ioctl and writev (L2).
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
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/subsystem.hpp"
#include "kernel/sx.hpp"
#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "linux_abi.hpp"

namespace ssl {
namespace {

constexpr uint64_t kPage = 4096;

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

// A standard descriptor of the process. Until the descriptor table, a Linux
// descriptor is the process's handle of the same number, and only the
// standard three exist: the handles the spawn gave as 0, 1 and 2.
bool standard_descriptor(const process::Process& proc, uint64_t descriptor) {
    return descriptor <= 2 && process::handle_is_open(proc, descriptor);
}

// write(fd, buffer, count).
process::SavedContext* linux_write(process::Thread& self, process::SavedContext* context) {
    const uint64_t descriptor = context->rdi;
    const uint64_t buffer = context->rsi;
    size_t count = static_cast<size_t>(context->rdx);
    if (descriptor > 2) {
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
    process::Process& proc = *self.process;
    const uint64_t descriptor = context->rdi;
    const uint64_t vector = context->rsi;
    const uint64_t count = context->rdx;
    if (!standard_descriptor(proc, descriptor)) {
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
    return standard_descriptor(proc, context->rdi) ? -abi::kENOTTY : -abi::kEBADF;
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
        case abi::kSysWrite:
            return linux_write(*self, context);
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
