#include "kernel/sxfs.hpp"

#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/rtc.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"
#include "sxfs/sxfs_format.h"
#include "sxfs/sxfs_ownership.h"

namespace {

// El formato on-disk (constantes de layout, structs, checksum y bit-math) vive
// en include/sxfs/sxfs_format.h como fuente de verdad unica, compartido con el
// tool de host. Aca solo se le ponen los nombres internos historicos para no
// reescribir los cientos de usos, mas la politica y los limites que son propios
// del driver del kernel (no on-disk).
// (Las constantes de layout que ya solo usa la validacion viven en el header;
// aca quedan las que el resto del driver referencia por su nombre historico.)
constexpr uint32_t kFlagClean = SXFS_FLAG_CLEAN;
constexpr uint16_t kInodeTypeUnused = SXFS_INODE_UNUSED;
constexpr uint16_t kInodeTypeFile = SXFS_INODE_FILE;
constexpr uint16_t kInodeTypeDirectory = SXFS_INODE_DIRECTORY;
constexpr uint32_t kPrimarySuperblockLba = SXFS_PRIMARY_SB_LBA;
constexpr uint32_t kSecondarySuperblockLba = SXFS_SECONDARY_SB_LBA;
constexpr uint32_t kBlockBitmapSectors = SXFS_BLOCK_BITMAP_SECTORS;
constexpr uint32_t kInodeBitmapSectors = SXFS_INODE_BITMAP_SECTORS;
constexpr uint32_t kJournalMetadataSectors = SXFS_JOURNAL_METADATA_SECTORS;
constexpr uint32_t kDataLba = SXFS_DATA_LBA;
constexpr uint32_t kMaxInodes = SXFS_MAX_INODES;
constexpr uint32_t kMaxRecords = SXFS_MAX_RECORDS;
constexpr uint32_t kRootInodeId = SXFS_ROOT_INODE;
constexpr uint32_t kMaxExtents = SXFS_MAX_EXTENTS;
constexpr uint32_t kMaxDirectoryDepth = 32;
constexpr uint32_t kMinimumGrowthSectors = 64; // growth policy, not on-disk
// Sectors file data never takes: what is left when a file write reports the
// volume full is still enough to rewrite a directory, so a full volume can
// rename and delete (docs/fs/SXFS_NAMESPACE.md). 128 sectors hold the copy of
// a directory of 819 entries.
constexpr uint32_t kDirectoryReserveSectors = 128;
// A directory's entries in memory, rounded up to whole sectors so the copy
// goes to disk straight from the buffer: 4096 entries are exactly 640 sectors.
constexpr uint32_t kDirScratchEntries = 4096;
constexpr uint32_t kNoSlot = 0xffffffffu;
constexpr size_t kBlockBitmapBytes = static_cast<size_t>(kBlockBitmapSectors) * block::kSectorSize;
constexpr size_t kInodeBitmapBytes = static_cast<size_t>(kInodeBitmapSectors) * block::kSectorSize;
constexpr size_t kMaxRelativePath = SXFS_MAX_RELATIVE_PATH;
constexpr size_t kMaxDirNameLength = SXFS_MAX_NAME_LENGTH;

// Bridge: el kernel indexa bitmaps/tabla en unidades de block::kSectorSize,
// mientras que el formato compartido define SXFS_SECTOR_SIZE. Ambos deben ser
// el mismo tamano de sector o el layout on-disk no cuadra.
static_assert(block::kSectorSize == SXFS_SECTOR_SIZE);

using Extent = sxfs_extent;
using Inode = sxfs_inode;
using Superblock = sxfs_superblock;
using JournalHeader = sxfs_journal_header;
using DirEntry = sxfs_dir_entry;
using fs::Status;

static_assert(kDirScratchEntries >= kMaxRecords);
static_assert((static_cast<size_t>(kDirScratchEntries) * sizeof(DirEntry)) % block::kSectorSize == 0);

// Magias del formato compartido, con los nombres internos historicos.
constexpr const char* kJournalMagic = sxfs_journal_magic;

struct MetadataSnapshot {
    Superblock superblock;
    uint8_t block_bitmap[kBlockBitmapBytes];
    uint8_t inode_bitmap[kInodeBitmapBytes];
    Inode inodes[kMaxInodes];
};

// Un volumen montado. Todo lo que antes eran globales de archivo vive aca
// adentro: el instalador necesita el SxFS del LiveCD (origen) y el de la
// particion destino montados AL MISMO TIEMPO, y con un solo juego de globales
// eso no se podia. El resto del driver no cambio de forma: cada funcion interna
// recibe su Volume& como primer parametro.
struct Volume {
    bool in_use;
    size_t device_index;
    sxfs::MountStatus status;
    bool metadata_ready;
    // Where the volume hangs in the VFS ("/" for the system disk). Every path
    // that enters through the public API is resolved against it, so a second
    // volume at "/mnt/target" does not take over the first one's paths.
    char mount_point[sxfs::kMountPointCapacity];
    // The vnode that publishes the root directory, which has no record.
    vfs::Vnode* root_vnode;
    Superblock superblock;
    uint8_t block_bitmap[kBlockBitmapBytes];
    uint8_t inode_bitmap[kInodeBitmapBytes];
    // Claims are rebuilt from the complete inode set during validation. They
    // are deliberately not the allocation bitmap: the latter permits free
    // future blocks, while this scratch bitmap must have exactly one owner.
    uint8_t extent_claims[kBlockBitmapBytes];
    Inode inodes[kMaxInodes];
    sxfs::FileRecord records[kMaxRecords];
    MetadataSnapshot snapshot;
    // The directory a mutation is rewriting (load_directory_slots,
    // store_directory). One at a time, under the mutation lock.
    DirEntry dir_scratch[kDirScratchEntries];
    // Por volumen y no global: escribir en el destino del instalador no tiene
    // por que serializar contra la raiz.
    volatile uint32_t mutation_lock;
    // While a mutation is in flight the snapshot is exactly what the home
    // metadata holds on disk, so a commit only logs and rewrites the sectors
    // that differ from it. Without a snapshot, or after a failed commit (the
    // disk may then hold part of a transaction that was rolled back in
    // memory), the next commit writes every metadata sector instead.
    bool snapshot_valid;
    bool full_commit_pending;
    // Metadata cost counters, so the cost of a commit can be MEASURED (fscheck)
    // rather than inferred from wall clock.
    uint32_t metadata_commits;
    uint64_t metadata_bytes_written;
};

// The root, the installer's target, and one more volume Disk Management mounts
// under /mnt. Each volume is about 3.7 MiB of BSS (bitmaps, inode table, record
// cache, snapshot and directory scratch), so raising it is not free.
constexpr size_t kMaxVolumes = 3;
Volume g_volumes[kMaxVolumes] = {};

size_t volume_index(const Volume& volume) {
    return static_cast<size_t>(&volume - &g_volumes[0]);
}

Volume* volume_for_id(sxfs::VolumeId id) {
    if (id >= kMaxVolumes || !g_volumes[id].in_use) {
        return nullptr;
    }
    return &g_volumes[id];
}

// The volume whose mount point contains path, the longest one if several do
// (one may be mounted at "/"). How an absolute path entering through the public
// API finds the volume it talks to.
Volume* volume_for_path(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    Volume* best = nullptr;
    size_t best_length = 0;
    for (Volume& volume : g_volumes) {
        if (!volume.in_use || fs::path_below(volume.mount_point, path) == nullptr) {
            continue;
        }
        const size_t length = strlen(volume.mount_point);
        if (best == nullptr || length > best_length) {
            best = &volume;
            best_length = length;
        }
    }
    return best;
}

enum class RecoveryState : uint8_t {
    failed = 0,
    writable = 1,
    read_only = 2,
};

// Validacion y checksum de metadata: la logica vive en el formato compartido
// (sxfs_format.h) para no divergir del builder de host. Aca solo wrappers con
// las firmas por-referencia que usa el resto del driver.
uint32_t superblock_checksum(const Superblock& superblock) {
    return sxfs_superblock_checksum(&superblock);
}

uint32_t journal_checksum(const JournalHeader& header) {
    return sxfs_journal_checksum(&header);
}

bool valid_superblock(const Superblock& superblock) {
    return sxfs_superblock_valid(&superblock);
}

bool valid_journal(const JournalHeader& header) {
    return sxfs_journal_valid(&header);
}

class MutationGuard {
public:
    explicit MutationGuard(Volume& volume) : volume_(volume) {
        while (__atomic_test_and_set(&volume_.mutation_lock, __ATOMIC_ACQUIRE)) {
        }
    }

    ~MutationGuard() {
        __atomic_clear(&volume_.mutation_lock, __ATOMIC_RELEASE);
    }

private:
    Volume& volume_;
};

size_t inode_index(uint32_t inode_id) {
    return inode_id == 0 ? static_cast<size_t>(-1) : static_cast<size_t>(inode_id - 1);
}

Inode* inode_for_id(Volume& volume, uint32_t inode_id) {
    const size_t index = inode_index(inode_id);
    if (index >= kMaxInodes) {
        return nullptr;
    }
    return &volume.inodes[index];
}

bool bitmap_test(const uint8_t* bitmap, uint32_t bit) {
    return sxfs_bitmap_test(bitmap, bit) != 0;
}

void bitmap_set(uint8_t* bitmap, uint32_t bit, bool value) {
    sxfs_bitmap_set(bitmap, bit, value ? 1 : 0);
}

bool extent_is_valid(const Volume& volume, const Extent& extent) {
    return extent.sector_count != 0 && extent.start_lba >= volume.superblock.data_lba &&
        extent.start_lba < volume.superblock.total_sectors &&
        extent.sector_count <= volume.superblock.total_sectors - extent.start_lba;
}

bool inode_metadata_is_valid(const Volume& volume, const Inode& inode, uint32_t inode_id) {
    if ((inode.type != kInodeTypeUnused && inode.inode_id != inode_id) ||
        (inode.type != kInodeTypeUnused && inode.type != kInodeTypeFile &&
         inode.type != kInodeTypeDirectory) ||
        inode.extent_count > kMaxExtents) {
        return false;
    }

    uint64_t capacity_sectors = 0;
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        const Extent& extent = inode.extents[index];
        if (!extent_is_valid(volume, extent)) {
            return false;
        }
        for (uint32_t other = index + 1; other < inode.extent_count; ++other) {
            const Extent& next = inode.extents[other];
            if (extent.start_lba < next.start_lba + next.sector_count &&
                next.start_lba < extent.start_lba + extent.sector_count) {
                return false;
            }
        }
        capacity_sectors += extent.sector_count;
    }

    if (inode.type == kInodeTypeUnused) {
        return inode.extent_count == 0 && inode.size == 0;
    }
    if (capacity_sectors > (SIZE_MAX / block::kSectorSize) ||
        inode.size > capacity_sectors * block::kSectorSize) {
        return false;
    }

    for (uint32_t index = 0; index < inode.extent_count; ++index) {
        const Extent& extent = inode.extents[index];
        for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
            if (!bitmap_test(volume.block_bitmap, extent.start_lba + sector)) {
                return false;
            }
        }
    }
    return true;
}

bool home_metadata_is_valid(Volume& volume) {
    memset(volume.extent_claims, 0, sizeof(volume.extent_claims));
    if (bitmap_test(volume.inode_bitmap, kRootInodeId - 1) == false) {
        return false;
    }
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        const bool allocated = bitmap_test(volume.inode_bitmap, inode_id - 1);
        const Inode& inode = volume.inodes[inode_id - 1];
        if (allocated != (inode.type != kInodeTypeUnused) ||
            !inode_metadata_is_valid(volume, inode, inode_id)) {
            return false;
        }

        for (uint32_t index = 0; index < inode.extent_count; ++index) {
            const Extent& extent = inode.extents[index];
            for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
                const uint32_t lba = extent.start_lba + sector;
                if (bitmap_test(volume.extent_claims, lba)) {
                    return false;
                }
                bitmap_set(volume.extent_claims, lba, true);
            }
        }
    }
    return true;
}

void snapshot_metadata(Volume& volume) {
    volume.snapshot.superblock = volume.superblock;
    memcpy(volume.snapshot.block_bitmap, volume.block_bitmap, sizeof(volume.block_bitmap));
    memcpy(volume.snapshot.inode_bitmap, volume.inode_bitmap, sizeof(volume.inode_bitmap));
    memcpy(volume.snapshot.inodes, volume.inodes, sizeof(volume.inodes));
    volume.snapshot_valid = true;
}

void restore_metadata(Volume& volume) {
    volume.superblock = volume.snapshot.superblock;
    memcpy(volume.block_bitmap, volume.snapshot.block_bitmap, sizeof(volume.block_bitmap));
    memcpy(volume.inode_bitmap, volume.snapshot.inode_bitmap, sizeof(volume.inode_bitmap));
    memcpy(volume.inodes, volume.snapshot.inodes, sizeof(volume.inodes));
}

bool read_home_metadata(Volume& volume) {
    return block::read(volume.device_index, volume.superblock.block_bitmap_lba, volume.superblock.block_bitmap_sectors, volume.block_bitmap) &&
        block::read(volume.device_index, volume.superblock.inode_bitmap_lba, volume.superblock.inode_bitmap_sectors, volume.inode_bitmap) &&
        block::read(volume.device_index, volume.superblock.inode_table_lba, volume.superblock.inode_table_sectors, volume.inodes);
}

bool write_home_metadata(Volume& volume) {
    return block::write(volume.device_index, volume.superblock.block_bitmap_lba, volume.superblock.block_bitmap_sectors, volume.block_bitmap) &&
        block::write(volume.device_index, volume.superblock.inode_bitmap_lba, volume.superblock.inode_bitmap_sectors, volume.inode_bitmap) &&
        block::write(volume.device_index, volume.superblock.inode_table_lba, volume.superblock.inode_table_sectors, volume.inodes);
}

// Whether a write is on the medium when it returns (durable) or may wait in
// the device cache for the caller's next block::flush (cached). The commit
// path batches cached writes between its own ordering points.
enum class Durability : uint8_t {
    durable,
    cached,
};

bool write_sectors(Volume& volume, uint32_t lba, uint32_t count, const void* data, Durability durability) {
    return durability == Durability::cached
        ? block::write_cached(volume.device_index, lba, count, data)
        : block::write(volume.device_index, lba, count, data);
}

bool write_superblocks(Volume& volume, uint32_t sequence, uint32_t flags, Durability durability) {
    volume.superblock.sequence = sequence;
    volume.superblock.flags = flags;
    volume.superblock.checksum = superblock_checksum(volume.superblock);
    return write_sectors(volume, kPrimarySuperblockLba, 1, &volume.superblock, durability) &&
        write_sectors(volume, kSecondarySuperblockLba, 1, &volume.superblock, durability);
}

bool clear_journal(Volume& volume, Durability durability) {
    JournalHeader header = {};
    return write_sectors(volume, volume.superblock.journal_lba, 1, &header, durability);
}

// The metadata as one linear run of sectors, [block bitmap | inode bitmap |
// inode table], which is both its on-disk order at home and its order in the
// journal mirror. In memory the three regions are separate arrays, so a run of
// sectors is only contiguous inside one region.
static_assert(SXFS_INODE_BITMAP_LBA == SXFS_BLOCK_BITMAP_LBA + SXFS_BLOCK_BITMAP_SECTORS);
static_assert(SXFS_INODE_TABLE_LBA == SXFS_INODE_BITMAP_LBA + SXFS_INODE_BITMAP_SECTORS);
static_assert(sizeof(Inode) * kMaxInodes == static_cast<size_t>(SXFS_INODE_TABLE_SECTORS) * block::kSectorSize);

constexpr uint32_t kInodeBitmapIndex = kBlockBitmapSectors;
constexpr uint32_t kInodeTableIndex = kBlockBitmapSectors + kInodeBitmapSectors;

struct MetadataView {
    uint8_t* block_bitmap;
    uint8_t* inode_bitmap;
    uint8_t* inode_table;
};

MetadataView live_metadata(Volume& volume) {
    return {volume.block_bitmap, volume.inode_bitmap, reinterpret_cast<uint8_t*>(volume.inodes)};
}

MetadataView snapshot_metadata_view(Volume& volume) {
    return {volume.snapshot.block_bitmap, volume.snapshot.inode_bitmap,
            reinterpret_cast<uint8_t*>(volume.snapshot.inodes)};
}

uint8_t* metadata_sector(const MetadataView& view, uint32_t index) {
    if (index < kInodeBitmapIndex) {
        return view.block_bitmap + static_cast<size_t>(index) * block::kSectorSize;
    }
    if (index < kInodeTableIndex) {
        return view.inode_bitmap + static_cast<size_t>(index - kInodeBitmapIndex) * block::kSectorSize;
    }
    return view.inode_table + static_cast<size_t>(index - kInodeTableIndex) * block::kSectorSize;
}

// First index past the in-memory region that holds sector `index`.
uint32_t metadata_region_end(uint32_t index) {
    if (index < kInodeBitmapIndex) {
        return kInodeBitmapIndex;
    }
    if (index < kInodeTableIndex) {
        return kInodeTableIndex;
    }
    return kJournalMetadataSectors;
}

// Reads or writes every metadata sector set in `map`, against the disk run
// that starts at `base_lba` (home or journal mirror), one command per run of
// adjacent sectors. Writes are cached: the caller places the flush.
bool transfer_metadata_sectors(Volume& volume, const uint8_t* map, uint32_t base_lba, bool write) {
    const MetadataView view = live_metadata(volume);
    uint32_t index = 0;
    while (index < kJournalMetadataSectors) {
        if (!sxfs_bitmap_test(map, index)) {
            ++index;
            continue;
        }
        const uint32_t limit = metadata_region_end(index);
        uint32_t end = index + 1u;
        while (end < limit && sxfs_bitmap_test(map, end)) {
            ++end;
        }
        uint8_t* data = metadata_sector(view, index);
        const bool ok = write
            ? block::write_cached(volume.device_index, base_lba + index, end - index, data)
            : block::read(volume.device_index, base_lba + index, end - index, data);
        if (!ok) {
            return false;
        }
        index = end;
    }
    return true;
}

// Marks in `map` the metadata sectors that differ from the snapshot, or all of
// them when the snapshot cannot be trusted to match the disk. Returns how many.
uint32_t build_dirty_map(Volume& volume, uint8_t* map) {
    memset(map, 0, SXFS_JOURNAL_MAP_BYTES);
    const bool full = !volume.snapshot_valid || volume.full_commit_pending;
    const MetadataView live = live_metadata(volume);
    const MetadataView before = snapshot_metadata_view(volume);
    uint32_t dirty = 0;
    for (uint32_t index = 0; index < kJournalMetadataSectors; ++index) {
        if (full || memcmp(metadata_sector(live, index), metadata_sector(before, index), block::kSectorSize) != 0) {
            sxfs_bitmap_set(map, index, 1);
            ++dirty;
        }
    }
    return dirty;
}

RecoveryState recover_journal(Volume& volume) {
    JournalHeader header = {};
    if (!block::read(volume.device_index, volume.superblock.journal_lba, 1, &header)) {
        return RecoveryState::failed;
    }
    if (!valid_journal(header) || header.pending == 0) {
        // Nothing to replay, so home must already be whole before anything is
        // written to the volume.
        if (!home_metadata_is_valid(volume)) {
            return RecoveryState::failed;
        }
        if ((volume.superblock.flags & kFlagClean) != 0) {
            return RecoveryState::writable;
        }
        return write_superblocks(volume, volume.superblock.sequence, kFlagClean, Durability::durable)
            ? RecoveryState::writable
            : RecoveryState::read_only;
    }

    // A full journal carries every metadata sector. A sparse one carries only
    // the sectors the transaction changed and is applied ON TOP of the home
    // metadata already in memory: every sector it does not name was the same
    // before and after the transaction, whatever the crash left at home.
    uint8_t map[SXFS_JOURNAL_MAP_BYTES];
    if (header.kind == SXFS_JOURNAL_KIND_SPARSE) {
        memcpy(map, header.sector_map, sizeof(map));
    } else {
        memset(map, 0xff, sizeof(map));
    }
    if (!transfer_metadata_sectors(volume, map, volume.superblock.journal_lba + 1u, /*write=*/false)) {
        return RecoveryState::failed;
    }

    if (!home_metadata_is_valid(volume)) {
        return RecoveryState::failed;
    }
    if (!write_home_metadata(volume) ||
        !write_superblocks(volume, header.sequence, kFlagClean, Durability::durable) ||
        !clear_journal(volume, Durability::durable)) {
        return RecoveryState::read_only;
    }
    return RecoveryState::writable;
}

// Commits the in-memory metadata with a sparse journal: only the sectors that
// changed since the snapshot are logged and then written home. Writes are
// cached (on a physical disk, queued for background writeback) and four
// ordering points order them, which is all crash safety needs:
//   1. the payload (and the transaction's file data) before the header;
//   2. the pending header before anything is written home;
//   3. home before the clean superblocks and the cleared header;
//   4. this commit's cleared header before the next commit's payload, which
//      reuses the journal slots.
// Within each step the writes may land in any order. The commit is durable
// once the writeback queue reaches it, or at block::sync.
bool commit_metadata(Volume& volume) {
    JournalHeader journal = {};
    const uint32_t dirty = build_dirty_map(volume, journal.sector_map);
    if (dirty == 0) {
        volume.snapshot_valid = false;
        // Nothing to log, but the mutation's file data may still be cached.
        return block::flush(volume.device_index);
    }

    const uint32_t next_sequence = volume.superblock.sequence + 1u;
    memcpy(journal.magic, kJournalMagic, sizeof(journal.magic));
    journal.sequence = next_sequence;
    journal.pending = 1;
    journal.metadata_sectors = kJournalMetadataSectors;
    journal.kind = SXFS_JOURNAL_KIND_SPARSE;
    journal.checksum = journal_checksum(journal);

    const size_t device = volume.device_index;
    if (!transfer_metadata_sectors(volume, journal.sector_map, volume.superblock.journal_lba + 1u, /*write=*/true) ||
        !block::flush(device) ||
        !block::write_cached(device, volume.superblock.journal_lba, 1, &journal) ||
        !write_superblocks(volume, next_sequence, 0, Durability::cached) ||
        !block::flush(device) ||
        !transfer_metadata_sectors(volume, journal.sector_map, volume.superblock.block_bitmap_lba, /*write=*/true) ||
        !block::flush(device) ||
        !write_superblocks(volume, next_sequence, kFlagClean, Durability::cached) ||
        !clear_journal(volume, Durability::cached) ||
        !block::flush(device)) {
        // The caller rolls memory back to the snapshot, but some of this
        // transaction may already be on disk: only a full commit reconciles.
        volume.full_commit_pending = true;
        return false;
    }
    volume.full_commit_pending = false;
    volume.snapshot_valid = false;

    // The changed sectors are written twice (journal and home); the overhead
    // is counted as before (journal header, superblocks, cleared header) so
    // the numbers stay comparable with the full-copy baseline.
    const uint64_t payload = static_cast<uint64_t>(dirty) * block::kSectorSize;
    const uint64_t overhead = 3u * static_cast<uint64_t>(block::kSectorSize);
    volume.metadata_commits += 1u;
    volume.metadata_bytes_written += 2u * payload + overhead;

    return true;
}

uint32_t inode_capacity_sectors(const Inode& inode) {
    return sxfs_inode_capacity_sectors(&inode);
}

size_t inode_capacity_bytes(const Inode& inode) {
    return static_cast<size_t>(inode_capacity_sectors(inode)) * block::kSectorSize;
}

bool parse_relative_path(Volume& volume, const char* path, char* relative, size_t capacity, bool allow_root = false) {
    if (path == nullptr || relative == nullptr || capacity == 0) {
        return false;
    }
    const char* cursor = fs::path_below(volume.mount_point, path);
    if (cursor == nullptr) {
        return false;
    }
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

sxfs::FileRecord* find_record_by_inode(Volume& volume, uint32_t inode_id) {
    for (sxfs::FileRecord& record : volume.records) {
        if (record.in_use && record.inode_id == inode_id) {
            return &record;
        }
    }
    return nullptr;
}

sxfs::FileRecord* find_record_by_relative_path(Volume& volume, const char* relative) {
    if (relative == nullptr || *relative == '\0') {
        return nullptr;
    }
    for (sxfs::FileRecord& record : volume.records) {
        if (record.in_use && strcmp(record.path, relative) == 0) {
            return &record;
        }
    }
    return nullptr;
}

sxfs::FileRecord* find_record_by_path(Volume& volume, const char* path) {
    char relative[256] = {};
    if (!parse_relative_path(volume, path, relative, sizeof(relative))) {
        return nullptr;
    }
    return find_record_by_relative_path(volume, relative);
}

uint32_t parent_inode_for_relative_path(Volume& volume, const char* relative_parent) {
    if (relative_parent == nullptr || *relative_parent == '\0') {
        return kRootInodeId;
    }
    sxfs::FileRecord* parent = find_record_by_relative_path(volume, relative_parent);
    return parent != nullptr && parent->directory ? parent->inode_id : 0;
}

bool make_absolute_path(Volume& volume, const char* relative, char* out, size_t capacity) {
    return fs::join_mount_path(volume.mount_point, relative, out, capacity);
}

sxfs::FileRecord* allocate_record(Volume& volume) {
    for (sxfs::FileRecord& record : volume.records) {
        if (!record.in_use) {
            memset(&record, 0, sizeof(record));
            record.in_use = true;
            // Los FileRecord viajan solos por la API publica (read_file, etc.):
            // sin esto no habria como volver al Volume del que salieron.
            record.volume = static_cast<uint16_t>(volume_index(volume));
            return &record;
        }
    }
    return nullptr;
}

void remove_record(Volume& volume, uint32_t inode_id) {
    sxfs::FileRecord* record = find_record_by_inode(volume, inode_id);
    if (record != nullptr) {
        memset(record, 0, sizeof(*record));
    }
}

void update_descendant_paths(Volume& volume, uint32_t directory_inode, const char* old_path, const char* new_path) {
    const size_t old_length = strlen(old_path);
    const size_t new_length = strlen(new_path);
    for (sxfs::FileRecord& record : volume.records) {
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
            const sxfs::FileRecord* parent = find_record_by_inode(volume, current);
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

uint32_t allocate_inode_id(Volume& volume) {
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        if (!bitmap_test(volume.inode_bitmap, inode_id - 1)) {
            bitmap_set(volume.inode_bitmap, inode_id - 1, true);
            return inode_id;
        }
    }
    return 0;
}

void release_inode_id(Volume& volume, uint32_t inode_id) {
    if (inode_id == 0 || inode_id > kMaxInodes) {
        return;
    }
    bitmap_set(volume.inode_bitmap, inode_id - 1, false);
    Inode* inode = inode_for_id(volume, inode_id);
    if (inode != nullptr) {
        memset(inode, 0, sizeof(*inode));
    }
}

// A run of sectors a mutation may take.
struct FreeRun {
    uint32_t start;
    uint32_t length;
};

// The allocation words of 64 sectors starting at `sector` (a multiple of 64),
// as the mutation in flight has to see them: a sector is busy when it is
// allocated now OR was allocated when the mutation started. The second half is
// what makes a transaction atomic for the blocks it frees: until the commit is
// down, the state before the transaction still owns them, so nothing the
// transaction writes may land there (docs/fs/SXFS_NAMESPACE.md).
uint64_t busy_word(const Volume& volume, uint32_t sector) {
    uint64_t live = 0;
    uint64_t before = 0;
    memcpy(&live, volume.block_bitmap + sector / 8u, sizeof(live));
    if (volume.snapshot_valid) {
        memcpy(&before, volume.snapshot.block_bitmap + sector / 8u, sizeof(before));
    }
    return live | before;
}

bool sector_available(const Volume& volume, uint32_t sector) {
    return !bitmap_test(volume.block_bitmap, sector) &&
        !(volume.snapshot_valid && bitmap_test(volume.snapshot.block_bitmap, sector));
}

// The first run of at least `wanted` available sectors, or, when there is
// none, the longest run there is (length 0 on a full volume). The run returned
// may be longer than asked. One pass over the bitmap, a word at a time where
// the words are uniform.
FreeRun find_free_run(const Volume& volume, uint32_t wanted) {
    FreeRun best = {0, 0};
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    uint32_t sector = volume.superblock.data_lba;
    const uint32_t end = volume.superblock.total_sectors;
    while (sector < end) {
        if ((sector % 64u) == 0 && end - sector >= 64u) {
            const uint64_t word = busy_word(volume, sector);
            if (word == ~0ull) {
                if (run_length > best.length) {
                    best = {run_start, run_length};
                }
                run_length = 0;
                sector += 64u;
                continue;
            }
            if (word == 0) {
                if (run_length == 0) {
                    run_start = sector;
                }
                run_length += 64u;
                if (run_length >= wanted) {
                    return {run_start, run_length};
                }
                sector += 64u;
                continue;
            }
        }
        if (sector_available(volume, sector)) {
            if (run_length == 0) {
                run_start = sector;
            }
            if (++run_length >= wanted) {
                return {run_start, run_length};
            }
        } else {
            if (run_length > best.length) {
                best = {run_start, run_length};
            }
            run_length = 0;
        }
        ++sector;
    }
    if (run_length > best.length) {
        best = {run_start, run_length};
    }
    return best;
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

uint32_t popcount64(uint64_t value) {
    // SWAR: the kernel has no popcnt guarantee and no libgcc to call.
    value = value - ((value >> 1) & 0x5555555555555555ull);
    value = (value & 0x3333333333333333ull) + ((value >> 2) & 0x3333333333333333ull);
    value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return static_cast<uint32_t>((value * 0x0101010101010101ull) >> 56);
}

// Counted a word at a time: one bit at a time it took a third of a second
// under the kernel lock on a 1 GiB volume, every time anything asked for the
// volume's usage (Task Manager does each second), and input was lost meanwhile.
uint32_t allocated_data_sector_count(Volume& volume) {
    uint32_t count = 0;
    uint32_t sector = volume.superblock.data_lba;
    const uint32_t end = volume.superblock.total_sectors;
    while (sector < end && (sector % 64u) != 0) {
        count += bitmap_test(volume.block_bitmap, sector) ? 1u : 0u;
        ++sector;
    }
    while (end - sector >= 64u) {
        uint64_t word = 0;
        memcpy(&word, volume.block_bitmap + sector / 8u, sizeof(word));
        count += popcount64(word);
        sector += 64u;
    }
    while (sector < end) {
        count += bitmap_test(volume.block_bitmap, sector) ? 1u : 0u;
        ++sector;
    }
    return count;
}

// Sectors of the data region a mutation in flight may take (see busy_word).
uint32_t available_sector_count(const Volume& volume) {
    uint32_t count = 0;
    uint32_t sector = volume.superblock.data_lba;
    const uint32_t end = volume.superblock.total_sectors;
    while (sector < end && (sector % 64u) != 0) {
        count += sector_available(volume, sector) ? 1u : 0u;
        ++sector;
    }
    while (end - sector >= 64u) {
        count += 64u - popcount64(busy_word(volume, sector));
        sector += 64u;
    }
    while (sector < end) {
        count += sector_available(volume, sector) ? 1u : 0u;
        ++sector;
    }
    return count;
}

void shrink_inode_to_sectors(Volume& volume, Inode& inode, uint32_t target_sectors);

// Gives `inode` `additional_sectors` more, in as few runs as the free space
// allows: the first run that holds all that is still missing, else the longest
// one. All or nothing: when the volume, or the inode's eight extents, cannot
// hold them, the inode and the bitmap are left as they were.
bool allocate_blocks(Volume& volume, Inode& inode, uint32_t additional_sectors) {
    const uint32_t capacity_before = sxfs_inode_capacity_sectors(&inode);
    uint32_t remaining = additional_sectors;
    while (remaining != 0) {
        const FreeRun run = find_free_run(volume, remaining);
        const uint32_t take = run.length < remaining ? run.length : remaining;
        if (take == 0 || !append_extent(inode, run.start, take)) {
            shrink_inode_to_sectors(volume, inode, capacity_before);
            return false;
        }
        for (uint32_t sector = 0; sector < take; ++sector) {
            bitmap_set(volume.block_bitmap, run.start + sector, true);
        }
        remaining -= take;
    }
    return true;
}

void free_inode_blocks(Volume& volume, Inode& inode) {
    const uint32_t extent_count = inode.extent_count < kMaxExtents ? inode.extent_count : kMaxExtents;
    for (uint32_t index = 0; index < extent_count; ++index) {
        const Extent& extent = inode.extents[index];
        for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
            bitmap_set(volume.block_bitmap, extent.start_lba + sector, false);
        }
    }
    memset(inode.extents, 0, sizeof(inode.extents));
    inode.extent_count = 0;
}

void shrink_inode_to_sectors(Volume& volume, Inode& inode, uint32_t target_sectors) {
    uint32_t kept = 0;
    const uint32_t extent_count = inode.extent_count < kMaxExtents ? inode.extent_count : kMaxExtents;
    for (uint32_t index = 0; index < extent_count; ++index) {
        Extent& extent = inode.extents[index];
        if (kept >= target_sectors) {
            for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
                bitmap_set(volume.block_bitmap, extent.start_lba + sector, false);
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
            bitmap_set(volume.block_bitmap, extent.start_lba + sector, false);
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

// Makes room in a FILE for `required_size` bytes. File data leaves
// kDirectoryReserveSectors alone, and grows by more than it was asked (so a
// file written in pieces does not end up as eight small extents) only while
// the volume has that much to give.
Status ensure_inode_capacity(Volume& volume, Inode& inode, size_t required_size) {
    const uint32_t current_sectors = inode_capacity_sectors(inode);
    if (required_size > static_cast<size_t>(SXFS_MAX_TOTAL_SECTORS) * block::kSectorSize) {
        return Status::no_space;
    }
    const uint32_t required_sectors = required_size == 0
        ? 0
        : static_cast<uint32_t>((required_size + block::kSectorSize - 1) / block::kSectorSize);
    if (required_sectors <= current_sectors) {
        return Status::ok;
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

    const uint32_t available = available_sector_count(volume);
    const uint32_t usable = available > kDirectoryReserveSectors ? available - kDirectoryReserveSectors : 0;
    if (required_sectors - current_sectors > usable) {
        return Status::no_space;
    }
    if (target_sectors - current_sectors <= usable &&
        allocate_blocks(volume, inode, target_sectors - current_sectors)) {
        return Status::ok;
    }
    return allocate_blocks(volume, inode, required_sectors - current_sectors) ? Status::ok : Status::no_space;
}

bool locate_inode_offset(const Inode& inode, size_t offset, uint32_t& lba, size_t& sector_offset) {
    size_t remaining = offset;
    const uint32_t extent_count = inode.extent_count < kMaxExtents ? inode.extent_count : kMaxExtents;
    for (uint32_t index = 0; index < extent_count; ++index) {
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

// Sectors from `offset` to the end of the extent holding it: the longest run
// that is contiguous on disk. 0 past the end of the inode.
uint32_t contiguous_sectors_at(const Inode& inode, size_t offset) {
    size_t remaining = offset;
    const uint32_t extent_count = inode.extent_count < kMaxExtents ? inode.extent_count : kMaxExtents;
    for (uint32_t index = 0; index < extent_count; ++index) {
        const Extent& extent = inode.extents[index];
        const size_t extent_bytes = static_cast<size_t>(extent.sector_count) * block::kSectorSize;
        if (remaining < extent_bytes) {
            return extent.sector_count - static_cast<uint32_t>(remaining / block::kSectorSize);
        }
        remaining -= extent_bytes;
    }
    return 0;
}

// One device command's worth; the drivers split nothing themselves.
constexpr uint32_t kMaxReadRunSectors = 128;

bool read_inode_bytes(Volume& volume, const Inode& inode, size_t offset, void* buffer, size_t count) {
    if (buffer == nullptr || count == 0) {
        return true;
    }
    if (offset > inode.size || count > inode.size - offset) {
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t remaining = count;
    size_t file_offset = offset;
    uint8_t sector[block::kSectorSize] = {};

    while (remaining != 0) {
        uint32_t lba = 0;
        size_t sector_offset = 0;
        if (!locate_inode_offset(inode, file_offset, lba, sector_offset)) {
            return false;
        }

        // Whole sectors that are contiguous on disk come in as one read,
        // straight into the caller's buffer.
        if (sector_offset == 0 && remaining >= block::kSectorSize) {
            uint32_t run = contiguous_sectors_at(inode, file_offset);
            const size_t whole = remaining / block::kSectorSize;
            if (run > whole) {
                run = static_cast<uint32_t>(whole);
            }
            if (run > kMaxReadRunSectors) {
                run = kMaxReadRunSectors;
            }
            if (!block::read(volume.device_index, lba, run, bytes + (count - remaining))) {
                return false;
            }
            const size_t run_bytes = static_cast<size_t>(run) * block::kSectorSize;
            file_offset += run_bytes;
            remaining -= run_bytes;
            continue;
        }

        if (!block::read(volume.device_index, lba, 1, sector)) {
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

bool write_inode_bytes(Volume& volume, const Inode& inode, size_t offset, const void* buffer, size_t count) {
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

        // Whole sectors that are contiguous on disk go out as one write
        // straight from the caller's buffer: one queue record and one device
        // command instead of one per sector.
        if (sector_offset == 0 && remaining >= block::kSectorSize) {
            uint32_t run = contiguous_sectors_at(inode, file_offset);
            const size_t whole = remaining / block::kSectorSize;
            if (run > whole) {
                run = static_cast<uint32_t>(whole);
            }
            if (run > 1) {
                if (!block::write_cached(volume.device_index, lba, run, bytes + (count - remaining))) {
                    return false;
                }
                const size_t run_bytes = static_cast<size_t>(run) * block::kSectorSize;
                file_offset += run_bytes;
                remaining -= run_bytes;
                continue;
            }
        }

        const size_t chunk = (block::kSectorSize - sector_offset) < remaining
            ? (block::kSectorSize - sector_offset)
            : remaining;

        if (sector_offset != 0 || chunk != block::kSectorSize) {
            if (!block::read(volume.device_index, lba, 1, sector)) {
                return false;
            }
        } else {
            memset(sector, 0, sizeof(sector));
        }

        memcpy(sector + sector_offset, bytes + (count - remaining), chunk);
        // Cached: the mutation places an ordering point after its data (its
        // commit's first one, or its own for an overwrite in place).
        if (!block::write_cached(volume.device_index, lba, 1, sector)) {
            return false;
        }

        file_offset += chunk;
        remaining -= chunk;
    }

    return true;
}

bool zero_inode_range(Volume& volume, const Inode& inode, size_t offset, size_t count) {
    uint8_t zero[block::kSectorSize] = {};
    size_t remaining = count;
    size_t file_offset = offset;
    while (remaining != 0) {
        const size_t chunk = remaining < sizeof(zero) ? remaining : sizeof(zero);
        if (!write_inode_bytes(volume, inode, file_offset, zero, chunk)) {
            return false;
        }
        file_offset += chunk;
        remaining -= chunk;
    }
    return true;
}

bool inode_is_doomed(const Volume& volume, const Inode& inode) {
    return sxfs_inode_doomed(volume.superblock.features, &inode) != 0;
}

// A slot is either free (its head all zero) or names an allocated inode of
// the entry's type with a well-formed name.
bool dir_entry_is_valid(Volume& volume, const DirEntry& entry) {
    if (entry.inode_id == 0) {
        return entry.name_length == 0 && entry.type == 0;
    }
    if (entry.name_length == 0 || entry.name_length > kMaxDirNameLength ||
        entry.name[entry.name_length] != '\0') {
        return false;
    }
    for (uint16_t index = 0; index < entry.name_length; ++index) {
        if (entry.name[index] == '\0') {
            return false;
        }
    }
    const Inode* child = inode_for_id(volume, entry.inode_id);
    return child != nullptr && child->type != kInodeTypeUnused &&
        ((child->type == kInodeTypeDirectory && entry.type == kInodeTypeDirectory) ||
         (child->type == kInodeTypeFile && entry.type == kInodeTypeFile));
}

bool read_dir_entry(Volume& volume, const Inode& directory, size_t offset, DirEntry& entry) {
    if (offset > directory.size || sizeof(entry) > directory.size - offset ||
        !read_inode_bytes(volume, directory, offset, &entry, sizeof(entry))) {
        return false;
    }
    return dir_entry_is_valid(volume, entry);
}

// An entry that is part of the tree: a used slot whose inode is not doomed.
bool dir_entry_is_live(Volume& volume, const DirEntry& entry) {
    if (entry.inode_id == 0) {
        return false;
    }
    const Inode* child = inode_for_id(volume, entry.inode_id);
    return child != nullptr && !inode_is_doomed(volume, *child);
}

/* Directories are never changed in place (docs/fs/SXFS_NAMESPACE.md). A
 * mutation reads the directory into volume.dir_scratch, changes it there, and
 * store_directory() writes the result to blocks nothing owned when the
 * mutation started; the directory's inode then points at the copy and the old
 * blocks are freed, all inside the mutation's metadata commit. A power cut
 * leaves either the old inode with the old blocks, untouched, or the new inode
 * with the new ones. */

// Reads every slot of `directory`, free ones included, into dir_scratch, in
// their on-disk positions.
Status load_directory_slots(Volume& volume, const Inode& directory, uint32_t& slots) {
    slots = 0;
    if (directory.type != kInodeTypeDirectory) {
        return Status::not_directory;
    }
    const uint32_t count = directory.size / static_cast<uint32_t>(sizeof(DirEntry));
    if (count > kMaxRecords) {
        return Status::io_error;
    }
    if (count != 0 &&
        !read_inode_bytes(volume, directory, 0, volume.dir_scratch, static_cast<size_t>(count) * sizeof(DirEntry))) {
        return Status::io_error;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (!dir_entry_is_valid(volume, volume.dir_scratch[index])) {
            return Status::io_error;
        }
    }
    slots = count;
    return Status::ok;
}

// Moves the live entries of dir_scratch to its front, in order, and returns
// how many there are. Free slots and entries of doomed inodes drop out.
uint32_t pack_directory_slots(Volume& volume, uint32_t slots) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < slots; ++index) {
        if (!dir_entry_is_live(volume, volume.dir_scratch[index])) {
            continue;
        }
        if (count != index) {
            volume.dir_scratch[count] = volume.dir_scratch[index];
        }
        ++count;
    }
    return count;
}

// The live entries of `directory`, packed, in dir_scratch.
Status load_directory(Volume& volume, const Inode& directory, uint32_t& count) {
    uint32_t slots = 0;
    const Status loaded = load_directory_slots(volume, directory, slots);
    count = loaded == Status::ok ? pack_directory_slots(volume, slots) : 0;
    return loaded;
}

uint32_t find_slot_by_name(const Volume& volume, uint32_t count, const char* name) {
    const size_t length = strlen(name);
    for (uint32_t index = 0; index < count; ++index) {
        const DirEntry& entry = volume.dir_scratch[index];
        if (entry.inode_id != 0 && entry.name_length == length && memcmp(entry.name, name, length) == 0) {
            return index;
        }
    }
    return kNoSlot;
}

uint32_t find_slot_by_inode(const Volume& volume, uint32_t count, uint32_t inode_id) {
    for (uint32_t index = 0; index < count; ++index) {
        if (volume.dir_scratch[index].inode_id == inode_id) {
            return index;
        }
    }
    return kNoSlot;
}

void remove_slot(Volume& volume, uint32_t& count, uint32_t slot) {
    for (uint32_t index = slot + 1u; index < count; ++index) {
        volume.dir_scratch[index - 1u] = volume.dir_scratch[index];
    }
    --count;
}

bool fill_slot(DirEntry& entry, uint32_t inode_id, uint16_t type, const char* name) {
    const size_t name_length = strlen(name);
    if (name_length == 0 || name_length > kMaxDirNameLength) {
        return false;
    }
    memset(&entry, 0, sizeof(entry));
    entry.inode_id = inode_id;
    entry.type = type;
    entry.name_length = static_cast<uint16_t>(name_length);
    memcpy(entry.name, name, name_length);
    return true;
}

// Writes the first `count` entries of dir_scratch as the new contents of
// `directory`, in blocks that were free when the mutation started, and frees
// the blocks it had. Must run between snapshot_metadata() and the commit: the
// commit's first ordering point is what puts the copy on the medium before the
// inode that names it. Also moves the directory's mtime, so a change of its
// entries is visible in one stat of it.
Status store_directory(Volume& volume, Inode& directory, uint32_t count) {
    const size_t size = static_cast<size_t>(count) * sizeof(DirEntry);
    const uint32_t sectors = static_cast<uint32_t>((size + block::kSectorSize - 1u) / block::kSectorSize);
    auto* bytes = reinterpret_cast<uint8_t*>(volume.dir_scratch);
    memset(bytes + size, 0, static_cast<size_t>(sectors) * block::kSectorSize - size);

    Inode fresh = directory;
    memset(fresh.extents, 0, sizeof(fresh.extents));
    fresh.extent_count = 0;
    if (sectors != 0 && !allocate_blocks(volume, fresh, sectors)) {
        return Status::no_space;
    }
    for (uint32_t index = 0; index < fresh.extent_count; ++index) {
        const Extent& extent = fresh.extents[index];
        if (!block::write_cached(volume.device_index, extent.start_lba, extent.sector_count, bytes)) {
            return Status::io_error;
        }
        bytes += static_cast<size_t>(extent.sector_count) * block::kSectorSize;
    }

    free_inode_blocks(volume, directory);
    memcpy(directory.extents, fresh.extents, sizeof(directory.extents));
    directory.extent_count = fresh.extent_count;
    directory.size = static_cast<uint32_t>(size);
    directory.mtime = rtc::epoch_seconds();
    return Status::ok;
}

// Whether `directory` has an entry that is part of the tree.
Status directory_is_empty(Volume& volume, const Inode& directory, bool& empty) {
    uint32_t count = 0;
    const Status loaded = load_directory(volume, directory, count);
    empty = count == 0;
    return loaded;
}

// Frees the slot of `inode_id` in `directory` where it is, with one
// single-sector write (SXFS_DIR_ENTRY_HEAD_BYTES). Only for an inode that is
// already doomed on disk: its entry no longer means anything, so there is no
// state to protect and no copy to make.
bool clear_slot_in_place(Volume& volume, const Inode& directory, uint32_t inode_id) {
    uint32_t slots = 0;
    if (load_directory_slots(volume, directory, slots) != Status::ok) {
        return false;
    }
    const uint32_t slot = find_slot_by_inode(volume, slots, inode_id);
    if (slot == kNoSlot) {
        return true;
    }
    uint32_t lba = 0;
    size_t sector_offset = 0;
    uint8_t sector[block::kSectorSize] = {};
    if (!locate_inode_offset(directory, static_cast<size_t>(slot) * sizeof(DirEntry), lba, sector_offset) ||
        sector_offset + SXFS_DIR_ENTRY_HEAD_BYTES > block::kSectorSize ||
        !block::read(volume.device_index, lba, 1, sector)) {
        return false;
    }
    memset(sector + sector_offset, 0, SXFS_DIR_ENTRY_HEAD_BYTES);
    return block::write_cached(volume.device_index, lba, 1, sector) && block::flush(volume.device_index);
}

bool build_record_recursive(Volume& volume, uint32_t directory_inode, uint32_t parent_inode,
                           const char* parent_relative, bool visited[kMaxInodes], uint32_t depth) {
    const Inode* directory = inode_for_id(volume, directory_inode);
    if (directory == nullptr || directory->type != kInodeTypeDirectory ||
        depth > kMaxDirectoryDepth || visited[directory_inode - 1]) {
        return false;
    }
    visited[directory_inode - 1] = true;

    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory->size; offset += sizeof(DirEntry)) {
        DirEntry entry = {};
        if (!read_dir_entry(volume, *directory, offset, entry)) {
            return false;
        }
        if (entry.inode_id == 0 || entry.name_length == 0) {
            continue;
        }

        const Inode* child = inode_for_id(volume, entry.inode_id);
        if (child == nullptr || child->type == kInodeTypeUnused) {
            return false;
        }
        if (inode_is_doomed(volume, *child)) {
            continue; // its deletion is committed; a writable mount removes the entry
        }

        sxfs::FileRecord* record = allocate_record(volume);
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

        if (record->directory &&
            !build_record_recursive(volume, record->inode_id, record->inode_id, record->path, visited, depth + 1)) {
            return false;
        }
    }

    return true;
}

bool rebuild_record_cache(Volume& volume) {
    memset(volume.records, 0, sizeof(volume.records));
    bool visited[kMaxInodes] = {};
    return build_record_recursive(volume, kRootInodeId, kRootInodeId, "", visited, 0);
}

void refresh_record_size(Volume& volume, uint32_t inode_id) {
    sxfs::FileRecord* record = find_record_by_inode(volume, inode_id);
    Inode* inode = inode_for_id(volume, inode_id);
    if (record != nullptr && inode != nullptr) {
        record->size = inode->size;
        if (record->vnode != nullptr) {
            record->vnode->size = inode->size;
        }
    }
}

// The inode's ownership on the vnode that publishes it, which is where the
// VFS checks permissions.
void publish_ownership(Volume& volume, uint32_t inode_id, vfs::Vnode* node) {
    const Inode* inode = inode_for_id(volume, inode_id);
    if (node != nullptr && inode != nullptr) {
        vfs::set_ownership(*node, inode->uid, inode->gid, inode->mode);
        node->mtime = inode->mtime;
    }
}

void mount_record(Volume& volume, sxfs::FileRecord& record) {
    char absolute[262] = {};
    if (!make_absolute_path(volume, record.path, absolute, sizeof(absolute))) {
        return;
    }

    if (record.directory) {
        record.vnode = vfs::ensure_directory(absolute);
    } else {
        record.vnode = vfs::install_volume_file(
            absolute,
            sxfs::driver(),
            &record,
            record.size,
            volume.status != sxfs::MountStatus::read_only);
    }
    publish_ownership(volume, record.inode_id, record.vnode);
}

bool initialize_record_mounts(Volume& volume) {
    vfs::Vnode* root = vfs::ensure_directory(volume.mount_point);
    if (root == nullptr) {
        return false;
    }
    volume.root_vnode = root;
    publish_ownership(volume, kRootInodeId, root);

    for (sxfs::FileRecord& record : volume.records) {
        if (record.in_use && record.directory) {
            mount_record(volume, record);
            if (record.vnode == nullptr) {
                return false;
            }
        }
    }

    for (sxfs::FileRecord& record : volume.records) {
        if (record.in_use && !record.directory) {
            mount_record(volume, record);
            if (record.vnode == nullptr) {
                return false;
            }
        }
    }

    return true;
}

bool record_is_ancestor(Volume& volume, uint32_t ancestor_inode, uint32_t inode_id) {
    uint32_t current = inode_id;
    while (current != 0 && current != kRootInodeId) {
        if (current == ancestor_inode) {
            return true;
        }
        const sxfs::FileRecord* record = find_record_by_inode(volume, current);
        current = record != nullptr ? record->parent_inode_id : 0;
    }
    return ancestor_inode == kRootInodeId;
}

// `relative` is where the inode is being created and `owner` who creates it.
// Without an owner it gets the default ownership of its path.
bool prepare_new_inode(Inode& inode, uint32_t inode_id, uint16_t type, const char* relative,
                       const vfs::Ownership* owner) {
    memset(&inode, 0, sizeof(inode));
    inode.inode_id = inode_id;
    inode.type = type;
    inode.link_count = 1;
    inode.mtime = rtc::epoch_seconds();
    if (owner != nullptr && sxfs_mode_matches_type(owner->mode, type)) {
        inode.uid = owner->uid;
        inode.gid = owner->gid;
        inode.mode = owner->mode;
    } else {
        sxfs_default_ownership(relative, type, &inode.uid, &inode.gid, &inode.mode);
    }
    return true;
}

// Gives every reachable inode without recorded ownership (mode 0) the default
// of its path. Returns how many it changed.
uint32_t apply_default_ownership(Volume& volume) {
    uint32_t changed = 0;
    Inode* root = inode_for_id(volume, kRootInodeId);
    if (root != nullptr && root->mode == 0) {
        sxfs_default_ownership("", root->type, &root->uid, &root->gid, &root->mode);
        ++changed;
    }
    for (sxfs::FileRecord& record : volume.records) {
        Inode* inode = record.in_use ? inode_for_id(volume, record.inode_id) : nullptr;
        if (inode != nullptr && inode->mode == 0) {
            sxfs_default_ownership(record.path, inode->type, &inode->uid, &inode->gid, &inode->mode);
            ++changed;
        }
    }
    return changed;
}

// SXFS_FEATURE_OWNERSHIP on mount (USERS_AND_PERMISSIONS.md, "What each inode
// stores"). A volume without the feature, or with inodes a driver that ignores
// it created, gets the defaults in one journalled commit, and the feature set.
// On a read-only mount the defaults are applied in memory only.
void stamp_ownership(Volume& volume) {
    const bool recorded = (volume.superblock.features & SXFS_FEATURE_OWNERSHIP) != 0;
    if (volume.status != sxfs::MountStatus::mounted) {
        (void)apply_default_ownership(volume);
        return;
    }

    MutationGuard guard(volume);
    snapshot_metadata(volume);
    const uint32_t changed = apply_default_ownership(volume);
    if (recorded && changed == 0) {
        volume.snapshot_valid = false;
        return;
    }
    volume.superblock.features |= SXFS_FEATURE_OWNERSHIP;
    if (changed == 0) {
        // Only the superblock changes, and a commit writes nothing when no
        // metadata sector did.
        volume.snapshot_valid = false;
        (void)write_superblocks(volume, volume.superblock.sequence, kFlagClean, Durability::durable);
        return;
    }
    if (!commit_metadata(volume)) {
        // The snapshot holds the superblock too, so this drops the feature.
        restore_metadata(volume);
        (void)apply_default_ownership(volume);
    }
}

// SXFS_FEATURE_LINK_COUNT on a writable mount: a volume without it gets 1 in
// every allocated inode that holds 0, and only once that is down, the bit.
// Two steps on purpose: the bit is in the superblock, which is not journalled,
// and a bit that reached the disk before the counts would doom every inode
// that still had a 0.
void adopt_link_counts(Volume& volume) {
    if (volume.status != sxfs::MountStatus::mounted ||
        (volume.superblock.features & SXFS_FEATURE_LINK_COUNT) != 0) {
        return;
    }
    MutationGuard guard(volume);
    snapshot_metadata(volume);
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        Inode& inode = volume.inodes[inode_id - 1];
        if (inode.type != kInodeTypeUnused && inode.link_count == 0) {
            inode.link_count = 1;
        }
    }
    if (!commit_metadata(volume)) {
        restore_metadata(volume);
        volume.snapshot_valid = false;
        return;
    }
    volume.superblock.features |= SXFS_FEATURE_LINK_COUNT;
    if (!write_superblocks(volume, volume.superblock.sequence, kFlagClean, Durability::durable)) {
        volume.superblock.features &= ~static_cast<uint32_t>(SXFS_FEATURE_LINK_COUNT);
    }
}

// Finishes the deletions a power cut left committed but not complete: clears
// the slots that still name a doomed inode, then frees every doomed inode in
// one commit. In that order, so a cut in the middle leaves doomed inodes
// nothing names, never an entry naming a free inode.
void finish_doomed(Volume& volume) {
    if (volume.status != sxfs::MountStatus::mounted ||
        (volume.superblock.features & SXFS_FEATURE_LINK_COUNT) == 0) {
        return;
    }
    bool any = false;
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes && !any; ++inode_id) {
        any = inode_is_doomed(volume, volume.inodes[inode_id - 1]);
    }
    if (!any) {
        return;
    }

    MutationGuard guard(volume);
    for (uint32_t directory_id = 1; directory_id <= kMaxInodes; ++directory_id) {
        const Inode& directory = volume.inodes[directory_id - 1];
        if (directory.type != kInodeTypeDirectory || inode_is_doomed(volume, directory)) {
            continue;
        }
        for (;;) {
            uint32_t slots = 0;
            if (load_directory_slots(volume, directory, slots) != Status::ok) {
                return; // a damaged directory: nothing is freed under it
            }
            uint32_t doomed_id = 0;
            for (uint32_t index = 0; index < slots && doomed_id == 0; ++index) {
                const DirEntry& entry = volume.dir_scratch[index];
                if (entry.inode_id != 0 && !dir_entry_is_live(volume, entry)) {
                    doomed_id = entry.inode_id;
                }
            }
            if (doomed_id == 0) {
                break;
            }
            if (!clear_slot_in_place(volume, directory, doomed_id)) {
                return;
            }
        }
    }

    snapshot_metadata(volume);
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        Inode& inode = volume.inodes[inode_id - 1];
        if (inode_is_doomed(volume, inode)) {
            free_inode_blocks(volume, inode);
            release_inode_id(volume, inode_id);
        }
    }
    if (!commit_metadata(volume)) {
        restore_metadata(volume);
        volume.snapshot_valid = false;
    }
}

bool load_filesystem_from_device(Volume& volume, size_t device_index) {
    Superblock primary = {};
    Superblock secondary = {};
    const bool primary_ok = block::read(device_index, kPrimarySuperblockLba, 1, &primary) && valid_superblock(primary);
    const bool secondary_ok = block::read(device_index, kSecondarySuperblockLba, 1, &secondary) && valid_superblock(secondary);
    if (!primary_ok && !secondary_ok) {
        return false;
    }

    volume.superblock = (!secondary_ok || (primary_ok && primary.sequence >= secondary.sequence)) ? primary : secondary;
    volume.device_index = device_index;
    block::DeviceInfo device_info = {};
    // Home metadata is validated only after the journal is replayed: a crash
    // in the middle of writing it home leaves it torn until then.
    if (!block::device_info(device_index, device_info) || volume.superblock.total_sectors > device_info.sector_count ||
        !read_home_metadata(volume)) {
        return false;
    }
    const RecoveryState recovery = recover_journal(volume);
    if (recovery == RecoveryState::failed || !home_metadata_is_valid(volume)) {
        return false;
    }
    volume.status = recovery == RecoveryState::read_only ? sxfs::MountStatus::read_only : sxfs::MountStatus::mounted;
    adopt_link_counts(volume);
    finish_doomed(volume);
    if (!rebuild_record_cache(volume)) {
        volume.status = sxfs::MountStatus::unavailable;
        return false;
    }
    volume.metadata_ready = true;
    stamp_ownership(volume);
    return true;
}

} // namespace

namespace sxfs {

void initialize() {
    memset(g_volumes, 0, sizeof(g_volumes));
}

VolumeId probe(size_t device_index, const char* mount_point) {
    if (mount_point == nullptr || *mount_point != '/') {
        return kInvalidVolume;
    }
    const size_t length = strlen(mount_point);
    if (length == 0 || length >= kMountPointCapacity) {
        return kInvalidVolume;
    }

    block::DeviceInfo info = {};
    if (!block::device_info(device_index, info) || !info.present || info.sector_count <= kDataLba) {
        return kInvalidVolume;
    }

    for (Volume& volume : g_volumes) {
        if (volume.in_use) {
            continue;
        }
        memset(&volume, 0, sizeof(volume));
        memcpy(volume.mount_point, mount_point, length + 1);
        volume.in_use = true;
        if (!load_filesystem_from_device(volume, device_index)) {
            // No es un SxFS (o esta roto): el slot vuelve a quedar libre y
            // fs:: sigue probando con el driver siguiente.
            memset(&volume, 0, sizeof(volume));
            return kInvalidVolume;
        }
        return volume_index(volume);
    }
    return kInvalidVolume;
}

bool attach(VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr || !volume->metadata_ready) {
        return false;
    }
    if (!initialize_record_mounts(*volume)) {
        return false;
    }
    // probe() ya fijo el status segun como salio la recuperacion del journal, y
    // attach() solo publica vnodes: no puede volver escribible a un volumen que
    // quedo en read_only porque la recuperacion no se pudo persistir. Promoverlo
    // partia el driver en dos mitades que no coincidian, porque mount_record()
    // corre antes y ya habia publicado los vnodes con writable=false mientras
    // sxfs::writable(volume) devolvia true.
    if (volume->status != MountStatus::read_only) {
        volume->status = MountStatus::mounted;
    }
    return true;
}

VolumeId root() {
    for (Volume& volume : g_volumes) {
        if (volume.in_use && strcmp(volume.mount_point, kRootMountPoint) == 0) {
            return volume_index(volume);
        }
    }
    return kInvalidVolume;
}

MountStatus status(VolumeId id) {
    const Volume* volume = volume_for_id(id);
    return volume != nullptr ? volume->status : MountStatus::unavailable;
}

bool mounted(VolumeId id) {
    const MountStatus state = status(id);
    return state == MountStatus::mounted || state == MountStatus::read_only;
}

bool writable(VolumeId id) {
    return status(id) == MountStatus::mounted;
}

size_t file_count(VolumeId id) {
    const Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return 0;
    }
    size_t count = 0;
    for (const FileRecord& record : volume->records) {
        if (record.in_use && !record.directory) {
            ++count;
        }
    }
    return count;
}

uint64_t total_bytes(VolumeId id) {
    const Volume* volume = volume_for_id(id);
    if (volume == nullptr || !volume->metadata_ready ||
        volume->superblock.total_sectors <= volume->superblock.data_lba) {
        return 0;
    }
    return static_cast<uint64_t>(volume->superblock.total_sectors - volume->superblock.data_lba) *
        block::kSectorSize;
}

uint64_t used_bytes(VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr || !volume->metadata_ready) {
        return 0;
    }
    return static_cast<uint64_t>(allocated_data_sector_count(*volume)) * block::kSectorSize;
}

uint64_t free_bytes(VolumeId id) {
    const uint64_t total = total_bytes(id);
    const uint64_t used = used_bytes(id);
    return total >= used ? (total - used) : 0;
}

// --- Informe de consistencia (sxfs::check) ---------------------------------
//
// Todo lo que sigue es de solo lectura. No escribe un byte, no toca el bitmap
// en disco y no repara nada: la intencion es que se pueda correr con el volumen
// montado y que el peor caso de una imagen corrupta sea un informe, no un
// segunda lesion.
//
// Dos decisiones que conviene no volver a discutir:
//
// 1. `extent_claims` se reutiliza como scratch. home_metadata_is_valid lo
//    borra y lo rellena en cada llamada y nadie lo lee fuera de ahi, asi que
//    no hay estado que preservar y el informe no cuesta BSS.
// 2. El recorrido del arbol es ITERATIVO con una pila explicita, y la deteccion
//    de nombres repetidos relee las entradas anteriores en vez de copiar el
//    directorio a memoria. El stack del kernel son 8 paginas (32 KiB,
//    kKernelStackPages): el arreglo de 255 entradas que usa el validador del host
//    son 20 KiB y con recursion eso no entra. Releer es O(n^2) lecturas sobre un
//    directorio que el formato limita a 255 entradas, a cambio de 0 bytes.

namespace {

// Recorre el arbol desde la raiz y deja en `visited` el conjunto de inodes
// alcanzables. Cuenta la poblacion que ve y los defectos de estructura.
void walk_reachable(Volume& volume, uint32_t root_id, uint8_t* visited, CheckReport& report) {
    uint32_t pending[kMaxDirectoryDepth];
    uint32_t depth = 0;

    if (!bitmap_test(visited, root_id - 1u)) {
        bitmap_set(visited, root_id - 1u, true);
        pending[depth++] = root_id;
    }

    while (depth != 0) {
        const uint32_t dir_id = pending[--depth];
        const Inode* directory = inode_for_id(volume, dir_id);
        if (directory == nullptr || directory->type != kInodeTypeDirectory) {
            ++report.unreadable_dirs;
            continue;
        }
        // Un directorio se cuenta cuando se lo visita, no cuando se lo empuja:
        // la raiz y los subdirectorios entran por la misma pila, y el conteo va
        // en el mismo lugar para los dos.
        ++report.directories;

        for (size_t offset = 0; offset + sizeof(DirEntry) <= directory->size;
             offset += sizeof(DirEntry)) {
            DirEntry entry = {};
            if (!read_dir_entry(volume, *directory, offset, entry)) {
                ++report.bad_dir_entries;
                continue;
            }
            if (entry.inode_id == 0) {
                continue; // a free slot
            }
            if (!dir_entry_is_live(volume, entry)) {
                continue; // names a doomed inode: counted as a pending deletion
            }

            // Nombre repetido dentro de ESTE directorio. Se releen las entradas
            // anteriores en vez de bufferizar: el directorio no entra en el stack.
            for (size_t previous = 0; previous < offset; previous += sizeof(DirEntry)) {
                DirEntry other = {};
                if (!read_dir_entry(volume, *directory, previous, other) ||
                    !dir_entry_is_live(volume, other)) {
                    continue;
                }
                if (other.name_length == entry.name_length &&
                    memcmp(other.name, entry.name, entry.name_length) == 0) {
                    ++report.duplicate_names;
                    break;
                }
            }

            // Dos caminos al mismo inodo. SxFS no tiene hard links: create_file
            // escribe link_count = 1 y nadie lo sube, asi que un segundo camino
            // es un defecto, no una caracteristica. El host sxfs-walk falla la
            // imagen por esto mismo.
            if (bitmap_test(visited, entry.inode_id - 1u)) {
                ++report.alias_inodes;
                continue; // no se sigue dos veces
            }
            bitmap_set(visited, entry.inode_id - 1u, true);

            if (entry.type == kInodeTypeDirectory) {
                if (depth < kMaxDirectoryDepth) {
                    pending[depth++] = entry.inode_id;
                } else {
                    ++report.unreadable_dirs; // mas profundo que el limite del formato
                }
            } else if (entry.type == kInodeTypeFile) {
                ++report.files;
            }
        }
    }
}

} // namespace

bool check(VolumeId id, CheckReport& report) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr || !volume->metadata_ready) {
        return false;
    }
    memset(&report, 0, sizeof(report));

    // El guard serializa contra los que mutan el volumen. Es un spinlock sin
    // espera activa, y el recorrido hace lecturas de disco: se acepta el coste
    // porque un informe a mitad de una mutacion daria una respuesta falsa.
    MutationGuard guard(*volume);

    const Superblock& superblock = volume->superblock;
    report.total_sectors = superblock.total_sectors;
    report.data_lba = superblock.data_lba;
    report.sequence = superblock.sequence;
    report.clean_shutdown = (superblock.flags & kFlagClean) != 0 ? 1u : 0u;
    report.ownership_recorded = (superblock.features & SXFS_FEATURE_OWNERSHIP) != 0 ? 1u : 0u;
    report.link_counts_recorded = (superblock.features & SXFS_FEATURE_LINK_COUNT) != 0 ? 1u : 0u;
    report.metadata_commits = volume->metadata_commits;
    report.metadata_bytes_written = volume->metadata_bytes_written;

    // clear_journal() borra el header a cero, y sxfs_journal_valid() exige la
    // magia: un journal limpio NO valida contra el. "Todo cero" es el estado
    // normal de un volumen sano, no un defecto, asi que el header solo se
    // interpreta cuando trae la magia, que es cuando hay algo que decir.
    JournalHeader journal = {};
    if (block::read(volume->device_index, superblock.journal_lba, 1, &journal) &&
        sxfs_magic_equals(journal.magic, sxfs_journal_magic)) {
        // Magia presente: hay transaccion o hay header danado. El checksum lo
        // separa. Sin validar no se puede confiar en `pending`.
        report.journal_valid = valid_journal(journal) ? 1u : 0u;
        report.journal_pending = valid_journal(journal) ? journal.pending : 1u;
    }

    // Solo el numero de inodos asignados. Los archivos y directorios se cuentan
    // en walk_reachable, que es el unico lugar donde "cuantos hay" y "cuantos
    // son alcanzables" se distinguen: inodes_allocated es el total del volumen y
    // files + directories es lo que la raiz puede ver. La diferencia son los
    // huerfanos, que se cuentan mas abajo.
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        if (bitmap_test(volume->inode_bitmap, inode_id - 1u)) {
            ++report.inodes_allocated;
        }
    }

    // Quien reclama cada sector. Mismo recorrido que home_metadata_is_valid pero
    // contando en vez de fallar, y con la comparacion contra el bitmap que
    // allows las dos direcciones.
    memset(volume->extent_claims, 0, sizeof(volume->extent_claims));
    for (uint32_t inode_id = 1; inode_id <= kMaxInodes; ++inode_id) {
        const Inode& inode = volume->inodes[inode_id - 1];
        for (uint32_t index = 0; index < inode.extent_count; ++index) {
            const Extent& extent = inode.extents[index];
            for (uint32_t sector = 0; sector < extent.sector_count; ++sector) {
                const uint32_t lba = extent.start_lba + sector;
                if (lba >= SXFS_MAX_TOTAL_SECTORS) {
                    // No deberia pasar: montar ya valida los extents. Se cuenta
                    // como bloque perdido para nosnow un finding con el bitmap.
                    ++report.lost_blocks;
                    continue;
                }
                if (bitmap_test(volume->extent_claims, lba)) {
                    ++report.double_claimed_blocks;
                    continue;
                }
                bitmap_set(volume->extent_claims, lba, true);
            }
        }
    }

    // Region de datos: la reconciliacion que no hacia nadie. Un bloque ocupado
    // en el bitmap sin dueno es una fuga; un bloque reclamado y libre en el
    // bitmap es lo que permitiria una doble asignacion.
    for (uint32_t sector = superblock.data_lba; sector < superblock.total_sectors; ++sector) {
        ++report.data_sectors;
        const bool used = bitmap_test(volume->block_bitmap, sector);
        const bool claimed = bitmap_test(volume->extent_claims, sector);
        if (used) {
            ++report.data_used_sectors;
            if (!claimed) {
                ++report.leaked_blocks;
            }
        }
        if (claimed) {
            ++report.data_claimed_sectors;
            if (!used) {
                ++report.lost_blocks;
            }
        }
    }

    // La region de metadata deberia estar reservada. sxfs_find_free_run arranca en
    // data_lba, asi que un sector libre aca no se puede entregar: es una
    // inconsistencia que reportar, no un corruption activa.
    for (uint32_t sector = 0; sector < superblock.data_lba; ++sector) {
        if (!bitmap_test(volume->block_bitmap, sector)) {
            ++report.metadata_unmarked;
        }
    }

    // Recorrido desde la raiz. `visited` es un bitmap de kMaxInodes bits en
    // memoria de pila: 32 bytes, contra los 20 KiB que necesitaria una copia de
    // las entradas de un directorio.
    uint8_t visited[(kMaxInodes + 7u) / 8u] = {};
    walk_reachable(*volume, kRootInodeId, visited, report);

    // El invariante de la reconciliacion de inodos: lo que el bitmap de inodos
    // tiene asignado es exactamente lo que el recorrido alcanzo mas lo que no.
    // Un inodo huerfano es la diferencia entre ambos, y por eso se cuenta
    // DESPUES de recorrer y no durante.
    for (uint32_t inode_id = kRootInodeId + 1u; inode_id <= kMaxInodes; ++inode_id) {
        if (!bitmap_test(volume->inode_bitmap, inode_id - 1u)) {
            continue;
        }
        if (inode_is_doomed(*volume, volume->inodes[inode_id - 1u])) {
            // A deletion that is committed and not finished: a file still open
            // when it was unlinked, or one a power cut interrupted. Not a
            // finding: the last close or the next writable mount frees it.
            ++report.pending_deletions;
        } else if (!bitmap_test(visited, inode_id - 1u)) {
            ++report.orphan_inodes;
        }
    }

    // Ownership is checked only where it is recorded, and only on what the
    // root reaches: an orphan is already a finding of its own.
    if (report.ownership_recorded != 0) {
        for (uint32_t inode_id = kRootInodeId; inode_id <= kMaxInodes; ++inode_id) {
            const Inode& inode = volume->inodes[inode_id - 1];
            if (bitmap_test(visited, inode_id - 1u) &&
                (inode.mode == 0 || !sxfs_mode_matches_type(inode.mode, inode.type))) {
                ++report.unowned_inodes;
            }
        }
    }

    return true;
}

bool sync(VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr || !volume->metadata_ready) {
        return false;
    }
    if (volume->status != MountStatus::mounted) {
        return true;
    }
    MutationGuard guard(*volume);
    return write_superblocks(*volume, volume->superblock.sequence, kFlagClean, Durability::durable) &&
        clear_journal(*volume, Durability::durable);
}
bool sync_deferred(VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (!volume || !volume->metadata_ready) return false;
    if (volume->status != MountStatus::mounted) return true;
    MutationGuard guard(*volume);
    return write_superblocks(*volume,volume->superblock.sequence,kFlagClean,Durability::cached) &&
        clear_journal(*volume,Durability::cached) && block::flush(volume->device_index);
}

bool read_file(FileRecord& file, size_t offset, void* buffer, size_t count) {
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr) {
        return false;
    }
    Inode* inode = inode_for_id(*volume, file.inode_id);
    if (!file.in_use || file.directory || inode == nullptr || buffer == nullptr) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (offset + count > inode->size) {
        return false;
    }
    return read_inode_bytes(*volume, *inode, offset, buffer, count);
}

namespace {

// A directory's mtime moves with its entries (store_directory); this carries
// it to the vnode that publishes the directory.
void refresh_directory_vnode(Volume& volume, uint32_t inode_id) {
    vfs::Vnode* node = nullptr;
    if (inode_id == kRootInodeId) {
        node = volume.root_vnode;
    } else if (FileRecord* record = find_record_by_inode(volume, inode_id)) {
        node = record->vnode;
    }
    const Inode* inode = inode_for_id(volume, inode_id);
    if (node != nullptr && inode != nullptr) {
        node->mtime = inode->mtime;
    }
}

// The deletion of `victim_id` when its parent cannot be rewritten for lack of
// room: instead of a copy of the directory, three steps that each leave a
// state a mount completes (docs/fs/SXFS_NAMESPACE.md, "Deleting on a full
// volume").
//   1. One commit dooms the inode (link_count 0) and frees its blocks. This is
//      the point of no return: from here every mount shows the name gone.
//   2. Its slot in the parent is cleared in place, one single-sector write.
//   3. One commit frees the inode.
// A failure after step 1 is not reported: the deletion is committed, and the
// next writable mount does what was left.
Status doom_in_place(Volume& volume, uint32_t victim_id, uint32_t parent_id) {
    Inode* victim = inode_for_id(volume, victim_id);
    Inode* parent = inode_for_id(volume, parent_id);
    if (victim == nullptr || parent == nullptr) {
        return Status::io_error;
    }
    snapshot_metadata(volume);
    victim->link_count = 0;
    free_inode_blocks(volume, *victim);
    victim->size = 0;
    if (!commit_metadata(volume)) {
        restore_metadata(volume);
        return Status::io_error;
    }
    if (clear_slot_in_place(volume, *parent, victim_id)) {
        snapshot_metadata(volume);
        release_inode_id(volume, victim_id);
        if (!commit_metadata(volume)) {
            restore_metadata(volume);
        }
    }
    return Status::ok;
}

} // namespace

Status write_file_status(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate,
                         size_t& written) {
    written = 0;
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || !file.in_use || file.directory) {
        return Status::invalid;
    }
    if (volume->status != MountStatus::mounted) {
        return Status::read_only;
    }

    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, file.inode_id);
    if (inode == nullptr || inode->type != kInodeTypeFile) {
        return Status::invalid;
    }
    if (truncate) {
        offset = 0;
    }
    // A file's size is 32 bits on disk.
    if (offset > UINT32_MAX || count > UINT32_MAX - offset) {
        return Status::no_space;
    }

    const size_t required_size = count == 0 ? (truncate ? 0 : inode->size) : (offset + count);
    const bool needs_metadata = truncate || required_size > inode->size ||
        required_size > inode_capacity_bytes(*inode);
    if (count != 0 && buffer == nullptr) {
        return Status::invalid;
    }

    const uint64_t now = rtc::epoch_seconds();
    if (needs_metadata) {
        snapshot_metadata(*volume);
        if (truncate) {
            shrink_inode_to_sectors(*volume, *inode, 0);
            inode->size = 0;
        }
        // The blocks a truncating write frees still belong to the state before
        // it, so the new contents go elsewhere and the old ones are whole
        // until the commit. Only when there is no elsewhere is the truncation
        // committed on its own, and the write then reuses what it freed.
        Status grown = ensure_inode_capacity(*volume, *inode, required_size);
        if (grown == Status::no_space && truncate && required_size != 0) {
            inode->mtime = now;
            if (!commit_metadata(*volume)) {
                restore_metadata(*volume);
                return Status::io_error;
            }
            refresh_record_size(*volume, file.inode_id);
            snapshot_metadata(*volume);
            grown = ensure_inode_capacity(*volume, *inode, required_size);
        }
        if (grown != Status::ok) {
            restore_metadata(*volume);
            return grown;
        }
        if (offset > inode->size && !zero_inode_range(*volume, *inode, inode->size, offset - inode->size)) {
            restore_metadata(*volume);
            return Status::io_error;
        }
    }

    if (count != 0) {
        if (!write_inode_bytes(*volume, *inode, offset, buffer, count)) {
            if (needs_metadata) {
                restore_metadata(*volume);
            }
            return Status::io_error;
        }
        written = count;
    }

    if (needs_metadata) {
        const size_t new_size = count == 0
            ? inode->size
            : ((offset + written) > inode->size ? (offset + written) : inode->size);
        inode->size = static_cast<uint32_t>(new_size);
        inode->mtime = now;
        if (!commit_metadata(*volume)) {
            restore_metadata(*volume);
            written = 0;
            return Status::io_error;
        }
    } else if (count != 0 && !block::flush(volume->device_index)) {
        // An overwrite in place commits nothing, so it places its own
        // ordering point after its data.
        written = 0;
        return Status::io_error;
    } else if (count != 0 && inode->mtime != now) {
        // The modification time of an overwrite in place. Only when the second
        // changed: a burst of small writes commits once a second, not once a
        // write. A failure here loses the time, not the data, which is down.
        snapshot_metadata(*volume);
        inode->mtime = now;
        if (!commit_metadata(*volume)) {
            restore_metadata(*volume);
        }
    }

    refresh_record_size(*volume, file.inode_id);
    return Status::ok;
}

bool write_file(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written) {
    return write_file_status(file, offset, buffer, count, truncate, written) == Status::ok;
}

Status truncate_file_status(FileRecord& file, size_t size) {
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || !file.in_use || file.directory) {
        return Status::invalid;
    }
    if (volume->status != MountStatus::mounted) {
        return Status::read_only;
    }
    if (size > UINT32_MAX) {
        return Status::no_space;
    }

    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, file.inode_id);
    if (inode == nullptr || inode->type != kInodeTypeFile) {
        return Status::invalid;
    }

    snapshot_metadata(*volume);
    if (size > inode->size) {
        const Status grown = ensure_inode_capacity(*volume, *inode, size);
        if (grown != Status::ok) {
            restore_metadata(*volume);
            return grown;
        }
        if (!zero_inode_range(*volume, *inode, inode->size, size - inode->size)) {
            restore_metadata(*volume);
            return Status::io_error;
        }
    } else {
        const uint32_t keep_sectors = size == 0
            ? 0
            : static_cast<uint32_t>((size + block::kSectorSize - 1) / block::kSectorSize);
        shrink_inode_to_sectors(*volume, *inode, keep_sectors);
    }

    inode->size = static_cast<uint32_t>(size);
    inode->mtime = rtc::epoch_seconds();
    if (!commit_metadata(*volume)) {
        restore_metadata(*volume);
        return Status::io_error;
    }
    refresh_record_size(*volume, file.inode_id);
    return Status::ok;
}

bool truncate_file(FileRecord& file, size_t size) {
    return truncate_file_status(file, size) == Status::ok;
}

Status remove_status(FileRecord& file);

Status create_status(const char* path, bool directory, const vfs::Ownership* owner, FileRecord*& created) {
    created = nullptr;
    Volume* volume = volume_for_path(path);
    if (volume == nullptr) {
        return Status::not_found;
    }
    if (volume->status != MountStatus::mounted) {
        return Status::read_only;
    }
    char relative[256] = {};
    char parent_relative[256] = {};
    char leaf[64] = {};
    if (!parse_relative_path(*volume, path, relative, sizeof(relative)) ||
        !split_relative_parent(relative, parent_relative, sizeof(parent_relative), leaf, sizeof(leaf))) {
        return Status::invalid;
    }
    if (find_record_by_relative_path(*volume, relative) != nullptr) {
        return Status::exists;
    }

    const uint32_t parent_inode = parent_inode_for_relative_path(*volume, parent_relative);
    Inode* parent = inode_for_id(*volume, parent_inode);
    if (parent == nullptr) {
        return Status::not_found;
    }
    if (parent->type != kInodeTypeDirectory) {
        return Status::not_directory;
    }

    const uint16_t type = directory ? kInodeTypeDirectory : kInodeTypeFile;
    FileRecord* record = nullptr;
    {
        MutationGuard guard(*volume);
        // The record is taken before anything is committed: a file on disk the
        // driver has no record for would exist until the next mount only.
        record = allocate_record(*volume);
        if (record == nullptr) {
            return Status::no_space;
        }
        snapshot_metadata(*volume);

        Status status = Status::ok;
        uint32_t count = 0;
        const uint32_t inode_id = allocate_inode_id(*volume);
        Inode* inode = inode_for_id(*volume, inode_id);
        if (inode_id == 0 || inode == nullptr) {
            status = Status::no_space;
        } else {
            (void)prepare_new_inode(*inode, inode_id, type, relative, owner);
            status = load_directory(*volume, *parent, count);
        }
        if (status == Status::ok && find_slot_by_name(*volume, count, leaf) != kNoSlot) {
            status = Status::exists;
        }
        if (status == Status::ok && count >= kMaxRecords) {
            status = Status::no_space;
        }
        if (status == Status::ok && !fill_slot(volume->dir_scratch[count], inode_id, type, leaf)) {
            status = Status::invalid;
        }
        if (status == Status::ok) {
            status = store_directory(*volume, *parent, count + 1u);
        }
        if (status == Status::ok && !commit_metadata(*volume)) {
            status = Status::io_error;
        }
        if (status != Status::ok) {
            restore_metadata(*volume);
            volume->snapshot_valid = false;
            memset(record, 0, sizeof(*record));
            return status;
        }

        record->directory = directory;
        record->inode_id = inode_id;
        record->parent_inode_id = parent_inode;
        record->size = 0;
        strcpy(record->name, leaf);
        strcpy(record->path, relative);
        if (mounted(volume_index(*volume))) {
            mount_record(*volume, *record);
        }
        refresh_directory_vnode(*volume, parent_inode);
    }

    // An attached volume whose tree has no room for the vnode: the file is
    // taken back off the disk, so the disk never holds what the tree cannot
    // show.
    if (volume->root_vnode != nullptr && record->vnode == nullptr) {
        (void)remove_status(*record);
        return Status::no_space;
    }
    created = record;
    return Status::ok;
}

FileRecord* create_file(const char* path, const vfs::Ownership* owner) {
    FileRecord* record = nullptr;
    (void)create_status(path, /*directory=*/false, owner, record);
    return record;
}

FileRecord* create_directory(const char* path, const vfs::Ownership* owner) {
    FileRecord* record = nullptr;
    (void)create_status(path, /*directory=*/true, owner, record);
    return record;
}

// Removes a file or an empty directory: its entry leaves the parent, and its
// inode and blocks are freed, in one commit.
Status remove_status(FileRecord& file) {
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || !file.in_use) {
        return Status::invalid;
    }
    if (volume->status != MountStatus::mounted) {
        return Status::read_only;
    }

    MutationGuard guard(*volume);
    const uint32_t inode_id = file.inode_id;
    const uint32_t parent_id = file.parent_inode_id;
    Inode* inode = inode_for_id(*volume, inode_id);
    Inode* parent = inode_for_id(*volume, parent_id);
    if (inode == nullptr || parent == nullptr ||
        inode->type != (file.directory ? kInodeTypeDirectory : kInodeTypeFile)) {
        return Status::invalid;
    }
    if (file.directory) {
        bool empty = false;
        const Status scanned = directory_is_empty(*volume, *inode, empty);
        if (scanned != Status::ok) {
            return scanned;
        }
        if (!empty) {
            return Status::not_empty;
        }
    }

    snapshot_metadata(*volume);
    uint32_t count = 0;
    Status status = load_directory(*volume, *parent, count);
    if (status == Status::ok) {
        const uint32_t slot = find_slot_by_inode(*volume, count, inode_id);
        if (slot == kNoSlot) {
            status = Status::io_error; // a record no entry backs
        } else {
            remove_slot(*volume, count, slot);
            status = store_directory(*volume, *parent, count);
        }
    }
    if (status == Status::ok) {
        free_inode_blocks(*volume, *inode);
        release_inode_id(*volume, inode_id);
        if (!commit_metadata(*volume)) {
            status = Status::io_error;
        }
    }
    if (status != Status::ok) {
        restore_metadata(*volume);
        volume->snapshot_valid = false;
        if (status != Status::no_space ||
            (volume->superblock.features & SXFS_FEATURE_LINK_COUNT) == 0) {
            return status;
        }
        // No room for the parent's copy. Deleting must not need free space.
        status = doom_in_place(*volume, inode_id, parent_id);
        if (status != Status::ok) {
            return status;
        }
    }
    remove_record(*volume, inode_id);
    refresh_directory_vnode(*volume, parent_id);
    return Status::ok;
}

bool remove_directory(FileRecord& file) {
    return file.in_use && file.directory && remove_status(file) == Status::ok;
}

bool unlink_file(FileRecord& file) {
    return file.in_use && !file.directory && remove_status(file) == Status::ok;
}

// Renames `old_path` to `new_path` in one commit. A name already at
// `new_path` is replaced, and what it named is freed in the same commit: a
// file replaces a file, a directory replaces an empty directory. `replaced`
// receives the vnode that published the replaced name, for the caller to take
// out of its tree.
Status rename_status(const char* old_path, const char* new_path, vfs::Vnode** replaced) {
    if (replaced != nullptr) {
        *replaced = nullptr;
    }
    Volume* volume = volume_for_path(old_path);
    if (volume == nullptr) {
        return Status::not_found;
    }
    // Moving between volumes would be a copy and a delete, not a rename.
    if (volume != volume_for_path(new_path)) {
        return Status::invalid;
    }
    if (volume->status != MountStatus::mounted) {
        return Status::read_only;
    }
    FileRecord* target = find_record_by_path(*volume, old_path);
    if (target == nullptr) {
        return Status::not_found;
    }
    char new_relative[256] = {};
    char new_parent_relative[256] = {};
    char new_leaf[64] = {};
    if (!parse_relative_path(*volume, new_path, new_relative, sizeof(new_relative)) ||
        !split_relative_parent(new_relative, new_parent_relative, sizeof(new_parent_relative), new_leaf, sizeof(new_leaf)) ||
        strcmp(target->path, new_relative) == 0) {
        return Status::invalid;
    }

    const uint32_t old_parent_inode = target->parent_inode_id;
    const uint32_t new_parent_inode = parent_inode_for_relative_path(*volume, new_parent_relative);
    Inode* old_parent = inode_for_id(*volume, old_parent_inode);
    Inode* new_parent = inode_for_id(*volume, new_parent_inode);
    if (old_parent == nullptr || new_parent == nullptr) {
        return Status::not_found;
    }
    if (new_parent->type != kInodeTypeDirectory) {
        return Status::not_directory;
    }
    if (target->directory &&
        (target->inode_id == new_parent_inode || record_is_ancestor(*volume, target->inode_id, new_parent_inode))) {
        return Status::invalid;
    }

    MutationGuard guard(*volume);
    FileRecord* victim = find_record_by_relative_path(*volume, new_relative);
    Inode* victim_inode = victim != nullptr ? inode_for_id(*volume, victim->inode_id) : nullptr;
    if (victim != nullptr) {
        if (victim_inode == nullptr) {
            return Status::io_error;
        }
        if (target->directory && !victim->directory) {
            return Status::not_directory;
        }
        if (!target->directory && victim->directory) {
            return Status::is_directory;
        }
        if (victim->directory) {
            bool empty = false;
            const Status scanned = directory_is_empty(*volume, *victim_inode, empty);
            if (scanned != Status::ok) {
                return scanned;
            }
            if (!empty) {
                return Status::not_empty;
            }
        }
    }
    const uint32_t victim_id = victim != nullptr ? victim->inode_id : 0;
    const uint16_t type = target->directory ? kInodeTypeDirectory : kInodeTypeFile;

    snapshot_metadata(*volume);
    uint32_t count = 0;
    Status status = load_directory(*volume, *old_parent, count);
    uint32_t slot = status == Status::ok ? find_slot_by_inode(*volume, count, target->inode_id) : kNoSlot;
    if (status == Status::ok && slot == kNoSlot) {
        status = Status::io_error;
    }
    if (status == Status::ok && old_parent_inode == new_parent_inode) {
        // One directory: the entry changes its name where it is.
        if (!fill_slot(volume->dir_scratch[slot], target->inode_id, type, new_leaf)) {
            status = Status::invalid;
        } else if (victim_id != 0) {
            const uint32_t victim_slot = find_slot_by_inode(*volume, count, victim_id);
            if (victim_slot == kNoSlot) {
                status = Status::io_error;
            } else {
                remove_slot(*volume, count, victim_slot);
            }
        }
        if (status == Status::ok) {
            status = store_directory(*volume, *old_parent, count);
        }
    } else if (status == Status::ok) {
        // Two directories, two copies, one commit.
        remove_slot(*volume, count, slot);
        status = store_directory(*volume, *old_parent, count);
        if (status == Status::ok) {
            status = load_directory(*volume, *new_parent, count);
        }
        if (status == Status::ok && victim_id != 0) {
            const uint32_t victim_slot = find_slot_by_inode(*volume, count, victim_id);
            if (victim_slot == kNoSlot) {
                status = Status::io_error;
            } else {
                remove_slot(*volume, count, victim_slot);
            }
        }
        if (status == Status::ok && victim_id == 0 && find_slot_by_name(*volume, count, new_leaf) != kNoSlot) {
            status = Status::exists;
        }
        if (status == Status::ok && count >= kMaxRecords) {
            status = Status::no_space;
        }
        if (status == Status::ok && !fill_slot(volume->dir_scratch[count], target->inode_id, type, new_leaf)) {
            status = Status::invalid;
        }
        if (status == Status::ok) {
            status = store_directory(*volume, *new_parent, count + 1u);
        }
    }
    if (status == Status::ok && victim_id != 0) {
        free_inode_blocks(*volume, *victim_inode);
        release_inode_id(*volume, victim_id);
    }
    if (status == Status::ok && !commit_metadata(*volume)) {
        status = Status::io_error;
    }
    if (status != Status::ok) {
        restore_metadata(*volume);
        volume->snapshot_valid = false;
        return status;
    }

    if (victim != nullptr) {
        if (replaced != nullptr) {
            *replaced = victim->vnode;
        }
        memset(victim, 0, sizeof(*victim));
    }
    char old_relative[256] = {};
    strcpy(old_relative, target->path);
    target->parent_inode_id = new_parent_inode;
    strcpy(target->name, new_leaf);
    strcpy(target->path, new_relative);
    if (target->directory) {
        update_descendant_paths(*volume, target->inode_id, old_relative, target->path);
    }
    refresh_directory_vnode(*volume, old_parent_inode);
    refresh_directory_vnode(*volume, new_parent_inode);
    return Status::ok;
}

bool rename_path(const char* old_path, const char* new_path) {
    return rename_status(old_path, new_path, nullptr) == Status::ok;
}

FileRecord* file_from_vnode(vfs::Vnode& node) {
    for (Volume& volume : g_volumes) {
        if (!volume.in_use) {
            continue;
        }
        for (FileRecord& record : volume.records) {
            if (record.in_use && record.vnode == &node) {
                return &record;
            }
        }
    }
    return nullptr;
}

void refresh_vnode(vfs::Vnode& node) {
    FileRecord* record = file_from_vnode(node);
    if (record == nullptr) {
        return;
    }
    Volume* volume = volume_for_id(record->volume);
    if (volume == nullptr) {
        return;
    }

    record->vnode = &node;
    node.writable = volume->status == MountStatus::mounted;
    publish_ownership(*volume, record->inode_id, &node);
    if (record->directory) {
        return;
    }

    Inode* inode = inode_for_id(*volume, record->inode_id);
    if (inode != nullptr) {
        record->size = inode->size;
        node.size = inode->size;
    }
}

namespace {

/* The fs::Driver operations: the VFS hands in vnodes and paths, these find
 * the FileRecord behind them and keep the vnode's size current. */

bool driver_read(vfs::Vnode& node, size_t offset, void* buffer, size_t count) {
    FileRecord* file = file_from_vnode(node);
    if (file == nullptr || !read_file(*file, offset, buffer, count)) {
        return false;
    }
    node.size = file->size;
    return true;
}

bool driver_write(vfs::Vnode& node, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written) {
    FileRecord* file = file_from_vnode(node);
    if (file == nullptr) {
        return false;
    }
    // The block drivers report their health periodically, not only on
    // failure: the margin left matters most when everything works.
    block::report_health(false);
    if (!write_file(*file, offset, buffer, count, truncate, written)) {
        // sxfs only fails a write when something really failed, most likely
        // the disk underneath: let the block drivers say which.
        block::report_health(true);
        return false;
    }
    node.size = file->size;
    return true;
}

bool driver_truncate(vfs::Vnode& node, size_t size) {
    FileRecord* file = file_from_vnode(node);
    if (file == nullptr || !truncate_file(*file, size)) {
        return false;
    }
    node.size = file->size;
    return true;
}

bool driver_unlink(vfs::Vnode& node) {
    FileRecord* file = file_from_vnode(node);
    return file != nullptr && unlink_file(*file);
}

bool driver_remove_directory(vfs::Vnode& node) {
    FileRecord* file = file_from_vnode(node);
    return file != nullptr && remove_directory(*file);
}

// Paths carry their volume (volume_for_path), so the id fs:: hands in is not
// needed here.
vfs::Vnode* driver_create_file(VolumeId, const char* path, const vfs::Ownership& owner) {
    FileRecord* file = create_file(path, &owner);
    if (file == nullptr) {
        return nullptr;
    }
    if (file->vnode == nullptr) {
        file->vnode = vfs::install_volume_file(path, driver(), file, file->size, true);
    }
    if (file->vnode != nullptr) {
        vfs::set_ownership(*file->vnode, owner.uid, owner.gid, owner.mode);
    }
    return file->vnode;
}

vfs::Vnode* driver_create_directory(VolumeId, const char* path, const vfs::Ownership& owner) {
    FileRecord* directory = create_directory(path, &owner);
    if (directory == nullptr) {
        return nullptr;
    }
    vfs::Vnode* node = directory->vnode != nullptr ? directory->vnode : vfs::ensure_directory(path);
    if (node != nullptr) {
        vfs::set_ownership(*node, owner.uid, owner.gid, owner.mode);
    }
    return node;
}

// chmod and chown: the new ownership in the inode, in one journalled commit.
bool driver_set_ownership(vfs::Vnode& node, uint32_t uid, uint32_t gid, uint16_t mode) {
    Volume* volume = nullptr;
    uint32_t inode_id = 0;
    if (FileRecord* record = file_from_vnode(node)) {
        volume = volume_for_id(record->volume);
        inode_id = record->inode_id;
    } else {
        for (Volume& candidate : g_volumes) {
            if (candidate.in_use && candidate.root_vnode == &node) {
                volume = &candidate;
                inode_id = kRootInodeId;
            }
        }
    }
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return false;
    }
    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, inode_id);
    if (inode == nullptr || !sxfs_mode_matches_type(mode, inode->type)) {
        return false;
    }
    snapshot_metadata(*volume);
    inode->uid = uid;
    inode->gid = gid;
    inode->mode = mode;
    if (!commit_metadata(*volume)) {
        restore_metadata(*volume);
        return false;
    }
    return true;
}

bool driver_rename(VolumeId, const char* old_path, const char* new_path) {
    return rename_path(old_path, new_path);
}

bool driver_stats(VolumeId volume, fs::VolumeStats& out) {
    if (!mounted(volume)) {
        return false;
    }
    out.writable = writable(volume);
    out.file_count = file_count(volume);
    out.total_bytes = total_bytes(volume);
    out.used_bytes = used_bytes(volume);
    out.free_bytes = out.total_bytes >= out.used_bytes ? out.total_bytes - out.used_bytes : 0;
    return true;
}

// fs::release has already synced the volume and taken its tree out of the VFS
// (vfs::detach_subtree), after checking nothing held any of its vnodes: no
// record is reachable any more, so its state can go.
void driver_unmount(VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr) {
        return;
    }
    (void)block::sync_all();
    memset(volume, 0, sizeof(*volume));
}

const fs::Driver kDriver = {
    .name = "sxfs",
    .priority = 100,
    .can_be_root = true,
    .probe = &probe,
    .attach = &attach,
    .unmount = &driver_unmount,
    .read = &driver_read,
    .write = &driver_write,
    .truncate = &driver_truncate,
    .unlink = &driver_unlink,
    .remove_directory = &driver_remove_directory,
    .refresh = &refresh_vnode,
    .create_file = &driver_create_file,
    .create_directory = &driver_create_directory,
    .rename = &driver_rename,
    .set_ownership = &driver_set_ownership,
    .stats = &driver_stats,
    .sync = &sync,
    // SxFS records owners per inode.
    .adopt_owner = nullptr,
    .sync_deferred = &sync_deferred,
};

} // namespace

const fs::Driver& driver() { return kDriver; }

} // namespace sxfs
