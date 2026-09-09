#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"

/* Puesta en marcha de los application processors.
 *
 * Fase 0 de docs/SMP_ROADMAP.md: los APs arrancan, se identifican y quedan
 * estacionados en un bucle de hlt. Ninguno planifica ni entra al kernel -- el
 * scheduler y el estado por CPU son de las fases siguientes -- asi que desde el
 * punto de vista del resto del sistema esto no cambia nada. Lo que si deja
 * establecido es el camino de arranque y que el ICR entrega IPIs de verdad, que
 * es de lo que dependen el reschedule y el shootdown de TLB despues.
 */
namespace smp {

// Tope de cores que el kernel sigue. Mas que eso se reporta y se deja dormido.
constexpr uint32_t kMaxCpus = 32;

// Cores que el bootloader reporto, BSP incluido. 1 si no hubo informacion de MP.
uint32_t cpu_count();

// Cores que respondieron al arranque, BSP incluido. Siempre >= 1.
uint32_t online_count();

uint32_t bsp_lapic_id();

// LAPIC id del core `index`. 0 si el indice se va de rango.
uint32_t lapic_id_of(uint32_t index);

// Arranca los APs y los deja estacionados. false si el bootloader no reporto
// informacion de MP o si ningun AP respondio; en los dos casos el sistema sigue
// andando normalmente sobre el BSP, que es todo lo que usa hoy.
bool initialize(const boot::BootInfo& boot_info);

// Manda un ping a cada AP en linea y espera el acuse. Es lo que comprueba que el
// ICR entrega de verdad, que es el prerrequisito del reschedule y del shootdown
// de TLB. true tambien cuando no hay APs: no hay nada que probar.
bool selftest_ipi();

} // namespace smp
