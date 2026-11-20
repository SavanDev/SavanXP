#pragma once

#include <stddef.h>
#include <stdint.h>

#include "boot/boot_info.hpp"
#include "kernel/iommu_tables.hpp"

/* AMD-Vi: phases M2 to M4 of docs/roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md
 * (docs/kernel/IOMMU.md). Every device starts in one shared identity domain,
 * which maps usable memory and the firmware's reserved ranges to themselves,
 * so drivers keep handing physical addresses to their devices. A driver may
 * move its device's isolation group into a domain of its own, where only what
 * it maps is reachable; a group leaving one goes to the blocked domain, which
 * maps nothing. A DMA anywhere else faults, is logged, and is handed to the
 * domain's owner. With interrupt remapping on (M4), a device's interrupts
 * reach a core only through the entries the kernel gave it.
 *
 * Everything here runs under the kernel lock. */
namespace amd_vi {

// The I/O APICs whose interrupts the units remap, each with the requester ID
// the IVRS gives it.
constexpr size_t kMaxIoApics = 8;
struct InterruptSources {
    size_t ioapic_count;
    uint8_t ioapic_id[kMaxIoApics];
    uint16_t ioapic_requester[kMaxIoApics];
};

/* Takes every unit in `report` over from the firmware and turns translation
 * on. `registers[i]` is unit i's register window, 16 KiB, already mapped.
 * With `interrupts`, interrupt remapping too: every device starts with no
 * entry at all, so none of its interrupts arrive until allow_interrupt.
 * False, with the reason logged and every unit left off, when anything
 * fails. Call once, before drivers start DMA or interrupts are routed. */
bool start(const iommu_tables::Report& report, volatile uint8_t* const* registers, const boot::BootInfo& boot_info,
    const InterruptSources* interrupts);

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

// ---- per-device domains (M3) ----------------------------------------------

// Domains are numbered by slot; these two always exist while running.
constexpr int kIdentityDomain = 0;
constexpr int kBlockedDomain = 1;
constexpr size_t kMaxDomains = 32;

/* Called from the event log drain for every I/O page fault a device in the
 * domain raised, after it was logged. `address` is the device address, zero
 * when the unit did not say (QEMU). */
using FaultHandler = void (*)(void* context, uint16_t requester, uint64_t address, bool write);

// A new empty domain; -1 when translation is off or none is left.
int create_domain(FaultHandler handler, void* context);
// Moves every device still in `domain` to the blocked domain, then frees it.
void destroy_domain(int domain);
/* Moves `requester`'s whole isolation group into `domain`. Refused when a
 * member is in another domain of its own (identity and blocked are left
 * freely) or the group cannot be worked out. */
bool attach(int domain, uint16_t requester);
// Moves `requester`'s isolation group to the blocked domain.
void block(uint16_t requester);
// Which domain `requester` is in.
int domain_of(uint16_t requester);
// The domain ID the units see for a slot.
uint16_t domain_id(int domain);

/* Maps device addresses to physical pages in a domain of its own (not
 * identity or blocked); `access` is kReadable and/or kWritable. Refused when
 * any page is already mapped. */
bool map(int domain, uint64_t device_address, uint64_t physical, uint64_t bytes, uint64_t access);
// Unmaps, and returns once no unit can use the old translations.
bool unmap(int domain, uint64_t device_address, uint64_t bytes);
// Page tables held by every domain together.
uint64_t table_pages();

// ---- interrupt remapping (M4) ---------------------------------------------

bool remapping_interrupts();

/* Lets `requester`'s isolation group raise `vector`, delivered to the local
 * APIC `apic_id`; again with another APIC moves it. True without doing
 * anything when remapping is off. False when the destination cannot be
 * named by an entry (above 254) or memory ran out. */
bool allow_interrupt(uint16_t requester, uint8_t vector, uint32_t apic_id);
// The same for an I/O APIC, by its MADT ID.
bool allow_ioapic_interrupt(uint8_t ioapic_id, uint8_t vector, uint32_t apic_id);

// Interrupts the units refused and reported (QEMU refuses without reporting).
uint64_t refused_interrupt_count();

} // namespace amd_vi
