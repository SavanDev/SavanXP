#include <stddef.h>
#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/kernel.hpp"
#include "kernel/panic.hpp"
#include "limine.h"

namespace {

constexpr size_t kMaxMemoryMapEntries = 512;

[[gnu::used, gnu::section(".limine_requests")]]
volatile uint64_t g_limine_base_revision[] = LIMINE_BASE_REVISION(5);

[[gnu::used, gnu::section(".limine_requests_start_marker")]]
volatile uint64_t g_limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_bootloader_info_request g_bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_firmware_type_request g_firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_hhdm_request g_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_framebuffer_request g_framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests")]]
volatile limine_memmap_request g_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

[[gnu::used, gnu::section(".limine_requests_end_marker")]]
volatile uint64_t g_limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

boot::MemoryRegion g_memory_regions[kMaxMemoryMapEntries];

boot::FirmwareType translate_firmware_type(uint64_t firmware_type) {
    switch (firmware_type) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS:
            return boot::FirmwareType::x86_bios;
        case LIMINE_FIRMWARE_TYPE_EFI32:
            return boot::FirmwareType::efi32;
        case LIMINE_FIRMWARE_TYPE_EFI64:
            return boot::FirmwareType::efi64;
        case LIMINE_FIRMWARE_TYPE_SBI:
            return boot::FirmwareType::sbi;
        default:
            return boot::FirmwareType::unknown;
    }
}

boot::MemoryRegionType translate_region_type(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
            return boot::MemoryRegionType::usable;
        case LIMINE_MEMMAP_RESERVED:
            return boot::MemoryRegionType::reserved;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            return boot::MemoryRegionType::acpi_reclaimable;
        case LIMINE_MEMMAP_ACPI_NVS:
            return boot::MemoryRegionType::acpi_nvs;
        case LIMINE_MEMMAP_BAD_MEMORY:
            return boot::MemoryRegionType::bad_memory;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return boot::MemoryRegionType::bootloader_reclaimable;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return boot::MemoryRegionType::executable_and_modules;
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return boot::MemoryRegionType::framebuffer;
        case LIMINE_MEMMAP_RESERVED_MAPPED:
            return boot::MemoryRegionType::reserved_mapped;
        default:
            return boot::MemoryRegionType::unknown;
    }
}

boot::BootInfo build_boot_info() {
    boot::BootInfo info = {};

    if (g_bootloader_info_request.response != nullptr) {
        info.bootloader_name = g_bootloader_info_request.response->name;
        info.bootloader_version = g_bootloader_info_request.response->version;
    }

    if (g_firmware_type_request.response != nullptr) {
        info.firmware_type = translate_firmware_type(
            g_firmware_type_request.response->firmware_type
        );
    } else {
        info.firmware_type = boot::FirmwareType::unknown;
    }

    if (g_hhdm_request.response != nullptr) {
        info.hhdm_offset = g_hhdm_request.response->offset;
    }

    if (g_framebuffer_request.response != nullptr &&
        g_framebuffer_request.response->framebuffer_count != 0) {
        limine_framebuffer* framebuffer = g_framebuffer_request.response->framebuffers[0];
        info.framebuffer.address = framebuffer->address;
        info.framebuffer.width = framebuffer->width;
        info.framebuffer.height = framebuffer->height;
        info.framebuffer.pitch = framebuffer->pitch;
        info.framebuffer.bpp = framebuffer->bpp;
        info.framebuffer.available = true;
    }

    if (g_memmap_request.response != nullptr) {
        const uint64_t entry_count = g_memmap_request.response->entry_count;
        const size_t copied_entries = entry_count < kMaxMemoryMapEntries
            ? static_cast<size_t>(entry_count)
            : kMaxMemoryMapEntries;

        for (size_t index = 0; index < copied_entries; ++index) {
            limine_memmap_entry* entry = g_memmap_request.response->entries[index];
            g_memory_regions[index] = {
                .base = entry->base,
                .length = entry->length,
                .type = translate_region_type(entry->type),
            };
        }

        info.memory_map = g_memory_regions;
        info.memory_map_entries = copied_entries;

        if (entry_count > copied_entries) {
            console::printf(
                "warning: memmap truncated from %u to %u entries\n",
                static_cast<unsigned>(entry_count),
                static_cast<unsigned>(copied_entries)
            );
        }
    }

    return info;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    console::early_init();

    if (!LIMINE_BASE_REVISION_SUPPORTED(g_limine_base_revision)) {
        panic("limine base revision is not supported by the bootloader");
    }

    boot::BootInfo boot_info = build_boot_info();
    console::init(boot_info);
    kernel_main(boot_info);
}
