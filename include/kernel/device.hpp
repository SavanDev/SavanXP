#pragma once

#include <stddef.h>
#include <stdint.h>

namespace device {

struct Device {
    const char* name;
    int (*read)(uint64_t user_buffer, size_t count);
    int (*write)(uint64_t user_buffer, size_t count);
    int (*ioctl)(uint64_t request, uint64_t argument);
    void (*close)();
};

void initialize();
bool ready();
bool register_node(const char* path, Device* device, bool writable);
int read(Device* device, uint64_t user_buffer, size_t count);
int write(Device* device, uint64_t user_buffer, size_t count);
int ioctl(Device* device, uint64_t request, uint64_t argument);
void close(Device* device);
void service_background();

} // namespace device
