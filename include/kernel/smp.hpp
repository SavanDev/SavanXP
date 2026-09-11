#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/cpu.hpp"

/* Los application processors y el estado de cada core.
 *
 * Fase 0 de docs/SMP_ROADMAP.md: los APs arrancan, se identifican y quedan
 * estacionados en un bucle de hlt, y un ping IPI comprueba que el ICR entrega.
 *
 * Fase 1: cada core tiene su propio TSS y su propia entrada smp::Cpu, indexada
 * por el selector de ese TSS (arch::x86_64::cpu_index()). El proceso en curso,
 * el idle y el pedido de replanificar viven ahi en vez de en globales. Ningun AP
 * planifica todavia: su entrada existe, pero su `current` sigue en nulo hasta
 * la fase 2.
 */
namespace smp {

constexpr uint32_t kMaxCpus = arch::x86_64::kMaxCpus;

struct Cpu {
    uint32_t index;
    uint32_t lapic_id;
    // El proceso que corre en este core. Nulo mientras el core no planifica.
    process::Process* current;
    // A que volver cuando no hay nada listo. Hoy solo el BSP tiene uno. Si el de
    // un AP tiene que ser un proceso como el del BSP (un slot de proceso, un
    // espacio de direcciones y 16 KiB de pila cada uno) o un bucle de hlt en el
    // kernel se decide en la fase 2, que es la primera que lo necesita.
    process::Process* idle;
    // Una syscall de este core desperto a otro proceso: la vuelta de esa syscall
    // tiene que entregarle la CPU ya, en vez de esperar al proximo tick.
    bool resched_pending;
};

// Una entrada por core posible, indexada por cpu_index(). Estatica y en cero
// desde el arranque, asi que this_cpu() ya es valida en el BSP antes de
// initialize(): el boot no tiene que ordenarse alrededor de SMP.
extern Cpu g_cpu_state[kMaxCpus];

// El estado del core que ejecuta esto. No guardar la referencia mas alla de una
// entrada al kernel: con la fase 2, un proceso puede volver a entrar por otro
// core.
inline Cpu& this_cpu() {
    return g_cpu_state[arch::x86_64::cpu_index()];
}

// Cores que el bootloader reporto, BSP incluido. 1 si no hubo informacion de MP.
uint32_t cpu_count();

// Cores que respondieron al arranque, BSP incluido. Siempre >= 1.
uint32_t online_count();

uint32_t bsp_lapic_id();

// LAPIC id del core de indice denso `index` (el de cpu_index()). 0 si el indice
// se va de rango o si ese core no arranco.
uint32_t lapic_id_of(uint32_t index);

// Arranca los APs, les asigna indice y TSS propio, y los deja estacionados.
// false si el bootloader no reporto informacion de MP o si ningun AP respondio;
// en los dos casos el sistema sigue andando normalmente sobre el BSP.
bool initialize(const boot::BootInfo& boot_info);

// Manda un ping a cada AP en linea y espera el acuse. Es lo que comprueba que el
// ICR entrega de verdad, que es el prerrequisito del reschedule y del shootdown
// de TLB. true tambien cuando no hay APs: no hay nada que probar.
bool selftest_ipi();

} // namespace smp
