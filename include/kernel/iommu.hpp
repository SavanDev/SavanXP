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

} // namespace iommu
