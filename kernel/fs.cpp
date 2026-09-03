#include "kernel/fs.hpp"

#include "kernel/block.hpp"
#include "kernel/string.hpp"

namespace {

struct Mount {
    bool in_use;
    const fs::Driver* driver;
    fs::VolumeId volume;
    size_t device_index;
    char mount_point[fs::kMountPointCapacity];
    bool attached;
};

const fs::Driver* g_drivers[fs::kMaxDrivers] = {};
size_t g_driver_count = 0;

Mount g_mounts[fs::kMaxMounts] = {};
size_t g_mount_count = 0;

bool copy_mount_point(char* out, const char* mount_point) {
    if (mount_point == nullptr || *mount_point != '/') {
        return false;
    }
    const size_t length = strlen(mount_point);
    if (length == 0 || length >= fs::kMountPointCapacity) {
        return false;
    }
    memcpy(out, mount_point, length + 1);
    return true;
}

// Indice del driver sin probar de mayor prioridad, igual que block::probe_all:
// seleccion directa sobre el array en vez de ordenarlo.
size_t next_driver(const bool* tried) {
    size_t best = fs::kMaxDrivers;
    for (size_t index = 0; index < g_driver_count; ++index) {
        if (tried[index]) {
            continue;
        }
        if (best == fs::kMaxDrivers || g_drivers[index]->priority > g_drivers[best]->priority) {
            best = index;
        }
    }
    return best;
}

} // namespace

namespace fs {

void initialize() {
    memset(g_mounts, 0, sizeof(g_mounts));
    g_mount_count = 0;
}

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || driver.probe == nullptr || driver.attach == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

size_t mount(size_t device_index, const char* mount_point) {
    if (g_mount_count >= kMaxMounts || find(mount_point) != kInvalidMount) {
        return kInvalidMount;
    }

    Mount& slot = g_mounts[g_mount_count];
    if (!copy_mount_point(slot.mount_point, mount_point)) {
        return kInvalidMount;
    }

    bool tried[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        const size_t index = next_driver(tried);
        if (index == kMaxDrivers) {
            break;
        }
        tried[index] = true;

        const VolumeId volume = g_drivers[index]->probe(device_index, slot.mount_point);
        if (volume == kInvalidVolume) {
            continue;
        }

        slot.in_use = true;
        slot.driver = g_drivers[index];
        slot.volume = volume;
        slot.device_index = device_index;
        slot.attached = false;
        return g_mount_count++;
    }

    memset(slot.mount_point, 0, sizeof(slot.mount_point));
    return kInvalidMount;
}

size_t mount_any(const char* mount_point) {
    if (!block::ready()) {
        return kInvalidMount;
    }
    for (size_t index = 0; index < block::device_count(); ++index) {
        block::DeviceInfo info = {};
        if (!block::device_info(index, info) || !info.present) {
            continue;
        }
        const size_t mounted = mount(index, mount_point);
        if (mounted != kInvalidMount) {
            return mounted;
        }
    }
    return kInvalidMount;
}

bool attach(size_t mount_index) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return false;
    }
    Mount& slot = g_mounts[mount_index];
    if (slot.attached) {
        return true;
    }
    slot.attached = slot.driver->attach(slot.volume);
    return slot.attached;
}

void unmount(size_t mount_index) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return;
    }
    Mount& slot = g_mounts[mount_index];
    if (slot.driver->unmount != nullptr) {
        slot.driver->unmount(slot.volume);
    }
    memset(&slot, 0, sizeof(slot));
    // El slot queda libre pero el conteo no baja: los indices de montaje ya
    // entregados tienen que seguir apuntando a lo mismo.
}

size_t mount_count() {
    return g_mount_count;
}

bool mount_info(size_t mount_index, MountInfo& out) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return false;
    }
    const Mount& slot = g_mounts[mount_index];
    out.driver_name = slot.driver->name;
    out.volume = slot.volume;
    out.device_index = slot.device_index;
    out.mount_point = slot.mount_point;
    out.attached = slot.attached;
    return true;
}

size_t find(const char* mount_point) {
    if (mount_point == nullptr) {
        return kInvalidMount;
    }
    for (size_t index = 0; index < g_mount_count; ++index) {
        if (g_mounts[index].in_use && strcmp(g_mounts[index].mount_point, mount_point) == 0) {
            return index;
        }
    }
    return kInvalidMount;
}

} // namespace fs
