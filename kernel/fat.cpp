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

/* The FAT driver (docs/roadmaps/open/FAT_ROADMAP.md). FatFs does the format; this file
 * maps it onto fs::Driver: probe mounts a FatFs volume, attach publishes its
 * whole tree, and an operation opens the file by its path, because FatFs
 * names files by path, not by cluster -- unless something holds the file, in
 * which case one FatFs file stays open for it (see "Files kept open"). */

namespace {

using fs::Status;


constexpr size_t kSectorSize = block::kSectorSize;
// Directories nested deeper than this are not published. Each level holds an
// open FatFs directory and a FILINFO, on the heap.
constexpr size_t kMaxDepth = 24;
// "N:" in front of a path inside the volume.
constexpr size_t kDrivePrefix = 2;
constexpr size_t kFatfsPathCapacity = kDrivePrefix + vfs::kMaxPathLength;

// A published file or directory. The path is the one FatFs opens, inside the
// volume ("/a/b"). A file's vnode points back here through its data; a
// directory's vnode is a plain in-memory one, found by searching the list.
struct Node {
    Node* next;
    uint8_t volume;
    bool directory;
    // A holder has the file (fs::Driver::open until close), so `kept` should
    // be open; it is not when opening it failed or while a rename moves it.
    bool held;
    // Opened with FA_WRITE: the volume and the file take writes.
    bool kept_writable;
    vfs::Vnode* vnode;
    // On the heap, about 600 bytes with its sector buffer.
    FIL* kept;
    char path[vfs::kMaxPathLength];
};

struct Volume {
    bool in_use;
    // The device is writable: what every vnode of the volume starts from.
    bool writable;
    // FAT has no owners: everything on the volume is published as this one's.
    // The system unless fs::mount was told otherwise (adopt_owner).
    fs::Owner owner;
    size_t device_index;
    FATFS fs;
    char mount_point[fs::kMountPointCapacity];
    Node* nodes;
    size_t file_count;
    // Entries attach left out: a name or a path the VFS cannot hold, or a
    // directory too deep.
    size_t skipped;
};

Volume g_volumes[fat_disk::kDrives] = {};

const uint8_t kZeros[kSectorSize] = {};

const fs::Driver& driver_ref();
FRESULT close_kept(Node& file);

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

Node* file_from_vnode(const vfs::Vnode& node) {
    if (node.backend != vfs::Backend::volume || node.volume_driver != &driver_ref() || node.data == nullptr) {
        return nullptr;
    }
    Node* file = static_cast<Node*>(node.data);
    return volume_for_id(file->volume) != nullptr && !file->directory ? file : nullptr;
}

Node* directory_from_vnode(const vfs::Vnode& node) {
    for (Volume& volume : g_volumes) {
        if (!volume.in_use) {
            continue;
        }
        for (Node* entry = volume.nodes; entry != nullptr; entry = entry->next) {
            if (entry->directory && entry->vnode == &node) {
                return entry;
            }
        }
    }
    return nullptr;
}

// FAT has no owners. Everything is the volume's owner's (the system's, or the
// person who mounted it): 0755 and 0644, without the write bits on a
// read-only volume or a file with FAT's read-only attribute. Files never get
// an execute bit, so nothing runs from a FAT volume.
void publish_ownership(vfs::Vnode& node, const Volume& volume, bool directory, bool writable, uint16_t date,
                       uint16_t time) {
    uint16_t mode = directory ? (vfs::kModeDirectory | 0755) : (vfs::kModeFile | 0644);
    if (!writable) {
        mode &= static_cast<uint16_t>(~0222);
    }
    vfs::set_ownership(node, volume.owner.uid, volume.owner.gid, mode);
    node.mtime = fat_disk::epoch_seconds(date, time);
}

bool file_writable(const Volume& volume, uint8_t attributes) {
    return volume.writable && (attributes & AM_RDO) == 0;
}

Node* new_node(uint8_t id, bool directory, const char* inner) {
    const size_t length = strlen(inner);
    if (length >= vfs::kMaxPathLength) {
        return nullptr;
    }
    Node* entry = static_cast<Node*>(heap::allocate(sizeof(Node)));
    if (entry == nullptr) {
        return nullptr;
    }
    memset(entry, 0, sizeof(Node));
    entry->volume = id;
    entry->directory = directory;
    memcpy(entry->path, inner, length + 1);
    return entry;
}

void link_node(Volume& volume, Node* entry) {
    entry->next = volume.nodes;
    volume.nodes = entry;
    if (!entry->directory) {
        ++volume.file_count;
    }
}

// Takes `entry` out of its volume's list and frees it.
void drop_node(Volume& volume, Node* entry) {
    for (Node** link = &volume.nodes; *link != nullptr; link = &(*link)->next) {
        if (*link == entry) {
            *link = entry->next;
            if (!entry->directory && volume.file_count > 0) {
                --volume.file_count;
            }
            heap::free(entry);
            return;
        }
    }
}

void free_nodes(Volume& volume) {
    Node* entry = volume.nodes;
    while (entry != nullptr) {
        Node* next = entry->next;
        heap::free(entry);
        entry = next;
    }
    volume.nodes = nullptr;
    volume.file_count = 0;
}

// One directory being published. On the heap, with its path buffers, so a
// deep tree does not eat the 32 KiB kernel stack.
struct Level {
    DIR dir;
    FILINFO info;
    char fatfs_path[kFatfsPathCapacity];
    char child[vfs::kMaxPathLength];
    char absolute[vfs::kMaxPathLength];
};

bool is_dot_entry(const char* name) {
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

// Publishes what the directory `inner` ("" for the root) holds, and below.
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

        const bool directory = (level->info.fattrib & AM_DIR) != 0;
        Node* entry = new_node(id, directory, child);
        if (entry == nullptr) {
            ok = false;
            break;
        }
        const bool writable = file_writable(volume, level->info.fattrib);
        entry->vnode = directory ? vfs::ensure_directory(absolute)
                                 : vfs::install_volume_file(absolute, driver_ref(), entry, level->info.fsize, writable);
        if (entry->vnode == nullptr) {
            heap::free(entry);
            ok = false;
            break;
        }
        publish_ownership(*entry->vnode, volume, directory, writable, level->info.fdate, level->info.ftime);
        link_node(volume, entry);

        if (directory) {
            if (depth + 1 >= kMaxDepth) {
                ++volume.skipped;
                continue;
            }
            ok = publish_directory(volume, id, child, depth + 1);
        }
    }

    (void)f_closedir(&level->dir);
    heap::free(level);
    return ok;
}

// FatFs extends a file past its end with whatever the clusters held. Every
// byte between the old end and `target` is written as zero instead, the way
// SxFS and POSIX fill a hole. The file is open for writing, positioned at its
// end.
bool zero_fill(FIL& handle, FSIZE_t target) {
    while (f_size(&handle) < target) {
        const FSIZE_t left = target - f_size(&handle);
        const UINT chunk = left < kSectorSize ? static_cast<UINT>(left) : static_cast<UINT>(kSectorSize);
        UINT done = 0;
        if (f_write(&handle, kZeros, chunk, &done) != FR_OK || done != chunk) {
            return false;
        }
    }
    return true;
}

// The path inside the volume of an absolute path under its mount point, or
// nullptr. The mount point itself is not a path a driver operation takes.
const char* inner_path(const Volume& volume, const char* path) {
    const char* inner = fs::path_below(volume.mount_point, path);
    return inner != nullptr && *inner != 0 ? inner : nullptr;
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
        volume.writable = info.writable;
        volume.owner = {cred::kSystemUid, cred::kSystemGid};
        volume.device_index = device_index;
        memcpy(volume.mount_point, mount_point, strlen(mount_point) + 1);
        return id;
    }
    return fs::kInvalidVolume;
}

bool attach(fs::VolumeId id, bool read_only) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return false;
    }
    if (read_only) {
        // Every vnode is published from this, and every write path checks it.
        volume->writable = false;
    }
    vfs::Vnode* root = vfs::ensure_directory(volume->mount_point);
    if (root == nullptr) {
        return false;
    }
    publish_ownership(*root, *volume, true, volume->writable, 0, 0);
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
    // Nothing holds a node any more; this only guards against a driver bug
    // leaving a FatFs file open on a volume about to go.
    for (Node* entry = volume->nodes; entry != nullptr; entry = entry->next) {
        (void)close_kept(*entry);
    }
    free_nodes(*volume);
    (void)f_mount(nullptr, root, 0);
    fat_disk::unbind(static_cast<uint8_t>(id));
    memset(volume, 0, sizeof(*volume));
}

// What a FatFs result means to the VFS. FR_DENIED is FatFs's answer to several
// things (a full directory, a read-only file, a directory that is not empty);
// `denied` is what it means for the operation that got it.
Status status_of(FRESULT result, Status denied = Status::no_space) {
    switch (result) {
        case FR_OK: return Status::ok;
        case FR_NO_FILE:
        case FR_NO_PATH: return Status::not_found;
        case FR_EXIST: return Status::exists;
        case FR_INVALID_NAME: return Status::invalid;
        case FR_WRITE_PROTECTED: return Status::read_only;
        case FR_DENIED: return denied;
        default: return Status::io_error;
    }
}

// A volume that takes no changes, or a path FatFs cannot be given.
Status writable_path(const Volume* volume, bool have_path) {
    if (volume == nullptr || !have_path) {
        return Status::invalid;
    }
    return volume->writable ? Status::ok : Status::read_only;
}

/* --- Files kept open --------------------------------------------------------
 *
 * While something holds a file's vnode, the driver keeps one FatFs file open
 * for it: a read goes on from where the previous one stopped instead of
 * opening the file by its path and walking its cluster chain from the start.
 * Every operation on that file goes through the kept one, because two open
 * FatFs files over the same file would each cache its size, its clusters and
 * a sector of it. A write is synced before it returns, as closing the file
 * after it used to do. The kept file is closed while a rename moves its
 * directory entry, which it points into, and while an operation has to
 * write through a file kept open read-only. */

bool open_kept(Node& file) {
    Volume* volume = volume_for_id(file.volume);
    char path[kFatfsPathCapacity] = {};
    if (volume == nullptr || file.directory || !drive_path(file.volume, file.path, path, sizeof(path))) {
        return false;
    }
    FIL* handle = static_cast<FIL*>(heap::allocate(sizeof(FIL)));
    if (handle == nullptr) {
        return false;
    }
    memset(handle, 0, sizeof(FIL));
    bool writable = volume->writable;
    FRESULT opened = f_open(handle, path, writable ? (FA_READ | FA_WRITE) : FA_READ);
    if (opened == FR_DENIED && writable) {
        // FAT's read-only bit: kept for reading.
        writable = false;
        opened = f_open(handle, path, FA_READ);
    }
    if (opened != FR_OK) {
        heap::free(handle);
        return false;
    }
    file.kept = handle;
    file.kept_writable = writable;
    return true;
}

FRESULT close_kept(Node& file) {
    if (file.kept == nullptr) {
        return FR_OK;
    }
    const FRESULT closed = f_close(file.kept);
    heap::free(file.kept);
    file.kept = nullptr;
    file.kept_writable = false;
    return closed;
}

// The FatFs file one operation works through: the kept one, or one opened by
// path for this operation and closed at its end.
struct Access {
    FIL* handle;
    bool temporary_open;
    FIL temporary;
};

FRESULT begin_access(Node& file, BYTE mode, Access& access) {
    access.handle = nullptr;
    access.temporary_open = false;
    if (file.kept != nullptr && ((mode & FA_WRITE) == 0 || file.kept_writable)) {
        access.handle = file.kept;
        return FR_OK;
    }
    // A write through a file kept for reading: never two open at once.
    (void)close_kept(file);
    char path[kFatfsPathCapacity] = {};
    if (!drive_path(file.volume, file.path, path, sizeof(path))) {
        return FR_INVALID_NAME;
    }
    memset(&access.temporary, 0, sizeof(access.temporary));
    const FRESULT opened = f_open(&access.temporary, path, mode);
    if (opened != FR_OK) {
        if (file.held) {
            (void)open_kept(file);
        }
        return opened;
    }
    access.handle = &access.temporary;
    access.temporary_open = true;
    return FR_OK;
}

// Closes a file opened for the operation, or syncs the kept one after a
// write; then opens the kept file again if the operation had to close it.
FRESULT end_access(Node& file, Access& access, bool wrote) {
    FRESULT result = FR_OK;
    if (access.temporary_open) {
        result = f_close(&access.temporary);
    } else if (wrote && access.handle != nullptr) {
        result = f_sync(access.handle);
    }
    access.handle = nullptr;
    access.temporary_open = false;
    if (file.held && file.kept == nullptr) {
        (void)open_kept(file);
    }
    return result;
}

Status read(vfs::Vnode& node, size_t offset, void* buffer, size_t count) {
    Node* file = file_from_vnode(node);
    if (file == nullptr) {
        return Status::invalid;
    }
    Access access;
    const FRESULT opened = begin_access(*file, FA_READ, access);
    if (opened != FR_OK) {
        return status_of(opened, Status::io_error);
    }
    UINT done = 0;
    const bool ok = f_lseek(access.handle, static_cast<FSIZE_t>(offset)) == FR_OK &&
        f_read(access.handle, buffer, static_cast<UINT>(count), &done) == FR_OK && done == count;
    node.size = f_size(access.handle);
    (void)end_access(*file, access, false);
    return ok ? Status::ok : Status::io_error;
}

// `truncate` empties the file first and writes from 0, as SxFS does. A full
// volume writes what fits: `written` says how much, and nothing written at all
// is a failure.
Status write(vfs::Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written) {
    written = 0;
    Node* file = file_from_vnode(node);
    Volume* volume = file != nullptr ? volume_for_id(file->volume) : nullptr;
    const Status allowed = writable_path(volume, file != nullptr);
    if (allowed != Status::ok) {
        return allowed;
    }
    Access access;
    const FRESULT opened = begin_access(*file, FA_WRITE, access);
    if (opened != FR_OK) {
        return status_of(opened, Status::read_only);
    }
    FIL& handle = *access.handle;
    bool ok = true;
    if (truncate) {
        ok = f_lseek(&handle, 0) == FR_OK && f_truncate(&handle) == FR_OK;
        offset = 0;
    }
    const FSIZE_t target = static_cast<FSIZE_t>(offset);
    if (ok && target > f_size(&handle)) {
        ok = f_lseek(&handle, f_size(&handle)) == FR_OK && zero_fill(handle, target);
    } else if (ok) {
        ok = f_lseek(&handle, target) == FR_OK;
    }
    Status status = ok ? Status::ok : Status::io_error;
    if (ok && count != 0) {
        UINT done = 0;
        const FRESULT wrote = f_write(&handle, buffer, static_cast<UINT>(count), &done);
        written = done;
        // FatFs reports a full volume as success with nothing written.
        status = wrote != FR_OK ? status_of(wrote) : (done != 0 ? Status::ok : Status::no_space);
    }
    node.size = f_size(&handle);
    if (end_access(*file, access, true) != FR_OK && status == Status::ok) {
        status = Status::io_error;
    }
    return status;
}

// Shrinks the file, or grows it with zeros.
Status truncate(vfs::Vnode& node, size_t size) {
    Node* file = file_from_vnode(node);
    Volume* volume = file != nullptr ? volume_for_id(file->volume) : nullptr;
    const Status allowed = writable_path(volume, file != nullptr);
    if (allowed != Status::ok) {
        return allowed;
    }
    Access access;
    const FRESULT opened = begin_access(*file, FA_WRITE, access);
    if (opened != FR_OK) {
        return status_of(opened, Status::read_only);
    }
    FIL& handle = *access.handle;
    const FSIZE_t target = static_cast<FSIZE_t>(size);
    Status status = Status::ok;
    if (target <= f_size(&handle)) {
        status = f_lseek(&handle, target) == FR_OK && f_truncate(&handle) == FR_OK ? Status::ok : Status::io_error;
    } else if (f_lseek(&handle, f_size(&handle)) != FR_OK) {
        status = Status::io_error;
    } else if (!zero_fill(handle, target)) {
        // Growing stops where the volume does.
        status = Status::no_space;
    }
    node.size = f_size(&handle);
    if (end_access(*file, access, true) != FR_OK && status == Status::ok) {
        status = Status::io_error;
    }
    return status;
}

// The file is gone from the disk at once. FAT has no way to keep an unlinked
// file alive, so the driver has no release and the VFS never asks it to
// (`keep_open` is always false here): unlinking a file that is open is EBUSY.
Status unlink(vfs::Vnode& node, bool) {
    Node* file = file_from_vnode(node);
    Volume* volume = file != nullptr ? volume_for_id(file->volume) : nullptr;
    char path[kFatfsPathCapacity] = {};
    const Status allowed = writable_path(volume, file != nullptr && drive_path(file->volume, file->path, path, sizeof(path)));
    if (allowed != Status::ok) {
        return allowed;
    }
    // The VFS never unlinks a held file here (no release), but a kept file
    // must not outlive the clusters it points at.
    file->held = false;
    (void)close_kept(*file);
    const FRESULT removed = f_unlink(path);
    if (removed != FR_OK) {
        return status_of(removed, Status::read_only);
    }
    node.data = nullptr;
    node.size = 0;
    drop_node(*volume, file);
    return Status::ok;
}

Status remove_directory(vfs::Vnode& node) {
    Node* directory = directory_from_vnode(node);
    Volume* volume = directory != nullptr ? volume_for_id(directory->volume) : nullptr;
    char path[kFatfsPathCapacity] = {};
    const Status allowed = writable_path(
        volume, directory != nullptr && drive_path(directory->volume, directory->path, path, sizeof(path)));
    if (allowed != Status::ok) {
        return allowed;
    }
    const FRESULT removed = f_unlink(path);
    if (removed != FR_OK) {
        return status_of(removed, Status::not_empty);
    }
    drop_node(*volume, directory);
    return Status::ok;
}

Status create_file(fs::VolumeId id, const char* path, const vfs::Ownership&, vfs::Vnode*& created) {
    created = nullptr;
    Volume* volume = volume_for_id(id);
    const char* inner = volume != nullptr ? inner_path(*volume, path) : nullptr;
    char fatfs_path[kFatfsPathCapacity] = {};
    const Status allowed = writable_path(
        volume, inner != nullptr && drive_path(static_cast<uint8_t>(id), inner, fatfs_path, sizeof(fatfs_path)));
    if (allowed != Status::ok) {
        return allowed;
    }
    Node* entry = new_node(static_cast<uint8_t>(id), false, inner);
    if (entry == nullptr) {
        return Status::no_space;
    }
    FIL handle = {};
    const FRESULT opened = f_open(&handle, fatfs_path, FA_WRITE | FA_CREATE_NEW);
    if (opened != FR_OK) {
        heap::free(entry);
        return status_of(opened);
    }
    const bool closed = f_close(&handle) == FR_OK;
    entry->vnode = closed ? vfs::install_volume_file(path, driver_ref(), entry, 0, true) : nullptr;
    if (entry->vnode == nullptr) {
        // The VFS is full: the file must not stay on the disk unpublished.
        (void)f_unlink(fatfs_path);
        heap::free(entry);
        return closed ? Status::no_space : Status::io_error;
    }
    const uint32_t now = get_fattime();
    publish_ownership(*entry->vnode, *volume, false, true, static_cast<uint16_t>(now >> 16), static_cast<uint16_t>(now));
    link_node(*volume, entry);
    created = entry->vnode;
    return Status::ok;
}

Status create_directory(fs::VolumeId id, const char* path, const vfs::Ownership&, vfs::Vnode*& created) {
    created = nullptr;
    Volume* volume = volume_for_id(id);
    const char* inner = volume != nullptr ? inner_path(*volume, path) : nullptr;
    char fatfs_path[kFatfsPathCapacity] = {};
    const Status allowed = writable_path(
        volume, inner != nullptr && drive_path(static_cast<uint8_t>(id), inner, fatfs_path, sizeof(fatfs_path)));
    if (allowed != Status::ok) {
        return allowed;
    }
    Node* entry = new_node(static_cast<uint8_t>(id), true, inner);
    if (entry == nullptr) {
        return Status::no_space;
    }
    const FRESULT made = f_mkdir(fatfs_path);
    if (made != FR_OK) {
        heap::free(entry);
        return status_of(made);
    }
    entry->vnode = vfs::ensure_directory(path);
    if (entry->vnode == nullptr) {
        (void)f_unlink(fatfs_path);
        heap::free(entry);
        return Status::no_space;
    }
    const uint32_t now = get_fattime();
    publish_ownership(*entry->vnode, *volume, true, true, static_cast<uint16_t>(now >> 16), static_cast<uint16_t>(now));
    link_node(*volume, entry);
    created = entry->vnode;
    return Status::ok;
}

// Opens again every held file whose kept file was closed.
void reopen_held(Volume& volume) {
    for (Node* entry = volume.nodes; entry != nullptr; entry = entry->next) {
        if (entry->held && entry->kept == nullptr) {
            (void)open_kept(*entry);
        }
    }
}

// `prefix` names `path` itself or a directory above it.
bool path_under(const char* path, const char* prefix, size_t prefix_length) {
    return strncmp(path, prefix, prefix_length) == 0 && (path[prefix_length] == 0 || path[prefix_length] == '/');
}

// Renames a file or a directory, and with a directory, the path every record
// below it carries. Every new path is checked to fit before anything moves.
// FAT cannot replace a name atomically (replaces_on_rename is false), so the
// VFS never calls this for a destination that exists.
Status rename(fs::VolumeId id, const char* old_path, const char* new_path, bool) {
    Volume* volume = volume_for_id(id);
    const char* old_inner = volume != nullptr ? inner_path(*volume, old_path) : nullptr;
    const char* new_inner = volume != nullptr ? inner_path(*volume, new_path) : nullptr;
    char old_fatfs[kFatfsPathCapacity] = {};
    char new_fatfs[kFatfsPathCapacity] = {};
    const Status allowed = writable_path(
        volume,
        old_inner != nullptr && new_inner != nullptr &&
            drive_path(static_cast<uint8_t>(id), old_inner, old_fatfs, sizeof(old_fatfs)) &&
            drive_path(static_cast<uint8_t>(id), new_inner, new_fatfs, sizeof(new_fatfs)));
    if (allowed != Status::ok) {
        return allowed;
    }
    const size_t old_length = strlen(old_inner);
    const size_t new_length = strlen(new_inner);
    // FatFs moves a directory into its own subtree without a word, which
    // leaves it hanging from itself, unreachable from the root.
    if (path_under(new_inner, old_inner, old_length)) {
        return Status::invalid;
    }
    for (Node* entry = volume->nodes; entry != nullptr; entry = entry->next) {
        if (path_under(entry->path, old_inner, old_length) &&
            strlen(entry->path) - old_length + new_length >= vfs::kMaxPathLength) {
            return Status::invalid;
        }
    }
    // A kept file points into the directory entry the rename moves: close
    // the ones below the old name, and open them again, by the new name or
    // the old one if the rename failed.
    for (Node* entry = volume->nodes; entry != nullptr; entry = entry->next) {
        if (entry->kept != nullptr && path_under(entry->path, old_inner, old_length)) {
            (void)close_kept(*entry);
        }
    }
    // f_rename refuses a target that exists.
    const FRESULT moved = f_rename(old_fatfs, new_fatfs);
    if (moved != FR_OK) {
        reopen_held(*volume);
        return status_of(moved);
    }
    // old_inner and new_inner point into the VFS's buffers, not into a record,
    // so rewriting the records cannot change them under the loop.
    for (Node* entry = volume->nodes; entry != nullptr; entry = entry->next) {
        if (!path_under(entry->path, old_inner, old_length)) {
            continue;
        }
        char renamed[vfs::kMaxPathLength] = {};
        const size_t tail = strlen(entry->path) - old_length;
        memcpy(renamed, new_inner, new_length);
        memcpy(renamed + new_length, entry->path + old_length, tail + 1);
        memcpy(entry->path, renamed, new_length + tail + 1);
    }
    reopen_held(*volume);
    return Status::ok;
}

void refresh(vfs::Vnode& node) {
    Node* file = file_from_vnode(node);
    Volume* volume = file != nullptr ? volume_for_id(file->volume) : nullptr;
    char path[kFatfsPathCapacity] = {};
    if (volume == nullptr || !drive_path(file->volume, file->path, path, sizeof(path))) {
        return;
    }
    FILINFO info = {};
    if (f_stat(path, &info) == FR_OK) {
        const bool writable = file_writable(*volume, info.fattrib);
        node.size = info.fsize;
        node.writable = writable;
        publish_ownership(node, *volume, false, writable, info.fdate, info.ftime);
    }
}

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
    out.writable = volume->writable;
    out.file_count = volume->file_count;
    out.total_bytes = static_cast<uint64_t>(volume->fs.n_fatent - 2) * cluster_bytes;
    out.free_bytes = static_cast<uint64_t>(free_clusters) * cluster_bytes;
    out.used_bytes = out.total_bytes >= out.free_bytes ? out.total_bytes - out.free_bytes : 0;
    return true;
}

// FatFs writes its FAT, directories and FSInfo before each operation returns
// (f_close, f_mkdir, f_unlink, f_rename all sync the volume); what is left is
// the device's own cache.
bool sync(fs::VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return false;
    }
    return !volume->writable || block::sync(volume->device_index);
}
bool sync_deferred(fs::VolumeId id) {
    Volume* volume = volume_for_id(id);
    return volume && (!volume->writable || block::flush(volume->device_index));
}

void adopt_owner(fs::VolumeId id, const fs::Owner& owner) {
    Volume* volume = volume_for_id(id);
    if (volume != nullptr) {
        volume->owner = owner;
    }
}

void open_file(vfs::Vnode& node) {
    Node* file = file_from_vnode(node);
    if (file == nullptr) {
        return;
    }
    file->held = true;
    if (file->kept == nullptr) {
        (void)open_kept(*file);
    }
}

void close_file(vfs::Vnode& node) {
    Node* file = file_from_vnode(node);
    if (file == nullptr) {
        return;
    }
    file->held = false;
    (void)close_kept(*file);
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
    // An unlinked file cannot outlive its name on FAT.
    .release = nullptr,
    .create_file = &create_file,
    .create_directory = &create_directory,
    .rename = &rename,
    .replaces_on_rename = false,
    // FAT has no owners: chmod and chown are refused.
    .set_ownership = nullptr,
    .stats = &stats,
    .sync = &sync,
    .adopt_owner = &adopt_owner,
    .sync_deferred = &sync_deferred,
    .open = &open_file,
    .close = &close_file,
};

const fs::Driver& driver_ref() {
    return kDriver;
}

} // namespace

namespace fat {

const fs::Driver& driver() {
    return kDriver;
}

} // namespace fat
