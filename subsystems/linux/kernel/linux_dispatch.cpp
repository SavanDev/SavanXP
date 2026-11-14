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
 * No call is implemented yet. Every number returns -ENOSYS and is logged
 * once per process, so a fixture's first run lists what it needs. A call
 * never returns success without doing its work.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/process.hpp"
#include "kernel/smp.hpp"
#include "kernel/subsystem.hpp"
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

process::SavedContext* dispatch_linux_syscall(process::SavedContext* context) {
    process::Thread* const self = smp::this_cpu().current;
    if (self == nullptr) {
        return context;
    }
    report_unimplemented(*self->process, context->rax);
    context->rax = static_cast<uint64_t>(-abi::kENOSYS);
    return context;
}

process::SavedContext* dispatch_linux_int80(process::SavedContext* context) {
    context->rax = static_cast<uint64_t>(-abi::kENOSYS);
    return context;
}

} // namespace
} // namespace ssl

namespace process {

void initialize_linux_subsystem() {
    subsystem::register_process_state_size(subsystem::Id::linux, sizeof(ssl::ProcessState));
    subsystem::register_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_int80);
    subsystem::register_trap_dispatcher(subsystem::Id::linux, &ssl::dispatch_linux_syscall);
}

} // namespace process
