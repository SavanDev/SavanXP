#include "kernel/power.hpp"

#include <stdint.h>

#include "kernel/acpi.hpp"
#include "kernel/device.hpp"
#include "kernel/object.hpp"
#include "savanxp/syscall.h"

namespace {

device::Device g_device = {
    .name = "power",
    .read = nullptr,
    .write = nullptr,
    .ioctl = nullptr,
    .close = nullptr,
    .can_read = nullptr,
};

bool g_ready = false;

int power_ioctl(uint64_t request, uint64_t /*argument*/, uint32_t granted_access) {
    if ((granted_access & object::access_write) == 0) {
        return -static_cast<int>(SAVANXP_EACCES);
    }
    switch (request) {
        case POWER_IOC_SHUTDOWN:
            acpi::shutdown(); // no retorna
        case POWER_IOC_REBOOT:
            acpi::reboot(); // no retorna
        default:
            return -static_cast<int>(SAVANXP_ENOSYS);
    }
}

} // namespace

namespace power {

void initialize() {
    g_device.ioctl = power_ioctl;
    g_ready = device::register_node("/dev/power", &g_device, true);
}

bool ready() {
    return g_ready;
}

} // namespace power
