#include "kernel/device.hpp"

#include "kernel/virtio_gpu.hpp"
#include "kernel/vfs.hpp"
#include "shared/syscall.h"

namespace {

bool g_ready = false;

int negative_error(savanxp_error_code code) {
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

bool register_node(const char* path, Device* device, bool writable) {
    if (!g_ready || path == nullptr || device == nullptr) {
        return false;
    }
    return vfs::install_external_file(path, vfs::Backend::device, device, 0, writable) != nullptr;
}

int read(Device* device, uint64_t user_buffer, size_t count) {
    if (device == nullptr) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (device->read == nullptr) {
        return negative_error(SAVANXP_ENOSYS);
    }
    return device->read(user_buffer, count);
}

int write(Device* device, uint64_t user_buffer, size_t count) {
    if (device == nullptr) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (device->write == nullptr) {
        return negative_error(SAVANXP_ENOSYS);
    }
    return device->write(user_buffer, count);
}

int ioctl(Device* device, uint64_t request, uint64_t argument) {
    if (device == nullptr) {
        return negative_error(SAVANXP_ENODEV);
    }
    if (device->ioctl == nullptr) {
        return negative_error(SAVANXP_ENOSYS);
    }
    return device->ioctl(request, argument);
}

void close(Device* device) {
    if (device != nullptr && device->close != nullptr) {
        device->close();
    }
}

void service_background() {
    virtio_gpu::poll();
}

} // namespace device
