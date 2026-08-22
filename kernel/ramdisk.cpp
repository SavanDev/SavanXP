#include "kernel/ramdisk.hpp"

#include <stddef.h>

#include "kernel/string.hpp"

namespace {

// Prioridad baja: enumera despues de ATA, asi un disco IDE persistente (dev) le
// gana a la imagen del LiveCD cuando svfs monta el primer SVFS2 que encuentra.
constexpr int kDriverPriority = 10;

struct Image {
    uint8_t* base;
    uint32_t sector_count;
    bool writable;
    const char* name;
};

Image g_image = {};

// El rango de LBA y el permiso de escritura ya los valido block::read/write.
bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    const Image& image = *static_cast<const Image*>(context);
    memcpy(
        buffer,
        image.base + static_cast<size_t>(lba) * block::kSectorSize,
        static_cast<size_t>(sector_count) * block::kSectorSize);
    return true;
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    Image& image = *static_cast<Image*>(context);
    memcpy(
        image.base + static_cast<size_t>(lba) * block::kSectorSize,
        buffer,
        static_cast<size_t>(sector_count) * block::kSectorSize);
    return true;
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
};

void enumerate() {
    if (g_image.base == nullptr || g_image.sector_count == 0) {
        return;
    }
    (void)block::register_device(kOps, &g_image, g_image.sector_count, g_image.writable, g_image.name);
}

const block::Driver kDriver = {
    "ramdisk",
    kDriverPriority,
    &enumerate,
};

} // namespace

namespace ramdisk {

void attach_image(void* base, uint64_t size_bytes, bool writable, const char* name) {
    if (base == nullptr || size_bytes < block::kSectorSize) {
        return;
    }

    g_image.base = static_cast<uint8_t*>(base);
    g_image.sector_count = static_cast<uint32_t>(size_bytes / block::kSectorSize);
    g_image.writable = writable;
    g_image.name = name;
}

const block::Driver& driver() { return kDriver; }

} // namespace ramdisk
