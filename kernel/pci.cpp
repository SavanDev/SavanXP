#include "kernel/pci.hpp"

#include "kernel/string.hpp"

namespace {

constexpr uint16_t kConfigAddressPort = 0x0cf8;
constexpr uint16_t kConfigDataPort = 0x0cfc;
constexpr size_t kMaxDevices = 64;

pci::DeviceInfo g_devices[kMaxDevices] = {};
size_t g_device_count = 0;
bool g_ready = false;

inline void out32(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint32_t in32(uint16_t port) {
    uint32_t value = 0;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return 0x80000000u |
        (static_cast<uint32_t>(bus) << 16) |
        (static_cast<uint32_t>(slot) << 11) |
        (static_cast<uint32_t>(function) << 8) |
        (offset & 0xfcu);
}

uint16_t vendor_id(uint8_t bus, uint8_t slot, uint8_t function) {
    return static_cast<uint16_t>(pci::read_config_u32(bus, slot, function, 0x00) & 0xffffu);
}

void append_device(uint8_t bus, uint8_t slot, uint8_t function) {
    if (g_device_count >= kMaxDevices) {
        return;
    }

    pci::DeviceInfo& info = g_devices[g_device_count];
    memset(&info, 0, sizeof(info));
    info.present = true;
    info.bus = bus;
    info.slot = slot;
    info.function = function;

    const uint32_t id = pci::read_config_u32(bus, slot, function, 0x00);
    const uint32_t class_info = pci::read_config_u32(bus, slot, function, 0x08);
    const uint32_t type_info = pci::read_config_u32(bus, slot, function, 0x0c);
    const uint32_t irq_info = pci::read_config_u32(bus, slot, function, 0x3c);

    info.vendor_id = static_cast<uint16_t>(id & 0xffffu);
    info.device_id = static_cast<uint16_t>((id >> 16) & 0xffffu);
    info.revision = static_cast<uint8_t>(class_info & 0xffu);
    info.prog_if = static_cast<uint8_t>((class_info >> 8) & 0xffu);
    info.subclass = static_cast<uint8_t>((class_info >> 16) & 0xffu);
    info.class_code = static_cast<uint8_t>((class_info >> 24) & 0xffu);
    info.header_type = static_cast<uint8_t>((type_info >> 16) & 0xffu);
    info.irq_line = static_cast<uint8_t>(irq_info & 0xffu);

    for (uint8_t index = 0; index < 6; ++index) {
        info.bar[index] = pci::read_config_u32(bus, slot, function, static_cast<uint8_t>(0x10 + index * 4));
    }

    ++g_device_count;
}

void scan_function(uint8_t bus, uint8_t slot, uint8_t function) {
    if (vendor_id(bus, slot, function) == 0xffffu) {
        return;
    }
    append_device(bus, slot, function);
}

void scan_slot(uint8_t bus, uint8_t slot) {
    if (vendor_id(bus, slot, 0) == 0xffffu) {
        return;
    }

    scan_function(bus, slot, 0);
    const uint8_t header_type = static_cast<uint8_t>((pci::read_config_u32(bus, slot, 0, 0x0c) >> 16) & 0xffu);
    if ((header_type & 0x80u) == 0) {
        return;
    }

    for (uint8_t function = 1; function < 8; ++function) {
        scan_function(bus, slot, function);
    }
}

} // namespace

namespace pci {

void initialize() {
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            scan_slot(static_cast<uint8_t>(bus), slot);
        }
    }

    g_ready = true;
}

bool ready() {
    return g_ready;
}

size_t device_count() {
    return g_device_count;
}

bool device_info(size_t index, DeviceInfo& info) {
    if (index >= g_device_count) {
        return false;
    }
    info = g_devices[index];
    return true;
}

bool find_device(uint16_t wanted_vendor, uint16_t wanted_device, DeviceInfo& info) {
    for (size_t index = 0; index < g_device_count; ++index) {
        if (g_devices[index].vendor_id == wanted_vendor && g_devices[index].device_id == wanted_device) {
            info = g_devices[index];
            return true;
        }
    }
    return false;
}

uint16_t read_config_u16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    const uint32_t value = read_config_u32(bus, slot, function, static_cast<uint8_t>(offset & 0xfcu));
    return static_cast<uint16_t>((value >> ((offset & 2u) * 8u)) & 0xffffu);
}

uint32_t read_config_u32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    out32(kConfigAddressPort, config_address(bus, slot, function, offset));
    return in32(kConfigDataPort);
}

void write_config_u16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value) {
    const uint8_t aligned = static_cast<uint8_t>(offset & 0xfcu);
    uint32_t current = read_config_u32(bus, slot, function, aligned);
    const uint32_t shift = (offset & 2u) * 8u;
    current &= ~(0xffffu << shift);
    current |= static_cast<uint32_t>(value) << shift;
    write_config_u32(bus, slot, function, aligned, current);
}

void write_config_u32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    out32(kConfigAddressPort, config_address(bus, slot, function, offset));
    out32(kConfigDataPort, value);
}

} // namespace pci
