// Protected-mode boundary for v0.4.0's local-APIC/IPI mechanism.
#include "lapic.hpp"
#include "cpu.hpp"
#include "paging.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/rtc.hpp"
#include "kernel/timer.hpp"

namespace i386::lapic {
namespace {
volatile uint32_t* g_registers;
uint32_t g_physical;
uint32_t g_timer_count;
bool g_ready;
// As in v0.4.0, a CPU that advertises x2APIC runs it; MSRs work the same in
// protected mode. Otherwise the registers are the MMIO page.
bool g_x2apic;
constexpr uint32_t kBaseMsr = 0x1b;
constexpr uint64_t kBaseEnable = 1u << 11;
constexpr uint64_t kBaseX2Apic = 1u << 10;
constexpr uint64_t kBaseAddressMask = 0x000ffffffffff000ull;
constexpr uint32_t kX2ApicMsrBase = 0x800;
uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t(hi) << 32) | lo;
}
void write_msr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" :: "c"(msr), "a"(uint32_t(value)), "d"(uint32_t(value >> 32)) : "memory");
}
uint64_t base_msr() { return read_msr(kBaseMsr); }
void write_base(uint64_t value) { write_msr(kBaseMsr, value); }
uint32_t read(uint32_t offset) {
    return g_x2apic ? uint32_t(read_msr(kX2ApicMsrBase + (offset >> 4))) : g_registers[offset / 4];
}
void write(uint32_t offset, uint32_t value) {
    if (g_x2apic) {
        write_msr(kX2ApicMsrBase + (offset >> 4), value);
        return;
    }
    g_registers[offset / 4] = value;
    (void)read(0x20); // Serialize posted APIC writes as in the reference.
}
bool enable(bool bsp) {
    const uint64_t base = base_msr();
    if (g_x2apic) {
        // Only an enabled xAPIC may switch to x2APIC; leaving x2APIC directly
        // for xAPIC would fault, so an already extended APIC stays as it is.
        if (!(base & kBaseX2Apic)) {
            write_base(base | kBaseEnable);
            write_base(base | kBaseEnable | kBaseX2Apic);
        }
    } else {
        if ((base & kBaseX2Apic) || (base & kBaseAddressMask) != g_physical) return false;
        write_base(base | kBaseEnable);
    }
    write(0xf0, 0x100u | 255u); // Software enable and a dedicated spurious gate.
    write(0x80, 0); // TPR: accept all ordinary vectors.
    write(0x320, 1u << 16); // Masked until this AP enters the scheduler.
    write(0x350, bsp ? 0x700u : 1u << 16); // Only the BSP owns PIC ExtINT.
    write(0x360, 1u << 16);
    return true;
}
bool delivered() {
    if (g_x2apic) return true; // x2APIC has no delivery-status bit.
    const uint64_t deadline = timer::monotonic_ms() + 100;
    do {
        if (!(read(0x300) & (1u << 12))) return true;
        asm volatile("pause");
    } while (timer::monotonic_ms() < deadline);
    return false;
}
bool wait_for_rtc_edge() {
    savanxp_realtime first{}, current{};
    if (!rtc::read_time(&first) || !first.valid) return false;
    const uint64_t deadline = timer::monotonic_ms() + 2000;
    do {
        boot_screen::animate();
        if (rtc::read_time(&current) && current.valid && current.second != first.second)
            return true;
    } while (timer::monotonic_ms() < deadline);
    return false;
}
}

uint32_t initial_id() {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    return b >> 24;
}
bool initialize(uint32_t address) {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    if (!(d & (1u << 9))) return false;
    g_x2apic = (c & (1u << 21)) != 0;
    if (!g_x2apic) {
        const uint64_t base = base_msr();
        if ((base & kBaseX2Apic) || (base & kBaseAddressMask) != address) return false;
        void* mapped = nullptr;
        if (!paging::map_kernel_mmio(address, 4096, &mapped)) return false;
        g_registers = static_cast<volatile uint32_t*>(mapped);
        g_physical = address;
    }
    g_ready = enable(true);
    if (g_ready) {
        console::printf("cpu: local APIC in %s mode (id %u)\n", g_x2apic ? "x2APIC" : "xAPIC", id());
    }
    return g_ready;
}
bool x2apic_mode() { return g_x2apic; }
void calibrate_timer(uint32_t frequency_hz) {
    constexpr uint32_t kOneShot = 0xffffffffu;
    constexpr uint32_t kFallbackCount = 50000;
    write(0x3e0, 3); // Divide by 16.
    write(0x320, (1u << 16) | kTimerVector);
    // v0.4.0 measures the APIC countdown between consecutive RTC seconds.
    uint32_t count = 0;
    if (wait_for_rtc_edge()) {
        write(0x380, kOneShot);
        if (wait_for_rtc_edge()) {
            const uint32_t remaining = read(0x390);
            if (remaining) count = (kOneShot - remaining) / frequency_hz;
        }
    }
    write(0x380, 0);
    g_timer_count = count ? count : kFallbackCount;
    console::printf("timer: local APIC %s count=%u\n", count ? "RTC calibration" : "fallback", g_timer_count);
}
bool start_timer() {
    if (!g_timer_count) return false;
    write(0x3e0, 3);
    write(0x320, (1u << 17) | kTimerVector); // Periodic, unmasked.
    write(0x380, g_timer_count);
    return true;
}
bool initialize_secondary() { return g_ready && enable(false); }
bool ready() { return g_ready; }
// The xAPIC ID is bits 31:24; the x2APIC ID register is the whole value.
uint32_t id() { return ready() ? (g_x2apic ? read(0x20) : read(0x20) >> 24) : initial_id(); }
void acknowledge() { if (ready()) write(0xb0, 0); }
bool send(uint32_t destination, uint32_t command) {
    if (!ready()) return false;
    if (g_x2apic) {
        // One 64-bit ICR MSR: destination above, command below.
        write_msr(kX2ApicMsrBase + (0x300 >> 4), (uint64_t(destination) << 32) | command);
        return true;
    }
    if (destination > 255 || !delivered()) return false;
    write(0x310, destination << 24);
    write(0x300, command);
    return delivered();
}
}

namespace arch::x86_64 {
bool local_apic_ready() { return i386::lapic::ready(); }
bool local_apic_x2apic_mode() { return i386::lapic::x2apic_mode(); }
uint32_t local_apic_id() { return i386::lapic::id(); }
void acknowledge_local_apic_interrupt() { i386::lapic::acknowledge(); }
bool send_ipi(uint32_t destination, uint8_t vector) {
    return vector >= 32 && i386::lapic::send(destination, (1u << 14) | vector);
}
bool register_interrupt_handler(uint8_t vector, IrqHandler handler, InterruptEoi eoi) {
    if (eoi == InterruptEoi::local_apic && !local_apic_ready()) return false;
    if (eoi == InterruptEoi::pic) return vector >= 0x20 && vector < 0x30 &&
        i386::cpu::register_shared_irq_handler(vector - 0x20, handler);
    return i386::cpu::register_external_handler(vector, handler, eoi == InterruptEoi::local_apic);
}
}
