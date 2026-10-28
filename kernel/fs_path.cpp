#include "kernel/fs.hpp"

#include "kernel/string.hpp"

/* Mount-point path arithmetic, apart from fs.cpp so the host SxFS test links
 * the same code the kernel runs instead of a copy of it. */
namespace fs {

const char* path_below(const char* mount_point, const char* path) {
    if (mount_point == nullptr || path == nullptr || path[0] != '/') {
        return nullptr;
    }
    if (strcmp(mount_point, "/") == 0) {
        return path[1] == '\0' ? path + 1 : path;
    }
    const size_t length = strlen(mount_point);
    if (strncmp(path, mount_point, length) != 0 ||
        (path[length] != '\0' && path[length] != '/')) {
        return nullptr;
    }
    return path + length;
}

bool join_mount_path(const char* mount_point, const char* relative, char* out, size_t capacity) {
    if (mount_point == nullptr || relative == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    const bool root = strcmp(mount_point, "/") == 0;
    const size_t prefix = root ? 0 : strlen(mount_point);
    const size_t relative_length = strlen(relative);
    if (relative_length == 0) {
        if (strlen(mount_point) + 1 > capacity) {
            return false;
        }
        strcpy(out, mount_point);
        return true;
    }
    if (prefix + 1 + relative_length + 1 > capacity) {
        return false;
    }
    memcpy(out, mount_point, prefix);
    out[prefix] = '/';
    strcpy(out + prefix + 1, relative);
    return true;
}

} // namespace fs
