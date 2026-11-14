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
 * Implemented (L1): write on the standard descriptors, exit and
 * exit_group. Every other number returns -ENOSYS and is logged once per
 * process, so a fixture's first run lists what it needs. A call never returns
 * success without doing its work.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/process.hpp"
#include "kernel/smp.hpp"
#include "kernel/string.hpp"
#include "kernel/subsystem.hpp"
#include "kernel/sx.hpp"
#include "kernel/vfs.hpp"
#include "linux_abi.hpp"

namespace ssl {
namespace {

// The per-process state the kernel reserves for SSL and never reads.
struct ProcessState {
    // One bit per system call number already reported as unimplemented.
    uint64_t reported[abi::kSyscallLimit / 64];
    // A number at or past the limit was reported.
    bool reported_out_of_range;
};

ProcessState* state_of(process::Process& proc) {
    return proc.subsystem_state_bytes == sizeof(ProcessState)
        ? reinterpret_cast<ProcessState*>(proc.subsystem_state)
        : nullptr;
}

void report_unimplemented(process::Process& proc, uint64_t number) {
    ProcessState* const state = state_of(proc);
    if (state != nullptr) {
        if (number < abi::kSyscallLimit) {
            uint64_t& word = state->reported[number / 64];
            const uint64_t bit = 1ull << (number % 64);
            if ((word & bit) != 0) {
                return;
            }
            word |= bit;
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

void set_result(process::SavedContext* context, int64_t value) {
    context->rax = static_cast<uint64_t>(value);
}

// write(fd, buffer, count). Until the descriptor table of L2, a Linux
// descriptor is the process's handle of the same number, and only the
// standard three exist: the handles the spawn gave as 0, 1 and 2.
process::SavedContext* linux_write(process::Thread& self, process::SavedContext* context) {
    const uint64_t descriptor = context->rdi;
    const uint64_t buffer = context->rsi;
    const size_t count = static_cast<size_t>(context->rdx);
    if (descriptor > 2) {
        set_result(context, -abi::kEBADF);
        return context;
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

process::SavedContext* dispatch_linux_syscall(process::SavedContext* context) {
    process::Thread* const self = smp::this_cpu().current;
    if (self == nullptr) {
        return context;
    }
    self->context = context;
    self->state = process::State::running;

    switch (context->rax) {
        case abi::kSysWrite:
            return linux_write(*self, context);
        case abi::kSysExit:
            // The calling thread; the last one takes the process with it.
            process::exit_current_thread(static_cast<int>(context->rdi & 0xff));
        case abi::kSysExitGroup:
            sx::exit_current(static_cast<int>(context->rdi & 0xff));
            return nullptr;
        default:
            report_unimplemented(*self->process, context->rax);
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
    subsystem::register_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_int80);
    subsystem::register_trap_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_syscall);
}

} // namespace process
