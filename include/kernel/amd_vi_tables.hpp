#pragma once

#include <stddef.h>
#include <stdint.h>

/* The in-memory formats AMD-Vi reads and writes (AMD I/O Virtualization
 * Technology specification, 48882): host I/O page tables, device table
 * entries, interrupt remapping tables, commands and event log entries.
 * Phases M2 to M4 of
 * docs/roadmaps/open/DRIVER_FRAMEWORK_ROADMAP.md; docs/kernel/IOMMU.md.
 *
 * Nothing here touches the unit or knows where memory comes from, so the
 * host tests build the same tables and walk them back. */
namespace amd_vi {

// Page table and device table entry bits shared by both.
constexpr uint64_t kPresent = 1ull << 0;
constexpr uint64_t kReadable = 1ull << 61;
constexpr uint64_t kWritable = 1ull << 62;
constexpr uint64_t kAddressMask = 0x000ffffffffff000ull;
// Four levels translate 48 bits, which every AMD-Vi unit supports (EFR HATS
// starts there) and which holds every physical address an x86 board has.
constexpr unsigned kLevels = 4;
constexpr uint64_t kPageSize = 4096;

// Where page tables come from. `allocate` returns the physical address of a
// zeroed 4 KiB table, or 0 when memory ran out; `access` turns a physical
// address the allocator returned into a pointer; `release` gives a table
// back (free_tables only).
struct TableMemory {
    uint64_t (*allocate)(void* context);
    uint64_t* (*access)(uint64_t physical, void* context);
    void (*release)(uint64_t physical, void* context);
    void* context;
};

/* Maps [base, end), both page aligned, to itself under the four-level table
 * at `root`: 2 MiB leaves where a whole aligned 2 MiB lies inside, 4 KiB
 * leaves elsewhere, readable and writable. Ranges may overlap or touch; what
 * is already mapped stays. False only when `allocate` failed, and what was
 * mapped before stays mapped. */
bool map_identity(const TableMemory& memory, uint64_t root, uint64_t base, uint64_t end);

/* Maps the `bytes` of device addresses from `device_address` to the
 * physical pages from `physical`, 4 KiB leaves only, with `access`
 * (kReadable: the device may read, kWritable: it may write, or both). All
 * three page aligned, `bytes` non-zero, the range below 2^48. Refused, with
 * nothing changed, when any page of the range is already mapped. False as
 * well when `allocate` failed; the pages mapped by this call are then
 * unmapped again (tables it allocated stay, empty). */
bool map_pages(const TableMemory& memory, uint64_t root, uint64_t device_address, uint64_t physical,
    uint64_t bytes, uint64_t access);

/* Clears the 4 KiB leaves of [device_address, device_address + bytes);
 * returns how many were mapped. Tables stay until free_tables. The unit may
 * still use the old translations until the domain's pages are invalidated. */
uint64_t unmap_pages(const TableMemory& memory, uint64_t root, uint64_t device_address, uint64_t bytes);

// Releases every table under `root` and `root` itself; never a leaf's page.
void free_tables(const TableMemory& memory, uint64_t root);

/* The walk the unit does for an access at `address` that needs `access`
 * (kReadable, kWritable or both): true with the physical address when it
 * would be allowed, false when it would fault. */
bool translate(const TableMemory& memory, uint64_t root, uint64_t address, uint64_t& physical,
    uint64_t access = kReadable | kWritable);

/* A valid entry that translates through the table at `root` in `domain`.
 * Only the first two quadwords: the interrupt half is set apart, by
 * interrupt_remapping. */
void device_table_entry(uint64_t root, uint16_t domain, uint64_t (&entry)[4]);

/* Interrupt remapping (M4). A device's interrupts are looked up in a table of
 * 32-bit entries indexed by bits 10:0 of the MSI data. The kernel sends every
 * interrupt with fixed delivery, so the index is the vector: a table of 256
 * entries, one per vector, leaves the messages devices and I/O APICs send
 * exactly as they were. */
constexpr size_t kInterruptTableEntries = 256;
constexpr uint64_t kInterruptTableBytes = kInterruptTableEntries * sizeof(uint32_t);

/* The third quadword of a device table entry: interrupts remapped through
 * the table at `table` (64-byte aligned), with the INIT, ExtINT, NMI, LINT0
 * and LINT1 pass-through bits the IVRS's DTE setting byte asks for. */
uint64_t interrupt_remapping(uint64_t table, uint8_t dte_setting);

// A remapping table entry: fixed delivery of `vector` to the local APIC
// `destination`, physical mode.
uint32_t interrupt_table_entry(uint8_t vector, uint8_t destination);

struct Command {
    uint32_t words[4];
};

// Stores `data` at `store` (8-byte aligned) once every earlier command is done.
Command completion_wait(uint64_t store, uint64_t data);
Command invalidate_device_entry(uint16_t device);
// Every page and page directory cached for `domain`.
Command invalidate_domain_pages(uint16_t domain);
// Every cache the unit has; only when EFR advertises IASup.
Command invalidate_all();
// What the unit cached of `device`'s interrupt remapping table.
Command invalidate_interrupt_table(uint16_t device);

constexpr uint8_t kEventIllegalDeviceTableEntry = 1;
constexpr uint8_t kEventIoPageFault = 2;
// Flags of an I/O page fault: the access was a write; the page was present
// (so the fault is a permission one).
constexpr uint16_t kFaultWrite = 1u << 5;
constexpr uint16_t kFaultPresent = 1u << 4;
// The faulting request was an interrupt. QEMU sets it on DMA faults as well.
constexpr uint16_t kFaultInterrupt = 1u << 3;
// Where interrupt messages are written.
constexpr uint64_t kInterruptWindowFirst = 0xfee00000;
constexpr uint64_t kInterruptWindowLast = 0xfeefffff;

struct Event {
    uint8_t code;     // zero: the unit has not finished writing the entry
    uint16_t device;  // requester ID
    uint16_t domain;  // or the PASID's low bits, by event
    uint16_t flags;
    uint64_t address;
};

Event decode_event(const uint32_t (&words)[4]);

// An I/O page fault event about an interrupt the unit refused: the interrupt
// flag and an address in the interrupt window.
bool is_refused_interrupt(const Event& event);
const char* event_name(uint8_t code);

} // namespace amd_vi
