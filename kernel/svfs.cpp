#include "kernel/svfs.hpp"

#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"

namespace {

constexpr char kMagic[8] = {'S', 'V', 'F', 'S', '1', '\0', '\0', '\0'};
constexpr uint32_t kVersion = 1;
constexpr size_t kMaxFiles = 64;
constexpr uint32_t kDirectoryLba = 1;
constexpr uint32_t kDirectorySectors = 8;
constexpr uint32_t kDefaultFileSectors = 16;

struct Superblock {
    char magic[8];
    uint32_t version;
    uint32_t total_sectors;
    uint32_t directory_lba;
    uint32_t directory_sectors;
    uint32_t data_lba;
    uint32_t max_files;
    uint32_t next_free_lba;
    uint8_t reserved[476];
};

struct DirectoryEntry {
    uint8_t in_use;
    uint8_t reserved[3];
    char name[48];
    uint32_t start_lba;
    uint32_t sector_count;
    uint32_t size;
};

static_assert(sizeof(Superblock) == block::kSectorSize);
static_assert(sizeof(DirectoryEntry) == 64);

Superblock g_superblock = {};
DirectoryEntry g_directory[kMaxFiles] = {};
svfs::FileRecord g_files[kMaxFiles] = {};
size_t g_device_index = static_cast<size_t>(-1);
svfs::MountStatus g_status = svfs::MountStatus::unavailable;

bool valid_superblock(const Superblock& superblock) {
    return memcmp(superblock.magic, kMagic, sizeof(kMagic)) == 0 &&
        superblock.version == kVersion &&
        superblock.directory_lba == kDirectoryLba &&
        superblock.directory_sectors == kDirectorySectors &&
        superblock.max_files == kMaxFiles &&
        superblock.data_lba == (kDirectoryLba + kDirectorySectors) &&
        superblock.next_free_lba >= superblock.data_lba &&
        superblock.next_free_lba <= superblock.total_sectors;
}

bool flush_superblock() {
    return block::write(g_device_index, 0, 1, &g_superblock);
}

bool flush_directory() {
    return block::write(g_device_index, g_superblock.directory_lba, g_superblock.directory_sectors, g_directory);
}

bool parse_leaf_name(const char* path, char* name, size_t capacity) {
    if (path == nullptr || name == nullptr || capacity == 0) {
        return false;
    }
    if (strncmp(path, "/disk/", 6) != 0) {
        return false;
    }

    const char* leaf = path + 6;
    if (*leaf == '\0') {
        return false;
    }

    size_t length = 0;
    while (leaf[length] != '\0') {
        if (leaf[length] == '/' || length + 1 >= capacity) {
            return false;
        }
        name[length] = leaf[length];
        ++length;
    }
    name[length] = '\0';
    return true;
}

size_t file_index(const svfs::FileRecord& file) {
    return static_cast<size_t>(&file - &g_files[0]);
}

void refresh_file_from_directory(size_t index) {
    const DirectoryEntry& entry = g_directory[index];
    svfs::FileRecord& file = g_files[index];
    memset(&file, 0, sizeof(file));
    if (entry.in_use == 0) {
        return;
    }

    file.in_use = true;
    strcpy(file.name, entry.name);
    file.start_lba = entry.start_lba;
    file.sector_count = entry.sector_count;
    file.size = entry.size;
}

svfs::FileRecord* find_file_by_name(const char* name) {
    for (size_t index = 0; index < kMaxFiles; ++index) {
        if (g_files[index].in_use && strcmp(g_files[index].name, name) == 0) {
            return &g_files[index];
        }
    }
    return nullptr;
}

size_t read_write_capacity(const svfs::FileRecord& file) {
    return static_cast<size_t>(file.sector_count) * block::kSectorSize;
}

bool zero_file_storage(const svfs::FileRecord& file) {
    uint8_t zero[block::kSectorSize] = {};
    for (uint32_t sector = 0; sector < file.sector_count; ++sector) {
        if (!block::write(g_device_index, file.start_lba + sector, 1, zero)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace svfs {

void initialize() {
    memset(&g_superblock, 0, sizeof(g_superblock));
    memset(g_directory, 0, sizeof(g_directory));
    memset(g_files, 0, sizeof(g_files));
    g_device_index = static_cast<size_t>(-1);
    g_status = MountStatus::unavailable;

    if (!block::ready()) {
        return;
    }

    for (size_t index = 0; index < block::device_count(); ++index) {
        block::DeviceInfo info = {};
        if (!block::device_info(index, info) || !info.present || info.sector_count <= kDirectoryLba + kDirectorySectors) {
            continue;
        }

        Superblock candidate = {};
        if (!block::read(index, 0, 1, &candidate) || !valid_superblock(candidate)) {
            continue;
        }

        DirectoryEntry directory[kMaxFiles] = {};
        if (!block::read(index, candidate.directory_lba, candidate.directory_sectors, directory)) {
            continue;
        }

        g_device_index = index;
        g_superblock = candidate;
        memcpy(g_directory, directory, sizeof(g_directory));
        for (size_t entry = 0; entry < kMaxFiles; ++entry) {
            refresh_file_from_directory(entry);
        }
        return;
    }
}

MountStatus status() {
    return g_status;
}

bool mounted() {
    return g_status == MountStatus::mounted;
}

bool mount_at_root() {
    if (g_device_index == static_cast<size_t>(-1)) {
        return false;
    }

    if (vfs::ensure_directory("/disk") == nullptr) {
        return false;
    }

    for (size_t index = 0; index < kMaxFiles; ++index) {
        FileRecord& file = g_files[index];
        if (!file.in_use) {
            continue;
        }

        char path[64] = "/disk/";
        strcpy(path + 6, file.name);
        file.vnode = vfs::install_external_file(path, vfs::Backend::svfs, &file, file.size, true);
        if (file.vnode == nullptr) {
            return false;
        }
    }

    g_status = MountStatus::mounted;
    return true;
}

size_t file_count() {
    size_t count = 0;
    for (const FileRecord& file : g_files) {
        if (file.in_use) {
            ++count;
        }
    }
    return count;
}

bool read_file(FileRecord& file, size_t offset, void* buffer, size_t count) {
    if (!file.in_use || buffer == nullptr || count == 0) {
        return false;
    }
    if (offset + count > file.size) {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t remaining = count;
    size_t file_offset = offset;
    uint8_t sector[block::kSectorSize] = {};

    while (remaining != 0) {
        const uint32_t lba = file.start_lba + static_cast<uint32_t>(file_offset / block::kSectorSize);
        const size_t sector_offset = file_offset % block::kSectorSize;
        const size_t chunk = (block::kSectorSize - sector_offset) < remaining
            ? (block::kSectorSize - sector_offset)
            : remaining;

        if (!block::read(g_device_index, lba, 1, sector)) {
            return false;
        }

        memcpy(bytes + (count - remaining), sector + sector_offset, chunk);
        file_offset += chunk;
        remaining -= chunk;
    }

    return true;
}

bool write_file(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written) {
    written = 0;
    if (!file.in_use) {
        return false;
    }

    if (truncate) {
        file.size = 0;
        g_directory[file_index(file)].size = 0;
        if (!flush_directory()) {
            return false;
        }
        if (file.vnode != nullptr) {
            file.vnode->size = 0;
        }
        if (count == 0) {
            return true;
        }
        offset = 0;
    }

    if (buffer == nullptr || count == 0) {
        return true;
    }

    const size_t capacity = read_write_capacity(file);
    if (offset >= capacity) {
        return true;
    }

    const size_t to_write = (capacity - offset) < count ? (capacity - offset) : count;
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    size_t remaining = to_write;
    size_t file_offset = offset;
    uint8_t sector[block::kSectorSize] = {};

    while (remaining != 0) {
        const uint32_t lba = file.start_lba + static_cast<uint32_t>(file_offset / block::kSectorSize);
        const size_t sector_offset = file_offset % block::kSectorSize;
        const size_t chunk = (block::kSectorSize - sector_offset) < remaining
            ? (block::kSectorSize - sector_offset)
            : remaining;

        if (sector_offset != 0 || chunk != block::kSectorSize) {
            if (!block::read(g_device_index, lba, 1, sector)) {
                return false;
            }
        } else {
            memset(sector, 0, sizeof(sector));
        }

        memcpy(sector + sector_offset, bytes + written, chunk);
        if (!block::write(g_device_index, lba, 1, sector)) {
            return false;
        }

        written += chunk;
        file_offset += chunk;
        remaining -= chunk;
    }

    const size_t new_size = offset + written;
    if (new_size > file.size) {
        file.size = static_cast<uint32_t>(new_size);
        g_directory[file_index(file)].size = file.size;
        if (!flush_directory()) {
            return false;
        }
        if (file.vnode != nullptr) {
            file.vnode->size = file.size;
        }
    }

    return true;
}

FileRecord* create_file(const char* path) {
    char name[48] = {};
    if (!parse_leaf_name(path, name, sizeof(name)) || g_device_index == static_cast<size_t>(-1)) {
        return nullptr;
    }

    if (FileRecord* existing = find_file_by_name(name)) {
        return existing;
    }

    size_t free_index = kMaxFiles;
    for (size_t index = 0; index < kMaxFiles; ++index) {
        if (!g_files[index].in_use) {
            free_index = index;
            break;
        }
    }
    if (free_index == kMaxFiles) {
        return nullptr;
    }

    if (g_superblock.next_free_lba + kDefaultFileSectors > g_superblock.total_sectors) {
        return nullptr;
    }

    DirectoryEntry& entry = g_directory[free_index];
    memset(&entry, 0, sizeof(entry));
    entry.in_use = 1;
    strcpy(entry.name, name);
    entry.start_lba = g_superblock.next_free_lba;
    entry.sector_count = kDefaultFileSectors;
    entry.size = 0;

    g_superblock.next_free_lba += kDefaultFileSectors;
    if (!flush_superblock() || !flush_directory()) {
        return nullptr;
    }

    refresh_file_from_directory(free_index);
    if (!zero_file_storage(g_files[free_index])) {
        return nullptr;
    }

    if (mounted()) {
        char vnode_path[64] = "/disk/";
        strcpy(vnode_path + 6, g_files[free_index].name);
        g_files[free_index].vnode = vfs::install_external_file(
            vnode_path,
            vfs::Backend::svfs,
            &g_files[free_index],
            0,
            true
        );
    }

    return &g_files[free_index];
}

FileRecord* file_from_vnode(vfs::Vnode& node) {
    for (FileRecord& file : g_files) {
        if (file.vnode == &node) {
            return &file;
        }
    }
    return nullptr;
}

} // namespace svfs
