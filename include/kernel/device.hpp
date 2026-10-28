#pragma once

#include <stddef.h>
#include <stdint.h>

namespace device {

struct Device {
    const char* name;
    int (*read)(uint64_t user_buffer, size_t count);
    int (*write)(uint64_t user_buffer, size_t count);
    int (*ioctl)(uint64_t request, uint64_t argument, uint32_t granted_access);
    void (*close)();
    bool (*can_read)();
};

void initialize();
bool ready();
// `permissions` (07777 bits) go on the node, owned by the system. 0, the
// default, is 0666 for a writable node and 0444 for a read-only one: open to
// every process, as devices were before ownership existed. A device only the
// system may touch (the keyboard: USERS_AND_PERMISSIONS.md, "Devices") passes
// 0600.
bool register_node(const char* path, Device* device, bool writable, uint16_t permissions = 0);
int read(Device* device, uint64_t user_buffer, size_t count);
int write(Device* device, uint64_t user_buffer, size_t count);
int ioctl(Device* device, uint64_t request, uint64_t argument, uint32_t granted_access);
void close(Device* device);
bool can_read(Device* device);
void service_background();

} // namespace device
