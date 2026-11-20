#pragma once

#include <stddef.h>
#include <stdint.h>

/* The in-memory formats AMD-Vi reads and writes (AMD I/O Virtualization
 * Technology specification, 48882): host I/O page tables, device table
 * entries, commands and event log entries. Phase M2 of
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
// address the allocator returned into a pointer.
struct TableMemory {
    uint64_t (*allocate)(void* context);
    uint64_t* (*access)(uint64_t physical, void* context);
    void* context;
};

/* Maps [base, end), both page aligned, to itself under the four-level table
 * at `root`: 2 MiB leaves where a whole aligned 2 MiB lies inside, 4 KiB
 * leaves elsewhere, readable and writable. Ranges may overlap or touch; what
 * is already mapped stays. False only when `allocate` failed, and what was
 * mapped before stays mapped. */
bool map_identity(const TableMemory& memory, uint64_t root, uint64_t base, uint64_t end);

/* The walk the unit does for a read and write at `address`: true with the
 * physical address when it would be allowed, false when it would fault. */
bool translate(const TableMemory& memory, uint64_t root, uint64_t address, uint64_t& physical);

/* A valid entry that translates through the table at `root` in `domain`.
 * Interrupts are left untranslated (IV clear) until M4. */
void device_table_entry(uint64_t root, uint16_t domain, uint64_t (&entry)[4]);

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

constexpr uint8_t kEventIllegalDeviceTableEntry = 1;
constexpr uint8_t kEventIoPageFault = 2;
// Flags of an I/O page fault: the access was a write; the page was present
// (so the fault is a permission one).
constexpr uint16_t kFaultWrite = 1u << 5;
constexpr uint16_t kFaultPresent = 1u << 4;

struct Event {
    uint8_t code;     // zero: the unit has not finished writing the entry
    uint16_t device;  // requester ID
    uint16_t domain;  // or the PASID's low bits, by event
    uint16_t flags;
    uint64_t address;
};

Event decode_event(const uint32_t (&words)[4]);
const char* event_name(uint8_t code);

} // namespace amd_vi
