#pragma once

#include <stdint.h>

namespace i386 {

namespace pit {

/* PIT (Intel 8254) canal 0 en IRQ0: el primer reloj del kernel i386.
 *
 * Un matiz de la arranque con IF=0: el PIT cuenta desde que se programa, pero
 * el PIC solo retiene un IRQ pendiente, asi que los ticks se pierden mientras
 * el arranque corre con interrupciones apagadas. uptime_ms() por eso arranca
 * practicamente en cero cuando el proceso entra en ring 3 y no mide el tiempo
 * de arranque; contarlo seria sondear el canal 0 en cada vuelta del bootstrap. */
constexpr uint32_t kFrequencyHz = 100;
constexpr uint32_t kTickMs = 1000 / kFrequencyHz; /* 10 ms por tick */

/* Programa el canal 0 (modo 3, divisor para 100 Hz), registra IRQ0 y abre su
 * mascara en el PIC. Se llama desde el bootstrap con IF=0: la primera entrega
 * ocurre cuando el proceso entra en ring 3 o el kernel se queda ocioso. */
void initialize();

/* Ticks entregados desde la primera interrupcion. Lectura alineada de 32
 * bits: en x86 es atomica contra el incremento de la IRQ. */
uint32_t ticks();

/* Milisegundos desde la primera interrupcion (ticks * kTickMs). */
uint64_t uptime_ms();

/* Synchronous bootstrap-only delay with interrupts enabled. Process SLEEP_MS
 * uses the scheduler's deadline/state mechanism instead of this busy context. */
void sleep_ms(uint32_t milliseconds);

}  // namespace pit

}  // namespace i386
