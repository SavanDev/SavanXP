#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pci {

struct DeviceInfo {
    bool present;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint32_t bar[6];
};

struct BarInfo {
    bool present;
    bool io_space;
    bool is_64bit;
    bool prefetchable;
    uint64_t base;
    uint64_t size;
};

struct VendorCapabilityInfo {
    bool present;
    uint8_t offset;
    uint8_t cfg_type;
    uint8_t bar_index;
    uint8_t cap_length;
    uint32_t capability_offset;
    uint32_t capability_length;
    uint32_t extra;
};

void initialize();
bool ready();
size_t device_count();
bool device_info(size_t index, DeviceInfo& info);
bool find_device(uint16_t vendor_id, uint16_t device_id, DeviceInfo& info);
bool bar_info(const DeviceInfo& device, uint8_t bar_index, BarInfo& info);
bool find_vendor_capability(const DeviceInfo& device, uint8_t cfg_type, VendorCapabilityInfo& info);
uint8_t read_config_u8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t read_config_u16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint32_t read_config_u32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void write_config_u16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);
void write_config_u32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);

} // namespace pci
