#pragma once
#include <stddef.h>
#include <stdint.h>

namespace i386::madt {
constexpr uint32_t kMaxCpus = 32;
struct Topology {
    uint32_t lapic_address;
    uint32_t reported;
    uint32_t count;
    uint32_t ids[kMaxCpus]; // Dense order, with the BSP at index zero.
};

inline uint32_t word(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Parse the v0.4.0 MADT boundary without trusting entry lengths or APIC IDs.
// Native delivery uses xAPIC: IDs above 255 and MMIO above 4 GiB are unsupported.
inline bool parse(const void* table, size_t available, uint32_t bsp, Topology& out) {
    out = {};
    if (!table || available < 44 || bsp > 255) return false;
    const auto* bytes = static_cast<const uint8_t*>(table);
    const uint32_t length = word(bytes + 4);
    if (bytes[0] != 'A' || bytes[1] != 'P' || bytes[2] != 'I' || bytes[3] != 'C' ||
        length < 44 || length > available) return false;
    uint8_t sum = 0;
    for (uint32_t n = 0; n < length; ++n) sum += bytes[n];
    if (sum) return false;
    Topology result = {};
    result.lapic_address = word(bytes + 36);
    result.ids[result.count++] = bsp;
    uint32_t seen[8] = {};
    bool found_bsp = false, override_seen = false;
    for (uint32_t offset = 44; offset < length;) {
        if (length - offset < 2) return false;
        const auto* entry = bytes + offset;
        const uint32_t size = entry[1];
        if (size < 2 || size > length - offset) return false;
        uint32_t id = 0, flags = 0;
        bool cpu = false;
        if (entry[0] == 0) {
            if (size < 8) return false;
            id = entry[3]; flags = word(entry + 4); cpu = true;
        } else if (entry[0] == 9) {
            if (size < 16) return false;
            id = word(entry + 4); flags = word(entry + 8); cpu = true;
        } else if (entry[0] == 5) {
            if (size < 12 || override_seen || word(entry + 8)) return false;
            result.lapic_address = word(entry + 4);
            override_seen = true;
        }
        // Online-capable but disabled CPUs require hotplug, not INIT/SIPI now.
        if (cpu && (flags & 1u)) {
            if (id > 255 || (seen[id / 32] & (1u << (id % 32)))) return false;
            seen[id / 32] |= 1u << (id % 32);
            ++result.reported;
            if (id == bsp) found_bsp = true;
            else if (result.count < kMaxCpus) result.ids[result.count++] = id;
        }
        offset += size;
    }
    if (!found_bsp || !result.lapic_address || (result.lapic_address & 4095u)) return false;
    out = result;
    return true;
}
}
