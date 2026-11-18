// Native tick delivery and the monotonic clock serve different purposes.
#include "timer32.hpp"
#include "cpu.hpp"
#include "lapic.hpp"
#include "process.hpp"
#include "kernel/acpi.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/timer.hpp"

namespace {
uint64_t g_tsc_per_us, g_tsc_base;
uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}
uint32_t g_frequency_hz;
timer::Backend g_backend = timer::Backend::none;
uint64_t g_ticks;
uint32_t g_pm_last;
uint64_t g_pm_ticks, g_pm_epoch_ns;
bool g_pm_active;

constexpr uint64_t kReportPeriod = 2000;
uint64_t g_report_ns, g_report_tick, g_handler_total_ns, g_handler_max_ns;

uint8_t in8(uint16_t port) { uint8_t value; asm volatile("inb %1,%0" : "=a"(value) : "Nd"(port)); return value; }
void out8(uint16_t port, uint8_t value) { asm volatile("outb %0,%1" :: "a"(value), "Nd"(port)); }

uint64_t tsc_ns() {
    if (!g_tsc_per_us) return 0;
    const uint64_t cycles = rdtsc() - g_tsc_base;
    const uint64_t us = cycles / g_tsc_per_us;
    const uint64_t remainder = cycles - us * g_tsc_per_us;
    return us * 1000 + remainder * 1000 / g_tsc_per_us;
}

void start_pit() {
    constexpr uint32_t kPitHz = 1193182;
    uint32_t divisor = (kPitHz + g_frequency_hz / 2) / g_frequency_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xffff) divisor = 0xffff;
    out8(0x43, 0x34); // Channel 0 rate generator, low byte then high byte.
    out8(0x40, uint8_t(divisor));
    out8(0x40, uint8_t(divisor >> 8));
    i386::cpu::enable_irq(0);
    console::printf("timer: PIT divisor=%u target_hz=%u\n", divisor, g_frequency_hz);
}
}

namespace timer {
// Same PIT-channel-2 calibration as v0.4.0's uACPI clock. It advances with
// IF clear, unlike IRQ ticks, so a device syscall can measure its own deadline.
void calibrate_monotonic() {
    if (g_tsc_per_us) return;
    constexpr uint16_t count = 11932;
    const uint8_t saved = in8(0x61);
    out8(0x61, (saved & ~2u) | 1u);
    out8(0x43, 0xb0); out8(0x42, count & 0xff); out8(0x42, count >> 8);
    const uint64_t start = rdtsc();
    while (!(in8(0x61) & 0x20)) asm volatile("pause");
    g_tsc_per_us = (rdtsc() - start) / 10000;
    if (!g_tsc_per_us) g_tsc_per_us = 1;
    g_tsc_base = rdtsc(); out8(0x61, saved);
}
void adopt_pm_timer() {
    if (g_pm_active || !acpi::ready() || !acpi::pm_timer_port() || !acpi::pm_timer_is_32bit()) return;
    g_pm_epoch_ns = tsc_ns();
    g_pm_last = acpi::pm_timer_read();
    g_pm_active = true;
}

uint64_t monotonic_ns() {
    if (!g_pm_active) return tsc_ns();
    // The BSP samples this during boot. Once scheduling begins the BKL
    // serializes both samples and the extension of the 32-bit counter.
    const uint32_t raw = acpi::pm_timer_read();
    g_pm_ticks += uint32_t(raw - g_pm_last);
    g_pm_last = raw;
    return g_pm_epoch_ns + (g_pm_ticks / acpi::kPmTimerHz) * 1000000000ull +
           (g_pm_ticks % acpi::kPmTimerHz) * 1000000000ull / acpi::kPmTimerHz;
}

uint64_t monotonic_ms() { return monotonic_ns() / 1000000ull; }
uint64_t ticks() { return g_ticks; }
uint32_t frequency_hz() { return g_frequency_hz; }
uint32_t tsc_khz() { return g_tsc_per_us * 1000; }
Backend backend() { return g_backend; }

void initialize(uint32_t frequency_hz) {
    g_frequency_hz = frequency_hz ? frequency_hz : 200;
    console::printf("timer: monotonic source=%s\n", g_pm_active ? "ACPI PM32" : "TSC");
    if (i386::lapic::ready()) {
        i386::lapic::calibrate_timer(g_frequency_hz);
        if (i386::lapic::start_timer()) {
            g_backend = Backend::local_apic;
            console::printf("timer: local APIC target_hz=%u\n", g_frequency_hz);
            return;
        }
    }
    g_backend = Backend::pit;
    start_pit();
}

bool start_on_secondary() {
    return g_backend == Backend::local_apic && i386::lapic::start_timer();
}
}

namespace i386::clock32 {
ContextFrame* tick(ContextFrame* frame) {
    if (cpu::index() != 0) return process::timer(frame);

    const uint64_t entry = timer::monotonic_ns();
    ++g_ticks;
    boot_screen::animate();
    auto* next = process::timer(frame);
    if (!entry) return next;
    const uint64_t end = timer::monotonic_ns();
    const uint64_t spent = end > entry ? end - entry : 0;
    if (!g_report_ns) {
        g_report_ns = entry;
        g_report_tick = g_ticks;
    } else {
        g_handler_total_ns += spent;
        if (spent > g_handler_max_ns) g_handler_max_ns = spent;
    }
    const uint64_t delivered = g_ticks - g_report_tick;
    if (delivered < kReportPeriod) return next;

    const uint64_t elapsed_ms = (end - g_report_ns) / 1000000;
    const uint64_t delivered_ms = delivered * 1000 / g_frequency_hz;
    const uint64_t lost_ms = elapsed_ms > delivered_ms ? elapsed_ms - delivered_ms : 0;
    console::printf("timer-stats: real=%llu ms ticks=%llu ms lost=%llu ms (%llu%%) handler avg=%llu us max=%llu us\n",
                    elapsed_ms, delivered_ms, lost_ms, elapsed_ms ? lost_ms * 100 / elapsed_ms : 0,
                    g_handler_total_ns / delivered / 1000, g_handler_max_ns / 1000);
    g_report_ns = end;
    g_report_tick = g_ticks;
    g_handler_total_ns = g_handler_max_ns = 0;
    return next;
}
}
