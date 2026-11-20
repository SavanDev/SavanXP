#pragma once

#include <stddef.h>
#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/iommu_tables.hpp"

/* AMD-Vi with one shared identity domain: phase M2 of
 * docs/roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md (docs/kernel/IOMMU.md).
 * Every device translates through the same tables, which map usable memory
 * and the firmware's reserved ranges to themselves, so drivers keep handing
 * physical addresses to their devices. A DMA anywhere else faults and is
 * logged. Interrupts are not remapped (M4). */
namespace amd_vi {

/* Takes every unit in `report` over from the firmware and turns translation
 * on. `registers[i]` is unit i's register window, 16 KiB, already mapped.
 * False, with the reason logged and every unit left off, when anything
 * fails. Call once, before drivers start DMA. */
bool start(const iommu_tables::Report& report, volatile uint8_t* const* registers, const boot::BootInfo& boot_info);

bool running();

/* Copies what each unit wrote to its event log into the system log, and
 * restarts a log that overflowed. Called from the BSP's tick, under the
 * kernel lock, and by the self-test. */
void drain_events();

// I/O page faults seen since start, and the last one.
uint64_t fault_count();
bool last_fault(uint16_t& device, uint64_t& address);
// Times an event log overflowed and was restarted.
uint64_t overflow_count();

// Whether the identity domain lets a device reach `address`.
bool identity_maps(uint64_t address);

} // namespace amd_vi
