#pragma once

#include <stddef.h>
#include <stdint.h>

namespace block {

constexpr uint32_t kSectorSize = 512;

struct DeviceInfo {
    bool present;
    uint32_t sector_count;
    bool writable;
    const char* name;
};

void initialize();
bool ready();
size_t device_count();
bool device_info(size_t index, DeviceInfo& info);
bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer);
bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer);

} // namespace block
