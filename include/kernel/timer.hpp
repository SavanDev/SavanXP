#pragma once

#include <stdint.h>

#include "kernel/process.hpp"

namespace timer {

enum class Backend : uint8_t {
    none = 0,
    local_apic = 1,
};

void initialize(uint32_t frequency_hz);
Backend backend();
uint32_t frequency_hz();
uint64_t ticks();
void wait_ticks(uint64_t tick_count);
process::SavedContext* handle_interrupt(process::SavedContext* context);

} // namespace timer
