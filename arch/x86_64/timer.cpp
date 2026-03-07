#include "kernel/timer.hpp"

#include "kernel/cpu.hpp"
#include "kernel/process.hpp"
#include "kernel/ps2.hpp"

namespace {

constexpr uint8_t kTimerVector = 48;
constexpr uint8_t kApicDivideBy16 = 0x3;
constexpr uint32_t kPeriodicInitialCount = 50000;

volatile uint64_t g_ticks = 0;
uint32_t g_frequency_hz = 0;
timer::Backend g_backend = timer::Backend::none;

} // namespace

namespace timer {

void initialize(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 100;
    }

    g_ticks = 0;
    g_frequency_hz = frequency_hz;
    g_backend = Backend::none;

    if (!arch::x86_64::initialize_local_apic()) {
        return;
    }

    if (!arch::x86_64::local_apic_start_periodic_timer(
            kTimerVector,
            kPeriodicInitialCount,
            kApicDivideBy16)) {
        return;
    }

    g_backend = Backend::local_apic;
}

Backend backend() {
    return g_backend;
}

uint32_t frequency_hz() {
    return g_frequency_hz;
}

uint64_t ticks() {
    return g_ticks;
}

void wait_ticks(uint64_t tick_count) {
    const uint64_t deadline = ticks() + tick_count;
    while (ticks() < deadline) {
        arch::x86_64::halt_once();
    }
}

process::SavedContext* handle_interrupt(process::SavedContext* context) {
    g_ticks = g_ticks + 1;
    ps2::poll();
    arch::x86_64::acknowledge_local_apic_interrupt();
    return process::handle_timer_tick(context);
}

} // namespace timer

extern "C" process::SavedContext* savanxp_handle_timer_interrupt(process::SavedContext* context) {
    return timer::handle_interrupt(context);
}
