#include "kernel/amd_vi_tables.hpp"

namespace amd_vi {

namespace {

constexpr size_t kEntriesPerTable = 512;
constexpr unsigned kNextLevelShift = 9;
constexpr uint64_t kNextLevelMask = 7ull << kNextLevelShift;
// The largest leaf used: 2 MiB, at level 2. Level 3 could hold 1 GiB leaves,
// but a 1 GiB run of RAM with nothing else in it is rare and the saving small.
constexpr unsigned kLargestLeafLevel = 2;

constexpr uint64_t kTranslationValid = 1ull << 1;
constexpr unsigned kModeShift = 9;

unsigned entry_shift(unsigned level) {
    return 12u + 9u * (level - 1u);
}

unsigned next_level(uint64_t entry) {
    return static_cast<unsigned>((entry & kNextLevelMask) >> kNextLevelShift);
}

bool map_level(const TableMemory& memory, uint64_t table_physical, unsigned level, uint64_t base, uint64_t end) {
    const unsigned shift = entry_shift(level);
    const uint64_t span = 1ull << shift;
    uint64_t* table = memory.access(table_physical, memory.context);

    for (uint64_t address = base; address < end;) {
        const uint64_t entry_start = address & ~(span - 1);
        const uint64_t entry_end = entry_start + span;
        const uint64_t chunk_end = end < entry_end ? end : entry_end;
        uint64_t& entry = table[(address >> shift) % kEntriesPerTable];

        if ((entry & kPresent) != 0 && next_level(entry) == 0) {
            // A leaf already maps all of this entry, to itself.
        } else if ((entry & kPresent) == 0 && level <= kLargestLeafLevel &&
                   address == entry_start && chunk_end == entry_end) {
            entry = entry_start | kPresent | kReadable | kWritable;
        } else {
            if ((entry & kPresent) == 0) {
                const uint64_t child = memory.allocate(memory.context);
                if (child == 0) {
                    return false;
                }
                entry = child | (static_cast<uint64_t>(level - 1) << kNextLevelShift) |
                    kPresent | kReadable | kWritable;
            }
            if (!map_level(memory, entry & kAddressMask, level - 1, address, chunk_end)) {
                return false;
            }
        }
        address = chunk_end;
    }
    return true;
}

Command command(uint32_t opcode) {
    Command result = {};
    result.words[1] = opcode << 28;
    return result;
}

} // namespace

bool map_identity(const TableMemory& memory, uint64_t root, uint64_t base, uint64_t end) {
    if (base >= end) {
        return true;
    }
    return map_level(memory, root, kLevels, base, end);
}

bool translate(const TableMemory& memory, uint64_t root, uint64_t address, uint64_t& physical) {
    if ((address >> entry_shift(kLevels + 1)) != 0) {
        return false;
    }
    uint64_t table_physical = root;
    for (unsigned level = kLevels; level >= 1; --level) {
        const unsigned shift = entry_shift(level);
        const uint64_t entry = memory.access(table_physical, memory.context)[(address >> shift) % kEntriesPerTable];
        if ((entry & kPresent) == 0 || (entry & (kReadable | kWritable)) != (kReadable | kWritable)) {
            return false;
        }
        const unsigned next = next_level(entry);
        if (next == 0) {
            const uint64_t span = 1ull << shift;
            physical = (entry & kAddressMask & ~(span - 1)) | (address & (span - 1));
            return true;
        }
        if (next != level - 1) {
            return false; // a skipped level: never written by map_identity
        }
        table_physical = entry & kAddressMask;
    }
    return false;
}

void device_table_entry(uint64_t root, uint16_t domain, uint64_t (&entry)[4]) {
    entry[0] = kPresent | kTranslationValid | (static_cast<uint64_t>(kLevels) << kModeShift) |
        (root & kAddressMask) | kReadable | kWritable;
    entry[1] = domain;
    entry[2] = 0;
    entry[3] = 0;
}

Command completion_wait(uint64_t store, uint64_t data) {
    Command result = command(0x1);
    result.words[0] = static_cast<uint32_t>(store & 0xfffffff8u) | 0x1u; // S: store when done
    result.words[1] |= static_cast<uint32_t>((store >> 32) & 0xfffffu);
    result.words[2] = static_cast<uint32_t>(data);
    result.words[3] = static_cast<uint32_t>(data >> 32);
    return result;
}

Command invalidate_device_entry(uint16_t device) {
    Command result = command(0x2);
    result.words[0] = device;
    return result;
}

Command invalidate_domain_pages(uint16_t domain) {
    Command result = command(0x3);
    result.words[1] |= domain;
    // The whole address space (S set with every address bit but the top one
    // set), page directories included (PDE).
    result.words[2] = 0xfffff000u | 0x2u | 0x1u;
    result.words[3] = 0x7fffffffu;
    return result;
}

Command invalidate_all() {
    return command(0x8);
}

Event decode_event(const uint32_t (&words)[4]) {
    Event event = {};
    event.code = static_cast<uint8_t>(words[1] >> 28);
    event.device = static_cast<uint16_t>(words[0] & 0xffffu);
    event.domain = static_cast<uint16_t>(words[1] & 0xffffu);
    event.flags = static_cast<uint16_t>((words[1] >> 16) & 0xfffu);
    event.address = words[2] | (static_cast<uint64_t>(words[3]) << 32);
    return event;
}

const char* event_name(uint8_t code) {
    switch (code) {
    case 1: return "illegal device table entry";
    case kEventIoPageFault: return "I/O page fault";
    case 3: return "device table hardware error";
    case 4: return "page table hardware error";
    case 5: return "illegal command";
    case 6: return "command hardware error";
    case 7: return "IOTLB invalidation timeout";
    case 8: return "invalid device request";
    default: return "event";
    }
}

} // namespace amd_vi
