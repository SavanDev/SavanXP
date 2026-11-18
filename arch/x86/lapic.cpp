// Protected-mode boundary for v0.4.0's local-APIC/IPI mechanism.
#include "lapic.hpp"
#include "cpu.hpp"
#include "paging.hpp"
#include "kernel/cpu.hpp"
#include "kernel/timer.hpp"

namespace i386::lapic {
namespace {
volatile uint32_t* g_registers;
uint32_t g_physical;
uint32_t g_timer_count;
constexpr uint32_t kBaseMsr = 0x1b;
constexpr uint64_t kBaseAddressMask = 0x000ffffffffff000ull;
uint64_t base_msr() {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(kBaseMsr));
    return (uint64_t(hi) << 32) | lo;
}
void write_base(uint64_t value) {
    asm volatile("wrmsr" :: "c"(kBaseMsr), "a"(uint32_t(value)), "d"(uint32_t(value >> 32)) : "memory");
}
uint32_t read(uint32_t offset) { return g_registers[offset / 4]; }
void write(uint32_t offset, uint32_t value) {
    g_registers[offset / 4] = value;
    (void)read(0x20); // Serialize posted APIC writes as in the reference.
}
bool enable(bool bsp) {
    const uint64_t base = base_msr();
    if ((base & (1u << 10)) || (base & kBaseAddressMask) != g_physical) return false;
    write_base(base | (1u << 11));
    write(0xf0, 0x100u | 255u); // Software enable and a dedicated spurious gate.
    write(0x80, 0); // TPR: accept all ordinary vectors.
    write(0x320, 1u << 16); // Masked until this AP enters the scheduler.
    write(0x350, bsp ? 0x700u : 1u << 16); // Only the BSP owns PIC ExtINT.
    write(0x360, 1u << 16);
    return true;
}
bool delivered() {
    const uint64_t deadline = timer::monotonic_ms() + 100;
    do {
        if (!(read(0x300) & (1u << 12))) return true;
        asm volatile("pause");
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
    const uint64_t base = base_msr();
    if ((base & (1u << 10)) || (base & kBaseAddressMask) != address) return false;
    void* mapped = nullptr;
    if (!paging::map_kernel_mmio(address, 4096, &mapped)) return false;
    g_registers = static_cast<volatile uint32_t*>(mapped);
    g_physical = address;
    return enable(true);
}
bool calibrate_timer() {
    if (!ready()) return false;
    write(0x3e0, 3); // Divide by 16.
    write(0x320, (1u << 16) | kTimerVector);
    write(0x380, 0xffffffffu);
    const uint64_t deadline = timer::monotonic_ns() + 10000000;
    while (timer::monotonic_ns() < deadline) asm volatile("pause");
    const uint32_t elapsed = 0xffffffffu - read(0x390);
    write(0x380, 0);
    if (elapsed < 100 || elapsed > 0x7fffffffu) return false;
    g_timer_count = elapsed; // 10 ms, matching the existing PIT tick.
    return true;
}
bool start_timer() {
    if (!g_timer_count) return false;
    write(0x3e0, 3);
    write(0x320, (1u << 17) | kTimerVector); // Periodic, unmasked.
    write(0x380, g_timer_count);
    return true;
}
bool initialize_secondary() { return g_registers && enable(false); }
bool ready() { return g_registers != nullptr; }
uint32_t id() { return ready() ? read(0x20) >> 24 : initial_id(); }
void acknowledge() { if (ready()) write(0xb0, 0); }
bool send(uint32_t destination, uint32_t command) {
    if (!ready() || destination > 255 || !delivered()) return false;
    write(0x310, destination << 24);
    write(0x300, command);
    return delivered();
}
}

namespace arch::x86_64 {
bool local_apic_ready() { return i386::lapic::ready(); }
bool local_apic_x2apic_mode() { return false; }
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
