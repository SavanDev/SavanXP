#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {

enum class NodeType : uint8_t {
    invalid = 0,
    directory = 1,
    file = 2,
};

struct Vnode {
    NodeType type;
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
const Vnode* root();
const Vnode* resolve(const char* path);
Vnode* open(const char* path, uint32_t flags);
size_t read(Vnode& node, size_t offset, void* buffer, size_t count);
size_t write(Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate);
const Vnode* child_at(const Vnode& directory, size_t index);
size_t child_count(const Vnode& directory);

} // namespace vfs
