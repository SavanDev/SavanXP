#include "kernel/device.hpp"

#include "kernel/display.hpp"
#include "kernel/vfs.hpp"
#include "abi/savanxp_abi.h"

namespace {

bool g_ready = false;

int negative_error(sx_status code) {
    return -static_cast<int>(code);
}

} // namespace

namespace device {

void initialize() {
    g_ready = true;
}

bool ready() {
    return g_ready;
}

bool register_node(const char* path, Device* device, bool writable, uint16_t permissions) {
    if (!g_ready || path == nullptr || device == nullptr) {
        return false;
    }
    vfs::Vnode* node = vfs::install_external_file(path, vfs::Backend::device, device, 0, writable);
    if (node == nullptr) {
        return false;
    }
    if (permissions == 0) {
        permissions = writable ? 0666 : 0444;
    }
    vfs::set_ownership(*node, cred::kSystemUid, cred::kSystemGid,
                       static_cast<uint16_t>(vfs::kModeFile | (permissions & vfs::kModePermissions)));
    return true;
}

int read(Device* device, uint64_t user_buffer, size_t count) {
    if (device == nullptr) {
        return negative_error(SX_ENODEV);
    }
    if (device->read == nullptr) {
        return negative_error(SX_ENOSYS);
    }
    return device->read(user_buffer, count);
}

int write(Device* device, uint64_t user_buffer, size_t count) {
    if (device == nullptr) {
        return negative_error(SX_ENODEV);
    }
    if (device->write == nullptr) {
        return negative_error(SX_ENOSYS);
    }
    return device->write(user_buffer, count);
}

int ioctl(Device* device, uint64_t request, uint64_t argument, uint32_t granted_access) {
    if (device == nullptr) {
        return negative_error(SX_ENODEV);
    }
    if (device->ioctl == nullptr) {
        return negative_error(SX_ENOSYS);
    }
    return device->ioctl(request, argument, granted_access);
}

void close(Device* device) {
    if (device != nullptr && device->close != nullptr) {
        device->close();
    }
}

bool can_read(Device* device) {
    if (device == nullptr || device->read == nullptr) {
        return false;
    }
    if (device->can_read != nullptr) {
        return device->can_read();
    }
    return true;
}

void service_background() {
    display::poll();
}

} // namespace device
