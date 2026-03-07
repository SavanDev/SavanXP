#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {
struct Vnode;
}

namespace svfs {

enum class MountStatus : uint8_t {
    unavailable = 0,
    mounted = 1,
};

struct FileRecord {
    bool in_use;
    bool directory;
    char name[48];
    uint32_t start_lba;
    uint32_t sector_count;
    uint32_t size;
    vfs::Vnode* vnode;
};

void initialize();
MountStatus status();
bool mounted();
bool mount_at_root();
size_t file_count();
bool read_file(FileRecord& file, size_t offset, void* buffer, size_t count);
bool write_file(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written);
bool truncate_file(FileRecord& file, size_t size);
FileRecord* create_file(const char* path);
FileRecord* create_directory(const char* path);
bool remove_directory(FileRecord& file);
bool rename_path(const char* old_path, const char* new_path);
bool unlink_file(FileRecord& file);
FileRecord* file_from_vnode(vfs::Vnode& node);

} // namespace svfs
