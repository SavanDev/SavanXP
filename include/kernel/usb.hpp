#pragma once

// The USB stack (kernel/usb/): xHCI controllers, the devices on their ports,
// and the class drivers that turn them into keyboards, pointers and disks.
// Polled, like every driver here. See docs/roadmaps/open/USB_ROADMAP.md.
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
void poll();

} // namespace usb
