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
// Registra un block device respaldado por una region de memoria (p.ej. un
// modulo de Limine). Permite montar SVFS2 en el LiveCD sin depender del
// controlador IDE emulado. Se registra despues de los ATA, asi que un disco
// IDE presente (dev) tiene prioridad sobre el ramdisk (ISO pura).
void register_ramdisk(void* base, uint64_t size_bytes, bool writable, const char* name);
bool ready();
size_t device_count();
bool device_info(size_t index, DeviceInfo& info);
bool read(size_t index, uint32_t lba, uint32_t sector_count, void* buffer);
bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer);

} // namespace block
