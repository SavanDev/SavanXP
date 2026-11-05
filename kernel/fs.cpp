#include "kernel/fs.hpp"

#include "abi/savanxp_abi.h"
#include "kernel/block.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"

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
// One past the highest slot in use; released slots leave holes.
size_t g_mount_count = 0;
uint32_t g_generation = 0;

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
    if (g_driver_count >= kMaxDrivers || driver.probe == nullptr || driver.attach == nullptr ||
        driver.read == nullptr || driver.refresh == nullptr) {
        return false;
    }
    g_drivers[g_driver_count] = &driver;
    ++g_driver_count;
    return true;
}

size_t mount(size_t device_index, const char* mount_point) {
    if (find(mount_point) != kInvalidMount) {
        return kInvalidMount;
    }
    size_t slot_index = 0;
    while (slot_index < kMaxMounts && g_mounts[slot_index].in_use) {
        ++slot_index;
    }
    if (slot_index >= kMaxMounts) {
        return kInvalidMount;
    }

    Mount& slot = g_mounts[slot_index];
    if (!copy_mount_point(slot.mount_point, mount_point)) {
        return kInvalidMount;
    }

    const bool root = strcmp(slot.mount_point, kRootMountPoint) == 0;
    bool tried[kMaxDrivers] = {};
    for (size_t attempt = 0; attempt < g_driver_count; ++attempt) {
        const size_t index = next_driver(tried);
        if (index == kMaxDrivers) {
            break;
        }
        tried[index] = true;
        if (root && !g_drivers[index]->can_be_root) {
            continue;
        }

        const VolumeId volume = g_drivers[index]->probe(device_index, slot.mount_point);
        if (volume == kInvalidVolume) {
            continue;
        }

        slot.in_use = true;
        slot.driver = g_drivers[index];
        slot.volume = volume;
        slot.device_index = device_index;
        slot.attached = false;
        if (slot_index + 1 > g_mount_count) {
            g_mount_count = slot_index + 1;
        }
        ++g_generation;
        return slot_index;
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

int release(size_t mount_index, bool (*referenced)(const vfs::Vnode& node)) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return -SX_EINVAL;
    }
    Mount& slot = g_mounts[mount_index];
    if (strcmp(slot.mount_point, kRootMountPoint) == 0 || slot.driver->unmount == nullptr) {
        return -SX_EINVAL;
    }
    if (slot.attached && slot.driver->sync != nullptr && !slot.driver->sync(slot.volume)) {
        return -SX_EIO;
    }
    // A volume that never attached may still have published part of its tree
    // before it failed, or nothing at all.
    if (!vfs::detach_subtree(slot.mount_point, referenced) &&
        (slot.attached || vfs::last_error() != SX_EINVAL)) {
        return -vfs::last_error();
    }
    slot.driver->unmount(slot.volume);
    memset(&slot, 0, sizeof(slot));
    // The slot is free for the next mount; indices handed out for other
    // mounts keep naming what they named.
    while (g_mount_count > 0 && !g_mounts[g_mount_count - 1].in_use) {
        --g_mount_count;
    }
    ++g_generation;
    return 0;
}

uint32_t generation() {
    return g_generation;
}

size_t mount_count() {
    return g_mount_count;
}

bool mount_info(size_t mount_index, MountInfo& out) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return false;
    }
    const Mount& slot = g_mounts[mount_index];
    out.driver = slot.driver;
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

size_t owner_of(const char* path) {
    if (path == nullptr || vfs::is_memory_path(path)) {
        return kInvalidMount;
    }
    size_t best = kInvalidMount;
    size_t best_length = 0;
    for (size_t index = 0; index < g_mount_count; ++index) {
        const Mount& slot = g_mounts[index];
        if (!slot.in_use || !slot.attached || path_below(slot.mount_point, path) == nullptr) {
            continue;
        }
        // Longest mount point wins; "/" has length 1 and loses to any other.
        const size_t length = strlen(slot.mount_point);
        if (best == kInvalidMount || length > best_length) {
            best = index;
            best_length = length;
        }
    }
    return best;
}

bool stats(size_t mount_index, VolumeStats& out) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return false;
    }
    const Mount& slot = g_mounts[mount_index];
    return slot.driver->stats != nullptr && slot.driver->stats(slot.volume, out);
}

bool sync(size_t mount_index) {
    if (mount_index >= g_mount_count || !g_mounts[mount_index].in_use) {
        return false;
    }
    const Mount& slot = g_mounts[mount_index];
    return slot.driver->sync == nullptr || slot.driver->sync(slot.volume);
}

bool sync_all() {
    bool ok = true;
    for (size_t index = 0; index < g_mount_count; ++index) {
        if (g_mounts[index].in_use && !sync(index)) {
            ok = false;
        }
    }
    return ok;
}

} // namespace fs
