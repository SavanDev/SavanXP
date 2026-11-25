#pragma once

#include <stdint.h>

namespace arch::x86 {

using IrqHandler = void (*)();

enum class InterruptEoi : uint8_t {
    none = 0,
    pic = 1,
    local_apic = 2,
};

constexpr uint32_t kMaxCpus = 32;

// Protected-mode TSS descriptors occupy one eight-byte GDT slot each.
constexpr uint16_t kTssSelectorBase = 0x28;
constexpr uint16_t kTssDescriptorSize = 8;

// Indice denso de este core: 0 es el BSP y los APs siguen en orden de arranque.
//
// Sale del selector del TSS cargado. Como cada core carga el suyo, `str`
// identifica al core con una lectura de registro: sin MSR ni MMIO del APIC (que
// bajo KVM pueden salir de la VM en cada acceso, y esto se consulta
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

// Vectores 64-71: mensajes entre cores.
//
// El ping lo manda el BSP durante el arranque para comprobar que el ICR entrega;
// su handler solo toca un atomico y no toma el lock del kernel.
//
// El de replanificar entra por su propio stub con contexto (como el timer) y
// si toma el lock: le dice a un core que pase por el scheduler ya, sea para
// tomar un proceso recien despertado o para terminar uno que mataron desde otro
// core.
constexpr uint8_t kIpiPingVector = 64;
constexpr uint8_t kIpiRescheduleVector = 65;

// IPI de vector fijo (modo fixed, destino fisico, flanco) a un core por su LAPIC
// id. false si el APIC local no esta listo, si el destino no entra en 8 bits en
// modo xAPIC, o si la entrega anterior no termino.
bool send_ipi(uint32_t destination_lapic_id, uint8_t vector);

bool register_irq_handler(uint8_t irq, IrqHandler handler);
bool register_interrupt_handler(uint8_t vector, IrqHandler handler, InterruptEoi eoi);
bool local_apic_ready();
bool local_apic_x2apic_mode();
uint32_t local_apic_id();
void acknowledge_local_apic_interrupt();

/* --- identidad del CPU (CPUID) -----------------------------------------------
 * Quien es este procesador, tal como el propio procesador se presenta. Lo pide
 * la ventana de propiedades del sistema, que muestra la marca y la velocidad
 * igual que el "About this Computer" de la epoca.
 *
 * El brand string son las tres hojas 0x80000002..4 concatenadas (48 bytes) y
 * viene con relleno de espacios adelante; query_cpu_identity lo entrega ya
 * recortado. Si la hoja extendida no existe -- un CPU muy viejo, o un
 * hipervisor que la oculta -- `brand` queda vacio y el llamador cae al vendor.
 */
constexpr uint32_t kCpuFeatureSse2 = 1u << 0;
constexpr uint32_t kCpuFeaturePae = 1u << 1;
constexpr uint32_t kCpuFeatureNx = 1u << 2;
constexpr uint32_t kCpuFeatureLongMode = 1u << 3;
constexpr uint32_t kCpuFeatureX2Apic = 1u << 4;
constexpr uint32_t kCpuFeatureHypervisor = 1u << 5;

struct CpuIdentity {
    char vendor[16];  // "GenuineIntel", "AuthenticAMD", ...
    char brand[52];   // hojas 0x80000002..4, recortado; vacio si no hay
    uint32_t features;
};

void query_cpu_identity(CpuIdentity& out);

void enable_irq(uint8_t irq);
void disable_interrupts();
// Save EFLAGS while masking interrupts, then restore the caller's state.
uint64_t save_and_disable_interrupts();
void restore_interrupts(uint64_t flags);
[[noreturn]] void halt_forever();

} // namespace arch::x86
