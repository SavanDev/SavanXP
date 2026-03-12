#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {

constexpr size_t kMaxPathLength = 256;
constexpr size_t kMaxPathComponentLength = 64;

enum class NodeType : uint8_t {
    invalid = 0,
    directory = 1,
    file = 2,
};

enum class Backend : uint8_t {
    memory = 0,
    svfs = 1,
    device = 2,
};

struct Vnode {
    NodeType type;
    Backend backend;
    const char* name;
    void* data;
    size_t size;
    bool writable;
    int parent;
    int first_child;
    int next_sibling;
};

void initialize(const void* archive, size_t size);
bool ready();
int last_error();
const Vnode* root();
bool normalize_path(const char* cwd, const char* input, char* output, size_t capacity);
const Vnode* resolve(const char* path);
Vnode* open(const char* path, uint32_t flags);
Vnode* ensure_directory(const char* path);
Vnode* mkdir(const char* path);
Vnode* install_external_file(const char* path, Backend backend, void* data, size_t size, bool writable);
bool unlink(const char* path);
bool rmdir(const char* path);
bool truncate(const char* path, size_t size);
bool rename(const char* old_path, const char* new_path);
size_t read(Vnode& node, size_t offset, void* buffer, size_t count);
size_t write(Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate);
const Vnode* child_at(const Vnode& directory, size_t index);
size_t child_count(const Vnode& directory);

} // namespace vfs
