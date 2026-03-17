#pragma once

#include <stdint.h>

#include "kernel/process.hpp"

namespace arch::x86_64 {

using IrqHandler = void (*)();

enum class InterruptEoi : uint8_t {
    none = 0,
    pic = 1,
    local_apic = 2,
};

void initialize_cpu();
bool register_irq_handler(uint8_t irq, IrqHandler handler);
bool register_interrupt_handler(uint8_t vector, IrqHandler handler, InterruptEoi eoi);
bool initialize_local_apic();
bool local_apic_ready();
bool local_apic_x2apic_mode();
bool local_apic_start_oneshot_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value);
bool local_apic_start_periodic_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value);
uint32_t local_apic_current_timer_count();
void initialize_syscall_gate();
void acknowledge_local_apic_interrupt();
void set_kernel_stack(uint64_t stack_top);
[[noreturn]] void resume_context(process::SavedContext* context, uint64_t cr3);
void enable_irq(uint8_t irq);
void disable_irq(uint8_t irq);
void enable_interrupts();
void disable_interrupts();
void halt_once();
[[noreturn]] void halt_forever();

} // namespace arch::x86_64
