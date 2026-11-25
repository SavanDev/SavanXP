#pragma once

// The pure half of the hardware inventory (docs/kernel/DEVICE_INVENTORY.md):
// what a set of class codes is, what a PCI function's raw BARs say, which
// bridge a function hangs from, and where the records go. No hardware access
// and no kernel state, so the host tests compile it as the kernel does.

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"

namespace inventory {

// enum sxn_device_class for a PCI function's class, subclass and
// programming interface. UNKNOWN for what has no group (0xff, 0x00 other
// than an old VGA, communication controllers).
uint32_t classify_pci(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

// enum sxn_device_class for a USB interface's class triple: HID boot
// keyboards and mice, hubs, mass storage, audio, communication.
uint32_t classify_usb_interface(uint8_t class_code, uint8_t subclass, uint8_t protocol);

// The resources a PCI function's BARs describe, from the values the boot
// scan read: kind, base and flags, never the size (sizing a BAR writes to
// the device). A header type 0 has six BARs, a bridge (1) two, CardBus (2)
// none here. A BAR that reads 0, or whose base is 0, is unassigned and
// left out; the upper half of a 64-bit BAR is folded into its lower one.
// Returns how many were written, at most `capacity`.
size_t decode_bars(const uint32_t bar[6], uint8_t header_type, sxn_device_resource* out, size_t capacity);

// One PCI function as the tree needs it.
struct PciNode {
    uint8_t bus;
    uint8_t header_type;   // the multifunction bit is ignored
    uint8_t secondary_bus; // bridges (header type 1 or 2): the bus behind them
};

// At most this many functions are ordered; the PCI table holds as many.
constexpr size_t kMaxPciNodes = 256;

// The bridge `index` hangs from: the bridge whose secondary bus is its bus,
// or -1 when it sits on bus 0 or no bridge leads to its bus.
long pci_parent(const PciNode* nodes, size_t count, size_t index);

// The order to list the functions in, each bridge before what is behind it,
// and each function's parent in that listing (-1: the computer). Functions
// caught in a loop of bridges (a secondary bus leading back to itself) are
// listed last with the computer as their parent, so a parent always comes
// first. `order` and `parent` take `count` entries; count is clamped to
// kMaxPciNodes. Returns how many were ordered.
size_t order_pci(const PciNode* nodes, size_t count, size_t* order, long* parent);

// Whether `claimant` may take a PCI function whose record names `holder` as
// its driver and is in `state` (enum sxn_device_state). A function nobody
// holds, or one its driver gave back (UNBOUND), is free; a driver may take
// its own function again; any other holder keeps it, including one that
// FAILED, since a driver that gave up may still own the device's DMA. An
// empty or missing claimant is never granted.
bool claim_allowed(const char* holder, uint32_t state, const char* claimant);

// NUL-terminated, cut to fit.
void copy_text(char* out, size_t capacity, const char* text);
// "PCI 00:1f.2".
void format_pci_location(char* out, size_t capacity, uint8_t bus, uint8_t device, uint8_t function);

// Where a snapshot's records go. take() receives each one with its position
// and returns false when it could not store it (a bad user buffer); the
// sink then stops calling it but keeps counting, so `total` is always how
// many instances there are.
struct Sink {
    bool (*take)(const sxn_device_info& record, size_t index, void* context);
    void* context;
    uint32_t generation;
    size_t total;
    bool failed;
};

void emit(Sink& sink, sxn_device_info& record);

} // namespace inventory
