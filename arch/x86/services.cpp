// Native architecture boundary for the reference PCI, audio, network and power services.
#include "services.hpp"
#include "cpu.hpp"
#include "paging.hpp"
#include "kernel/ac97.hpp"
#include "kernel/acpi.hpp"
#include "kernel/audio.hpp"
#include "kernel/audio_device.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/cpu.hpp"
#include "kernel/net.hpp"
#include "kernel/nic.hpp"
#include "kernel/power.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/clipboard.hpp"
#include "kernel/serial_dev.hpp"
#include "kernel/rtl8139.hpp"
#include "kernel/virtio_net.hpp"
#include "kernel/virtio_sound.hpp"
#include "kernel/uacpi_glue.hpp"

namespace {
uint8_t in8(uint16_t port) { uint8_t v; asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }
void out8(uint16_t port, uint8_t v) { asm volatile("outb %0,%1" :: "a"(v), "Nd"(port)); }
}
namespace arch::x86 {
// CPU services for the shared kernel drivers.
bool register_irq_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= 16 || !handler) return false;
    if (irq >= 3 && irq != 8 && irq != 13) {
        const uint16_t elcr = irq < 8 ? 0x4d0 : 0x4d1;
        out8(elcr, in8(elcr) | (1u << (irq & 7)));
    }
    return i386::cpu::register_shared_irq_handler(irq, handler);
}
void enable_irq(uint8_t irq) { i386::cpu::enable_irq(irq); }
uint64_t save_and_disable_interrupts() {
    uint32_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
void restore_interrupts(uint64_t flags) {
    if ((flags & (1u << 9)) != 0) asm volatile("sti" : : : "memory");
}
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
void start_acpi(uint32_t rsdp_physical) {
    acpi::start_sci();
    uacpi_glue::bringup(rsdp_physical);
}
void initialize_devices() {
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
