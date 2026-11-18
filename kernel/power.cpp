#include "kernel/power.hpp"

#include <stddef.h>
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

// The process the power button tells (POWER_IOC_WATCH_BUTTON), and whether a
// press is waiting for it to read. Both change only under the big kernel lock:
// in a system call or in the SCI handler.
uint32_t g_button_watcher = 0;
bool g_button_pending = false;

bool watcher_alive() {
    if (g_button_watcher == 0) {
        return false;
    }
    const process::Process* watcher = process::find(g_button_watcher);
    return watcher != nullptr && watcher->life == process::Lifecycle::alive;
}

int power_read(uint64_t user_buffer, size_t count) {
    const process::Process* caller = process::current();
    if (caller == nullptr || caller->pid != g_button_watcher) {
        return -static_cast<int>(SX_EACCES);
    }
    if (count < sizeof(savanxp_power_event)) {
        return -static_cast<int>(SX_EINVAL);
    }
    if (!g_button_pending) {
        return -static_cast<int>(SX_EAGAIN);
    }
    const savanxp_power_event event = {.kind = SAVANXP_POWER_EVENT_BUTTON, .reserved = 0};
    if (!process::copy_to_user(user_buffer, &event, sizeof(event))) {
        return -static_cast<int>(SX_EINVAL);
    }
    g_button_pending = false;
    return static_cast<int>(sizeof(event));
}

bool power_can_read() {
    return g_button_pending;
}

int power_ioctl(uint64_t request, uint64_t /*argument*/, uint32_t granted_access) {
    if (request == POWER_IOC_WATCH_BUTTON) {
        const process::Process* caller = process::current();
        if ((granted_access & object::access_read) == 0) {
            return -static_cast<int>(SX_EACCES);
        }
        if (caller == nullptr || !cred::acts_as_system(caller->credentials)) {
            return -static_cast<int>(SX_EPERM);
        }
        g_button_watcher = caller->pid;
        g_button_pending = false;
        return 0;
    }
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

void button_pressed() {
    // Nobody to ask, or the last press was never read: whoever watched is not
    // answering, and a button that does nothing is worse than an abrupt
    // power-off.
    if (!watcher_alive() || g_button_pending) {
        shutdown();
    }
    g_button_pending = true;
    console::printf("power: power button pressed; telling pid %u\n", g_button_watcher);
}

void initialize() {
    g_device.read = power_read;
    g_device.can_read = power_can_read;
    g_device.ioctl = power_ioctl;
    g_ready = device::register_node("/dev/power", &g_device, true);
}

bool ready() {
    return g_ready;
}

} // namespace power
