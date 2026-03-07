#include "kernel/block.hpp"

#include <stdint.h>

#include "kernel/string.hpp"

namespace {

constexpr uint16_t kPrimaryIoBase = 0x1f0;
constexpr uint16_t kPrimaryControlBase = 0x3f6;
constexpr uint16_t kSecondaryIoBase = 0x170;
constexpr uint16_t kSecondaryControlBase = 0x376;
constexpr size_t kMaxDevices = 4;

constexpr uint8_t kStatusErr = 0x01;
constexpr uint8_t kStatusDrq = 0x08;
constexpr uint8_t kStatusDfq = 0x20;
constexpr uint8_t kStatusBsy = 0x80;

constexpr uint8_t kCommandIdentify = 0xec;
constexpr uint8_t kCommandReadSectors = 0x20;
constexpr uint8_t kCommandWriteSectors = 0x30;
constexpr uint8_t kCommandCacheFlush = 0xe7;

struct Device {
    bool present;
    uint16_t io_base;
    uint16_t control_base;
    uint8_t drive_select;
    uint32_t sector_count;
    bool writable;
    const char* name;
};

Device g_devices[kMaxDevices] = {};
size_t g_device_count = 0;
bool g_ready = false;

void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t inw(uint16_t port) {
    uint16_t value = 0;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void io_wait() {
    outb(0x80, 0);
}

void wait_400ns(const Device& device) {
    (void)inb(device.control_base);
    (void)inb(device.control_base);
    (void)inb(device.control_base);
    (void)inb(device.control_base);
}

void select_drive(const Device& device, uint32_t lba) {
    outb(
        static_cast<uint16_t>(device.io_base + 6),
        static_cast<uint8_t>(0xe0u | device.drive_select | ((lba >> 24) & 0x0f))
    );
    wait_400ns(device);
}

bool poll_status(const Device& device, bool require_drq) {
    uint8_t status = 0;
    uint32_t spins = 1000000;
    do {
        status = inb(static_cast<uint16_t>(device.io_base + 7));
    } while ((status & kStatusBsy) != 0 && --spins != 0);
    if (spins == 0) {
        return false;
    }

    if ((status & (kStatusErr | kStatusDfq)) != 0) {
        return false;
    }

    if (require_drq) {
        spins = 1000000;
        while ((status & kStatusDrq) == 0 && --spins != 0) {
            status = inb(static_cast<uint16_t>(device.io_base + 7));
            if ((status & (kStatusErr | kStatusDfq)) != 0) {
                return false;
            }
        }
        if ((status & kStatusDrq) == 0) {
            return false;
        }
    }

    return true;
}

bool identify_device(Device& device) {
    select_drive(device, 0);
    outb(static_cast<uint16_t>(device.control_base), 0);
    outb(static_cast<uint16_t>(device.io_base + 2), 0);
    outb(static_cast<uint16_t>(device.io_base + 3), 0);
    outb(static_cast<uint16_t>(device.io_base + 4), 0);
    outb(static_cast<uint16_t>(device.io_base + 5), 0);
    outb(static_cast<uint16_t>(device.io_base + 7), kCommandIdentify);

    const uint8_t initial_status = inb(static_cast<uint16_t>(device.io_base + 7));
    if (initial_status == 0) {
        return false;
    }

    uint8_t lba_mid = inb(static_cast<uint16_t>(device.io_base + 4));
    uint8_t lba_high = inb(static_cast<uint16_t>(device.io_base + 5));
    if (lba_mid != 0 || lba_high != 0) {
        return false;
    }

    if (!poll_status(device, true)) {
        return false;
    }

    uint16_t identify[256] = {};
    for (size_t index = 0; index < 256; ++index) {
        identify[index] = inw(device.io_base);
    }

    const uint32_t sector_count =
        static_cast<uint32_t>(identify[60]) |
        (static_cast<uint32_t>(identify[61]) << 16);
    if (sector_count == 0) {
        return false;
    }

    device.present = true;
    device.sector_count = sector_count;
    device.writable = true;
    return true;
}

bool device_for_index(size_t index, Device*& device) {
    if (index >= g_device_count) {
        device = nullptr;
        return false;
    }
    device = &g_devices[index];
    return device->present;
}

bool rw_sectors(Device& device, uint32_t lba, uint32_t sector_count, void* buffer, bool write) {
    if (!device.present || buffer == nullptr || sector_count == 0 || sector_count > 255) {
        return false;
    }
    if (lba >= device.sector_count || sector_count > (device.sector_count - lba)) {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);

    select_drive(device, lba);
    outb(static_cast<uint16_t>(device.io_base + 1), 0);
    outb(static_cast<uint16_t>(device.io_base + 2), static_cast<uint8_t>(sector_count));
    outb(static_cast<uint16_t>(device.io_base + 3), static_cast<uint8_t>(lba & 0xff));
    outb(static_cast<uint16_t>(device.io_base + 4), static_cast<uint8_t>((lba >> 8) & 0xff));
    outb(static_cast<uint16_t>(device.io_base + 5), static_cast<uint8_t>((lba >> 16) & 0xff));
    outb(static_cast<uint16_t>(device.io_base + 7), write ? kCommandWriteSectors : kCommandReadSectors);

    for (uint32_t sector = 0; sector < sector_count; ++sector) {
        if (!poll_status(device, true)) {
            return false;
        }

        if (write) {
            for (size_t word = 0; word < (block::kSectorSize / sizeof(uint16_t)); ++word) {
                const size_t byte_index = static_cast<size_t>(sector) * block::kSectorSize + word * sizeof(uint16_t);
                const uint16_t value =
                    static_cast<uint16_t>(bytes[byte_index]) |
                    (static_cast<uint16_t>(bytes[byte_index + 1]) << 8);
                outw(device.io_base, value);
            }
        } else {
            for (size_t word = 0; word < (block::kSectorSize / sizeof(uint16_t)); ++word) {
                const uint16_t value = inw(device.io_base);
                const size_t byte_index = static_cast<size_t>(sector) * block::kSectorSize + word * sizeof(uint16_t);
                bytes[byte_index] = static_cast<uint8_t>(value & 0xff);
                bytes[byte_index + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
            }
        }
    }

    if (write) {
        outb(static_cast<uint16_t>(device.io_base + 7), kCommandCacheFlush);
        if (!poll_status(device, false)) {
            return false;
        }
    }

    return true;
}

} // namespace

namespace block {

void initialize() {
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_ready = false;

    Device detected[kMaxDevices] = {
        {false, kPrimaryIoBase, kPrimaryControlBase, 0x00, 0, false, "ata0"},
        {false, kPrimaryIoBase, kPrimaryControlBase, 0x10, 0, false, "ata1"},
        {false, kSecondaryIoBase, kSecondaryControlBase, 0x00, 0, false, "ata2"},
        {false, kSecondaryIoBase, kSecondaryControlBase, 0x10, 0, false, "ata3"},
    };

    for (size_t index = 0; index < kMaxDevices; ++index) {
        if (identify_device(detected[index])) {
            g_devices[g_device_count++] = detected[index];
        }
        io_wait();
    }

    g_ready = g_device_count != 0;
}

bool ready() {
    return g_ready;
}

size_t device_count() {
    return g_device_count;
}

bool device_info(size_t index, DeviceInfo& info) {
    Device* device = nullptr;
    if (!device_for_index(index, device)) {
        return false;
    }

    info.present = device->present;
    info.sector_count = device->sector_count;
    info.writable = device->writable;
    info.name = device->name;
    return true;
}

bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer) {
    Device* device = nullptr;
    if (!device_for_index(index, device)) {
        return false;
    }
    return rw_sectors(*device, lba, sector_count, buffer, false);
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    Device* device = nullptr;
    if (!device_for_index(index, device) || !device->writable) {
        return false;
    }
    return rw_sectors(*device, lba, sector_count, const_cast<void*>(buffer), true);
}

} // namespace block
