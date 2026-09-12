#include "kernel/timer.hpp"

#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/input.hpp"
#include "kernel/process.hpp"
#include "kernel/rtc.hpp"

namespace
{

    constexpr uint8_t kTimerVector = 48;
    constexpr uint8_t kPitIrq = 0;
    constexpr uint8_t kApicDivideBy16 = 0x3;
    constexpr uint32_t kCalibrationOneShotCount = 0xffffffffu;
    constexpr uint32_t kFallbackPeriodicInitialCount = 50000u;
    constexpr uint32_t kPitBaseFrequencyHz = 1193182u;
    constexpr uint16_t kPitChannel0Data = 0x40;
    constexpr uint16_t kPitCommand = 0x43;

    volatile uint64_t g_ticks = 0;
    uint32_t g_frequency_hz = 0;
    timer::Backend g_backend = timer::Backend::none;

    /* --- entrega del tick, medida contra el TSC ------------------------------
     *
     * El timer es PERIODICO por hardware, asi que no se pierde un tick por
     * re-armarlo tarde: se pierde porque la interrupcion no se puede ENTREGAR,
     * y eso pasa cuando el handler anterior todavia esta corriendo con IF=0.
     * O sea que el costo de este handler se paga en atraso del reloj, y de
     * uptime_ms cuelgan sleep_ms, los plazos de poll, el RTO y el reloj de
     * juego de cualquier port.
     *
     * Que se mida solo es lo unico que vuelve visible esa perdida desde
     * adentro: el TSC dice cuanto tiempo paso de verdad y g_ticks cuantas
     * interrupciones llegaron. La diferencia son los ticks que no se
     * entregaron. Mismo criterio que la linea `windowd-stats:` del compositor.
     */
    constexpr uint64_t kTickReportPeriod = 2000;
    uint64_t g_report_begin_ns = 0;
    uint64_t g_report_begin_tick = 0;
    uint64_t g_handler_ns_total = 0;
    uint64_t g_handler_ns_max = 0;

    void report_tick_delivery(uint64_t now_ns)
    {
        const uint64_t elapsed_ns = now_ns - g_report_begin_ns;
        const uint64_t delivered = g_ticks - g_report_begin_tick;
        const uint64_t expected_ms = elapsed_ns / 1000000ull;
        const uint64_t delivered_ms =
            g_frequency_hz != 0 ? (delivered * 1000ull) / g_frequency_hz : delivered;
        const uint64_t lost_ms = expected_ms > delivered_ms ? expected_ms - delivered_ms : 0;

        console::printf(
            "timer-stats: real=%llu ms ticks=%llu ms perdido=%llu ms (%llu%%) handler avg=%llu us max=%llu us\n",
            (unsigned long long)expected_ms,
            (unsigned long long)delivered_ms,
            (unsigned long long)lost_ms,
            (unsigned long long)(expected_ms != 0 ? (lost_ms * 100ull) / expected_ms : 0),
            (unsigned long long)(delivered != 0 ? (g_handler_ns_total / delivered) / 1000ull : 0),
            (unsigned long long)(g_handler_ns_max / 1000ull));

        g_report_begin_ns = now_ns;
        g_report_begin_tick = g_ticks;
        g_handler_ns_total = 0;
        g_handler_ns_max = 0;
    }

    void out8(uint16_t port, uint8_t value)
    {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
    }

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
            // Calibrar el APIC cuesta dos bordes de segundo del RTC, casi dos
            // segundos de espera activa y el tramo mas largo del arranque:
            // sin esto la barra del splash se congela justo ahi.
            boot_screen::animate();
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

    bool start_pit_timer(uint32_t frequency_hz)
    {
        if (frequency_hz == 0)
        {
            return false;
        }

        uint32_t divisor = (kPitBaseFrequencyHz + (frequency_hz / 2u)) / frequency_hz;
        if (divisor == 0)
        {
            divisor = 1;
        }
        if (divisor > 0xffffu)
        {
            divisor = 0xffffu;
        }

        out8(kPitCommand, 0x34); // channel 0, lobyte/hibyte, rate generator
        out8(kPitChannel0Data, static_cast<uint8_t>(divisor & 0xffu));
        out8(kPitChannel0Data, static_cast<uint8_t>((divisor >> 8) & 0xffu));

        arch::x86_64::enable_irq(kPitIrq);
        console::printf(
            "timer: PIT divisor=%u target_hz=%u\n",
            static_cast<unsigned>(divisor),
            static_cast<unsigned>(frequency_hz));
        return true;
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
            if (start_pit_timer(frequency_hz))
            {
                g_backend = Backend::pit;
            }
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
            if (start_pit_timer(frequency_hz))
            {
                g_backend = Backend::pit;
            }
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
        const uint64_t entry_ns = monotonic_ns();

        g_ticks = g_ticks + 1;
        // La barra del splash avanza con el reloj y no con los pasos del
        // arranque: entre paso y paso pueden pasar segundos, y una barra
        // clavada es justo lo que parece un cuelgue. No hace nada una vez
        // que el splash suelta la pantalla.
        boot_screen::animate();
        device::service_background();
        input::poll();
        if (g_backend == Backend::local_apic)
        {
            arch::x86_64::acknowledge_local_apic_interrupt();
        }
        else if (g_backend == Backend::pit)
        {
            arch::x86_64::acknowledge_pic_irq(kPitIrq);
        }

        process::SavedContext *next = process::handle_timer_tick(context);

        // Sin TSC calibrado no hay con que medir, y el reporte se apaga solo.
        if (entry_ns != 0)
        {
            const uint64_t exit_ns = monotonic_ns();
            const uint64_t spent_ns = exit_ns > entry_ns ? exit_ns - entry_ns : 0;

            g_handler_ns_total += spent_ns;
            if (spent_ns > g_handler_ns_max)
            {
                g_handler_ns_max = spent_ns;
            }
            if (g_report_begin_ns == 0)
            {
                g_report_begin_ns = entry_ns;
                g_report_begin_tick = g_ticks;
            }
            else if (g_ticks - g_report_begin_tick >= kTickReportPeriod)
            {
                report_tick_delivery(exit_ns);
            }
        }
        return next;
    }

} // namespace timer

extern "C" process::SavedContext *savanxp_handle_timer_interrupt(process::SavedContext *context)
{
    return timer::handle_interrupt(context);
}
