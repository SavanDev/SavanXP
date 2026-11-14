#include "context.hpp"

#include "cpu.hpp"
#include "kernel/string.hpp"

namespace i386 {

void initialize_fpu_state(FpuState& state) {
    memset(&state, 0, sizeof(state));
    // Architectural reset state: empty x87 tags, round-to-nearest, masked faults.
    state.bytes[0] = 0x7f;
    state.bytes[1] = 0x03;
    state.bytes[24] = 0x80;
    state.bytes[25] = 0x1f;
}

void save_fpu_state(FpuState& state) {
    asm volatile("fxsave (%0)" : : "r"(&state) : "memory");
}

void restore_fpu_state(const FpuState& state) {
    asm volatile("fxrstor (%0)" : : "r"(&state) : "memory");
}

void initialize_user_frame(ContextFrame& frame, uint32_t entry, uint32_t stack,
                           uint32_t image_base, uint32_t argc, uint32_t argv) {
    frame = {};
    frame.gs = frame.fs = frame.es = frame.ds = cpu::kUserDataSelector;
    frame.eip = entry;
    frame.cs = cpu::kUserCodeSelector;
    frame.eflags = 0x202;
    frame.user_esp = stack;
    frame.ss = cpu::kUserDataSelector;
    frame.edi = argc;
    frame.esi = argv;
    frame.ebp = image_base;
    frame.edx = 0x00c0ffee;
}

} // namespace i386
