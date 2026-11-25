#pragma once

// The start and stop self-test every driver written to
// docs/kernel/DRIVER_LIFECYCLE.md shares: a failure injected at each stage of
// its start, then starts and stops, each checked for pages, claims, state
// and interrupt handlers left behind.

#include <stddef.h>
#include <stdint.h>

namespace pci {
struct DeviceInfo;
}

namespace lifecycle {

// What a driver hands the self-test: its own start and stop, unchanged.
// Every call but find names the device, so a driver with several instances
// (virtio-input's tablet and keyboard) is tested one instance at a time.
struct Subject {
    // The name the boot option, the failure injection and the inventory use.
    const char* driver;
    // How many stages its start has, for failure injection.
    unsigned stages;
    // The nth device (from 0) it could take, without taking it; false past
    // the last. Every one found is tested.
    bool (*find)(size_t nth, pci::DeviceInfo& device);
    bool (*start)(const pci::DeviceInfo& device);
    // Beyond start, as far as a class core would take it (the link up, a
    // stream playing); nullptr when start is as far as it goes.
    bool (*bring_up)(const pci::DeviceInfo& device);
    bool (*stop)(const pci::DeviceInfo& device);
    // True when nothing of the driver's state for this device is left: its
    // record of what it acquired empty, no domain, not started.
    bool (*idle)(const pci::DeviceInfo& device);
    // The vector its interrupt is on while started, 0 when it polls;
    // nullptr when the driver never takes one.
    uint8_t (*vector)(const pci::DeviceInfo& device);
};

// Boot option `lifecycle-selftest=<driver>[,<driver>...]`, run right after
// the inventory, before any class core takes a device. Each named driver
// must have at least one device to test, and every one must pass; the log
// says `lifecycle: selftest passed`, or `lifecycle: selftest failed: ...`
// with the driver and the step. Every device is left free for its class
// core to take as usual.
void run_boot_selftest(const char* names, size_t length);

} // namespace lifecycle
