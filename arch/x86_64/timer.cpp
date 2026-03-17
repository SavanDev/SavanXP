#include "kernel/timer.hpp"

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/input.hpp"
#include "kernel/process.hpp"
#include "kernel/rtc.hpp"

namespace
{

    constexpr uint8_t kTimerVector = 48;
    constexpr uint8_t kApicDivideBy16 = 0x3;
    constexpr uint32_t kCalibrationOneShotCount = 0xffffffffu;
    constexpr uint32_t kFallbackPeriodicInitialCount = 50000u;

    volatile uint64_t g_ticks = 0;
    uint32_t g_frequency_hz = 0;
    timer::Backend g_backend = timer::Backend::none;

    bool same_second(const savanxp_realtime &left, const savanxp_realtime &right)
    {
        return left.year == right.year &&
               left.month == right.month &&
               left.day == right.day &&
               left.hour == right.hour &&
               left.minute == right.minute &&
               left.second == right.second;
    }

    bool wait_for_next_rtc_second(savanxp_realtime &value)
    {
        savanxp_realtime initial = {};
        savanxp_realtime current = {};

        if (!rtc::read_time(&initial) || initial.valid == 0)
        {
            return false;
        }

        for (uint32_t spin = 0; spin < 5000000u; ++spin)
        {
            if (!rtc::read_time(&current) || current.valid == 0)
            {
                continue;
            }
            if (!same_second(initial, current))
            {
                value = current;
                return true;
            }
        }

        return false;
    }

    bool calibrate_periodic_initial_count(uint32_t frequency_hz, uint32_t &initial_count)
    {
        savanxp_realtime start = {};
        savanxp_realtime end = {};

        if (frequency_hz == 0)
        {
            return false;
        }
        if (!wait_for_next_rtc_second(start))
        {
            return false;
        }
        if (!arch::x86_64::local_apic_start_oneshot_timer(kTimerVector, kCalibrationOneShotCount, kApicDivideBy16))
        {
            return false;
        }
        if (!wait_for_next_rtc_second(end))
        {
            return false;
        }

        const uint32_t current_count = arch::x86_64::local_apic_current_timer_count();
        if (current_count == 0 || current_count >= kCalibrationOneShotCount)
        {
            return false;
        }

        const uint64_t elapsed_counts = static_cast<uint64_t>(kCalibrationOneShotCount - current_count);
        initial_count = static_cast<uint32_t>(elapsed_counts / frequency_hz);
        return initial_count != 0;
    }

} // namespace

namespace timer
{

    void initialize(uint32_t frequency_hz)
    {
        if (frequency_hz == 0)
        {
            frequency_hz = 200;
        }

        g_ticks = 0;
        g_frequency_hz = frequency_hz;
        g_backend = Backend::none;

        if (!arch::x86_64::initialize_local_apic())
        {
            return;
        }

        uint32_t periodic_initial_count = 0;
        if (!calibrate_periodic_initial_count(frequency_hz, periodic_initial_count))
        {
            periodic_initial_count = kFallbackPeriodicInitialCount;
            console::printf(
                "timer: local APIC using fallback divisor=%u target_hz=%u\n",
                static_cast<unsigned>(periodic_initial_count),
                static_cast<unsigned>(frequency_hz));
        }
        else
        {
            console::printf(
                "timer: local APIC calibrated divisor=%u target_hz=%u\n",
                static_cast<unsigned>(periodic_initial_count),
                static_cast<unsigned>(frequency_hz));
        }

        if (!arch::x86_64::local_apic_start_periodic_timer(
                kTimerVector,
                periodic_initial_count,
                kApicDivideBy16))
        {
            return;
        }

        g_backend = Backend::local_apic;
    }

    Backend backend()
    {
        return g_backend;
    }

    uint32_t frequency_hz()
    {
        return g_frequency_hz;
    }

    uint64_t ticks()
    {
        return g_ticks;
    }

    void wait_ticks(uint64_t tick_count)
    {
        const uint64_t deadline = ticks() + tick_count;
        while (ticks() < deadline)
        {
            arch::x86_64::halt_once();
        }
    }

    process::SavedContext *handle_interrupt(process::SavedContext *context)
    {
        g_ticks = g_ticks + 1;
        input::poll();
        arch::x86_64::acknowledge_local_apic_interrupt();
        return process::handle_timer_tick(context);
    }

} // namespace timer

extern "C" process::SavedContext *savanxp_handle_timer_interrupt(process::SavedContext *context)
{
    return timer::handle_interrupt(context);
}
