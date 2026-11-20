#pragma once

#include "boot/boot_info.hpp"
#include "kernel/iommu_tables.hpp"

/* The IOMMU layer (docs/kernel/IOMMU.md). Phase M1 of
 * docs/roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md: discovery and report only.
 * Nothing here changes what the hardware does. */
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

/* Boot option `iommu-selftest`, for QEMU's `edu` device: a DMA through the
 * identity domain must arrive, one outside it must fault, be logged, and
 * leave the device and the kernel working. Logs `iommu: selftest passed` or
 * why it failed. Call after pci::initialize. */
void selftest();

} // namespace iommu
