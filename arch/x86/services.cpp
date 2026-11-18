// Native architecture boundary for the reference PCI, audio, network and power services.
#include "services.hpp"
#include "cpu.hpp"
#include "paging.hpp"
#include "pit.hpp"
#include "kernel/ac97.hpp"
#include "kernel/acpi.hpp"
#include "kernel/audio.hpp"
#include "kernel/audio_device.hpp"
#include "kernel/cpu.hpp"
#include "kernel/net.hpp"
#include "kernel/nic.hpp"
#include "kernel/pci.hpp"
#include "kernel/power.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/clipboard.hpp"
#include "kernel/serial_dev.hpp"
#include "kernel/rtl8139.hpp"
#include "kernel/virtio_net.hpp"
#include "kernel/virtio_sound.hpp"
#include "kernel/timer.hpp"
#include "kernel/uacpi_glue.hpp"

namespace {
uint64_t g_tsc_per_us, g_tsc_base;
uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t(hi) << 32) | lo;
}
uint8_t in8(uint16_t port) { uint8_t v; asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }
void out8(uint16_t port, uint8_t v) { asm volatile("outb %0,%1" :: "a"(v), "Nd"(port)); }
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
uint64_t monotonic_ns() { return g_tsc_per_us ? ((rdtsc() - g_tsc_base) * 1000ull) / g_tsc_per_us : 0; }
uint64_t monotonic_ms() { return monotonic_ns() / 1000000ull; }
uint64_t ticks() { return i386::pit::ticks(); }
uint32_t frequency_hz() { return i386::pit::kFrequencyHz; }
uint32_t tsc_khz() { return g_tsc_per_us * 1000; }
}
namespace arch::x86_64 {
// Retained generic drivers use this historical namespace for the CPU boundary.
bool register_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= 16 || !handler) return false;
    if (irq >= 3 && irq != 8 && irq != 13) {
        const uint16_t elcr = irq < 8 ? 0x4d0 : 0x4d1;
        out8(elcr, in8(elcr) | (1u << (irq & 7)));
    }
    return i386::cpu::register_shared_irq_handler(irq, handler);
}
void enable_irq(uint8_t irq) { i386::cpu::enable_irq(irq); }
void disable_interrupts() { i386::cpu::disable_interrupts(); }
[[noreturn]] void halt_forever() { i386::cpu::halt_forever(); }
}
namespace i386::services {
uint32_t initialize_firmware(uint32_t information) {
    boot::BootInfo boot = {};
    boot.hhdm_offset = paging::kDirectMapBase;
    const auto* bytes = reinterpret_cast<const uint8_t*>(information);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(bytes);
    for (uint32_t offset = 8; offset + 8 <= size;) {
        const auto* tag = reinterpret_cast<const uint32_t*>(bytes + offset);
        if (!tag[0]) break;
        if (tag[1] < 8 || tag[1] > size - offset) break;
        if ((tag[0] == 14 && tag[1] >= 28 && !boot.acpi_rsdp_address) || (tag[0] == 15 && tag[1] >= 44))
            boot.acpi_rsdp_address = information + offset + 8;
        offset = (offset + tag[1] + 7) & ~7u;
    }
    acpi::initialize(boot);
    return boot.acpi_rsdp_address;
}
void initialize(uint32_t rsdp_physical) {
    acpi::start_sci();
    uacpi_glue::bringup(rsdp_physical);
    audio::register_driver(ac97::driver());
    audio::register_driver(virtio_sound::driver());
    audio::bind_best();
    audio_device::initialize();
    nic::register_driver(rtl8139::driver());
    nic::register_driver(virtio_net::driver());
    nic::bind_best();
    net::initialize();
    power::initialize();
    pcspeaker::initialize();
    clipboard::initialize();
    serial_dev::initialize();
}
}
