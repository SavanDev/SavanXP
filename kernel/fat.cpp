#include "kernel/fat.hpp"

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/credentials.hpp"
#include "kernel/fat_diskio.hpp"
#include "kernel/fs.hpp"
#include "kernel/heap.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"

#include "ff.h"

/* The FAT driver (docs/fs/FAT_ROADMAP.md). FatFs does the format; this file
 * maps it onto fs::Driver: probe mounts a FatFs volume, attach publishes its
 * whole tree, and every file operation reopens the file by its path, because
 * FatFs names files by path, not by cluster. Read-only in this phase. */

namespace {

constexpr size_t kSectorSize = block::kSectorSize;
// Directories nested deeper than this are not published. Each level holds an
// open FatFs directory and a FILINFO, on the heap.
constexpr size_t kMaxDepth = 24;
// "N:" in front of a path inside the volume.
constexpr size_t kDrivePrefix = 2;

// A published file. The path is the one FatFs opens, inside the volume.
struct FileNode {
    FileNode* next;
    uint8_t volume;
    vfs::Vnode* vnode;
    char path[vfs::kMaxPathLength];
};

struct Volume {
    bool in_use;
    FATFS fs;
    char mount_point[fs::kMountPointCapacity];
    FileNode* files;
    size_t file_count;
    // Entries attach left out: a name or a path the VFS cannot hold, or a
    // directory too deep.
    size_t skipped;
};

Volume g_volumes[fat_disk::kDrives] = {};

const fs::Driver& driver_ref();

Volume* volume_for_id(fs::VolumeId id) {
    return id < fat_disk::kDrives && g_volumes[id].in_use ? &g_volumes[id] : nullptr;
}

// "N:" followed by `inner` ("/a/b", or "" for the root).
bool drive_path(uint8_t volume, const char* inner, char* out, size_t capacity) {
    const size_t length = strlen(inner);
    if (kDrivePrefix + 1 + length >= capacity) {
        return false;
    }
    out[0] = static_cast<char>('0' + volume);
    out[1] = ':';
    if (length == 0) {
        out[2] = '/';
        out[3] = 0;
        return true;
    }
    memcpy(out + kDrivePrefix, inner, length + 1);
    return true;
}

void drive_root(uint8_t volume, char* out) {
    out[0] = static_cast<char>('0' + volume);
    out[1] = ':';
    out[2] = 0;
}

FileNode* file_from_vnode(const vfs::Vnode& node) {
    if (node.backend != vfs::Backend::volume || node.volume_driver != &driver_ref() || node.data == nullptr) {
        return nullptr;
    }
    FileNode* file = static_cast<FileNode*>(node.data);
    return volume_for_id(file->volume) != nullptr ? file : nullptr;
}

// FAT has no owners. Everything is the system's; the write bits go while the
// volume is read-only, and files never get an execute bit, so nothing runs
// from a FAT volume.
void publish_ownership(vfs::Vnode& node, bool directory, uint16_t date, uint16_t time) {
    const uint16_t mode = directory ? (vfs::kModeDirectory | 0555) : (vfs::kModeFile | 0444);
    vfs::set_ownership(node, cred::kSystemUid, cred::kSystemGid, mode);
    node.mtime = fat_disk::epoch_seconds(date, time);
}

void free_files(Volume& volume) {
    FileNode* file = volume.files;
    while (file != nullptr) {
        FileNode* next = file->next;
        heap::free(file);
        file = next;
    }
    volume.files = nullptr;
    volume.file_count = 0;
}

// One directory being published. On the heap, with its path buffers, so a
// deep tree does not eat the 32 KiB kernel stack.
struct Level {
    DIR dir;
    FILINFO info;
    char fatfs_path[kDrivePrefix + vfs::kMaxPathLength];
    char child[vfs::kMaxPathLength];
    char absolute[vfs::kMaxPathLength];
};

bool is_dot_entry(const char* name) {
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

// Publishes the directory `inner` ("" for the root) and everything below.
// False only when the VFS refused a node or memory ran out: the volume is then
// left half published, and fs::release takes it back.
bool publish_directory(Volume& volume, uint8_t id, const char* inner, size_t depth) {
    Level* level = static_cast<Level*>(heap::allocate(sizeof(Level)));
    if (level == nullptr) {
        return false;
    }
    memset(level, 0, sizeof(Level));
    if (!drive_path(id, inner, level->fatfs_path, sizeof(level->fatfs_path)) ||
        f_opendir(&level->dir, level->fatfs_path) != FR_OK) {
        heap::free(level);
        ++volume.skipped;
        return true;
    }

    bool ok = true;
    while (ok) {
        if (f_readdir(&level->dir, &level->info) != FR_OK || level->info.fname[0] == 0) {
            break;
        }
        const char* name = level->info.fname;
        if (is_dot_entry(name)) {
            continue;
        }

        char* child = level->child;
        char* absolute = level->absolute;
        const size_t inner_length = strlen(inner);
        const size_t name_length = strlen(name);
        if (name_length >= vfs::kMaxPathComponentLength || inner_length + 1 + name_length >= sizeof(level->child)) {
            ++volume.skipped;
            continue;
        }
        memcpy(child, inner, inner_length);
        child[inner_length] = '/';
        memcpy(child + inner_length + 1, name, name_length + 1);
        if (!fs::join_mount_path(volume.mount_point, child + 1, absolute, sizeof(level->absolute))) {
            ++volume.skipped;
            continue;
        }

        const uint16_t date = level->info.fdate;
        const uint16_t time = level->info.ftime;
        if ((level->info.fattrib & AM_DIR) != 0) {
            vfs::Vnode* node = vfs::ensure_directory(absolute);
            if (node == nullptr) {
                ok = false;
                break;
            }
            publish_ownership(*node, true, date, time);
            if (depth + 1 >= kMaxDepth) {
                ++volume.skipped;
                continue;
            }
            ok = publish_directory(volume, id, child, depth + 1);
            continue;
        }

        FileNode* file = static_cast<FileNode*>(heap::allocate(sizeof(FileNode)));
        if (file == nullptr) {
            ok = false;
            break;
        }
        memset(file, 0, sizeof(FileNode));
        file->volume = id;
        memcpy(file->path, child, strlen(child) + 1);
        file->vnode = vfs::install_volume_file(absolute, driver_ref(), file, level->info.fsize, false);
        if (file->vnode == nullptr) {
            heap::free(file);
            ok = false;
            break;
        }
        publish_ownership(*file->vnode, false, date, time);
        file->next = volume.files;
        volume.files = file;
        ++volume.file_count;
    }

    (void)f_closedir(&level->dir);
    heap::free(level);
    return ok;
}

/* --- fs::Driver ------------------------------------------------------------ */

fs::VolumeId probe(size_t device_index, const char* mount_point) {
    if (mount_point == nullptr || *mount_point != '/' || strlen(mount_point) >= fs::kMountPointCapacity) {
        return fs::kInvalidVolume;
    }
    block::DeviceInfo info = {};
    if (!block::device_info(device_index, info) || !info.present || info.sector_count < 2) {
        return fs::kInvalidVolume;
    }
    // Only a FAT boot sector at sector 0. Left to itself, FatFs would look for
    // a FAT partition in an MBR or GPT and mount that, and the partition would
    // then be mounted a second time through its own device.
    uint8_t sector[kSectorSize] = {};
    if (!block::read(device_index, 0, 1, sector) || !fat::is_boot_sector(sector)) {
        return fs::kInvalidVolume;
    }

    for (uint8_t id = 0; id < fat_disk::kDrives; ++id) {
        Volume& volume = g_volumes[id];
        if (volume.in_use || fat_disk::bound(id)) {
            continue;
        }
        memset(&volume, 0, sizeof(volume));
        if (!fat_disk::bind(id, device_index)) {
            return fs::kInvalidVolume;
        }
        char root[4] = {};
        drive_root(id, root);
        const FRESULT result = f_mount(&volume.fs, root, 1);
        if (result != FR_OK) {
            (void)f_mount(nullptr, root, 0);
            fat_disk::unbind(id);
            console::printf("fat: %s: FAT boot sector, but FatFs refused it (%d)\n",
                            info.name != nullptr ? info.name : "?", static_cast<int>(result));
            return fs::kInvalidVolume;
        }
        volume.in_use = true;
        memcpy(volume.mount_point, mount_point, strlen(mount_point) + 1);
        return id;
    }
    return fs::kInvalidVolume;
}

bool attach(fs::VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return false;
    }
    vfs::Vnode* root = vfs::ensure_directory(volume->mount_point);
    if (root == nullptr) {
        return false;
    }
    publish_ownership(*root, true, 0, 0);
    if (!publish_directory(*volume, static_cast<uint8_t>(id), "", 0)) {
        console::printf("fat: %s: the tree does not fit in the VFS, not mounted\n", volume->mount_point);
        return false;
    }
    if (volume->skipped != 0) {
        console::printf("fat: %s: %u entries left out (name too long, path too long or too deep)\n",
                        volume->mount_point, static_cast<unsigned>(volume->skipped));
    }
    return true;
}

// fs::release has synced the volume and taken its tree out of the VFS, after
// checking nothing held a node of it.
void unmount(fs::VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return;
    }
    char root[4] = {};
    drive_root(static_cast<uint8_t>(id), root);
    free_files(*volume);
    (void)f_mount(nullptr, root, 0);
    fat_disk::unbind(static_cast<uint8_t>(id));
    memset(volume, 0, sizeof(*volume));
}

bool read(vfs::Vnode& node, size_t offset, void* buffer, size_t count) {
    FileNode* file = file_from_vnode(node);
    char path[kDrivePrefix + vfs::kMaxPathLength] = {};
    if (file == nullptr || !drive_path(file->volume, file->path, path, sizeof(path))) {
        return false;
    }
    FIL handle = {};
    if (f_open(&handle, path, FA_READ) != FR_OK) {
        return false;
    }
    UINT done = 0;
    const bool ok = f_lseek(&handle, static_cast<FSIZE_t>(offset)) == FR_OK &&
        f_read(&handle, buffer, static_cast<UINT>(count), &done) == FR_OK && done == count;
    node.size = f_size(&handle);
    (void)f_close(&handle);
    return ok;
}

void refresh(vfs::Vnode& node) {
    FileNode* file = file_from_vnode(node);
    char path[kDrivePrefix + vfs::kMaxPathLength] = {};
    if (file == nullptr || !drive_path(file->volume, file->path, path, sizeof(path))) {
        return;
    }
    FILINFO info = {};
    if (f_stat(path, &info) == FR_OK) {
        node.size = info.fsize;
        publish_ownership(node, false, info.fdate, info.ftime);
    }
    node.writable = false;
}

// Read-only for now: the VFS calls these without checking them for nullptr,
// so they exist and refuse.
bool write(vfs::Vnode&, size_t, const void*, size_t, bool, size_t& written) {
    written = 0;
    return false;
}
bool truncate(vfs::Vnode&, size_t) { return false; }
bool unlink(vfs::Vnode&) { return false; }
bool remove_directory(vfs::Vnode&) { return false; }
vfs::Vnode* create_file(fs::VolumeId, const char*, const vfs::Ownership&) { return nullptr; }
vfs::Vnode* create_directory(fs::VolumeId, const char*, const vfs::Ownership&) { return nullptr; }
bool rename(fs::VolumeId, const char*, const char*) { return false; }

bool stats(fs::VolumeId id, fs::VolumeStats& out) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return false;
    }
    char root[4] = {};
    drive_root(static_cast<uint8_t>(id), root);
    DWORD free_clusters = 0;
    FATFS* fatfs = nullptr;
    if (f_getfree(root, &free_clusters, &fatfs) != FR_OK) {
        return false;
    }
    const uint64_t cluster_bytes = static_cast<uint64_t>(volume->fs.csize) * kSectorSize;
    out.writable = false;
    out.file_count = volume->file_count;
    out.total_bytes = static_cast<uint64_t>(volume->fs.n_fatent - 2) * cluster_bytes;
    out.free_bytes = static_cast<uint64_t>(free_clusters) * cluster_bytes;
    out.used_bytes = out.total_bytes >= out.free_bytes ? out.total_bytes - out.free_bytes : 0;
    return true;
}

// Nothing is ever dirty on a read-only volume.
bool sync(fs::VolumeId id) {
    return volume_for_id(id) != nullptr;
}

const fs::Driver kDriver = {
    .name = "fat",
    // Below SxFS (100): it probes first.
    .priority = 50,
    .can_be_root = false,
    .probe = &probe,
    .attach = &attach,
    .unmount = &unmount,
    .read = &read,
    .write = &write,
    .truncate = &truncate,
    .unlink = &unlink,
    .remove_directory = &remove_directory,
    .refresh = &refresh,
    .create_file = &create_file,
    .create_directory = &create_directory,
    .rename = &rename,
    .set_ownership = nullptr,
    .stats = &stats,
    .sync = &sync,
};

const fs::Driver& driver_ref() {
    return kDriver;
}

} // namespace

namespace fat {

const fs::Driver& driver() {
    return kDriver;
}

bool is_boot_sector(const uint8_t* sector) {
    const bool signature = sector[510] == 0x55 && sector[511] == 0xaa;
    return signature && (memcmp(sector + 54, "FAT", 3) == 0 || memcmp(sector + 82, "FAT32", 5) == 0);
}

} // namespace fat
