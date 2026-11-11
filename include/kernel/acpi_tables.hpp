#pragma once

#include <stddef.h>
#include <stdint.h>

/* Bounded readers for the ACPI tables the kernel parses by hand
 * (docs/kernel/KERNEL_SECURITY.md, "ACPI table walk").
 *
 * Firmware is a hostile input here: every length and every physical address
 * in a table is checked before it is followed. Nothing in this file touches
 * the machine, so the same code runs in the host tests against tables built
 * to be wrong. */
namespace acpi_tables {

struct [[gnu::packed]] Rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    // Revision 2 and later only:
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] SdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] MadtEntryHeader {
    uint8_t type;
    uint8_t length;
};

// The first 20 bytes, which every revision checksums.
constexpr size_t kRsdpV1Length = 20;
// No table the kernel reads is anywhere near this; a DSDT is the largest and
// runs to a few hundred KiB. A length above it is a corrupt header.
constexpr uint32_t kMaxTableLength = 16u << 20;
// The RSDT or XSDT: room for eight thousand tables.
constexpr uint32_t kMaxRootLength = 64u << 10;
// What the MADT carries before its entries: the local APIC address and flags.
constexpr size_t kMadtFixedLength = sizeof(SdtHeader) + 8;

/* Turns `length` bytes of physical memory at `physical` into a pointer, or
 * returns nullptr when that range cannot be read. Every dereference below
 * goes through one. */
struct Reader {
    const void* (*map)(uint64_t physical, size_t length, void* context);
    void* context;
};

bool checksum_ok(const void* data, size_t length);

// Signature and the 20-byte checksum. `rsdp` must be readable for
// sizeof(Rsdp) bytes.
bool rsdp_valid(const Rsdp& rsdp);

/* The whole table at `physical`, readable for its own `length`, or nullptr
 * when the header cannot be read or the length is below a header or above
 * `max_length`. The checksum is not part of the answer: firmware that ships a
 * table with a wrong one exists, and refusing its FADT would cost the machine
 * its power button for no gain, because a checksum is not a defense. Callers
 * report it (table_checksum_ok) and go on. */
const SdtHeader* map_table(const Reader& reader, uint64_t physical, uint32_t max_length = kMaxTableLength);

bool table_checksum_ok(const SdtHeader& table);

/* Walks the XSDT, or the RSDT when there is no usable XSDT, for the first
 * table with `signature`. Entries that are zero, unreadable or malformed are
 * skipped, not followed. */
const SdtHeader* find_table(const Reader& reader, const Rsdp& rsdp, const char (&signature)[5]);

/* The MADT entry after `previous`, or the first one when `previous` is
 * nullptr; nullptr at the end. An entry is returned only when all of its
 * declared length lies inside the table, and a zero or one-byte length ends
 * the walk. The caller still compares the length with the entry it expects. */
const MadtEntryHeader* madt_next_entry(const SdtHeader& madt, const MadtEntryHeader* previous);

/* Finds the \_S5_ package in a DSDT without interpreting AML and decodes its
 * first two elements, the sleep types for PM1a and PM1b. False when there is
 * none or it runs past the end of the table. */
bool find_s5(const SdtHeader& dsdt, uint8_t& slp_typ_a, uint8_t& slp_typ_b);

} // namespace acpi_tables
