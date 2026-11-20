#pragma once

#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/iommu_tables.hpp"
#include "kernel/pci.hpp"

/* The IOMMU layer (docs/kernel/IOMMU.md), phases M1 to M3 of
 * docs/roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md: discovery and report, one
 * identity domain for every device, and domains of their own for the drivers
 * that ask. Everything here runs under the kernel lock. */
namespace iommu {

/* Reads the IVRS or the DMAR through acpi::find_table, checks it, reads each
 * unit's capability and status registers without writing them, and logs
 * `iommu: AMD-Vi, 1 unit, ...` or `iommu: none`. Call after acpi::initialize
 * and vm::initialize. */
void discover(const boot::BootInfo& boot_info);

// What discover found, or nullptr when there is no usable IOMMU.
const iommu_tables::Report* report();

/* Phase M2: with the `iommu` boot option, and without `noiommu`, which always
 * wins, turns AMD-Vi translation on with one identity domain. Logs why it
 * stays off otherwise. Call after discover and before any driver starts
 * DMA. */
void start(const boot::BootInfo& boot_info);

// Whether devices' DMA goes through the IOMMU.
bool translating();

// The BSP's tick: drains the units' event logs into the system log every
// 100 ms. Under the kernel lock.
void tick(uint64_t now_ms);

/* Boot option `iommu-selftest`, for QEMU's `edu` device. M2: a DMA through
 * the identity domain must arrive, one outside it must fault, be logged, and
 * leave the device and the kernel working. M3: edu in a domain of its own
 * reaches only what is mapped there, with the access it was given; faults
 * reach the domain's owner; destroying the domain blocks the device. Logs
 * `iommu: selftest passed` or why it failed. Call after pci::initialize. */
void selftest();

// ---- domains of their own (M3) --------------------------------------------

/* A driver's domain. A device in it reaches only the device addresses the
 * driver mapped, which need not be the physical ones. */
struct Domain {
    int index = -1;
    bool valid() const { return index >= 0; }
};

// What a mapping lets the device do.
constexpr unsigned kDeviceReads = 1u << 0;
constexpr unsigned kDeviceWrites = 1u << 1;

/* Called, under the kernel lock and from the BSP's tick, for every I/O page
 * fault a device in the domain raised. `device_address` is zero when the
 * unit did not say which (QEMU). A device that faulted is still in the
 * domain: moving it to the blocked domain, with block(), is the owner's
 * call. */
using FaultHandler = void (*)(void* context, uint16_t requester, uint64_t device_address, bool write);

/* A new domain with nothing mapped. False when translation is off (the
 * driver then hands physical addresses to its device, as before) or no
 * domain is left. */
bool create_domain(FaultHandler handler, void* context, Domain& domain);

/* Every device still in the domain moves to the blocked domain, then the
 * domain and its page tables are freed. `domain` is left invalid. */
void destroy_domain(Domain& domain);

/* Moves the function's whole isolation group (docs/kernel/IOMMU.md) into the
 * domain, or nothing: refused when a member already is in another driver's
 * domain. */
bool attach(const Domain& domain, const pci::DeviceInfo& device);

// Moves the function's isolation group to the blocked domain: no DMA at all.
void block(const pci::DeviceInfo& device);

/* Maps `bytes` of device addresses from `device_address` to the physical
 * pages from `physical`, all page aligned. Refused when any page is mapped
 * already. */
bool map(const Domain& domain, uint64_t device_address, uint64_t physical, uint64_t bytes, unsigned access);

// Unmaps, and returns only once no unit can still use the old translation.
bool unmap(const Domain& domain, uint64_t device_address, uint64_t bytes);

// The requester ID the IOMMU sees for a function on segment 0.
uint16_t requester_id(const pci::DeviceInfo& device);

} // namespace iommu
