#pragma once

// The hardware inventory (docs/kernel/DEVICE_INVENTORY.md): one list of the
// computer, its PCI functions, the devices drivers find off any bus, and the
// USB devices and interfaces, with the driver using each one and its state.
// Drivers report what they took; listing never touches the hardware.
//
// Everything here runs under the kernel lock, as the drivers and the system
// calls that reach it do.

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "kernel/inventory_rules.hpp"

namespace pci {
struct DeviceInfo;
}

namespace inventory {

// Gives the computer and every PCI function of the boot scan its id. Runs
// once, right after pci::initialize() and before any driver starts.
void initialize();

// A new id for an instance found later (a USB device, an interface). Ids
// are never reused while the system runs.
uint32_t allocate_id();
// The computer's id: the parent of whatever hangs from no bus.
uint32_t root_id();
// Something listed changed: an instance came, went or changed state.
void changed();

// A driver takes the PCI function to start it, before it touches anything
// of the function (sizing a BAR with pci::bar_info() included). Ownership
// is exclusive: false when another driver holds it (claim_allowed() in
// inventory_rules.hpp), and the driver leaves the function alone. On true,
// until pci_started() it is listed as FAILED (START_FAILED), so every early
// return of a start path reads as the failure it is without the driver
// saying so. A class other than SXN_DEVICE_CLASS_UNKNOWN replaces the one
// its class codes give (virtio-rng's class code says nothing). The calls
// below that report on a function are for its holder only.
[[nodiscard]] bool pci_claim(const pci::DeviceInfo& device, const char* driver,
    uint32_t device_class = SXN_DEVICE_CLASS_UNKNOWN);
// The holder gives the function back, quiet, with nothing of it in use: it
// is listed as UNBOUND (NOT_CLAIMED) and any driver may take it. False, and
// nothing changes, when `driver` is not the holder.
bool pci_release(const pci::DeviceInfo& device, const char* driver);
// Who holds the function and in what state; false when it was not scanned.
// `driver` is empty when nobody does.
bool pci_holder(const pci::DeviceInfo& device, const char*& driver, uint32_t& state, uint32_t& reason);
void pci_started(const pci::DeviceInfo& device);
// Taken, then lost: a NIC that does not come up when asked.
void pci_failed(const pci::DeviceInfo& device);
// Taken, started, and nothing on it the driver supports: released.
void pci_unsupported(const pci::DeviceInfo& device);
// What the hardware calls itself (an NVMe model, the codec an HD Audio
// controller carries).
void pci_name(const pci::DeviceInfo& device, const char* name);
// Every unclaimed function of this class and subclass was kept from its
// drivers by a boot option (`nousb`).
void pci_boot_option(uint8_t class_code, uint8_t subclass);
// The id of the function at bus:slot.function; 0 when it was not scanned.
uint32_t pci_id(uint8_t bus, uint8_t slot, uint8_t function);

// A device a driver found without a bus to list it: the PS/2 keyboard and
// mouse, the boot framebuffer, the LiveCD's RAM disk. Its parent is the
// computer.
struct PlatformDevice {
    uint32_t device_class;
    uint32_t state;
    uint32_t reason;
    uint32_t flags;
    const char* name;
    const char* driver;
    const char* location;
    sxn_device_resource resources[4];
    uint32_t resource_count;
};
// Its id, or 0 when the table is full.
uint32_t platform_add(const PlatformDevice& device);

// Every instance, parents first, into `sink`. Returns sink.total.
size_t snapshot(Sink& sink);

// Boot option `inventory-selftest`, once every driver has started: a held
// function refuses another driver and stays as it was, a free one is taken,
// refused to a second driver, taken again by its own and given back. Only
// the records change; no device is touched. Logs `inventory: selftest
// passed` or the step that failed.
void selftest();

} // namespace inventory
