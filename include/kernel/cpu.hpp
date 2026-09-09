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

/* --- SMP ---------------------------------------------------------------------
 * Puesta en marcha de los application processors. El BSP corre initialize_cpu();
 * un AP corre ap_initialize_cpu(), que es deliberadamente mas angosto: apunta el
 * core a las tablas que ya armo el BSP y le prepara la FPU, nada mas.
 *
 * Dos cosas que un AP no hace, y conviene saber por que:
 *  - `ltr`. Hay un unico TSS; cargarlo en un segundo core prende su bit Busy y
 *    da #GP. Un TSS por core es trabajo de la fase 1 (docs/SMP_ROADMAP.md).
 *  - LINT0 = ExtINT. La regla del APIC es un solo ExtINT por sistema y ese lugar
 *    ya lo tomo el BSP en initialize_local_apic().
 */
void ap_initialize_cpu();
bool ap_initialize_local_apic();

// Vectores 64-71: mensajes entre cores. Por ahora solo el ping del arranque.
constexpr uint8_t kIpiPingVector = 64;

// IPI de vector fijo (modo fixed, destino fisico, flanco) a un core por su LAPIC
// id. false si el APIC local no esta listo, si el destino no entra en 8 bits en
// modo xAPIC, o si la entrega anterior no termino.
bool send_ipi(uint32_t destination_lapic_id, uint8_t vector);

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
