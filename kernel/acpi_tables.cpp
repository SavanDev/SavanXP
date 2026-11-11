#include "kernel/acpi_tables.hpp"

#include "kernel/string.hpp"

namespace acpi_tables {

namespace {

constexpr uint8_t kAmlNameOp = 0x08;
constexpr uint8_t kAmlPackageOp = 0x12;
constexpr uint8_t kAmlBytePrefix = 0x0a;

template <typename T>
const T* map_as(const Reader& reader, uint64_t physical, size_t length) {
    if (reader.map == nullptr || physical == 0) {
        return nullptr;
    }
    return static_cast<const T*>(reader.map(physical, length, reader.context));
}

} // namespace

bool checksum_ok(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return sum == 0;
}

bool rsdp_valid(const Rsdp& rsdp) {
    return memcmp(rsdp.signature, "RSD PTR ", 8) == 0 && checksum_ok(&rsdp, kRsdpV1Length);
}

const SdtHeader* map_table(const Reader& reader, uint64_t physical, uint32_t max_length) {
    const SdtHeader* header = map_as<SdtHeader>(reader, physical, sizeof(SdtHeader));
    if (header == nullptr) {
        return nullptr;
    }
    const uint32_t length = header->length;
    if (length < sizeof(SdtHeader) || length > max_length) {
        return nullptr;
    }
    // Asked for again at its full length: the header being readable says
    // nothing about the bytes after it.
    return map_as<SdtHeader>(reader, physical, length);
}

bool table_checksum_ok(const SdtHeader& table) {
    return checksum_ok(&table, table.length);
}

const SdtHeader* find_table(const Reader& reader, const Rsdp& rsdp, const char (&signature)[5]) {
    // The XSDT fields exist only from revision 2 on, and only when the
    // structure says it is long enough to hold them. A wrong extended
    // checksum sends us to the RSDT, unless there is none to go to.
    const bool has_xsdt = rsdp.revision >= 2 && rsdp.length >= sizeof(Rsdp) && rsdp.xsdt_address != 0;
    const bool use_xsdt = has_xsdt && (checksum_ok(&rsdp, sizeof(Rsdp)) || rsdp.rsdt_address == 0);

    const SdtHeader* root = map_table(reader, use_xsdt ? rsdp.xsdt_address : rsdp.rsdt_address, kMaxRootLength);
    if (root == nullptr || memcmp(root->signature, use_xsdt ? "XSDT" : "RSDT", 4) != 0) {
        return nullptr;
    }

    const size_t entry_size = use_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    const size_t entry_count = (root->length - sizeof(SdtHeader)) / entry_size;
    const uint8_t* entries = reinterpret_cast<const uint8_t*>(root) + sizeof(SdtHeader);

    for (size_t i = 0; i < entry_count; ++i) {
        uint64_t physical = 0;
        memcpy(&physical, entries + i * entry_size, entry_size);
        const SdtHeader* table = map_table(reader, physical);
        if (table != nullptr && memcmp(table->signature, signature, 4) == 0) {
            return table;
        }
    }
    return nullptr;
}

const MadtEntryHeader* madt_next_entry(const SdtHeader& madt, const MadtEntryHeader* previous) {
    if (madt.length < kMadtFixedLength) {
        return nullptr;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&madt);
    const uint8_t* end = base + madt.length;
    // `previous` came from here, so its length was already checked to lie
    // inside the table.
    const uint8_t* next = previous == nullptr
        ? base + kMadtFixedLength
        : reinterpret_cast<const uint8_t*>(previous) + previous->length;

    if (static_cast<size_t>(end - next) < sizeof(MadtEntryHeader)) {
        return nullptr;
    }
    const auto* entry = reinterpret_cast<const MadtEntryHeader*>(next);
    if (entry->length < sizeof(MadtEntryHeader) || entry->length > static_cast<size_t>(end - next)) {
        return nullptr;
    }
    return entry;
}

bool find_s5(const SdtHeader& dsdt, uint8_t& slp_typ_a, uint8_t& slp_typ_b) {
    if (dsdt.length <= sizeof(SdtHeader)) {
        return false;
    }
    const uint8_t* body = reinterpret_cast<const uint8_t*>(&dsdt) + sizeof(SdtHeader);
    const size_t body_length = dsdt.length - sizeof(SdtHeader);

    for (size_t i = 0; i + 4 <= body_length; ++i) {
        if (memcmp(body + i, "_S5_", 4) != 0) {
            continue;
        }
        // A name definition: NameOp, optionally with the root prefix between
        // it and the name.
        const bool preceded_by_name_op =
            (i >= 1 && body[i - 1] == kAmlNameOp) ||
            (i >= 2 && body[i - 2] == kAmlNameOp && body[i - 1] == '\\');
        if (!preceded_by_name_op) {
            continue;
        }

        // From here every step checks the cursor before it reads: the
        // package is firmware's word for how long it is, not ours.
        size_t at = i + 4;
        if (at >= body_length || body[at] != kAmlPackageOp) {
            continue;
        }
        at += 1; // PackageOp
        if (at >= body_length) {
            continue;
        }
        at += static_cast<size_t>((body[at] & 0xc0u) >> 6) + 1; // PkgLength: lead byte and its extra bytes
        at += 1; // NumElements

        if (at < body_length && body[at] == kAmlBytePrefix) {
            at += 1;
        }
        if (at >= body_length) {
            continue;
        }
        const uint8_t type_a = static_cast<uint8_t>(body[at] & 0x07u);
        at += 1;

        if (at < body_length && body[at] == kAmlBytePrefix) {
            at += 1;
        }
        if (at >= body_length) {
            continue;
        }
        slp_typ_a = type_a;
        slp_typ_b = static_cast<uint8_t>(body[at] & 0x07u);
        return true;
    }
    return false;
}

} // namespace acpi_tables
