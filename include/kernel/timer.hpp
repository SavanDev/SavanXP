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

// Calibra el reloj de monotonic_ns() contra el PIT, sin interrupciones y sin
// depender de la ACPI. El arranque la llama temprano para tener reloj desde el
// principio; el bring-up de uACPI, que es quien la necesitaba, la vuelve a
// llamar y no hace nada. Cuesta 10 ms y se hace una sola vez.
void calibrate_monotonic();

// Frecuencia del TSC en kHz, del mismo calibrado que alimenta monotonic_ns().
// Es la velocidad que la ventana de propiedades del sistema muestra como reloj
// del procesador. 0 si todavia no se calibro.
uint32_t tsc_khz();

} // namespace timer
