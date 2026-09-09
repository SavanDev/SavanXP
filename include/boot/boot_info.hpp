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
    // Bytes utilizables desde `address`: lo que queda de la region de
    // framebuffer del mapa de memoria a partir de ahi. El firmware entrega un
    // modo que ocupa pitch*height, pero la apertura de VRAM suele ser mucho
    // mas grande, y ese resto tambien esta mapeado por el HHDM. Es el techo
    // real para cambiar de modo, y lo que hace posible el doble buffer, que
    // necesita mas alto virtual que el modo visible. Nunca menor a pitch*height.
    uint64_t mapped_bytes;
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
};

} // namespace boot
