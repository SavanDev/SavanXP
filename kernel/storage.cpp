#include "kernel/storage.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/credentials.hpp"
#include "kernel/fat.hpp"
#include "kernel/fs.hpp"
#include "kernel/console.hpp"
#include "kernel/object.hpp"
#include "kernel/partition.hpp"
#include "kernel/string.hpp"
#include "kernel/process_detail.hpp"
#include "kernel/sx.hpp"
#include "kernel/vfs.hpp"
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

// Bumped by every write through these calls: what a device holds may have
// changed even though no device or mount did.
uint32_t g_content_generation = 0;

size_t index_of(uint32_t id) {
    for (size_t index = 0; index < block::device_count(); ++index) {
        block::DeviceInfo info = {};
        if (block::device_info(index, info) && info.present && info.id == id) {
            return index;
        }
    }
    return block::kInvalidDevice;
}

// Where a device sits on its physical disk: the disk's index and the range.
struct Extent {
    size_t disk;
    uint64_t start;
    uint64_t sectors;
};

Extent extent_of(size_t device_index) {
    block::DeviceInfo info = {};
    (void)block::device_info(device_index, info);
    partition::Info slice = {};
    if (partition::info(device_index, slice)) {
        return {slice.parent_index, slice.start_lba, slice.sector_count};
    }
    return {device_index, 0, info.sector_count};
}

// Whether any mounted volume on the same disk overlaps [start, start+count)
// of that disk.
bool overlaps_mounted(size_t disk, uint64_t start, uint64_t count) {
    for (size_t index = 0; index < fs::mount_count(); ++index) {
        fs::MountInfo mounted = {};
        if (!fs::mount_info(index, mounted) || !mounted.attached) {
            continue;
        }
        const Extent volume = extent_of(mounted.device_index);
        if (volume.disk != disk) {
            continue;
        }
        if (start < volume.start + volume.sectors && volume.start < start + count) {
            return true;
        }
    }
    return false;
}

// Whether anything still holds a vnode: an open file or directory handle, or a
// section mapped from it.
bool vnode_referenced(const vfs::Vnode& node) {
    return process::detail::vnode_has_open_references(node) || object::section_uses_vnode(node);
}

constexpr const char kMountParent[] = "/mnt";
constexpr size_t kMaxMountName = 24;

// "/mnt/<name>", the name one component of [A-Za-z0-9._-], not "." or "..".
bool valid_mount_path(const char* path) {
    const size_t prefix = sizeof(kMountParent) - 1;
    if (path == nullptr || strncmp(path, kMountParent, prefix) != 0 || path[prefix] != '/') {
        return false;
    }
    const char* name = path + prefix + 1;
    size_t length = 0;
    for (; name[length] != 0; ++length) {
        const char c = name[length];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-';
        if (!ok || length >= kMaxMountName) {
            return false;
        }
    }
    return length > 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

int check_range(size_t index, uint64_t lba, uint32_t count, const void* buffer) {
    block::DeviceInfo info = {};
    if (index == block::kInvalidDevice || !block::device_info(index, info) || !info.present) {
        return sx::negative_error(SX_ENODEV);
    }
    if (buffer == nullptr || count == 0 || count > storage::kMaxTransferSectors ||
        lba >= info.sector_count || count > info.sector_count - lba) {
        return sx::negative_error(SX_EINVAL);
    }
    return 0;
}

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
    if (fat::is_boot_sector(sector)) {
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
    return block::generation() + fs::generation() + g_content_generation;
}

int mount(uint32_t id, const char* path, const fs::Owner& owner) {
    const size_t index = index_of(id);
    if (index == block::kInvalidDevice) {
        return sx::negative_error(SX_ENODEV);
    }
    if (!valid_mount_path(path)) {
        return sx::negative_error(SX_EINVAL);
    }
    if (vfs::resolve(path) != nullptr || fs::find(path) != fs::kInvalidMount) {
        return sx::negative_error(SX_EEXIST);
    }
    // A partitioned disk is its table, not a volume; and nothing may be
    // mounted twice, nor over a mounted partition or disk.
    partition::Info slice = {};
    if (!partition::info(index, slice) && partition::disk_info(index).scheme != partition::Scheme::none) {
        return sx::negative_error(SX_EINVAL);
    }
    const Extent device = extent_of(index);
    if (overlaps_mounted(device.disk, device.start, device.sectors)) {
        return sx::negative_error(SX_EBUSY);
    }
    const vfs::Vnode* parent = vfs::resolve(kMountParent);
    if (parent == nullptr) {
        parent = vfs::mkdir(cred::system(), kMountParent, 0755);
    }
    if (parent == nullptr || parent->type != vfs::NodeType::directory) {
        return sx::negative_error(SX_EIO);
    }
    const size_t mounted = fs::mount(index, path, nullptr, &owner);
    if (mounted == fs::kInvalidMount) {
        return sx::negative_error(SX_EIO);
    }
    if (!fs::attach(mounted)) {
        (void)fs::release(mounted, nullptr);
        return sx::negative_error(SX_EIO);
    }
    return 0;
}

int unmount(const char* path) {
    if (path == nullptr) {
        return sx::negative_error(SX_EINVAL);
    }
    const size_t mounted = fs::find(path);
    if (mounted == fs::kInvalidMount || strcmp(path, fs::kRootMountPoint) == 0 ||
        strcmp(path, fs::kBootMountPoint) == 0) {
        return sx::negative_error(SX_EINVAL);
    }
    return fs::release(mounted, &vnode_referenced);
}

size_t mount_boot_volume() {
    // A root carrying its own /boot keeps it: mounting over it would hide it.
    if (vfs::resolve(fs::kBootMountPoint) != nullptr) {
        console::printf("storage: the root has a %s of its own; no boot volume mounted\n", fs::kBootMountPoint);
        return fs::kInvalidMount;
    }
    char kernel_path[vfs::kMaxPathLength] = {};
    if (!fs::join_mount_path(fs::kBootMountPoint, "boot/kernel.elf", kernel_path, sizeof(kernel_path))) {
        return fs::kInvalidMount;
    }
    // Two passes: EFI System Partitions, then everything else (the
    // development boot drive is an MBR partition of another type).
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t index = 0; index < block::device_count(); ++index) {
            block::DeviceInfo info = {};
            if (!block::device_info(index, info) || !info.present || mount_of(index) != fs::kInvalidMount) {
                continue;
            }
            partition::Info slice = {};
            const bool esp = partition::info(index, slice) && slice.esp;
            if (esp != (pass == 0)) {
                continue;
            }
            const size_t mounted = fs::mount(index, fs::kBootMountPoint, fat::driver().name);
            if (mounted == fs::kInvalidMount) {
                continue;
            }
            const vfs::Vnode* kernel = fs::attach(mounted) ? vfs::resolve(kernel_path) : nullptr;
            if (kernel != nullptr && kernel->type == vfs::NodeType::file) {
                console::printf("storage: %s is %s\n", fs::kBootMountPoint, info.name != nullptr ? info.name : "?");
                return mounted;
            }
            // Nothing can hold a node of a volume nobody has seen yet.
            (void)fs::release(mounted, nullptr);
        }
    }
    return fs::kInvalidMount;
}

int read(uint32_t id, uint64_t lba, uint32_t count, void* buffer) {
    const size_t index = index_of(id);
    const int status = check_range(index, lba, count, buffer);
    if (status != 0) {
        return status;
    }
    return block::read(index, static_cast<uint32_t>(lba), count, buffer) ? 0 : sx::negative_error(SX_EIO);
}

int write(uint32_t id, uint64_t lba, uint32_t count, const void* buffer) {
    const size_t index = index_of(id);
    const int status = check_range(index, lba, count, buffer);
    if (status != 0) {
        return status;
    }
    block::DeviceInfo info = {};
    (void)block::device_info(index, info);
    if (!info.writable) {
        return sx::negative_error(SX_EPERM);
    }
    const Extent device = extent_of(index);
    if (overlaps_mounted(device.disk, device.start + lba, count)) {
        return sx::negative_error(SX_EBUSY);
    }
    ++g_content_generation;
    return block::write(index, static_cast<uint32_t>(lba), count, buffer) ? 0 : sx::negative_error(SX_EIO);
}

int rescan(uint32_t id) {
    const size_t index = index_of(id);
    if (index == block::kInvalidDevice) {
        return sx::negative_error(SX_ENODEV);
    }
    partition::Info slice = {};
    if (partition::info(index, slice)) {
        return sx::negative_error(SX_EINVAL);
    }
    block::DeviceInfo info = {};
    (void)block::device_info(index, info);
    if (overlaps_mounted(index, 0, info.sector_count)) {
        return sx::negative_error(SX_EBUSY);
    }
    // Nothing of the old partitions may be left in flight when their slots go.
    if (!block::sync_all() || !partition::rescan(index)) {
        return sx::negative_error(SX_EIO);
    }
    ++g_content_generation;
    return 0;
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
