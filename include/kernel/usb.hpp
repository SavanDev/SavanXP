#pragma once

#include "kernel/block.hpp"

namespace inventory {
struct Sink;
}

// The USB stack (kernel/usb/): xHCI controllers, the devices on their ports,
// and the class drivers that turn them into keyboards, pointers and disks.
// Polled, like every driver here. See docs/roadmaps/done/USB_ROADMAP.md.
namespace usb {

// Starts every xHCI controller and enumerates what is plugged in. Runs once
// at boot, after pci:: and before input and storage are probed.
void initialize();
// Registers USB keyboards and pointers with the input layer. Runs after the
// other input drivers, so that they all coexist (docs/kernel/INPUT.md).
void start_input();
// Whether any controller started.
bool ready();
// Drains the controllers' events and lets the class drivers act on them.
// Short work only: it runs from the timer tick.
void poll();
// The long work: devices plugged in or pulled out of a root port or a hub,
// and pulled sticks whose volumes can now be released. Runs where the disk
// writeback queue drains, when a core has nothing else to do. Returns
// whether it did something.
bool service();
// USB sticks as block devices (usb0, usb1...), below the system disk and the
// LiveCD's volume in the probe order.
const block::Driver& storage_driver();
// Every USB device present and its interfaces, as hardware inventory
// records, hubs before what hangs from them (docs/kernel/DEVICE_INVENTORY.md).
// Reads only what attach recorded: no transfer.
void describe(inventory::Sink& sink);

} // namespace usb
