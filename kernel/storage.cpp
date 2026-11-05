#include "kernel/storage.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/fs.hpp"
#include "kernel/partition.hpp"
#include "kernel/string.hpp"
#include "sxfs/sxfs_format.h"

namespace {

constexpr size_t kMaxCache = 32;

// What is on a device that is not mounted, read from its first sectors. Kept
// per device id until the generation moves, so a Disk Management window that
// refreshes every couple of seconds does not read every disk each time.
struct Detected {
    uint32_t id;
    uint32_t generation;
    char fs_name[16];
};

Detected g_detected[kMaxCache] = {};
size_t g_detected_next = 0;

void copy_string(char* out, size_t capacity, const char* text) {
    size_t length = 0;
    if (text != nullptr) {
        while (text[length] != 0 && length + 1 < capacity) {
            out[length] = text[length];
            ++length;
        }
    }
    out[length] = 0;
}

// Index of the attached mount of `device_index`, or fs::kInvalidMount.
size_t mount_of(size_t device_index) {
    for (size_t index = 0; index < fs::mount_count(); ++index) {
        fs::MountInfo info = {};
        if (fs::mount_info(index, info) && info.attached && info.device_index == device_index) {
            return index;
        }
    }
    return fs::kInvalidMount;
}

bool is_root_mount(size_t mount_index) {
    fs::MountInfo info = {};
    return mount_index != fs::kInvalidMount && fs::mount_info(mount_index, info) &&
        strcmp(info.mount_point, fs::kRootMountPoint) == 0;
}

void detect(size_t device_index, char* out, size_t capacity) {
    uint8_t sector[block::kSectorSize] = {};
    out[0] = 0;
    if (!block::read(device_index, 0, 1, sector)) {
        return;
    }
    if (memcmp(sector, sxfs_superblock_magic, sizeof(sxfs_superblock_magic)) == 0) {
        copy_string(out, capacity, "sxfs");
        return;
    }
    const bool boot_signature = sector[510] == 0x55 && sector[511] == 0xaa;
    if (boot_signature && (memcmp(sector + 54, "FAT", 3) == 0 || memcmp(sector + 82, "FAT32", 5) == 0)) {
        copy_string(out, capacity, "fat");
        return;
    }
    // The primary superblock may be the one a crash left behind.
    if (block::read(device_index, 1, 1, sector) &&
        memcmp(sector, sxfs_superblock_magic, sizeof(sxfs_superblock_magic)) == 0) {
        copy_string(out, capacity, "sxfs");
    }
}

void detected_fs(size_t device_index, uint32_t id, char* out, size_t capacity) {
    const uint32_t now = storage::generation();
    for (size_t index = 0; index < kMaxCache; ++index) {
        if (g_detected[index].id == id && g_detected[index].generation == now) {
            copy_string(out, capacity, g_detected[index].fs_name);
            return;
        }
    }
    Detected& slot = g_detected[g_detected_next];
    g_detected_next = (g_detected_next + 1) % kMaxCache;
    slot.id = id;
    slot.generation = now;
    detect(device_index, slot.fs_name, sizeof(slot.fs_name));
    copy_string(out, capacity, slot.fs_name);
}

sxn_storage_scheme to_abi(partition::Scheme scheme) {
    switch (scheme) {
    case partition::Scheme::mbr: return SXN_STORAGE_SCHEME_MBR;
    case partition::Scheme::gpt: return SXN_STORAGE_SCHEME_GPT;
    case partition::Scheme::damaged: return SXN_STORAGE_SCHEME_DAMAGED;
    default: return SXN_STORAGE_SCHEME_NONE;
    }
}

} // namespace

namespace storage {

uint32_t generation() {
    return block::generation();
}

bool snapshot(size_t wanted, sxn_storage_device& out) {
    out = {};
    size_t seen = 0;
    size_t device_index = 0;
    block::DeviceInfo info = {};
    for (; device_index < block::device_count(); ++device_index) {
        if (block::device_info(device_index, info) && info.present) {
            if (seen == wanted) {
                break;
            }
            ++seen;
        }
    }
    if (device_index >= block::device_count()) {
        return false;
    }

    out.id = info.id;
    out.generation = generation();
    out.sector_count = info.sector_count;
    out.device_sectors = info.sector_count;
    copy_string(out.name, sizeof(out.name), info.name);
    if (info.writable) {
        out.flags |= SXN_STORAGE_WRITABLE;
    }

    partition::Info slice = {};
    if (partition::info(device_index, slice)) {
        block::DeviceInfo parent = {};
        out.kind = SXN_STORAGE_PARTITION;
        out.parent_id = block::device_info(slice.parent_index, parent) ? parent.id : 0;
        out.scheme = to_abi(slice.scheme);
        out.partition_number = slice.number;
        out.mbr_type = slice.mbr_type;
        out.start_lba = slice.start_lba;
        memcpy(out.type_guid, slice.type_guid, sizeof(out.type_guid));
        memcpy(out.unique_guid, slice.unique_guid, sizeof(out.unique_guid));
        copy_string(out.label, sizeof(out.label), slice.name);
        if (slice.esp) {
            out.flags |= SXN_STORAGE_ESP;
        }
        if (parent.identity != nullptr) {
            copy_string(out.model, sizeof(out.model), parent.identity->model);
        }
    } else {
        const partition::DiskInfo table = partition::disk_info(device_index);
        out.kind = (info.identity != nullptr && info.identity->memory) ? SXN_STORAGE_MEMORY : SXN_STORAGE_DISK;
        out.scheme = to_abi(table.scheme);
        if (table.scheme == partition::Scheme::gpt) {
            memcpy(out.unique_guid, table.disk_guid, sizeof(out.unique_guid));
        }
        if (table.gpt_from_backup) {
            out.flags |= SXN_STORAGE_GPT_BACKUP;
        }
        if (info.identity != nullptr) {
            copy_string(out.model, sizeof(out.model), info.identity->model);
            copy_string(out.serial, sizeof(out.serial), info.identity->serial);
            if (info.identity->full_sectors > info.sector_count) {
                out.device_sectors = info.identity->full_sectors;
                out.flags |= SXN_STORAGE_TRUNCATED;
            }
        }
        // A disk is busy when any of its partitions is mounted.
        for (size_t child = 0; child < block::device_count(); ++child) {
            partition::Info child_slice = {};
            if (!partition::info(child, child_slice) || child_slice.parent_index != device_index) {
                continue;
            }
            const size_t child_mount = mount_of(child);
            if (child_mount != fs::kInvalidMount) {
                out.flags |= SXN_STORAGE_CHILD_MOUNTED;
                if (is_root_mount(child_mount)) {
                    out.flags |= SXN_STORAGE_SYSTEM;
                }
            }
        }
    }

    const size_t mount = mount_of(device_index);
    if (mount != fs::kInvalidMount) {
        fs::MountInfo mounted = {};
        fs::VolumeStats stats = {};
        out.flags |= SXN_STORAGE_MOUNTED;
        if (is_root_mount(mount)) {
            out.flags |= SXN_STORAGE_SYSTEM;
        }
        if (fs::mount_info(mount, mounted)) {
            copy_string(out.fs_name, sizeof(out.fs_name), mounted.driver_name);
            copy_string(out.mount_point, sizeof(out.mount_point), mounted.mount_point);
        }
        if (fs::stats(mount, stats)) {
            out.total_bytes = stats.total_bytes;
            out.used_bytes = stats.used_bytes;
            out.free_bytes = stats.free_bytes;
            out.file_count = static_cast<uint32_t>(stats.file_count);
        }
    } else if (out.kind != SXN_STORAGE_DISK || out.scheme == SXN_STORAGE_SCHEME_NONE) {
        // A partitioned disk's first sector is its table, not a volume.
        detected_fs(device_index, info.id, out.fs_name, sizeof(out.fs_name));
    }
    return true;
}

} // namespace storage
