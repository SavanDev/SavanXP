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
 * Puesta en marcha de los application processors y estado por core.
 *
 * El BSP corre initialize_cpu(); un AP corre ap_initialize_cpu(), que es
 * deliberadamente mas angosto: apunta el core a la GDT y la IDT que ya armo el
 * BSP y le prepara la FPU, nada mas. Recien cuando sabe que indice le toca
 * carga su propio TSS, con load_task_register().
 *
 * Lo que un AP nunca hace: programar LINT0 = ExtINT. La regla del APIC es un
 * solo ExtINT por sistema y ese lugar ya lo tomo el BSP en
 * initialize_local_apic(). Por eso ap_initialize_local_apic() existe aparte.
 */
constexpr uint32_t kMaxCpus = 32;

// La GDT es una sola para todos los cores; lo que es de cada core es su TSS.
// Cada TSS tiene su descriptor en la GDT, uno detras de otro a partir de este
// selector, y en modo largo un descriptor de TSS ocupa dos entradas (16 bytes).
constexpr uint16_t kTssSelectorBase = 0x28;
constexpr uint16_t kTssDescriptorSize = 16;

// Indice denso de este core: 0 es el BSP y los APs siguen en orden de arranque.
//
// Sale del selector del TSS cargado. Como cada core carga el suyo, `str`
// identifica al core con una lectura de registro: sin MSR ni MMIO del APIC (que
// bajo KVM o WHPX pueden salir de la VM en cada acceso, y esto se consulta
// decenas de veces por syscall) y sin depender de nada que el userland pueda
// tocar, porque `ltr` es privilegiada.
//
// El asm no es volatile a proposito: TR no cambia durante una entrada al
// kernel, que no es preemptible, asi que el compilador puede fusionar lecturas
// repetidas dentro de una misma funcion. Antes del primer `ltr` TR vale 0 y
// esto devuelve 0, que en ese punto del boot es el BSP.
inline uint32_t cpu_index() {
    uint16_t selector = 0;
    asm("str %0" : "=r"(selector));
    const uint32_t index =
        static_cast<uint32_t>(selector - kTssSelectorBase) / kTssDescriptorSize;
    return index < kMaxCpus ? index : 0;
}

// Lo mismo sin fusionar: lee TR de verdad cada vez. Es para el arranque de un
// AP, el unico lugar donde TR cambia, y donde una lectura adelantada por encima
// del `ltr` devolveria el core equivocado.
inline uint16_t read_task_register() {
    uint16_t selector = 0;
    asm volatile("str %0" : "=r"(selector) : : "memory");
    return selector;
}

void ap_initialize_cpu();
bool ap_initialize_local_apic();

// Carga en este core el TSS del core `index`. Un AP lo hace una sola vez, al
// arrancar; el BSP ya cargo el suyo (el 0) en initialize_cpu().
void load_task_register(uint32_t index);

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
