#pragma once

#include <stdint.h>

/* x86 port I/O, the one copy of it (docs/kernel/ARCH.md).
 *
 * Port I/O is an x86 address space of its own, separate from memory: only
 * drivers for PC-platform hardware use it (ATA, PS/2, RTC, PIT, PCI config
 * mechanism #1, the ACPI fixed registers, AC'97, RTL8139). A driver opts in
 * with `using namespace arch::x86_64::port_io;` inside its own anonymous
 * namespace, which also marks it as a driver that only exists on x86.
 *
 * The instructions are the same in 32-bit protected mode, so an i386 build
 * reuses this header as it is. */
namespace arch::x86_64::port_io {

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline void out16(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline void out32(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint16_t in16(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline uint32_t in32(uint16_t port) {
    uint32_t value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

// A write to the POST diagnostic port, which nothing decodes: about a
// microsecond of delay on real hardware, for the old devices that need time
// between two accesses (the 8259 PIC, slow ATA controllers).
inline void io_wait() {
    out8(0x80, 0);
}

} // namespace arch::x86_64::port_io
