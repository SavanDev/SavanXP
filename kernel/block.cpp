#include "kernel/block.hpp"

#include <stdint.h>

#include "kernel/string.hpp"

namespace {

// Los 4 slots ATA + el ramdisk del LiveCD, mas las particiones que el driver
// partition:: recorta encima de ellos (hasta 8), con lugar de sobra para lo
// que sumen drivers nuevos sin volver a tocar el core.
constexpr size_t kMaxBlockDevices = 16;

struct Device {
    bool present;
    const block::DeviceOps* ops;
    void* context;
    uint32_t sector_count;
    bool writable;
    const char* name;
};

Device g_devices[kMaxBlockDevices] = {};
size_t g_device_count = 0;
bool g_ready = false;

const block::Driver* g_drivers[block::kMaxDrivers] = {};
size_t g_driver_count = 0;

bool device_for_index(size_t index, Device*& device) {
    if (index >= g_device_count) {
        device = nullptr;
        return false;
    }
    device = &g_devices[index];
    return device->present;
}

// Validacion comun a cualquier medio. La hace el core una sola vez y los
// drivers se quedan solo con lo suyo: el tope de 255 sectores por comando, por
// ejemplo, es del protocolo ATA y no del contrato de block::.
bool request_in_range(const Device& device, uint32_t lba, uint32_t sector_count, const void* buffer) {
    if (!device.present || device.ops == nullptr || buffer == nullptr || sector_count == 0) {
        return false;
    }
    return lba < device.sector_count && sector_count <= (device.sector_count - lba);
}

} // namespace

namespace block {

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || driver.enumerate == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

bool register_device(
    const DeviceOps& ops,
    void* context,
    uint32_t sector_count,
    bool writable,
    const char* name)
{
    if (ops.read == nullptr || sector_count == 0 || g_device_count >= kMaxBlockDevices) {
        return false;
    }

    Device& device = g_devices[g_device_count];
    device.present = true;
    device.ops = &ops;
    device.context = context;
    device.sector_count = sector_count;
    // Un driver sin write op queda solo-lectura aunque lo pida escribible.
    device.writable = writable && ops.write != nullptr;
    device.name = name;
    ++g_device_count;
    g_ready = true;
    return true;
}

size_t probe_all() {
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_ready = false;

    // Seleccion directa sobre el array en vez de ordenarlo, igual que
    // display::bind_best. La diferencia es que aca no se corta en el primero:
    // enumeran todos los drivers y sus devices conviven.
    bool enumerated[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        size_t best = kMaxDrivers;
        for (size_t i = 0; i < g_driver_count; ++i) {
            if (enumerated[i]) {
                continue;
            }
            if (best == kMaxDrivers || g_drivers[i]->priority > g_drivers[best]->priority) {
                best = i;
            }
        }
        if (best == kMaxDrivers) {
            break;
        }

        enumerated[best] = true;
        g_drivers[best]->enumerate();
    }
    return g_device_count;
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
    if (!device_for_index(index, device) ||
        !request_in_range(*device, lba, sector_count, buffer)) {
        return false;
    }
    return device->ops->read(device->context, lba, sector_count, buffer);
}

bool write(size_t index, uint32_t lba, uint32_t sector_count, const void* buffer) {
    Device* device = nullptr;
    if (!device_for_index(index, device) || !device->writable ||
        !request_in_range(*device, lba, sector_count, buffer)) {
        return false;
    }
    return device->ops->write(device->context, lba, sector_count, buffer);
}

} // namespace block
