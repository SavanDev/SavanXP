#pragma once

#include <stdint.h>

#include "kernel/process.hpp"

namespace timer {

enum class Backend : uint8_t {
    none = 0,
    local_apic = 1,
    pit = 2,
};

void initialize(uint32_t frequency_hz);
Backend backend();
uint32_t frequency_hz();
uint64_t ticks();
void wait_ticks(uint64_t tick_count);
process::SavedContext* handle_interrupt(process::SavedContext* context);

// Reloj monotono por TSC, calibrado por calibrate_monotonic() al principio del
// arranque (implementado en uacpi_glue.cpp, que es quien lo estreno). A
// diferencia de ticks(), avanza aunque las interrupciones
// esten deshabilitadas (IF=0) -- necesario para esperas seguras durante el
// boot temprano, antes de que el kernel habilite interrupciones globalmente.
// Devuelve 0 si todavia no se calibro.
uint64_t monotonic_ns();

// Lo mismo en milisegundos, que es la unidad en la que el kernel lleva sus
// vencimientos. 0 si todavia no se calibro.
uint64_t monotonic_ms();

// Calibra el reloj de monotonic_ns() contra el PIT, sin interrupciones y sin
// depender de la ACPI. El arranque la llama temprano para tener reloj desde el
// principio; el bring-up de uACPI, que es quien la necesitaba, la vuelve a
// llamar y no hace nada. Cuesta 10 ms y se hace una sola vez.
void calibrate_monotonic();

// Pasa el reloj de pared del TSC al PM timer de la ACPI, si la maquina lo
// expone. Se llama despues de acpi::initialize y es idempotente.
//
// El TSC cuenta ciclos del procesador REAL. Bajo un hipervisor eso no es el
// tiempo que ve la maquina virtual: VirtualBox, con el guest ocupado, atrasa su
// reloj virtual y lo recupera despues, y el TSC se le adelanta -- medido, hasta
// 5,6x contra el RTC. Todo lo que se dosifica contra ese reloj se desmadra, y el
// audio es donde mas se nota: el productor cree que paso mas tiempo del que
// paso, escribe de mas y el driver termina descartando lo que no entra.
//
// El PM timer, en cambio, lo mueve el mismo tiempo virtual que a los devices
// emulados (el DAC entre ellos), asi que productor y consumidor vuelven a estar
// de acuerdo. Es un contador libre, no una cuenta de interrupciones: no pierde
// tiempo cuando la maquina haltea, que era el problema del reloj anterior.
//
// Sin PM timer (o si es de 24 bits, que da la vuelta cada 4,7 s) el reloj sigue
// siendo el TSC.
void adopt_pm_timer();

// Frecuencia del TSC en kHz, del mismo calibrado que alimenta monotonic_ns().
// Es la velocidad que la ventana de propiedades del sistema muestra como reloj
// del procesador. 0 si todavia no se calibro.
uint32_t tsc_khz();

} // namespace timer
