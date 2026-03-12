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

enum class EntryType : uint8_t {
    file = 0,
    directory = 1,
};

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

constexpr uint8_t kEntryUnused = 0;
constexpr uint8_t kEntryActive = 1;
constexpr uint8_t kEntryDeleted = 2;

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

EntryType entry_type(const DirectoryEntry& entry) {
    return entry.reserved[0] == static_cast<uint8_t>(EntryType::directory)
        ? EntryType::directory
        : EntryType::file;
}

void set_entry_type(DirectoryEntry& entry, EntryType type) {
    entry.reserved[0] = static_cast<uint8_t>(type);
}

bool parse_relative_path(const char* path, char* relative, size_t capacity, bool allow_root = false) {
    if (path == nullptr || relative == nullptr || capacity == 0) {
        return false;
    }
    if (strncmp(path, "/disk", 5) != 0 || (path[5] != '\0' && path[5] != '/')) {
        return false;
    }

    const char* cursor = path + 5;
    while (*cursor == '/') {
        ++cursor;
    }

    if (*cursor == '\0') {
        if (!allow_root) {
            return false;
        }
        relative[0] = '\0';
        return true;
    }

    size_t length = 0;
    bool previous_slash = false;
    while (*cursor != '\0') {
        const char current = *cursor++;
        if (current == '/') {
            if (previous_slash || *cursor == '\0') {
                return false;
            }
            previous_slash = true;
        } else {
            previous_slash = false;
        }

        if (length + 1 >= capacity) {
            return false;
        }
        relative[length++] = current;
    }

    relative[length] = '\0';
    return true;
}

bool split_relative_parent(const char* relative, char* parent, size_t parent_capacity) {
    if (relative == nullptr || parent == nullptr || parent_capacity == 0) {
        return false;
    }

    size_t length = strlen(relative);
    size_t slash = length;
    while (slash > 0 && relative[slash - 1] != '/') {
        --slash;
    }

    if (slash == 0) {
        parent[0] = '\0';
        return true;
    }

    const size_t parent_length = slash - 1;
    if (parent_length >= parent_capacity) {
        return false;
    }

    memcpy(parent, relative, parent_length);
    parent[parent_length] = '\0';
    return true;
}

size_t file_index(const svfs::FileRecord& file) {
    return static_cast<size_t>(&file - &g_files[0]);
}

void refresh_file_from_directory(size_t index) {
    const DirectoryEntry& entry = g_directory[index];
    svfs::FileRecord& file = g_files[index];
    vfs::Vnode* existing_vnode = file.vnode;
    memset(&file, 0, sizeof(file));
    file.vnode = existing_vnode;
    if (entry.in_use != kEntryActive) {
        return;
    }

    file.in_use = true;
    file.directory = entry_type(entry) == EntryType::directory;
    strcpy(file.name, entry.name);
    file.start_lba = entry.start_lba;
    file.sector_count = entry.sector_count;
    file.size = entry.size;
    if (file.vnode != nullptr) {
        file.vnode->size = file.size;
    }
}

svfs::FileRecord* find_file_by_name(const char* name) {
    for (size_t index = 0; index < kMaxFiles; ++index) {
        if (g_files[index].in_use && strcmp(g_files[index].name, name) == 0) {
            return &g_files[index];
        }
    }
    return nullptr;
}

bool relative_directory_exists(const char* name) {
    if (name == nullptr || *name == '\0') {
        return true;
    }

    svfs::FileRecord* entry = find_file_by_name(name);
    return entry != nullptr && entry->directory;
}

bool relative_path_is_prefix(const char* prefix, const char* path) {
    if (prefix == nullptr || path == nullptr) {
        return false;
    }
    const size_t prefix_length = strlen(prefix);
    if (prefix_length == 0) {
        return true;
    }
    return strncmp(path, prefix, prefix_length) == 0 &&
        (path[prefix_length] == '\0' || path[prefix_length] == '/');
}

bool relative_path_has_children(const char* name) {
    if (name == nullptr || *name == '\0') {
        for (const svfs::FileRecord& file : g_files) {
            if (file.in_use) {
                return true;
            }
        }
        return false;
    }

    for (const svfs::FileRecord& file : g_files) {
        if (!file.in_use) {
            continue;
        }
        if (relative_path_is_prefix(name, file.name) && strcmp(file.name, name) != 0) {
            return true;
        }
    }
    return false;
}

bool relative_path_exists_conflict(const char* old_name, const char* new_name) {
    for (const svfs::FileRecord& file : g_files) {
        if (!file.in_use || strcmp(file.name, old_name) == 0) {
            continue;
        }
        if (strcmp(file.name, new_name) == 0) {
            return true;
        }
    }
    return false;
}

bool can_rename_subtree(const svfs::FileRecord& target, const char* new_relative) {
    if (relative_path_exists_conflict(target.name, new_relative)) {
        return false;
    }
    if (!target.directory) {
        return true;
    }
    if (relative_path_is_prefix(target.name, new_relative)) {
        return false;
    }

    const size_t old_prefix_length = strlen(target.name);
    for (const svfs::FileRecord& file : g_files) {
        if (!file.in_use || !relative_path_is_prefix(target.name, file.name)) {
            continue;
        }

        char candidate[48] = {};
        size_t candidate_length = 0;
        const size_t new_prefix_length = strlen(new_relative);
        if (new_prefix_length >= sizeof(candidate)) {
            return false;
        }
        memcpy(candidate, new_relative, new_prefix_length);
        candidate_length = new_prefix_length;

        const char* suffix = file.name + old_prefix_length;
        while (*suffix != '\0') {
            if (candidate_length + 1 >= sizeof(candidate)) {
                return false;
            }
            candidate[candidate_length++] = *suffix++;
        }
        candidate[candidate_length] = '\0';

        if (relative_path_exists_conflict(file.name, candidate)) {
            return false;
        }
    }
    return true;
}

size_t read_write_capacity(const svfs::FileRecord& file) {
    return static_cast<size_t>(file.sector_count) * block::kSectorSize;
}

uint32_t sectors_for_size(size_t size) {
    return size == 0 ? 0u : static_cast<uint32_t>((size + block::kSectorSize - 1) / block::kSectorSize);
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

bool zero_storage_range(uint32_t start_lba, uint32_t sector_count) {
    uint8_t zero[block::kSectorSize] = {};
    for (uint32_t sector = 0; sector < sector_count; ++sector) {
        if (!block::write(g_device_index, start_lba + sector, 1, zero)) {
            return false;
        }
    }
    return true;
}

bool relocate_file_storage(svfs::FileRecord& file, uint32_t new_start_lba, uint32_t new_sector_count) {
    uint8_t sector[block::kSectorSize] = {};

    if (!zero_storage_range(new_start_lba, new_sector_count)) {
        return false;
    }

    for (uint32_t index = 0; index < file.sector_count; ++index) {
        if (!block::read(g_device_index, file.start_lba + index, 1, sector)) {
            return false;
        }
        if (!block::write(g_device_index, new_start_lba + index, 1, sector)) {
            return false;
        }
    }

    return true;
}

bool ensure_file_capacity(svfs::FileRecord& file, size_t required_size) {
    if (!file.in_use || file.directory) {
        return false;
    }

    const size_t current_capacity = read_write_capacity(file);
    if (required_size <= current_capacity) {
        return true;
    }

    uint32_t required_sectors = sectors_for_size(required_size);
    uint32_t new_sector_count = file.sector_count;

    if (new_sector_count == 0) {
        new_sector_count = kDefaultFileSectors;
    }
    while (new_sector_count < required_sectors) {
        if (new_sector_count < kDefaultFileSectors) {
            new_sector_count = kDefaultFileSectors;
        } else {
            new_sector_count *= 2;
        }
    }

    DirectoryEntry& entry = g_directory[file_index(file)];
    const uint32_t current_end_lba = file.start_lba + file.sector_count;

    if (current_end_lba == g_superblock.next_free_lba) {
        const uint32_t additional_sectors = new_sector_count - file.sector_count;
        if (g_superblock.next_free_lba + additional_sectors <= g_superblock.total_sectors) {
            if (!zero_storage_range(g_superblock.next_free_lba, additional_sectors)) {
                return false;
            }

            g_superblock.next_free_lba += additional_sectors;
            entry.sector_count = new_sector_count;
            if (!flush_superblock() || !flush_directory()) {
                return false;
            }

            file.sector_count = new_sector_count;
            return true;
        }
    }

    if (g_superblock.next_free_lba + new_sector_count > g_superblock.total_sectors) {
        return false;
    }

    const uint32_t new_start_lba = g_superblock.next_free_lba;
    if (!relocate_file_storage(file, new_start_lba, new_sector_count)) {
        return false;
    }

    g_superblock.next_free_lba += new_sector_count;
    entry.start_lba = new_start_lba;
    entry.sector_count = new_sector_count;
    if (!flush_superblock() || !flush_directory()) {
        return false;
    }

    file.start_lba = new_start_lba;
    file.sector_count = new_sector_count;
    return true;
}

svfs::FileRecord* install_directory_entry(const char* relative_name) {
    if (relative_name == nullptr || *relative_name == '\0') {
        return nullptr;
    }

    if (svfs::FileRecord* existing = find_file_by_name(relative_name)) {
        return existing->directory ? existing : nullptr;
    }

    char parent[48] = {};
    if (!split_relative_parent(relative_name, parent, sizeof(parent)) || !relative_directory_exists(parent)) {
        return nullptr;
    }

    size_t entry_index = kMaxFiles;
    for (size_t index = 0; index < kMaxFiles; ++index) {
        if (g_directory[index].in_use == kEntryUnused || g_directory[index].in_use == kEntryDeleted) {
            entry_index = index;
            break;
        }
    }
    if (entry_index == kMaxFiles) {
        return nullptr;
    }

    DirectoryEntry& entry = g_directory[entry_index];
    memset(&entry, 0, sizeof(entry));
    entry.in_use = kEntryActive;
    set_entry_type(entry, EntryType::directory);
    strcpy(entry.name, relative_name);
    if (!flush_directory()) {
        memset(&entry, 0, sizeof(entry));
        return nullptr;
    }

    refresh_file_from_directory(entry_index);
    return &g_files[entry_index];
}

void mount_record(svfs::FileRecord& file) {
    char path[64] = "/disk/";
    strcpy(path + 6, file.name);
    if (file.directory) {
        file.vnode = vfs::ensure_directory(path);
    } else {
        file.vnode = vfs::install_external_file(path, vfs::Backend::svfs, &file, file.size, true);
    }
}

void ensure_standard_directories() {
    (void)install_directory_entry("bin");
    (void)install_directory_entry("tmp");
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
        ensure_standard_directories();
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
        if (!file.in_use || !file.directory) {
            continue;
        }
        mount_record(file);
        if (file.vnode == nullptr) {
            return false;
        }
    }

    for (size_t index = 0; index < kMaxFiles; ++index) {
        FileRecord& file = g_files[index];
        if (!file.in_use || file.directory) {
            continue;
        }
        mount_record(file);
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
        if (file.in_use && !file.directory) {
            ++count;
        }
    }
    return count;
}

bool read_file(FileRecord& file, size_t offset, void* buffer, size_t count) {
    if (!file.in_use || file.directory || buffer == nullptr || count == 0) {
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
    if (!file.in_use || file.directory) {
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

    if (!ensure_file_capacity(file, offset + count)) {
        return true;
    }

    const size_t capacity = read_write_capacity(file);
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

bool truncate_file(FileRecord& file, size_t size) {
    if (!file.in_use || file.directory) {
        return false;
    }

    if (!ensure_file_capacity(file, size)) {
        return false;
    }

    if (size > file.size) {
        uint8_t zero[block::kSectorSize] = {};
        size_t remaining = size - file.size;
        size_t file_offset = file.size;
        while (remaining != 0) {
            const uint32_t lba = file.start_lba + static_cast<uint32_t>(file_offset / block::kSectorSize);
            const size_t sector_offset = file_offset % block::kSectorSize;
            const size_t chunk = (block::kSectorSize - sector_offset) < remaining
                ? (block::kSectorSize - sector_offset)
                : remaining;
            uint8_t sector[block::kSectorSize] = {};
            if (sector_offset != 0 || chunk != block::kSectorSize) {
                if (!block::read(g_device_index, lba, 1, sector)) {
                    return false;
                }
            }
            memcpy(sector + sector_offset, zero, chunk);
            if (!block::write(g_device_index, lba, 1, sector)) {
                return false;
            }
            file_offset += chunk;
            remaining -= chunk;
        }
    }

    file.size = static_cast<uint32_t>(size);
    g_directory[file_index(file)].size = file.size;
    if (!flush_directory()) {
        return false;
    }
    if (file.vnode != nullptr) {
        file.vnode->size = file.size;
    }
    return true;
}

FileRecord* create_file(const char* path) {
    char relative[48] = {};
    if (!parse_relative_path(path, relative, sizeof(relative)) || g_device_index == static_cast<size_t>(-1)) {
        return nullptr;
    }

    char parent[48] = {};
    if (!split_relative_parent(relative, parent, sizeof(parent)) || !relative_directory_exists(parent)) {
        return nullptr;
    }

    if (FileRecord* existing = find_file_by_name(relative)) {
        return existing->directory ? nullptr : existing;
    }

    size_t free_index = kMaxFiles;
    size_t reusable_index = kMaxFiles;
    for (size_t index = 0; index < kMaxFiles; ++index) {
        if (g_directory[index].in_use == kEntryDeleted && g_directory[index].sector_count >= kDefaultFileSectors) {
            reusable_index = index;
            break;
        }
        if (g_directory[index].in_use == kEntryUnused) {
            free_index = index;
            break;
        }
    }
    const bool reusing_extent = reusable_index != kMaxFiles;
    if (!reusing_extent && free_index == kMaxFiles) {
        return nullptr;
    }
    if (!reusing_extent && g_superblock.next_free_lba + kDefaultFileSectors > g_superblock.total_sectors) {
        return nullptr;
    }

    const size_t entry_index = reusing_extent ? reusable_index : free_index;
    DirectoryEntry& entry = g_directory[entry_index];
    const uint32_t preserved_lba = entry.start_lba;
    const uint32_t preserved_sectors = entry.sector_count;
    memset(&entry, 0, sizeof(entry));
    entry.in_use = kEntryActive;
    set_entry_type(entry, EntryType::file);
    strcpy(entry.name, relative);
    entry.start_lba = reusing_extent ? preserved_lba : g_superblock.next_free_lba;
    entry.sector_count = reusing_extent ? preserved_sectors : kDefaultFileSectors;
    entry.size = 0;

    if (!reusing_extent) {
        g_superblock.next_free_lba += kDefaultFileSectors;
    }
    if ((!reusing_extent && !flush_superblock()) || !flush_directory()) {
        return nullptr;
    }

    refresh_file_from_directory(entry_index);
    if (!zero_file_storage(g_files[entry_index])) {
        return nullptr;
    }

    if (mounted()) {
        mount_record(g_files[entry_index]);
    }

    return &g_files[entry_index];
}

FileRecord* create_directory(const char* path) {
    char relative[48] = {};
    if (!parse_relative_path(path, relative, sizeof(relative)) || g_device_index == static_cast<size_t>(-1)) {
        return nullptr;
    }
    FileRecord* directory = install_directory_entry(relative);
    if (directory != nullptr && mounted() && directory->vnode == nullptr) {
        mount_record(*directory);
    }
    return directory;
}

bool remove_directory(FileRecord& file) {
    if (!file.in_use || !file.directory || relative_path_has_children(file.name)) {
        return false;
    }

    DirectoryEntry& entry = g_directory[file_index(file)];
    entry.in_use = kEntryDeleted;
    memset(entry.name, 0, sizeof(entry.name));
    entry.size = 0;
    entry.start_lba = 0;
    entry.sector_count = 0;
    set_entry_type(entry, EntryType::directory);
    if (!flush_directory()) {
        entry.in_use = kEntryActive;
        strcpy(entry.name, file.name);
        set_entry_type(entry, EntryType::directory);
        return false;
    }

    memset(&file, 0, sizeof(file));
    return true;
}

bool rename_path(const char* old_path, const char* new_path) {
    char old_relative[48] = {};
    char new_relative[48] = {};
    if (!parse_relative_path(old_path, old_relative, sizeof(old_relative)) ||
        !parse_relative_path(new_path, new_relative, sizeof(new_relative)) ||
        strcmp(old_relative, new_relative) == 0) {
        return false;
    }

    FileRecord* target = find_file_by_name(old_relative);
    if (target == nullptr) {
        return false;
    }

    char new_parent[48] = {};
    if (!split_relative_parent(new_relative, new_parent, sizeof(new_parent)) || !relative_directory_exists(new_parent)) {
        return false;
    }
    if (!can_rename_subtree(*target, new_relative)) {
        return false;
    }

    const size_t old_prefix_length = strlen(old_relative);
    const size_t new_prefix_length = strlen(new_relative);
    for (size_t index = 0; index < kMaxFiles; ++index) {
        FileRecord& file = g_files[index];
        if (!file.in_use || !relative_path_is_prefix(old_relative, file.name)) {
            continue;
        }

        char candidate[48] = {};
        memcpy(candidate, new_relative, new_prefix_length);
        size_t candidate_length = new_prefix_length;
        const char* suffix = file.name + old_prefix_length;
        while (*suffix != '\0') {
            if (candidate_length + 1 >= sizeof(candidate)) {
                return false;
            }
            candidate[candidate_length++] = *suffix++;
        }
        candidate[candidate_length] = '\0';
        strcpy(g_directory[index].name, candidate);
    }

    if (!flush_directory()) {
        return false;
    }

    for (size_t index = 0; index < kMaxFiles; ++index) {
        refresh_file_from_directory(index);
    }
    return true;
}

bool unlink_file(FileRecord& file) {
    if (!file.in_use || file.directory) {
        return false;
    }

    DirectoryEntry& entry = g_directory[file_index(file)];
    entry.in_use = kEntryDeleted;
    memset(entry.name, 0, sizeof(entry.name));
    entry.size = 0;
    set_entry_type(entry, EntryType::file);
    if (!flush_directory()) {
        entry.in_use = kEntryActive;
        strcpy(entry.name, file.name);
        entry.size = file.size;
        return false;
    }

    memset(&file, 0, sizeof(file));
    return true;
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
