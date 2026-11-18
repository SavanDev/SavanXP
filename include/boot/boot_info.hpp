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
    // Whether this descriptor passed framebuffer::adopt, the one check every
    // writer relies on (splash, console, display backends). `delivered` says
    // whether the bootloader handed one over at all.
    bool available;
    bool delivered;
    // Usable bytes from `address`: the rest of the memory map's framebuffer
    // region from there on. The firmware sets a mode that takes pitch*height,
    // but the VRAM aperture is usually much larger, and that remainder is
    // mapped by the HHDM too. It is the real ceiling for changing mode, and
    // what makes double buffering possible, which needs more virtual height
    // than the visible mode. Set by framebuffer::adopt: never less than
    // pitch*height when `available`, 0 when rejected.
    uint64_t mapped_bytes;
    // The pixel layout as Limine reports it: memory model 1 is RGB, and each
    // channel is `size` bits starting at bit `shift` of a pixel.
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    // Bytes from `address` to the end of the memory map's framebuffer region
    // that holds it, as found, never raised to fit the mode; 0 when no region
    // holds it. framebuffer::adopt checks it against the visible mode and sets
    // `mapped_bytes` from it.
    uint64_t region_bytes;
};

struct MemoryRegion {
    uint64_t base;
    uint64_t length;
    MemoryRegionType type;
};

// Un core tal como lo reporta el bootloader. `start_slot` apunta a la casilla
// que el bootloader sondea para este AP: escribirle la direccion de una funcion
// lo saca de su bucle de espera y lo lanza ahi, en modo largo, con pila propia y
// las interrupciones apagadas. Es nula en el BSP, que ya viene corriendo.
//
// La casilla vive en memoria bootloader-reclaimable, que este kernel nunca
// reclama (memory::initialize solo toma regiones usable), asi que sigue siendo
// valida despues del boot.
struct CpuInfo {
    uint32_t processor_id;
    uint32_t lapic_id;
    volatile uint64_t* start_slot;
};

struct SmpInfo {
    bool available;
    // El firmware ya entrego los APIC locales en modo x2APIC.
    bool x2apic;
    uint32_t bsp_lapic_id;
    // Cores reportados, BSP incluido. Puede ser mayor que `cpu_entries` si el
    // arreglo se quedo corto.
    uint64_t cpu_count;
    const CpuInfo* cpus;
    size_t cpu_entries;
};

struct BootInfo {
    const char* bootloader_name;
    const char* bootloader_version;
    FirmwareType firmware_type;
    uint64_t hhdm_offset;
    uint64_t acpi_rsdp_address;
    FramebufferInfo framebuffer;
    const void* initramfs_address;
    uint64_t initramfs_size;
    const void* disk_image_address;
    uint64_t disk_image_size;
    const MemoryRegion* memory_map;
    size_t memory_map_entries;
    SmpInfo smp;
    // The boot entry's command line (limine.conf `cmdline:`), words separated
    // by spaces; nullptr when the loader gave none. See boot::has_option.
    const char* cmdline;
};

// Whether `option` is one of the space-separated words of `cmdline`.
inline bool has_option(const char* cmdline, const char* option) {
    if (cmdline == nullptr || option == nullptr || option[0] == 0) {
        return false;
    }
    for (const char* word = cmdline; *word != 0;) {
        while (*word == ' ') {
            ++word;
        }
        size_t i = 0;
        while (option[i] != 0 && word[i] == option[i]) {
            ++i;
        }
        if (option[i] == 0 && (word[i] == ' ' || word[i] == 0)) {
            return true;
        }
        while (*word != 0 && *word != ' ') {
            ++word;
        }
    }
    return false;
}

} // namespace boot
