#include "kernel/partition.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/string.hpp"
#include "shared/crc32.h"

namespace {

// Below ata (100) and ramdisk (10): probe_all sorts by descending priority, so
// this enumerate runs when no physical disk is left to appear, and can read
// every disk's table.
constexpr int kDriverPriority = -1000;

// Four primaries per disk cover the installer's disk (ESP + root) and the
// typical USB stick. When it fills up the rest stay unregistered and the
// system keeps working.
constexpr size_t kMaxSlices = 8;

// Disk records are indexed by block device index.
constexpr size_t kMaxDisks = 32;

constexpr size_t kMbrTableOffset = 446;
constexpr size_t kMbrEntrySize = 16;
constexpr size_t kMbrEntryCount = 4;
constexpr uint8_t kMbrTypeEmpty = 0x00;
constexpr uint8_t kMbrTypeExtendedChs = 0x05;
constexpr uint8_t kMbrTypeExtendedLba = 0x0f;
constexpr uint8_t kMbrTypeExtendedLinux = 0x85;
constexpr uint8_t kMbrTypeGptProtective = 0xee;
constexpr uint8_t kMbrTypeEsp = 0xef;

constexpr uint32_t kGptHeaderLba = 1;
constexpr uint32_t kGptMinHeaderSize = 92;
// Partitions are registered from the first 128 entries, the size every tool
// writes. The CRC still covers the whole array the header declares, up to
// kGptMaxArrayBytes.
constexpr size_t kGptMaxEntries = 128;
constexpr size_t kGptMaxArrayBytes = 32 * 1024;

// C12A7328-F81F-11D2-BA4B-00A0C93EC93B as stored on disk: the first three
// fields little-endian, the last 8 bytes as they are.
const uint8_t kEspTypeGuid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

struct Slice {
    bool present;
    size_t device_index;
    size_t parent_index;
    partition::Info info;
};

Slice g_slices[kMaxSlices] = {};
size_t g_slice_count = 0;

partition::DiskInfo g_disks[kMaxDisks] = {};

// block::DeviceInfo keeps the name by pointer, it does not copy it: the
// buffer has to outlive enumerate.
char g_names[kMaxSlices][24] = {};

// The entry array of the GPT being read. Static: 32 KiB does not belong on a
// kernel stack, and enumerate runs once at a time under the kernel lock.
uint8_t g_entries[kGptMaxArrayBytes];

uint16_t load_le16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}

uint32_t load_le32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t load_le64(const uint8_t* data) {
    return static_cast<uint64_t>(load_le32(data)) |
        (static_cast<uint64_t>(load_le32(data + 4)) << 32);
}

bool is_extended(uint8_t type) {
    return type == kMbrTypeExtendedChs || type == kMbrTypeExtendedLba ||
        type == kMbrTypeExtendedLinux;
}

// "<parent>p<n>", n 1-based as in sda1. Built by hand: the kernel has no
// snprintf.
void build_name(char* out, size_t capacity, const char* parent, size_t ordinal) {
    size_t cursor = 0;
    if (parent != nullptr) {
        while (parent[cursor] != 0 && cursor + 5 < capacity) {
            out[cursor] = parent[cursor];
            ++cursor;
        }
    }
    out[cursor++] = 'p';
    if (ordinal >= 100) {
        out[cursor++] = static_cast<char>('0' + ((ordinal / 100) % 10));
    }
    if (ordinal >= 10) {
        out[cursor++] = static_cast<char>('0' + ((ordinal / 10) % 10));
    }
    out[cursor++] = static_cast<char>('0' + (ordinal % 10));
    out[cursor] = 0;
}

// UTF-16LE GPT name to ASCII, stopping at the first NUL.
void copy_gpt_name(const uint8_t* source, char* out) {
    size_t length = 0;
    for (size_t index = 0; index + 1 < partition::kNameCapacity; ++index) {
        const uint16_t unit = load_le16(source + index * 2);
        if (unit == 0) {
            break;
        }
        out[length++] = (unit >= 0x20 && unit < 0x7f) ? static_cast<char>(unit) : '?';
    }
    out[length] = 0;
}

bool read_op(void* context, uint32_t lba, uint32_t sector_count, void* buffer) {
    const Slice& slice = *static_cast<Slice*>(context);
    // block::read already checked the range against the partition's size:
    // all that is left is moving the origin.
    return block::read(slice.parent_index, slice.info.start_lba + lba, sector_count, buffer);
}

bool write_op(void* context, uint32_t lba, uint32_t sector_count, const void* buffer) {
    const Slice& slice = *static_cast<Slice*>(context);
    return block::write_cached(slice.parent_index, slice.info.start_lba + lba, sector_count, buffer);
}

// A partition has no cache of its own: flushing it flushes the disk.
bool flush_op(void* context) {
    const Slice& slice = *static_cast<Slice*>(context);
    return block::flush(slice.parent_index);
}

const block::DeviceOps kOps = {
    &read_op,
    &write_op,
    &flush_op,
    /*deferred_writes=*/false, // the disk underneath defers
};

// Clips the partition to the parent's real size before registering it: a
// table that lies (a truncated image, a disk cloned onto a smaller one) must
// not be able to produce reads outside the device.
bool add_slice(
    size_t parent_index,
    const char* parent_name,
    bool parent_writable,
    uint32_t parent_sectors,
    const partition::Info& info)
{
    if (g_slice_count >= kMaxSlices || info.sector_count == 0 || info.start_lba >= parent_sectors) {
        return false;
    }

    Slice& slice = g_slices[g_slice_count];
    slice = {};
    slice.present = true;
    slice.parent_index = parent_index;
    slice.info = info;
    slice.info.parent_index = parent_index;
    const uint32_t available = parent_sectors - info.start_lba;
    if (slice.info.sector_count > available) {
        slice.info.sector_count = available;
    }
    slice.device_index = block::device_count();

    build_name(g_names[g_slice_count], sizeof(g_names[g_slice_count]), parent_name, info.number);
    if (!block::register_device(kOps, &slice, slice.info.sector_count, parent_writable, g_names[g_slice_count])) {
        slice.present = false;
        return false;
    }
    ++g_slice_count;
    return true;
}

struct GptHeader {
    uint64_t alternate_lba;
    uint64_t entry_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint8_t disk_guid[16];
};

// Reads and checks the GPT header at `lba` and its entry array into
// g_entries: signature, header CRC, its own LBA, the array's geometry and CRC.
bool load_gpt(size_t parent_index, uint32_t parent_sectors, uint32_t lba, GptHeader& out) {
    uint8_t sector[block::kSectorSize] = {};
    if (lba >= parent_sectors || !block::read(parent_index, lba, 1, sector)) {
        return false;
    }
    if (memcmp(sector, "EFI PART", 8) != 0) {
        return false;
    }
    const uint32_t header_size = load_le32(sector + 12);
    if (header_size < kGptMinHeaderSize || header_size > block::kSectorSize) {
        return false;
    }
    const uint32_t stored_crc = load_le32(sector + 16);
    memset(sector + 16, 0, 4);
    if (sx_crc32(sector, header_size) != stored_crc) {
        return false;
    }
    if (load_le64(sector + 24) != lba) {
        return false;
    }

    out.alternate_lba = load_le64(sector + 32);
    out.entry_lba = load_le64(sector + 72);
    out.entry_count = load_le32(sector + 80);
    out.entry_size = load_le32(sector + 84);
    memcpy(out.disk_guid, sector + 56, sizeof(out.disk_guid));
    const uint32_t entries_crc = load_le32(sector + 88);

    if (out.entry_size < 128 || out.entry_size > block::kSectorSize ||
        (block::kSectorSize % out.entry_size) != 0 || out.entry_count == 0) {
        return false;
    }
    const uint64_t array_bytes = static_cast<uint64_t>(out.entry_count) * out.entry_size;
    if (array_bytes > sizeof(g_entries)) {
        return false;
    }
    const uint32_t array_sectors =
        static_cast<uint32_t>((array_bytes + block::kSectorSize - 1) / block::kSectorSize);
    if (out.entry_lba == 0 || out.entry_lba >= parent_sectors ||
        array_sectors > parent_sectors - out.entry_lba) {
        return false;
    }
    if (!block::read(parent_index, static_cast<uint32_t>(out.entry_lba), array_sectors, g_entries)) {
        return false;
    }
    return sx_crc32(g_entries, static_cast<size_t>(array_bytes)) == entries_crc;
}

// Returns the disk's scheme when sector 1 says "EFI PART" (gpt or damaged), or
// none when it does not, so the caller falls back to the MBR.
partition::Scheme scan_gpt(size_t parent_index, const char* parent_name, bool parent_writable, uint32_t parent_sectors) {
    uint8_t sector[block::kSectorSize] = {};
    if (!block::read(parent_index, kGptHeaderLba, 1, sector) || memcmp(sector, "EFI PART", 8) != 0) {
        return partition::Scheme::none;
    }

    partition::DiskInfo& disk = g_disks[parent_index];
    GptHeader header = {};
    if (!load_gpt(parent_index, parent_sectors, kGptHeaderLba, header)) {
        // The backup header sits on the disk's last sector. The primary's
        // alternate_lba is not trusted: the primary is what failed.
        if (!load_gpt(parent_index, parent_sectors, parent_sectors - 1, header)) {
            console::printf("partition: %s: GPT headers damaged, disk left unpartitioned\n", parent_name);
            return partition::Scheme::damaged;
        }
        console::printf("partition: %s: primary GPT damaged, using the backup\n", parent_name);
        disk.gpt_from_backup = true;
    }
    memcpy(disk.disk_guid, header.disk_guid, sizeof(disk.disk_guid));

    const uint32_t total = header.entry_count < kGptMaxEntries ? header.entry_count : kGptMaxEntries;
    const uint8_t zero_guid[16] = {};
    for (uint32_t index = 0; index < total; ++index) {
        const uint8_t* entry = g_entries + static_cast<size_t>(index) * header.entry_size;
        if (memcmp(entry, zero_guid, sizeof(zero_guid)) == 0) {
            continue;
        }
        const uint64_t first = load_le64(entry + 32);
        const uint64_t last = load_le64(entry + 40);
        // block:: addresses 32 bits (2 TiB with 512-byte sectors): a partition
        // past that is skipped whole instead of registered truncated and
        // lying about its contents.
        if (last < first || first > 0xffffffffull || last > 0xffffffffull) {
            continue;
        }
        partition::Info info = {};
        info.start_lba = static_cast<uint32_t>(first);
        info.sector_count = static_cast<uint32_t>(last - first + 1);
        info.scheme = partition::Scheme::gpt;
        info.number = index + 1;
        info.esp = memcmp(entry, kEspTypeGuid, sizeof(kEspTypeGuid)) == 0;
        memcpy(info.type_guid, entry, partition::kGuidSize);
        memcpy(info.unique_guid, entry + 16, partition::kGuidSize);
        copy_gpt_name(entry + 56, info.name);
        add_slice(parent_index, parent_name, parent_writable, parent_sectors, info);
    }
    return partition::Scheme::gpt;
}

void scan_mbr(
    size_t parent_index,
    const char* parent_name,
    bool parent_writable,
    uint32_t parent_sectors,
    const uint8_t* boot_sector)
{
    for (size_t index = 0; index < kMbrEntryCount; ++index) {
        const uint8_t* entry = boot_sector + kMbrTableOffset + (index * kMbrEntrySize);
        const uint8_t type = entry[4];
        // Extended partitions need walking the EBR chain. Nothing needs it yet:
        // the installer writes GPT and factory USB sticks carry one primary.
        if (type == kMbrTypeEmpty || is_extended(type)) {
            continue;
        }
        partition::Info info = {};
        info.start_lba = load_le32(entry + 8);
        info.sector_count = load_le32(entry + 12);
        info.scheme = partition::Scheme::mbr;
        info.number = static_cast<uint32_t>(index + 1);
        info.mbr_type = type;
        info.esp = type == kMbrTypeEsp;
        add_slice(parent_index, parent_name, parent_writable, parent_sectors, info);
    }
}

bool has_protective_mbr(const uint8_t* boot_sector) {
    for (size_t entry = 0; entry < kMbrEntryCount; ++entry) {
        if (boot_sector[kMbrTableOffset + (entry * kMbrEntrySize) + 4] == kMbrTypeGptProtective) {
            return true;
        }
    }
    return false;
}

void enumerate() {
    memset(g_slices, 0, sizeof(g_slices));
    memset(g_names, 0, sizeof(g_names));
    memset(g_disks, 0, sizeof(g_disks));
    g_slice_count = 0;

    // Snapshot before the loop: add_slice registers new devices and the count
    // grows while iterating. Freezing it keeps a partition from being sliced.
    const size_t parents = block::device_count();
    for (size_t index = 0; index < parents && index < kMaxDisks; ++index) {
        block::DeviceInfo info = {};
        if (!block::device_info(index, info) || !info.present || info.sector_count < 2) {
            continue;
        }

        uint8_t boot_sector[block::kSectorSize] = {};
        if (!block::read(index, 0, 1, boot_sector)) {
            continue;
        }
        if (load_le16(boot_sector + 510) != 0xaa55) {
            continue;
        }

        // Protective MBR: the 4-entry table lies on purpose (a single 0xEE
        // partition covering the disk) and the real one is the GPT. The GPT is
        // tried even without it, because there are hybrid images without 0xEE;
        // the MBR is only read when there is no GPT at all.
        const partition::Scheme gpt = scan_gpt(index, info.name, info.writable, info.sector_count);
        if (gpt != partition::Scheme::none) {
            g_disks[index].scheme = gpt;
            continue;
        }
        if (has_protective_mbr(boot_sector)) {
            g_disks[index].scheme = partition::Scheme::damaged;
            continue;
        }
        g_disks[index].scheme = partition::Scheme::mbr;
        scan_mbr(index, info.name, info.writable, info.sector_count, boot_sector);
    }
}

const block::Driver kDriver = {
    "partition",
    kDriverPriority,
    &enumerate,
    nullptr,
};

} // namespace

namespace partition {

const block::Driver& driver() { return kDriver; }

bool info(size_t device_index, Info& out) {
    for (size_t index = 0; index < g_slice_count; ++index) {
        const Slice& slice = g_slices[index];
        if (!slice.present || slice.device_index != device_index) {
            continue;
        }
        out = slice.info;
        return true;
    }
    return false;
}

DiskInfo disk_info(size_t device_index) {
    if (device_index >= kMaxDisks) {
        return {};
    }
    return g_disks[device_index];
}

} // namespace partition
