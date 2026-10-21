#include "kernel/power.hpp"

#include <stdint.h>

#include "kernel/acpi.hpp"
#include "kernel/block.hpp"
#include "kernel/device.hpp"
#include "kernel/object.hpp"
#include "abi/savanxp_abi.h"

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
        return -static_cast<int>(SX_EACCES);
    }
    switch (request) {
        // Writes still in the block writeback queue would die with the power.
        case POWER_IOC_SHUTDOWN:
            (void)block::sync_all();
            acpi::shutdown(); // does not return
        case POWER_IOC_REBOOT:
            (void)block::sync_all();
            acpi::reboot(); // does not return
        default:
            return -static_cast<int>(SX_ENOSYS);
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
