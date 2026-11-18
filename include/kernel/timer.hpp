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

// Start this AP's local timer with the BSP's calibrated count.
bool start_on_secondary();

// Independent of IRQ delivery and usable with IF clear. Returns zero
// until the initial TSC calibration is available.
uint64_t monotonic_ns();
uint64_t monotonic_ms();

// Bootstrap the monotonic clock by timing PIT channel 2 against the TSC.
void calibrate_monotonic();

// Preserve the monotonic epoch while switching to the firmware's 32-bit PM
// counter. Keep the TSC for absent or 24-bit counters, which wrap in 4.7 s.
void adopt_pm_timer();

// Calibrated TSC frequency for the system information API.
uint32_t tsc_khz();

} // namespace timer
