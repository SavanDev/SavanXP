#include "kernel/pci.hpp"

#include "arch/x86/io.hpp"
#include "kernel/console.hpp"
#include "kernel/string.hpp"

namespace {

using namespace arch::x86::port_io;

constexpr uint16_t kConfigAddressPort = 0x0cf8;
constexpr uint16_t kConfigDataPort = 0x0cfc;
// The development host has 41 functions; a board of the ICH era with every
// UHCI companion enabled has about 30. 256 leaves room for anything with a
// few add-in cards, and what does not fit is counted and reported.
constexpr size_t kMaxDevices = 256;
constexpr uint16_t kPciCommandOffset = 0x04;
constexpr uint16_t kPciStatusOffset = 0x06;
constexpr uint16_t kPciCapabilitiesPointerOffset = 0x34;
constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciStatusCapabilities = 1u << 4;
constexpr uint8_t kCapabilityVendorSpecific = 0x09;

pci::DeviceInfo g_devices[kMaxDevices] = {};
size_t g_device_count = 0;
size_t g_dropped_count = 0;
bool g_ready = false;

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

uint64_t decode_bar_size(uint32_t sized_low, uint32_t sized_high, bool io_space, bool is_64bit) {
    const uint32_t low_mask = io_space ? (sized_low & ~0x3u) : (sized_low & ~0x0fu);
    if (low_mask != 0) {
        // Region fits within 4 GiB: complement the low mask in 32-bit width so
        // the unused upper bits stay clear (a 64-bit complement would set them).
        return static_cast<uint64_t>(static_cast<uint32_t>(~low_mask) + 1u);
    }
    if (is_64bit && sized_high != 0) {
        // Region spans >= 4 GiB: the size is determined by the high dword.
        return ~(static_cast<uint64_t>(sized_high) << 32) + 1ULL;
    }
    return 0;
}

void append_device(uint8_t bus, uint8_t slot, uint8_t function) {
    if (g_device_count >= kMaxDevices) {
        ++g_dropped_count;
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

void append_hex(char* line, size_t& length, size_t capacity, uint32_t value, int digits) {
    static const char kDigits[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0 && length + 1 < capacity; shift -= 4) {
        line[length++] = kDigits[(value >> shift) & 0xfu];
    }
    line[length] = '\0';
}

void append_text(char* line, size_t& length, size_t capacity, const char* text) {
    for (const char* cursor = text; *cursor != '\0' && length + 1 < capacity; ++cursor) {
        line[length++] = *cursor;
    }
    line[length] = '\0';
}

// Every function found, one line each, in the order lspci prints them:
// bus:slot.function, vendor:device, [subsystem], class.subclass.prog_if,
// revision and the firmware's IRQ line. On a machine with no serial port this
// is how the devices of a run reach Log Viewer.
void report_devices() {
    console::printf("pci: %u function(s)\n", static_cast<unsigned int>(g_device_count));
    for (size_t index = 0; index < g_device_count; ++index) {
        const pci::DeviceInfo& device = g_devices[index];
        char line[96] = {};
        size_t length = 0;
        const size_t capacity = sizeof(line);
        append_text(line, length, capacity, "pci: ");
        append_hex(line, length, capacity, device.bus, 2);
        append_text(line, length, capacity, ":");
        append_hex(line, length, capacity, device.slot, 2);
        append_text(line, length, capacity, ".");
        append_hex(line, length, capacity, device.function, 1);
        append_text(line, length, capacity, " ");
        append_hex(line, length, capacity, device.vendor_id, 4);
        append_text(line, length, capacity, ":");
        append_hex(line, length, capacity, device.device_id, 4);
        append_text(line, length, capacity, " [");
        append_hex(line, length, capacity, device.subsystem_vendor_id, 4);
        append_text(line, length, capacity, ":");
        append_hex(line, length, capacity, device.subsystem_device_id, 4);
        append_text(line, length, capacity, "] class ");
        append_hex(line, length, capacity, device.class_code, 2);
        append_text(line, length, capacity, ".");
        append_hex(line, length, capacity, device.subclass, 2);
        append_text(line, length, capacity, ".");
        append_hex(line, length, capacity, device.prog_if, 2);
        append_text(line, length, capacity, " rev ");
        append_hex(line, length, capacity, device.revision, 2);
        append_text(line, length, capacity, " irq ");
        console::printf("%s%u\n", line, static_cast<unsigned int>(device.irq_line));
    }
    if (g_dropped_count != 0) {
        console::printf("pci: table full at %u functions, %u more not recorded\n",
            static_cast<unsigned int>(kMaxDevices), static_cast<unsigned int>(g_dropped_count));
    }
}

} // namespace

namespace pci {

void initialize() {
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_dropped_count = 0;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            scan_slot(static_cast<uint8_t>(bus), slot);
        }
    }

    report_devices();
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
    info.size = decode_bar_size(sized_low, sized_high, io_space, is_64bit);
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

bool find_capability(const DeviceInfo& device, uint8_t cap_id, uint8_t& cap_offset) {
    cap_offset = 0;
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
        const uint8_t id = read_config_u8(device.bus, device.slot, device.function, pointer + 0);
        const uint8_t next = static_cast<uint8_t>(read_config_u8(device.bus, device.slot, device.function, pointer + 1) & 0xfcu);
        if (id == cap_id) {
            cap_offset = pointer;
            return true;
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

void write_config_u8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint8_t value) {
    const uint8_t aligned = static_cast<uint8_t>(offset & 0xfcu);
    uint32_t current = read_config_u32(bus, slot, function, aligned);
    const uint32_t shift = (offset & 3u) * 8u;
    current &= ~(0xffu << shift);
    current |= static_cast<uint32_t>(value) << shift;
    write_config_u32(bus, slot, function, aligned, current);
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
