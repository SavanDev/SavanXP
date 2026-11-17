#include "kernel/power.hpp"

#include <stdint.h>

#include "kernel/acpi.hpp"
#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/credentials.hpp"
#include "kernel/device.hpp"
#include "kernel/fs.hpp"
#include "kernel/object.hpp"
#include "kernel/panic.hpp"
#include "kernel/process.hpp"
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
        case POWER_IOC_SHUTDOWN:
            power::shutdown();
        case POWER_IOC_REBOOT:
            power::reboot();
        case POWER_IOC_PANIC: {
            const process::Process* caller = process::current();
            if (caller == nullptr || !cred::acts_as_system(caller->credentials)) {
                return -static_cast<int>(SX_EPERM);
            }
            (void)block::sync_all();
            panic("power: panic requested through /dev/power");
        }
        default:
            return -static_cast<int>(SX_ENOSYS);
    }
}

// Leaves the disks as a clean unmount would. The volumes go first: their sync
// writes the clean superblocks through the writeback queue, which the block
// layer then drains before telling each driver the power is going away.
void prepare(const char* what) {
    console::printf("power: %s: syncing volumes\n", what);
    if (!fs::sync_all()) {
        console::printf("power: a volume did not sync; it will replay its journal at the next mount\n");
    }
    if (!block::shutdown_all()) {
        console::printf("power: the writeback queue did not drain; queued writes are lost\n");
    }
}

} // namespace

namespace power {

void shutdown() {
    prepare("shutdown");
    acpi::shutdown();
}

void reboot() {
    prepare("reboot");
    acpi::reboot();
}

void initialize() {
    g_device.ioctl = power_ioctl;
    g_ready = device::register_node("/dev/power", &g_device, true);
}

bool ready() {
    return g_ready;
}

} // namespace power
