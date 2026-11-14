#pragma once

#include <stddef.h>
#include <stdint.h>

namespace i386 {

// Shared IRQ/syscall return layout. user_esp/ss exist only for ring 3 frames.
struct ContextFrame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, saved_esp, ebx, edx, ecx, eax;
    uint32_t vector;
    uint32_t eip, cs, eflags, user_esp, ss;
};
static_assert(sizeof(ContextFrame) == 72);
static_assert(offsetof(ContextFrame, vector) == 48);
static_assert(offsetof(ContextFrame, cs) == 56);

struct alignas(16) FpuState {
    uint8_t bytes[512];
};

void initialize_fpu_state(FpuState& state);
void save_fpu_state(FpuState& state);
void restore_fpu_state(const FpuState& state);
void initialize_user_frame(ContextFrame& frame, uint32_t entry, uint32_t stack,
                           uint32_t image_base, uint32_t argc, uint32_t argv);
extern "C" [[noreturn]] void i386_resume(ContextFrame* frame);

} // namespace i386
