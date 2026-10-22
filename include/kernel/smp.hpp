#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/cpu.hpp"

/* Los application processors y el estado de cada core.
 *
 * Fase 0 de docs/kernel/SMP_ROADMAP.md: los APs arrancan, se identifican y quedan
 * estacionados en un bucle de hlt, y un ping IPI comprueba que el ICR entrega.
 *
 * Fase 1: cada core tiene su propio TSS y su propia entrada smp::Cpu, indexada
 * por el selector de ese TSS (arch::x86_64::cpu_index()). El proceso en curso,
 * el idle y el pedido de replanificar viven ahi en vez de en globales.
 *
 * Fase 2: todos los cores corren procesos, de a uno por vez adentro del kernel.
 * Lo que lo hace posible es el lock grande del kernel (ver mas abajo): se toma
 * en cada entrada y se suelta en cada salida, y es lo que convierte al kernel
 * no preemptible de un solo core en uno correcto con varios.
 */
namespace smp {

constexpr uint32_t kMaxCpus = arch::x86_64::kMaxCpus;

struct Cpu {
    uint32_t index;
    uint32_t lapic_id;
    // The thread running on this core. Null while the core does not schedule.
    process::Thread* current;
    // What to fall back to when nothing is ready. One per scheduling core,
    // and of that core only: it never migrates. It is the thread of a process
    // like any other (a yield loop in ring 3) for the same reason as on the
    // BSP: the scheduler cannot return to anything but a SavedContext.
    process::Thread* idle;
    // A thread that ended on this core while its process lives on. It cannot
    // free its own kernel stack, which is the one in use until the scheduler
    // switches away; the next pass through the scheduler here frees it.
    process::Thread* retired;
    // Una syscall de este core desperto a otro proceso: la vuelta de esa
    // syscall tiene que entregarle la CPU ya, en vez de esperar al proximo tick.
    bool resched_pending;
    // Ya se le mando un IPI de replanificar y todavia no paso por el scheduler.
    // Evita repetirlo mientras el primero esta en vuelo.
    bool kicked;
};

// Una entrada por core posible, indexada por cpu_index(). Estatica y en cero
// desde el arranque, asi que this_cpu() ya es valida en el BSP antes de
// initialize(): el boot no tiene que ordenarse alrededor de SMP.
extern Cpu g_cpu_state[kMaxCpus];

// El estado del core que ejecuta esto. No guardar la referencia mas alla de una
// entrada al kernel: un proceso puede volver a entrar por otro core.
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

// Comprueba en un AP que la TLB perezosa del kernel funciona (vm::sync_kernel_tlb):
// el AP lee una pagina del kernel, el BSP la apunta a otra pagina fisica en la
// misma direccion, y el AP la vuelve a leer sin sincronizar y sincronizando. La
// segunda lectura tiene que ver la pagina nueva. true tambien sin APs.
bool selftest_tlb();

/* --- planificar en todos los cores ------------------------------------------
 *
 * El BSP arranca el scheduler en process::start_init(). Ahi pregunta que cores
 * pueden planificar, les crea un idle a cada uno y recien entonces los suelta
 * con start_scheduling(). Un AP que no planifica sigue estacionado como en la
 * fase 0: no molesta, no cuenta como en linea y no tiene idle.
 */

// Este core (por indice denso) puede correr procesos: el BSP siempre, y un AP
// si respondio al arranque, el ping IPI llego y hay timer del APIC local para
// preemptarlo. Es la pregunta que hace start_init() para decidir a quien
// crearle un idle.
bool may_schedule(uint32_t index);

// Este core corre procesos: podia (may_schedule) y tiene idle. Hasta
// start_scheduling(), solo el BSP.
bool can_schedule(uint32_t index);

// Cores que corren procesos, BSP incluido. Es el numero de idles que hay.
uint32_t scheduling_count();

// Suelta a los APs estacionados hacia el scheduler. Lo llama el BSP con el lock
// del kernel tomado y los idles ya creados: cada AP lo espera antes de entrar.
void start_scheduling();

// Asks core `index` to enter the kernel now instead of at its next tick.
// terminate_process() uses it so the core of a process killed from another
// one ends it right away, and the user TLB shootdown so a core sharing an
// address space flushes (vm::sync_user_tlb, on taking the lock). Does nothing
// if the core already has a request in flight or is this core.
void kick(uint32_t index);

/* --- el lock grande del kernel ----------------------------------------------
 *
 * Un solo lock para todo el estado del kernel. Se toma en las entradas
 * (syscall, timer, IRQ externa, excepcion de ring 3, IPI de replanificar) y se
 * suelta en la salida, despues de cambiar de pila: soltarlo antes dejaria que
 * otro core retome el proceso saliente sobre la pila que este core todavia usa.
 *
 * El vector de ping (y cualquier IPI que no toque estado del kernel) NO lo toma:
 * el BSP lo manda durante el arranque, cuando ya corre codigo del kernel, y
 * espera el acuse.
 *
 * El codigo del arranque en kernel_main corre SIN el lock: los APs estan
 * estacionados y no hay con quien competir. Por eso los helpers de abajo
 * miran si el lock lo tiene este core en vez de suponerlo.
 */
void lock_kernel();
void unlock_kernel();
bool kernel_locked_here();

// Esperar la proxima interrupcion desde adentro del kernel: suelta el lock si
// este core lo tiene, `sti; hlt`, `cli`, y lo vuelve a tomar. Vuelve siempre con
// IF=0. Es el reemplazo de cualquier `sti; hlt` en el kernel: dormir con el lock
// tomado deja a todos los demas cores girando hasta la proxima interrupcion.
//
// Un proceso que se duerme aca puede ser desalojado por el tick con su contexto
// en ring 0. Ese contexto solo se retoma en este mismo core (ver
// runnable_here() en process.cpp): el codigo del kernel en la mitad de una
// syscall no esta preparado para despertarse en otro.
void wait_for_interrupt();

} // namespace smp
