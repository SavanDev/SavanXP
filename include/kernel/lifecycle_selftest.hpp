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
struct Subject {
    const char* driver;
    // How many stages its start has, for failure injection.
    unsigned stages;
    // The first device it could take, without taking it.
    bool (*find)(pci::DeviceInfo& device);
    bool (*start)(const pci::DeviceInfo& device);
    // Beyond start, as far as a class core would take it (the link up, a
    // stream playing); nullptr when start is as far as it goes.
    bool (*bring_up)();
    bool (*stop)();
    // True when nothing of the driver's own state is left: its record of
    // what it acquired empty, no domain, not started.
    bool (*idle)();
    // The vector its interrupt is on while started, 0 when it polls.
    uint8_t (*vector)();
};

// Boot option `lifecycle-selftest=<driver>[,<driver>...]`, run before the
// class cores take any device. Each named driver must have a device to test
// and pass; the log says `lifecycle: selftest passed`, or `lifecycle:
// selftest failed: ...` with the driver and the step. Every device is left
// free for its class core to take as usual.
void run_boot_selftest(const char* names, size_t length);

} // namespace lifecycle
