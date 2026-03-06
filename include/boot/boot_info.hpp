#pragma once

#include <stddef.h>
#include <stdint.h>

namespace boot {

enum class FirmwareType : uint64_t {
    unknown = 0,
    x86_bios = 1,
    efi32 = 2,
    efi64 = 3,
    sbi = 4,
};

enum class MemoryRegionType : uint64_t {
    usable = 0,
    reserved = 1,
    acpi_reclaimable = 2,
    acpi_nvs = 3,
    bad_memory = 4,
    bootloader_reclaimable = 5,
    executable_and_modules = 6,
    framebuffer = 7,
    reserved_mapped = 8,
    unknown = 0xffffffffffffffffULL,
};

struct FramebufferInfo {
    void* address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    bool available;
};

struct MemoryRegion {
    uint64_t base;
    uint64_t length;
    MemoryRegionType type;
};

struct BootInfo {
    const char* bootloader_name;
    const char* bootloader_version;
    FirmwareType firmware_type;
    uint64_t hhdm_offset;
    FramebufferInfo framebuffer;
    const MemoryRegion* memory_map;
    size_t memory_map_entries;
};

} // namespace boot
