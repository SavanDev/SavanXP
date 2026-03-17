#include "kernel/pci.hpp"

#include "kernel/string.hpp"

namespace {

constexpr uint16_t kConfigAddressPort = 0x0cf8;
constexpr uint16_t kConfigDataPort = 0x0cfc;
constexpr size_t kMaxDevices = 64;
constexpr uint16_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciStatusOffset = 0x06;
constexpr uint16_t kPciCapabilitiesPointerOffset = 0x34;
constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciStatusCapabilities = 1u << 4;
constexpr uint8_t kCapabilityVendorSpecific = 0x09;

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

uint64_t decode_bar_base(uint32_t low, uint32_t high) {
    if ((low & 0x1u) != 0) {
        return static_cast<uint64_t>(low & ~0x3u);
    }

    uint64_t base = static_cast<uint64_t>(low & ~0x0fu);
    if ((low & 0x6u) == 0x4u) {
        base |= static_cast<uint64_t>(high) << 32;
    }
    return base;
}

uint64_t decode_bar_size(uint32_t low, uint32_t high) {
    if ((low & 0x1u) != 0) {
        const uint32_t mask = low & ~0x3u;
        return mask != 0 ? static_cast<uint64_t>(~mask + 1u) : 0;
    }

    uint64_t mask = static_cast<uint64_t>(low & ~0x0fu);
    if ((low & 0x6u) == 0x4u) {
        mask |= static_cast<uint64_t>(high) << 32;
    }
    return mask != 0 ? (~mask + 1ULL) : 0;
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
    const uint32_t subsystem_info = pci::read_config_u32(bus, slot, function, 0x2c);
    const uint32_t irq_info = pci::read_config_u32(bus, slot, function, 0x3c);

    info.vendor_id = static_cast<uint16_t>(id & 0xffffu);
    info.device_id = static_cast<uint16_t>((id >> 16) & 0xffffu);
    info.subsystem_vendor_id = static_cast<uint16_t>(subsystem_info & 0xffffu);
    info.subsystem_device_id = static_cast<uint16_t>((subsystem_info >> 16) & 0xffffu);
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

bool bar_info(const DeviceInfo& device, uint8_t bar_index, BarInfo& info) {
    memset(&info, 0, sizeof(info));
    if (!device.present || bar_index >= 6) {
        return false;
    }

    const uint8_t low_offset = static_cast<uint8_t>(0x10 + bar_index * 4);
    const uint32_t original_low = read_config_u32(device.bus, device.slot, device.function, low_offset);
    if (original_low == 0) {
        return false;
    }

    const bool io_space = (original_low & 0x1u) != 0;
    const bool is_64bit = !io_space && (original_low & 0x6u) == 0x4u;
    if (is_64bit && bar_index + 1 >= 6) {
        return false;
    }

    const uint16_t command = read_config_u16(device.bus, device.slot, device.function, kPciCommandOffset);
    uint16_t disabled_command = command;
    if (io_space) {
        disabled_command = static_cast<uint16_t>(disabled_command & ~kPciCommandIo);
    } else {
        disabled_command = static_cast<uint16_t>(disabled_command & ~kPciCommandMemory);
    }
    write_config_u16(device.bus, device.slot, device.function, kPciCommandOffset, disabled_command);

    const uint32_t original_high = is_64bit
        ? read_config_u32(device.bus, device.slot, device.function, static_cast<uint8_t>(low_offset + 4))
        : 0u;

    write_config_u32(device.bus, device.slot, device.function, low_offset, 0xffffffffu);
    if (is_64bit) {
        write_config_u32(device.bus, device.slot, device.function, static_cast<uint8_t>(low_offset + 4), 0xffffffffu);
    }

    const uint32_t sized_low = read_config_u32(device.bus, device.slot, device.function, low_offset);
    const uint32_t sized_high = is_64bit
        ? read_config_u32(device.bus, device.slot, device.function, static_cast<uint8_t>(low_offset + 4))
        : 0u;

    write_config_u32(device.bus, device.slot, device.function, low_offset, original_low);
    if (is_64bit) {
        write_config_u32(device.bus, device.slot, device.function, static_cast<uint8_t>(low_offset + 4), original_high);
    }
    write_config_u16(device.bus, device.slot, device.function, kPciCommandOffset, command);

    info.present = true;
    info.io_space = io_space;
    info.is_64bit = is_64bit;
    info.prefetchable = !io_space && (original_low & 0x8u) != 0;
    info.base = decode_bar_base(original_low, original_high);
    info.size = decode_bar_size(sized_low, sized_high);
    return info.base != 0 && info.size != 0;
}

bool find_vendor_capability(const DeviceInfo& device, uint8_t wanted_cfg_type, VendorCapabilityInfo& info) {
    memset(&info, 0, sizeof(info));
    if (!device.present) {
        return false;
    }

    const uint16_t status = read_config_u16(device.bus, device.slot, device.function, kPciStatusOffset);
    if ((status & kPciStatusCapabilities) == 0) {
        return false;
    }

    uint8_t pointer = static_cast<uint8_t>(read_config_u8(
        device.bus,
        device.slot,
        device.function,
        kPciCapabilitiesPointerOffset
    ) & 0xfcu);

    for (size_t visited = 0; pointer >= 0x40u && visited < 64; ++visited) {
        const uint8_t cap_id = read_config_u8(device.bus, device.slot, device.function, pointer + 0);
        const uint8_t next = static_cast<uint8_t>(read_config_u8(device.bus, device.slot, device.function, pointer + 1) & 0xfcu);
        if (cap_id == kCapabilityVendorSpecific) {
            const uint8_t cap_length = read_config_u8(device.bus, device.slot, device.function, pointer + 2);
            const uint8_t cfg_type = read_config_u8(device.bus, device.slot, device.function, pointer + 3);
            if (cap_length >= 16 && cfg_type == wanted_cfg_type) {
                info.present = true;
                info.offset = pointer;
                info.cfg_type = cfg_type;
                info.bar_index = read_config_u8(device.bus, device.slot, device.function, pointer + 4);
                info.cap_length = cap_length;
                info.capability_offset = read_config_u32(device.bus, device.slot, device.function, pointer + 8);
                info.capability_length = read_config_u32(device.bus, device.slot, device.function, pointer + 12);
                info.extra = cap_length >= 20
                    ? read_config_u32(device.bus, device.slot, device.function, pointer + 16)
                    : 0u;
                return info.bar_index < 6;
            }
        }
        if (next == pointer) {
            break;
        }
        pointer = next;
    }

    return false;
}

uint8_t read_config_u8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    const uint32_t value = read_config_u32(bus, slot, function, static_cast<uint8_t>(offset & 0xfcu));
    return static_cast<uint8_t>((value >> ((offset & 3u) * 8u)) & 0xffu);
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
