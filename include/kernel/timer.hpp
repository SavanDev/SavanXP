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

// Reloj monotono por TSC, calibrado durante el bring-up de ACPI (ver
// uacpi_glue.cpp). A diferencia de ticks(), avanza aunque las interrupciones
// esten deshabilitadas (IF=0) -- necesario para esperas seguras durante el
// boot temprano, antes de que el kernel habilite interrupciones globalmente.
// Devuelve 0 si todavia no se calibro.
uint64_t monotonic_ns();

} // namespace timer
