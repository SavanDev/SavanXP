#include "kernel/svfs.hpp"

#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"

namespace {

constexpr char kSuperblockMagic[8] = {'S', 'V', 'F', 'S', '2', '\0', '\0', '\0'};
constexpr char kJournalMagic[8] = {'S', 'V', 'J', 'N', 'L', '2', '\0', '\0'};
constexpr uint32_t kVersion = 2;
constexpr uint32_t kFlagClean = 1u;
constexpr uint16_t kInodeTypeUnused = 0;
constexpr uint16_t kInodeTypeFile = 1;
constexpr uint16_t kInodeTypeDirectory = 2;
constexpr uint32_t kPrimarySuperblockLba = 0;
constexpr uint32_t kSecondarySuperblockLba = 1;
constexpr uint32_t kJournalLba = 2;
constexpr uint32_t kBlockBitmapSectors = 32;
constexpr uint32_t kInodeBitmapSectors = 1;
constexpr uint32_t kInodeTableSectors = 32;
constexpr uint32_t kJournalMetadataSectors = kBlockBitmapSectors + kInodeBitmapSectors + kInodeTableSectors;
constexpr uint32_t kJournalSectors = 1 + kJournalMetadataSectors;
constexpr uint32_t kBlockBitmapLba = kJournalLba + kJournalSectors;
constexpr uint32_t kInodeBitmapLba = kBlockBitmapLba + kBlockBitmapSectors;
constexpr uint32_t kInodeTableLba = kInodeBitmapLba + kInodeBitmapSectors;
constexpr uint32_t kDataLba = kInodeTableLba + kInodeTableSectors;
constexpr uint32_t kMaxInodes = 128;
constexpr uint32_t kMaxRecords = kMaxInodes - 1;
constexpr uint32_t kRootInodeId = 1;
constexpr uint32_t kMaxExtents = 8;
constexpr uint32_t kMinimumGrowthSectors = 64;
constexpr size_t kBlockBitmapBytes = static_cast<size_t>(kBlockBitmapSectors) * block::kSectorSize;
constexpr size_t kInodeBitmapBytes = static_cast<size_t>(kInodeBitmapSectors) * block::kSectorSize;
constexpr size_t kMaxRelativePath = 255;
constexpr size_t kMaxDirNameLength = 63;

struct Extent {
    uint32_t start_lba;
    uint32_t sector_count;
};

struct Inode {
    uint32_t inode_id;
    uint16_t type;
    uint16_t reserved0;
    uint32_t size;
    uint32_t link_count;
    uint32_t extent_count;
    Extent extents[kMaxExtents];
    uint8_t reserved[44];
};

struct Superblock {
    char magic[8];
    uint32_t version;
    uint32_t checksum;
    uint32_t sequence;
    uint32_t flags;
    uint32_t total_sectors;
    uint32_t journal_lba;
    uint32_t journal_sectors;
    uint32_t block_bitmap_lba;
    uint32_t block_bitmap_sectors;
    uint32_t inode_bitmap_lba;
    uint32_t inode_bitmap_sectors;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t data_lba;
    uint32_t max_inodes;
    uint32_t root_inode;
    uint8_t reserved[440];
};

struct JournalHeader {
    char magic[8];
    uint32_t checksum;
    uint32_t sequence;
    uint32_t pending;
    uint32_t metadata_sectors;
    uint32_t reserved[122];
};

struct DirEntry {
    uint32_t inode_id;
    uint16_t type;
    uint16_t name_length;
    char name[64];
    uint8_t reserved[8];
};

struct MetadataSnapshot {
    Superblock superblock;
    uint8_t block_bitmap[kBlockBitmapBytes];
    uint8_t inode_bitmap[kInodeBitmapBytes];
    Inode inodes[kMaxInodes];
};

static_assert(sizeof(Inode) == 128);
static_assert(sizeof(Superblock) == block::kSectorSize);
static_assert(sizeof(JournalHeader) == block::kSectorSize);
static_assert(sizeof(DirEntry) == 80);

Superblock g_superblock = {};
uint8_t g_block_bitmap[kBlockBitmapBytes] = {};
uint8_t g_inode_bitmap[kInodeBitmapBytes] = {};
Inode g_inodes[kMaxInodes] = {};
svfs::FileRecord g_records[kMaxRecords] = {};
MetadataSnapshot g_snapshot = {};
size_t g_device_index = static_cast<size_t>(-1);
svfs::MountStatus g_status = svfs::MountStatus::unavailable;
bool g_metadata_ready = false;
volatile uint32_t g_mutation_lock = 0;

uint32_t checksum_bytes(const void* data, size_t count) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t value = 2166136261u;
    for (size_t index = 0; index < count; ++index) {
        value ^= bytes[index];
        value *= 16777619u;
    }
    return value;
}

uint32_t superblock_checksum(const Superblock& superblock) {
    Superblock copy = superblock;
    copy.checksum = 0;
    return checksum_bytes(&copy, sizeof(copy));
}

uint32_t journal_checksum(const JournalHeader& header) {
    JournalHeader copy = header;
    copy.checksum = 0;
    return checksum_bytes(&copy, sizeof(copy));
}

bool valid_superblock(const Superblock& superblock) {
    return memcmp(superblock.magic, kSuperblockMagic, sizeof(kSuperblockMagic)) == 0 &&
        superblock.version == kVersion &&
        superblock.checksum == superblock_checksum(superblock) &&
        superblock.journal_lba == kJournalLba &&
        superblock.journal_sectors == kJournalSectors &&
        superblock.block_bitmap_lba == kBlockBitmapLba &&
        superblock.block_bitmap_sectors == kBlockBitmapSectors &&
        superblock.inode_bitmap_lba == kInodeBitmapLba &&
        superblock.inode_bitmap_sectors == kInodeBitmapSectors &&
        superblock.inode_table_lba == kInodeTableLba &&
        superblock.inode_table_sectors == kInodeTableSectors &&
        superblock.data_lba == kDataLba &&
        superblock.max_inodes == kMaxInodes &&
        superblock.root_inode == kRootInodeId &&
        superblock.total_sectors > kDataLba;
}

bool valid_journal(const JournalHeader& header) {
    return memcmp(header.magic, kJournalMagic, sizeof(kJournalMagic)) == 0 &&
        header.checksum == journal_checksum(header) &&
        header.metadata_sectors == kJournalMetadataSectors;
}

class MutationGuard {
public:
    MutationGuard() {
        while (__atomic_test_and_set(&g_mutation_lock, __ATOMIC_ACQUIRE)) {
        }
    }

    ~MutationGuard() {
        __atomic_clear(&g_mutation_lock, __ATOMIC_RELEASE);
    }
};

size_t inode_index(uint32_t inode_id) {
    return inode_id == 0 ? static_cast<size_t>(-1) : static_cast<size_t>(inode_id - 1);
}

Inode* inode_for_id(uint32_t inode_id) {
    const size_t index = inode_index(inode_id);
    if (index >= kMaxInodes) {
        return nullptr;
    }
    return &g_inodes[index];
}

bool bitmap_test(const uint8_t* bitmap, uint32_t bit) {
    const uint32_t byte = bit / 8u;
    const uint32_t shift = bit % 8u;
    return (bitmap[byte] & static_cast<uint8_t>(1u << shift)) != 0;
}

void bitmap_set(uint8_t* bitmap, uint32_t bit, bool value) {
    const uint32_t byte = bit / 8u;
    const uint32_t shift = bit % 8u;
    const uint8_t mask = static_cast<uint8_t>(1u << shift);
    if (value) {
        bitmap[byte] |= mask;
    } else {
        bitmap[byte] &= static_cast<uint8_t>(~mask);
    }
}

void snapshot_metadata() {
    g_snapshot.superblock = g_superblock;
    memcpy(g_snapshot.block_bitmap, g_block_bitmap, sizeof(g_block_bitmap));
    memcpy(g_snapshot.inode_bitmap, g_inode_bitmap, sizeof(g_inode_bitmap));
    memcpy(g_snapshot.inodes, g_inodes, sizeof(g_inodes));
}

void restore_metadata() {
    g_superblock = g_snapshot.superblock;
    memcpy(g_block_bitmap, g_snapshot.block_bitmap, sizeof(g_block_bitmap));
    memcpy(g_inode_bitmap, g_snapshot.inode_bitmap, sizeof(g_inode_bitmap));
    memcpy(g_inodes, g_snapshot.inodes, sizeof(g_inodes));
}

bool read_home_metadata() {
    return block::read(g_device_index, g_superblock.block_bitmap_lba, g_superblock.block_bitmap_sectors, g_block_bitmap) &&
        block::read(g_device_index, g_superblock.inode_bitmap_lba, g_superblock.inode_bitmap_sectors, g_inode_bitmap) &&
        block::read(g_device_index, g_superblock.inode_table_lba, g_superblock.inode_table_sectors, g_inodes);
}

bool write_home_metadata() {
    return block::write(g_device_index, g_superblock.block_bitmap_lba, g_superblock.block_bitmap_sectors, g_block_bitmap) &&
        block::write(g_device_index, g_superblock.inode_bitmap_lba, g_superblock.inode_bitmap_sectors, g_inode_bitmap) &&
        block::write(g_device_index, g_superblock.inode_table_lba, g_superblock.inode_table_sectors, g_inodes);
}

bool write_superblocks(uint32_t sequence, uint32_t flags) {
    g_superblock.sequence = sequence;
    g_superblock.flags = flags;
    g_superblock.checksum = superblock_checksum(g_superblock);
    return block::write(g_device_index, kPrimarySuperblockLba, 1, &g_superblock) &&
        block::write(g_device_index, kSecondarySuperblockLba, 1, &g_superblock);
}

bool clear_journal() {
    JournalHeader header = {};
    return block::write(g_device_index, g_superblock.journal_lba, 1, &header);
}

bool write_journal_payload() {
    const uint32_t cursor = g_superblock.journal_lba + 1;
    return block::write(g_device_index, cursor, g_superblock.block_bitmap_sectors, g_block_bitmap) &&
        block::write(g_device_index, cursor + g_superblock.block_bitmap_sectors, g_superblock.inode_bitmap_sectors, g_inode_bitmap) &&
        block::write(
            g_device_index,
            cursor + g_superblock.block_bitmap_sectors + g_superblock.inode_bitmap_sectors,
            g_superblock.inode_table_sectors,
            g_inodes);
}

bool replay_journal() {
    JournalHeader header = {};
    if (!block::read(g_device_index, g_superblock.journal_lba, 1, &header)) {
        return false;
    }
    if (!valid_journal(header) || header.pending == 0) {
        return (g_superblock.flags & kFlagClean) != 0 ? true : write_superblocks(g_superblock.sequence, kFlagClean);
    }

    const uint32_t cursor = g_superblock.journal_lba + 1;
    if (!block::read(g_device_index, cursor, g_superblock.block_bitmap_sectors, g_block_bitmap) ||
        !block::read(g_device_index, cursor + g_superblock.block_bitmap_sectors, g_superblock.inode_bitmap_sectors, g_inode_bitmap) ||
        !block::read(
            g_device_index,
            cursor + g_superblock.block_bitmap_sectors + g_superblock.inode_bitmap_sectors,
            g_superblock.inode_table_sectors,
            g_inodes)) {
        return false;
    }

    if (!write_home_metadata() || !write_superblocks(header.sequence, kFlagClean) || !clear_journal()) {
        return false;
    }
    return true;
}

bool commit_metadata() {
    const uint32_t next_sequence = g_superblock.sequence + 1u;
    JournalHeader journal = {};
    memcpy(journal.magic, kJournalMagic, sizeof(journal.magic));
    journal.sequence = next_sequence;
    journal.pending = 1;
    journal.metadata_sectors = kJournalMetadataSectors;
    journal.checksum = journal_checksum(journal);

    if (!write_journal_payload() ||
        !block::write(g_device_index, g_superblock.journal_lba, 1, &journal) ||
        !write_superblocks(next_sequence, 0) ||
        !write_home_metadata() ||
        !write_superblocks(next_sequence, kFlagClean) ||
        !clear_journal()) {
        return false;
    }

    return true;
}

uint32_t inode_capacity_sectors(const Inode& inode) {
    uint32_t sectors = 0;
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        sectors += inode.extents[index].sector_count;
    }
    return sectors;
}

size_t inode_capacity_bytes(const Inode& inode) {
    return static_cast<size_t>(inode_capacity_sectors(inode)) * block::kSectorSize;
}

bool parse_relative_path(const char* path, char* relative, size_t capacity, bool allow_root = false) {
    if (path == nullptr || relative == nullptr || capacity == 0) {
        return false;
    }
    if (strncmp(path, "/disk", 5) != 0 || (path[5] != '\0' && path[5] != '/')) {
        return false;
    }

    const char* cursor = path + 5;
    while (*cursor == '/') {
        ++cursor;
    }

    if (*cursor == '\0') {
        if (!allow_root) {
            return false;
        }
        relative[0] = '\0';
        return true;
    }

    const size_t length = strlen(cursor);
    if (length >= capacity || length > kMaxRelativePath) {
        return false;
    }
    memcpy(relative, cursor, length + 1);
    return true;
}

bool split_relative_parent(const char* relative, char* parent, size_t parent_capacity, char* leaf, size_t leaf_capacity) {
    if (relative == nullptr || parent == nullptr || leaf == nullptr || parent_capacity == 0 || leaf_capacity == 0) {
        return false;
    }

    const size_t length = strlen(relative);
    size_t slash = length;
    while (slash > 0 && relative[slash - 1] != '/') {
        --slash;
    }

    const size_t leaf_length = length - slash;
    if (leaf_length == 0 || leaf_length >= leaf_capacity || leaf_length > kMaxDirNameLength) {
        return false;
    }

    memcpy(leaf, relative + slash, leaf_length);
    leaf[leaf_length] = '\0';

    if (slash == 0) {
        parent[0] = '\0';
        return true;
    }

    const size_t parent_length = slash - 1;
    if (parent_length >= parent_capacity) {
        return false;
    }

    memcpy(parent, relative, parent_length);
    parent[parent_length] = '\0';
    return true;
}

bool join_relative_path(const char* parent, const char* leaf, char* out, size_t capacity) {
    const size_t parent_length = parent != nullptr ? strlen(parent) : 0;
    const size_t leaf_length = leaf != nullptr ? strlen(leaf) : 0;
    const size_t needed = parent_length == 0 ? leaf_length : (parent_length + 1 + leaf_length);
    if (needed >= capacity || needed > kMaxRelativePath) {
        return false;
    }

    size_t written = 0;
    if (parent_length != 0) {
        memcpy(out, parent, parent_length);
        written += parent_length;
        out[written++] = '/';
    }
    memcpy(out + written, leaf, leaf_length);
    out[written + leaf_length] = '\0';
    return true;
}

svfs::FileRecord* find_record_by_inode(uint32_t inode_id) {
    for (svfs::FileRecord& record : g_records) {
        if (record.in_use && record.inode_id == inode_id) {
            return &record;
        }
    }
    return nullptr;
}

svfs::FileRecord* find_record_by_relative_path(const char* relative) {
    if (relative == nullptr || *relative == '\0') {
        return nullptr;
    }
    for (svfs::FileRecord& record : g_records) {
        if (record.in_use && strcmp(record.path, relative) == 0) {
            return &record;
        }
    }
    return nullptr;
}

svfs::FileRecord* find_record_by_path(const char* path) {
    char relative[256] = {};
    if (!parse_relative_path(path, relative, sizeof(relative))) {
        return nullptr;
    }
    return find_record_by_relative_path(relative);
}

uint32_t parent_inode_for_relative_path(const char* relative_parent) {
    if (relative_parent == nullptr || *relative_parent == '\0') {
        return kRootInodeId;
    }
    svfs::FileRecord* parent = find_record_by_relative_path(relative_parent);
    return parent != nullptr && parent->directory ? parent->inode_id : 0;
}

bool make_absolute_path(const char* relative, char* out, size_t capacity) {
    if (relative == nullptr || out == nullptr || capacity < 6) {
        return false;
    }
    if (*relative == '\0') {
        strcpy(out, "/disk");
        return true;
    }
    const size_t relative_length = strlen(relative);
    if (relative_length + 6 > capacity) {
        return false;
    }
    strcpy(out, "/disk/");
    strcpy(out + 6, relative);
    return true;
}

svfs::FileRecord* allocate_record() {
    for (svfs::FileRecord& record : g_records) {
        if (!record.in_use) {
            memset(&record, 0, sizeof(record));
            record.in_use = true;
            return &record;
        }
    }
    return nullptr;
}

void remove_record(uint32_t inode_id) {
    svfs::FileRecord* record = find_record_by_inode(inode_id);
    if (record != nullptr) {
        memset(record, 0, sizeof(*record));
    }
}

void update_descendant_paths(uint32_t directory_inode, const char* old_path, const char* new_path) {
    const size_t old_length = strlen(old_path);
    const size_t new_length = strlen(new_path);
    for (svfs::FileRecord& record : g_records) {
        if (!record.in_use || record.inode_id == directory_inode) {
            continue;
        }

        uint32_t current = record.parent_inode_id;
        bool descendant = false;
        while (current != 0 && current != kRootInodeId) {
            if (current == directory_inode) {
                descendant = true;
                break;
            }
            const svfs::FileRecord* parent = find_record_by_inode(current);
            current = parent != nullptr ? parent->parent_inode_id : 0;
        }
        if (!descendant) {
            continue;
        }

        if (strncmp(record.path, old_path, old_length) == 0 &&
            (record.path[old_length] == '\0' || record.path[old_length] == '/')) {
            char updated[256] = {};
            if (new_length + strlen(record.path + old_length) >= sizeof(updated)) {
                continue;
            }
            memcpy(updated, new_path, new_length);
            strcpy(updated + new_length, record.path + old_length);
            strcpy(record.path, updated);
        }
    }
}

uint32_t allocate_inode_id() {
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        if (!bitmap_test(g_inode_bitmap, inode_id - 1)) {
            bitmap_set(g_inode_bitmap, inode_id - 1, true);
            return inode_id;
        }
    }
    return 0;
}

void release_inode_id(uint32_t inode_id) {
    if (inode_id == 0 || inode_id > kMaxInodes) {
        return;
    }
    bitmap_set(g_inode_bitmap, inode_id - 1, false);
    Inode* inode = inode_for_id(inode_id);
    if (inode != nullptr) {
        memset(inode, 0, sizeof(*inode));
    }
}

uint32_t find_free_run(uint32_t minimum_sectors) {
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t sector = g_superblock.data_lba; sector < g_superblock.total_sectors; ++sector) {
        if (!bitmap_test(g_block_bitmap, sector)) {
            if (run_length == 0) {
                run_start = sector;
            }
            ++run_length;
            if (run_length >= minimum_sectors) {
                return run_start;
            }
        } else {
            run_length = 0;
        }
    }
    return 0;
}

bool append_extent(Inode& inode, uint32_t start_lba, uint32_t sector_count) {
    if (sector_count == 0) {
        return true;
    }
    if (inode.extent_count != 0) {
        Extent& last = inode.extents[inode.extent_count - 1];
        if (last.start_lba + last.sector_count == start_lba) {
            last.sector_count += sector_count;
            return true;
        }
    }
    if (inode.extent_count >= kMaxExtents) {
        return false;
    }
    inode.extents[inode.extent_count].start_lba = start_lba;
    inode.extents[inode.extent_count].sector_count = sector_count;
    inode.extent_count += 1;
    return true;
}

bool allocate_blocks(Inode& inode, uint32_t additional_sectors) {
    while (additional_sectors != 0) {
        uint32_t run_start = 0;
        uint32_t best = additional_sectors;

        while (best != 0 && run_start == 0) {
            run_start = find_free_run(best);
            if (run_start == 0) {
                --best;
            }
        }
        if (run_start == 0 || best == 0) {
            return false;
        }

        for (uint32_t sector = 0; sector < best; ++sector) {
            bitmap_set(g_block_bitmap, run_start + sector, true);
        }
        if (!append_extent(inode, run_start, best)) {
            return false;
        }
        additional_sectors -= best;
    }
    return true;
}

void free_inode_blocks(Inode& inode) {
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        const Extent& extent = inode.extents[index];
        for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
            bitmap_set(g_block_bitmap, extent.start_lba + sector, false);
        }
    }
    memset(inode.extents, 0, sizeof(inode.extents));
    inode.extent_count = 0;
}

void shrink_inode_to_sectors(Inode& inode, uint32_t target_sectors) {
    uint32_t kept = 0;
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        Extent& extent = inode.extents[index];
        if (kept >= target_sectors) {
            for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
                bitmap_set(g_block_bitmap, extent.start_lba + sector, false);
            }
            memset(&extent, 0, sizeof(extent));
            continue;
        }

        if (kept + extent.sector_count <= target_sectors) {
            kept += extent.sector_count;
            continue;
        }

        const uint32_t keep_here = target_sectors - kept;
        for (uint32_t sector = keep_here; sector < extent.sector_count; ++sector) {
            bitmap_set(g_block_bitmap, extent.start_lba + sector, false);
        }
        extent.sector_count = keep_here;
        kept = target_sectors;
    }

    uint32_t new_extent_count = 0;
    while (new_extent_count < inode.extent_count && inode.extents[new_extent_count].sector_count != 0) {
        ++new_extent_count;
    }
    inode.extent_count = new_extent_count;
}

bool ensure_inode_capacity(Inode& inode, size_t required_size) {
    const uint32_t current_sectors = inode_capacity_sectors(inode);
    const uint32_t required_sectors = required_size == 0
        ? 0
        : static_cast<uint32_t>((required_size + block::kSectorSize - 1) / block::kSectorSize);
    if (required_sectors <= current_sectors) {
        return true;
    }

    uint32_t target_sectors = required_sectors;
    if (current_sectors < kMinimumGrowthSectors) {
        target_sectors = kMinimumGrowthSectors;
    } else if (target_sectors < current_sectors * 2u) {
        target_sectors = current_sectors * 2u;
    }
    if (target_sectors < required_sectors) {
        target_sectors = required_sectors;
    }

    return allocate_blocks(inode, target_sectors - current_sectors);
}

bool locate_inode_offset(const Inode& inode, size_t offset, uint32_t& lba, size_t& sector_offset) {
    size_t remaining = offset;
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        const Extent& extent = inode.extents[index];
        const size_t extent_bytes = static_cast<size_t>(extent.sector_count) * block::kSectorSize;
        if (remaining < extent_bytes) {
            lba = extent.start_lba + static_cast<uint32_t>(remaining / block::kSectorSize);
            sector_offset = remaining % block::kSectorSize;
            return true;
        }
        remaining -= extent_bytes;
    }
    return false;
}

bool read_inode_bytes(const Inode& inode, size_t offset, void* buffer, size_t count) {
    if (buffer == nullptr || count == 0) {
        return true;
    }
    if (offset + count > inode.size) {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t remaining = count;
    size_t file_offset = offset;
    uint8_t sector[block::kSectorSize] = {};

    while (remaining != 0) {
        uint32_t lba = 0;
        size_t sector_offset = 0;
        if (!locate_inode_offset(inode, file_offset, lba, sector_offset) ||
            !block::read(g_device_index, lba, 1, sector)) {
            return false;
        }

        const size_t chunk = (block::kSectorSize - sector_offset) < remaining
            ? (block::kSectorSize - sector_offset)
            : remaining;
        memcpy(bytes + (count - remaining), sector + sector_offset, chunk);
        file_offset += chunk;
        remaining -= chunk;
    }

    return true;
}

bool write_inode_bytes(const Inode& inode, size_t offset, const void* buffer, size_t count) {
    if (buffer == nullptr || count == 0) {
        return true;
    }

    const auto* bytes = static_cast<const uint8_t*>(buffer);
    size_t remaining = count;
    size_t file_offset = offset;
    uint8_t sector[block::kSectorSize] = {};

    while (remaining != 0) {
        uint32_t lba = 0;
        size_t sector_offset = 0;
        if (!locate_inode_offset(inode, file_offset, lba, sector_offset)) {
            return false;
        }

        const size_t chunk = (block::kSectorSize - sector_offset) < remaining
            ? (block::kSectorSize - sector_offset)
            : remaining;

        if (sector_offset != 0 || chunk != block::kSectorSize) {
            if (!block::read(g_device_index, lba, 1, sector)) {
                return false;
            }
        } else {
            memset(sector, 0, sizeof(sector));
        }

        memcpy(sector + sector_offset, bytes + (count - remaining), chunk);
        if (!block::write(g_device_index, lba, 1, sector)) {
            return false;
        }

        file_offset += chunk;
        remaining -= chunk;
    }

    return true;
}

bool zero_inode_range(const Inode& inode, size_t offset, size_t count) {
    uint8_t zero[block::kSectorSize] = {};
    size_t remaining = count;
    size_t file_offset = offset;
    while (remaining != 0) {
        const size_t chunk = remaining < sizeof(zero) ? remaining : sizeof(zero);
        if (!write_inode_bytes(inode, file_offset, zero, chunk)) {
            return false;
        }
        file_offset += chunk;
        remaining -= chunk;
    }
    return true;
}

bool read_dir_entry(const Inode& directory, size_t offset, DirEntry& entry) {
    if (offset + sizeof(entry) > directory.size) {
        return false;
    }
    return read_inode_bytes(directory, offset, &entry, sizeof(entry));
}

bool write_dir_entry(const Inode& directory, size_t offset, const DirEntry& entry) {
    return write_inode_bytes(directory, offset, &entry, sizeof(entry));
}

bool find_directory_entry(const Inode& directory, const char* name, DirEntry& entry, size_t& offset_out) {
    if (directory.type != kInodeTypeDirectory) {
        return false;
    }
    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory.size; offset += sizeof(DirEntry)) {
        DirEntry current = {};
        if (!read_dir_entry(directory, offset, current)) {
            return false;
        }
        if (current.inode_id == 0 || current.name_length == 0) {
            continue;
        }
        if (current.name_length <= kMaxDirNameLength &&
            strncmp(current.name, name, current.name_length) == 0 &&
            name[current.name_length] == '\0') {
            entry = current;
            offset_out = offset;
            return true;
        }
    }
    return false;
}

bool directory_has_children(const Inode& directory) {
    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory.size; offset += sizeof(DirEntry)) {
        DirEntry entry = {};
        if (!read_dir_entry(directory, offset, entry)) {
            return false;
        }
        if (entry.inode_id != 0 && entry.name_length != 0) {
            return true;
        }
    }
    return false;
}

bool add_directory_entry(Inode& directory, uint32_t inode_id, uint16_t type, const char* name) {
    DirEntry entry = {};
    const size_t name_length = strlen(name);
    if (name_length == 0 || name_length > kMaxDirNameLength) {
        return false;
    }

    size_t target_offset = directory.size;
    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory.size; offset += sizeof(DirEntry)) {
        DirEntry current = {};
        if (!read_dir_entry(directory, offset, current)) {
            return false;
        }
        if (current.inode_id == 0 || current.name_length == 0) {
            target_offset = offset;
            break;
        }
    }

    const size_t required_size = target_offset + sizeof(DirEntry);
    if (!ensure_inode_capacity(directory, required_size)) {
        return false;
    }
    if (required_size > directory.size) {
        directory.size = static_cast<uint32_t>(required_size);
    }

    entry.inode_id = inode_id;
    entry.type = type;
    entry.name_length = static_cast<uint16_t>(name_length);
    memcpy(entry.name, name, name_length);
    entry.name[name_length] = '\0';
    return write_dir_entry(directory, target_offset, entry);
}

bool remove_directory_entry(Inode& directory, const char* name) {
    DirEntry entry = {};
    size_t offset = 0;
    if (!find_directory_entry(directory, name, entry, offset)) {
        return false;
    }
    DirEntry empty = {};
    return write_dir_entry(directory, offset, empty);
}

bool build_record_recursive(uint32_t directory_inode, uint32_t parent_inode, const char* parent_relative) {
    const Inode* directory = inode_for_id(directory_inode);
    if (directory == nullptr || directory->type != kInodeTypeDirectory) {
        return false;
    }

    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory->size; offset += sizeof(DirEntry)) {
        DirEntry entry = {};
        if (!read_dir_entry(*directory, offset, entry)) {
            return false;
        }
        if (entry.inode_id == 0 || entry.name_length == 0) {
            continue;
        }

        const Inode* child = inode_for_id(entry.inode_id);
        if (child == nullptr || child->type == kInodeTypeUnused) {
            return false;
        }

        svfs::FileRecord* record = allocate_record();
        if (record == nullptr) {
            return false;
        }

        char relative[256] = {};
        if (!join_relative_path(parent_relative, entry.name, relative, sizeof(relative))) {
            return false;
        }

        record->directory = child->type == kInodeTypeDirectory;
        record->inode_id = child->inode_id;
        record->parent_inode_id = parent_inode;
        record->size = child->size;
        strcpy(record->name, entry.name);
        strcpy(record->path, relative);

        if (record->directory && !build_record_recursive(record->inode_id, record->inode_id, record->path)) {
            return false;
        }
    }

    return true;
}

bool rebuild_record_cache() {
    memset(g_records, 0, sizeof(g_records));
    return build_record_recursive(kRootInodeId, kRootInodeId, "");
}

void refresh_record_size(uint32_t inode_id) {
    svfs::FileRecord* record = find_record_by_inode(inode_id);
    Inode* inode = inode_for_id(inode_id);
    if (record != nullptr && inode != nullptr) {
        record->size = inode->size;
        if (record->vnode != nullptr) {
            record->vnode->size = inode->size;
        }
    }
}

void mount_record(svfs::FileRecord& record) {
    char absolute[262] = {};
    if (!make_absolute_path(record.path, absolute, sizeof(absolute))) {
        return;
    }

    if (record.directory) {
        record.vnode = vfs::ensure_directory(absolute);
    } else {
        record.vnode = vfs::install_external_file(absolute, vfs::Backend::svfs, &record, record.size, true);
    }
}

bool initialize_record_mounts() {
    if (vfs::ensure_directory("/disk") == nullptr) {
        return false;
    }

    for (svfs::FileRecord& record : g_records) {
        if (record.in_use && record.directory) {
            mount_record(record);
            if (record.vnode == nullptr) {
                return false;
            }
        }
    }

    for (svfs::FileRecord& record : g_records) {
        if (record.in_use && !record.directory) {
            mount_record(record);
            if (record.vnode == nullptr) {
                return false;
            }
        }
    }

    return true;
}

bool record_is_ancestor(uint32_t ancestor_inode, uint32_t inode_id) {
    uint32_t current = inode_id;
    while (current != 0 && current != kRootInodeId) {
        if (current == ancestor_inode) {
            return true;
        }
        const svfs::FileRecord* record = find_record_by_inode(current);
        current = record != nullptr ? record->parent_inode_id : 0;
    }
    return ancestor_inode == kRootInodeId;
}

bool load_filesystem_from_device(size_t device_index) {
    Superblock primary = {};
    Superblock secondary = {};
    const bool primary_ok = block::read(device_index, kPrimarySuperblockLba, 1, &primary) && valid_superblock(primary);
    const bool secondary_ok = block::read(device_index, kSecondarySuperblockLba, 1, &secondary) && valid_superblock(secondary);
    if (!primary_ok && !secondary_ok) {
        return false;
    }

    g_superblock = (!secondary_ok || (primary_ok && primary.sequence >= secondary.sequence)) ? primary : secondary;
    g_device_index = device_index;
    if (!read_home_metadata() || !replay_journal() || !rebuild_record_cache()) {
        return false;
    }
    g_metadata_ready = true;
    return true;
}

bool prepare_new_inode(Inode& inode, uint32_t inode_id, uint16_t type) {
    memset(&inode, 0, sizeof(inode));
    inode.inode_id = inode_id;
    inode.type = type;
    inode.link_count = 1;
    return true;
}

} // namespace

namespace svfs {

void initialize() {
    memset(&g_superblock, 0, sizeof(g_superblock));
    memset(g_block_bitmap, 0, sizeof(g_block_bitmap));
    memset(g_inode_bitmap, 0, sizeof(g_inode_bitmap));
    memset(g_inodes, 0, sizeof(g_inodes));
    memset(g_records, 0, sizeof(g_records));
    g_device_index = static_cast<size_t>(-1);
    g_status = MountStatus::unavailable;
    g_metadata_ready = false;

    if (!block::ready()) {
        return;
    }

    for (size_t index = 0; index < block::device_count(); ++index) {
        block::DeviceInfo info = {};
        if (!block::device_info(index, info) || !info.present || info.sector_count <= kDataLba) {
            continue;
        }
        if (load_filesystem_from_device(index)) {
            return;
        }
    }
}

MountStatus status() {
    return g_status;
}

bool mounted() {
    return g_status == MountStatus::mounted;
}

bool mount_at_root() {
    if (g_device_index == static_cast<size_t>(-1) || !g_metadata_ready) {
        return false;
    }
    if (!initialize_record_mounts()) {
        return false;
    }
    g_status = MountStatus::mounted;
    return true;
}

size_t file_count() {
    size_t count = 0;
    for (const FileRecord& record : g_records) {
        if (record.in_use && !record.directory) {
            ++count;
        }
    }
    return count;
}

bool sync() {
    if (g_device_index == static_cast<size_t>(-1) || !g_metadata_ready) {
        return false;
    }
    MutationGuard guard;
    return write_superblocks(g_superblock.sequence, kFlagClean) && clear_journal();
}

bool read_file(FileRecord& file, size_t offset, void* buffer, size_t count) {
    Inode* inode = inode_for_id(file.inode_id);
    if (!file.in_use || file.directory || inode == nullptr || buffer == nullptr) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (offset + count > inode->size) {
        return false;
    }
    return read_inode_bytes(*inode, offset, buffer, count);
}

bool write_file(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written) {
    written = 0;
    if (!file.in_use || file.directory) {
        return false;
    }

    MutationGuard guard;
    Inode* inode = inode_for_id(file.inode_id);
    if (inode == nullptr || inode->type != kInodeTypeFile) {
        return false;
    }

    const size_t required_size = count == 0 ? (truncate ? 0 : inode->size) : (offset + count);
    const bool needs_metadata = truncate || required_size > inode->size || required_size > inode_capacity_bytes(*inode);

    if (needs_metadata) {
        snapshot_metadata();
        if (truncate) {
            shrink_inode_to_sectors(*inode, 0);
            inode->size = 0;
            offset = 0;
        }
        if (!ensure_inode_capacity(*inode, required_size)) {
            restore_metadata();
            return false;
        }
        if (offset > inode->size && !zero_inode_range(*inode, inode->size, offset - inode->size)) {
            restore_metadata();
            return false;
        }
    } else if (buffer == nullptr && count != 0) {
        return false;
    }

    if (count != 0) {
        if (buffer == nullptr || !write_inode_bytes(*inode, offset, buffer, count)) {
            if (needs_metadata) {
                restore_metadata();
            }
            return false;
        }
        written = count;
    }

    if (needs_metadata) {
        const size_t new_size = count == 0 ? inode->size : ((offset + written) > inode->size ? (offset + written) : inode->size);
        inode->size = static_cast<uint32_t>(new_size);
        if (!commit_metadata()) {
            restore_metadata();
            return false;
        }
    }

    refresh_record_size(file.inode_id);
    return true;
}

bool truncate_file(FileRecord& file, size_t size) {
    if (!file.in_use || file.directory) {
        return false;
    }

    MutationGuard guard;
    Inode* inode = inode_for_id(file.inode_id);
    if (inode == nullptr || inode->type != kInodeTypeFile) {
        return false;
    }

    snapshot_metadata();
    if (size > inode->size) {
        if (!ensure_inode_capacity(*inode, size) ||
            !zero_inode_range(*inode, inode->size, size - inode->size)) {
            restore_metadata();
            return false;
        }
    } else {
        const uint32_t keep_sectors = size == 0
            ? 0
            : static_cast<uint32_t>((size + block::kSectorSize - 1) / block::kSectorSize);
        shrink_inode_to_sectors(*inode, keep_sectors);
    }

    inode->size = static_cast<uint32_t>(size);
    if (!commit_metadata()) {
        restore_metadata();
        return false;
    }
    refresh_record_size(file.inode_id);
    return true;
}

FileRecord* create_file(const char* path) {
    char relative[256] = {};
    char parent_relative[256] = {};
    char leaf[64] = {};
    if (!parse_relative_path(path, relative, sizeof(relative)) ||
        !split_relative_parent(relative, parent_relative, sizeof(parent_relative), leaf, sizeof(leaf)) ||
        find_record_by_relative_path(relative) != nullptr) {
        return nullptr;
    }

    const uint32_t parent_inode = parent_inode_for_relative_path(parent_relative);
    Inode* parent = inode_for_id(parent_inode);
    if (parent == nullptr || parent->type != kInodeTypeDirectory) {
        return nullptr;
    }

    MutationGuard guard;
    snapshot_metadata();

    const uint32_t inode_id = allocate_inode_id();
    Inode* inode = inode_for_id(inode_id);
    if (inode_id == 0 || inode == nullptr || !prepare_new_inode(*inode, inode_id, kInodeTypeFile) ||
        !add_directory_entry(*parent, inode_id, kInodeTypeFile, leaf) ||
        !commit_metadata()) {
        restore_metadata();
        return nullptr;
    }

    FileRecord* record = allocate_record();
    if (record == nullptr) {
        return nullptr;
    }
    record->directory = false;
    record->inode_id = inode_id;
    record->parent_inode_id = parent_inode;
    record->size = 0;
    strcpy(record->name, leaf);
    strcpy(record->path, relative);
    if (mounted()) {
        mount_record(*record);
    }
    return record;
}

FileRecord* create_directory(const char* path) {
    char relative[256] = {};
    char parent_relative[256] = {};
    char leaf[64] = {};
    if (!parse_relative_path(path, relative, sizeof(relative)) ||
        !split_relative_parent(relative, parent_relative, sizeof(parent_relative), leaf, sizeof(leaf)) ||
        find_record_by_relative_path(relative) != nullptr) {
        return nullptr;
    }

    const uint32_t parent_inode = parent_inode_for_relative_path(parent_relative);
    Inode* parent = inode_for_id(parent_inode);
    if (parent == nullptr || parent->type != kInodeTypeDirectory) {
        return nullptr;
    }

    MutationGuard guard;
    snapshot_metadata();

    const uint32_t inode_id = allocate_inode_id();
    Inode* inode = inode_for_id(inode_id);
    if (inode_id == 0 || inode == nullptr || !prepare_new_inode(*inode, inode_id, kInodeTypeDirectory) ||
        !add_directory_entry(*parent, inode_id, kInodeTypeDirectory, leaf) ||
        !commit_metadata()) {
        restore_metadata();
        return nullptr;
    }

    FileRecord* record = allocate_record();
    if (record == nullptr) {
        return nullptr;
    }
    record->directory = true;
    record->inode_id = inode_id;
    record->parent_inode_id = parent_inode;
    record->size = 0;
    strcpy(record->name, leaf);
    strcpy(record->path, relative);
    if (mounted()) {
        mount_record(*record);
    }
    return record;
}

bool remove_directory(FileRecord& file) {
    if (!file.in_use || !file.directory) {
        return false;
    }

    MutationGuard guard;
    Inode* inode = inode_for_id(file.inode_id);
    Inode* parent = inode_for_id(file.parent_inode_id);
    if (inode == nullptr || parent == nullptr || inode->type != kInodeTypeDirectory || directory_has_children(*inode)) {
        return false;
    }

    snapshot_metadata();
    if (!remove_directory_entry(*parent, file.name)) {
        restore_metadata();
        return false;
    }
    free_inode_blocks(*inode);
    release_inode_id(file.inode_id);
    if (!commit_metadata()) {
        restore_metadata();
        return false;
    }
    remove_record(file.inode_id);
    return true;
}

bool rename_path(const char* old_path, const char* new_path) {
    FileRecord* target = find_record_by_path(old_path);
    char new_relative[256] = {};
    char new_parent_relative[256] = {};
    char new_leaf[64] = {};
    if (target == nullptr ||
        !parse_relative_path(new_path, new_relative, sizeof(new_relative)) ||
        !split_relative_parent(new_relative, new_parent_relative, sizeof(new_parent_relative), new_leaf, sizeof(new_leaf)) ||
        strcmp(target->path, new_relative) == 0 ||
        find_record_by_relative_path(new_relative) != nullptr) {
        return false;
    }

    const uint32_t new_parent_inode = parent_inode_for_relative_path(new_parent_relative);
    Inode* old_parent = inode_for_id(target->parent_inode_id);
    Inode* new_parent = inode_for_id(new_parent_inode);
    if (old_parent == nullptr || new_parent == nullptr || new_parent->type != kInodeTypeDirectory) {
        return false;
    }
    if (target->directory && (target->inode_id == new_parent_inode || record_is_ancestor(target->inode_id, new_parent_inode))) {
        return false;
    }

    MutationGuard guard;
    snapshot_metadata();
    if (!remove_directory_entry(*old_parent, target->name) ||
        !add_directory_entry(*new_parent, target->inode_id, target->directory ? kInodeTypeDirectory : kInodeTypeFile, new_leaf) ||
        !commit_metadata()) {
        restore_metadata();
        return false;
    }

    char old_relative[256] = {};
    strcpy(old_relative, target->path);
    target->parent_inode_id = new_parent_inode;
    strcpy(target->name, new_leaf);
    strcpy(target->path, new_relative);
    if (target->directory) {
        update_descendant_paths(target->inode_id, old_relative, target->path);
    }
    return true;
}

bool unlink_file(FileRecord& file) {
    if (!file.in_use || file.directory) {
        return false;
    }

    MutationGuard guard;
    Inode* inode = inode_for_id(file.inode_id);
    Inode* parent = inode_for_id(file.parent_inode_id);
    if (inode == nullptr || parent == nullptr || inode->type != kInodeTypeFile) {
        return false;
    }

    snapshot_metadata();
    if (!remove_directory_entry(*parent, file.name)) {
        restore_metadata();
        return false;
    }
    free_inode_blocks(*inode);
    release_inode_id(file.inode_id);
    if (!commit_metadata()) {
        restore_metadata();
        return false;
    }
    remove_record(file.inode_id);
    return true;
}

FileRecord* file_from_vnode(vfs::Vnode& node) {
    for (FileRecord& record : g_records) {
        if (record.in_use && record.vnode == &node) {
            return &record;
        }
    }
    return nullptr;
}

} // namespace svfs
