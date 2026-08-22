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
uint32_t local_apic_id();
bool local_apic_start_oneshot_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value);
bool local_apic_start_periodic_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value);
uint32_t local_apic_current_timer_count();
void initialize_syscall_gate();
void acknowledge_local_apic_interrupt();
void acknowledge_pic_irq(uint8_t irq);
void set_kernel_stack(uint64_t stack_top);
[[noreturn]] void resume_context(process::SavedContext* context, uint64_t cr3);

/* --- FPU/SSE (estado por proceso) --------------------------------------------
 * El kernel se compila -mno-sse, asi que nunca toca la FPU: el estado x87/SSE
 * pertenece siempre al userland en ejecucion. enable_fpu() habilita SSE en el
 * boot y captura un estado limpio; el scheduler hace fpu_save/fpu_restore al
 * cambiar de proceso, y fpu_init_area() siembra el estado limpio en un proceso
 * nuevo. Areas de 512 bytes alineadas a 16 (formato FXSAVE). */
constexpr uint64_t kFpuStateSize = 512;
void enable_fpu();
void fpu_save(void* area);
void fpu_restore(const void* area);
void fpu_init_area(void* area);
void enable_irq(uint8_t irq);
void disable_irq(uint8_t irq);
void enable_interrupts();
void disable_interrupts();
void halt_once();
[[noreturn]] void halt_forever();

} // namespace arch::x86_64
