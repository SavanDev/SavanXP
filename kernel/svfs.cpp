#include "kernel/svfs.hpp"

#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/string.hpp"
#include "kernel/vfs.hpp"
#include "svfs/svfs_format.h"

namespace {

// El formato on-disk (constantes de layout, structs, checksum y bit-math) vive
// en include/svfs/svfs_format.h como fuente de verdad unica, compartido con el
// tool de host. Aca solo se le ponen los nombres internos historicos para no
// reescribir los cientos de usos, mas la politica y los limites que son propios
// del driver del kernel (no on-disk).
// (Las constantes de layout que ya solo usa la validacion viven en el header;
// aca quedan las que el resto del driver referencia por su nombre historico.)
constexpr uint32_t kFlagClean = SVFS_FLAG_CLEAN;
constexpr uint16_t kInodeTypeUnused = SVFS_INODE_UNUSED;
constexpr uint16_t kInodeTypeFile = SVFS_INODE_FILE;
constexpr uint16_t kInodeTypeDirectory = SVFS_INODE_DIRECTORY;
constexpr uint32_t kPrimarySuperblockLba = SVFS_PRIMARY_SB_LBA;
constexpr uint32_t kSecondarySuperblockLba = SVFS_SECONDARY_SB_LBA;
constexpr uint32_t kBlockBitmapSectors = SVFS_BLOCK_BITMAP_SECTORS;
constexpr uint32_t kInodeBitmapSectors = SVFS_INODE_BITMAP_SECTORS;
constexpr uint32_t kJournalMetadataSectors = SVFS_JOURNAL_METADATA_SECTORS;
constexpr uint32_t kDataLba = SVFS_DATA_LBA;
constexpr uint32_t kMaxInodes = SVFS_MAX_INODES;
constexpr uint32_t kMaxRecords = SVFS_MAX_RECORDS;
constexpr uint32_t kRootInodeId = SVFS_ROOT_INODE;
constexpr uint32_t kMaxExtents = SVFS_MAX_EXTENTS;
constexpr uint32_t kMinimumGrowthSectors = 64; // politica de crecimiento, no on-disk
constexpr size_t kBlockBitmapBytes = static_cast<size_t>(kBlockBitmapSectors) * block::kSectorSize;
constexpr size_t kInodeBitmapBytes = static_cast<size_t>(kInodeBitmapSectors) * block::kSectorSize;
constexpr size_t kMaxRelativePath = 255;
constexpr size_t kMaxDirNameLength = 63;

// Bridge: el kernel indexa bitmaps/tabla en unidades de block::kSectorSize,
// mientras que el formato compartido define SVFS_SECTOR_SIZE. Ambos deben ser
// el mismo tamano de sector o el layout on-disk no cuadra.
static_assert(block::kSectorSize == SVFS_SECTOR_SIZE);

using Extent = svfs_extent;
using Inode = svfs_inode;
using Superblock = svfs_superblock;
using JournalHeader = svfs_journal_header;
using DirEntry = svfs_dir_entry;

// Magias del formato compartido, con los nombres internos historicos.
constexpr const char* kJournalMagic = svfs_journal_magic;

struct MetadataSnapshot {
    Superblock superblock;
    uint8_t block_bitmap[kBlockBitmapBytes];
    uint8_t inode_bitmap[kInodeBitmapBytes];
    Inode inodes[kMaxInodes];
};

// Un volumen montado. Todo lo que antes eran globales de archivo vive aca
// adentro: el instalador necesita el SVFS2 del LiveCD (origen) y el de la
// particion destino montados AL MISMO TIEMPO, y con un solo juego de globales
// eso no se podia. El resto del driver no cambio de forma: cada funcion interna
// recibe su Volume& como primer parametro.
struct Volume {
    bool in_use;
    size_t device_index;
    svfs::MountStatus status;
    bool metadata_ready;
    // Prefijo bajo el que cuelga del vfs ("/disk"). Todas las rutas que entran
    // por la API publica se resuelven contra esto, asi que un segundo volumen
    // en "/mnt/destino" no le pisa las rutas al primero.
    char mount_point[svfs::kMountPointCapacity];
    Superblock superblock;
    uint8_t block_bitmap[kBlockBitmapBytes];
    uint8_t inode_bitmap[kInodeBitmapBytes];
    Inode inodes[kMaxInodes];
    svfs::FileRecord records[kMaxRecords];
    MetadataSnapshot snapshot;
    // Por volumen y no global: escribir en el destino del instalador no tiene
    // por que serializar contra la raiz.
    volatile uint32_t mutation_lock;
};

// Raiz + destino del instalador. Cada volumen pesa ~185 KiB de BSS (bitmaps,
// tabla de inodos, cache de records y snapshot), asi que subirlo no es gratis.
constexpr size_t kMaxVolumes = 2;
Volume g_volumes[kMaxVolumes] = {};

size_t volume_index(const Volume& volume) {
    return static_cast<size_t>(&volume - &g_volumes[0]);
}

Volume* volume_for_id(svfs::VolumeId id) {
    if (id >= kMaxVolumes || !g_volumes[id].in_use) {
        return nullptr;
    }
    return &g_volumes[id];
}

// Volumen cuyo mount point es prefijo de path. Es como se resuelve a que
// volumen le habla una ruta absoluta que entra por la API publica.
Volume* volume_for_path(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    for (Volume& volume : g_volumes) {
        if (!volume.in_use) {
            continue;
        }
        const size_t prefix = strlen(volume.mount_point);
        if (strncmp(path, volume.mount_point, prefix) == 0 &&
            (path[prefix] == 0 || path[prefix] == '/')) {
            return &volume;
        }
    }
    return nullptr;
}

enum class RecoveryState : uint8_t {
    failed = 0,
    writable = 1,
    read_only = 2,
};

// Validacion y checksum de metadata: la logica vive en el formato compartido
// (svfs_format.h) para no divergir del builder de host. Aca solo wrappers con
// las firmas por-referencia que usa el resto del driver.
uint32_t superblock_checksum(const Superblock& superblock) {
    return svfs_superblock_checksum(&superblock);
}

uint32_t journal_checksum(const JournalHeader& header) {
    return svfs_journal_checksum(&header);
}

bool valid_superblock(const Superblock& superblock) {
    return svfs_superblock_valid(&superblock);
}

bool valid_journal(const JournalHeader& header) {
    return svfs_journal_valid(&header);
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
    return svfs_bitmap_test(bitmap, bit) != 0;
}

void bitmap_set(uint8_t* bitmap, uint32_t bit, bool value) {
    svfs_bitmap_set(bitmap, bit, value ? 1 : 0);
}

void snapshot_metadata(Volume& volume) {
    volume.snapshot.superblock = volume.superblock;
    memcpy(volume.snapshot.block_bitmap, volume.block_bitmap, sizeof(volume.block_bitmap));
    memcpy(volume.snapshot.inode_bitmap, volume.inode_bitmap, sizeof(volume.inode_bitmap));
    memcpy(volume.snapshot.inodes, volume.inodes, sizeof(volume.inodes));
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

bool write_superblocks(Volume& volume, uint32_t sequence, uint32_t flags) {
    volume.superblock.sequence = sequence;
    volume.superblock.flags = flags;
    volume.superblock.checksum = superblock_checksum(volume.superblock);
    return block::write(volume.device_index, kPrimarySuperblockLba, 1, &volume.superblock) &&
        block::write(volume.device_index, kSecondarySuperblockLba, 1, &volume.superblock);
}

bool clear_journal(Volume& volume) {
    JournalHeader header = {};
    return block::write(volume.device_index, volume.superblock.journal_lba, 1, &header);
}

bool write_journal_payload(Volume& volume) {
    const uint32_t cursor = volume.superblock.journal_lba + 1;
    return block::write(volume.device_index, cursor, volume.superblock.block_bitmap_sectors, volume.block_bitmap) &&
        block::write(volume.device_index, cursor + volume.superblock.block_bitmap_sectors, volume.superblock.inode_bitmap_sectors, volume.inode_bitmap) &&
        block::write(
            volume.device_index,
            cursor + volume.superblock.block_bitmap_sectors + volume.superblock.inode_bitmap_sectors,
            volume.superblock.inode_table_sectors,
            volume.inodes);
}

RecoveryState recover_journal(Volume& volume) {
    JournalHeader header = {};
    if (!block::read(volume.device_index, volume.superblock.journal_lba, 1, &header)) {
        return RecoveryState::failed;
    }
    if (!valid_journal(header) || header.pending == 0) {
        if ((volume.superblock.flags & kFlagClean) != 0) {
            return RecoveryState::writable;
        }
        return write_superblocks(volume, volume.superblock.sequence, kFlagClean)
            ? RecoveryState::writable
            : RecoveryState::read_only;
    }

    const uint32_t cursor = volume.superblock.journal_lba + 1;
    if (!block::read(volume.device_index, cursor, volume.superblock.block_bitmap_sectors, volume.block_bitmap) ||
        !block::read(volume.device_index, cursor + volume.superblock.block_bitmap_sectors, volume.superblock.inode_bitmap_sectors, volume.inode_bitmap) ||
        !block::read(
            volume.device_index,
            cursor + volume.superblock.block_bitmap_sectors + volume.superblock.inode_bitmap_sectors,
            volume.superblock.inode_table_sectors,
            volume.inodes)) {
        return RecoveryState::failed;
    }

    if (!write_home_metadata(volume) || !write_superblocks(volume, header.sequence, kFlagClean) || !clear_journal(volume)) {
        return RecoveryState::read_only;
    }
    return RecoveryState::writable;
}

bool commit_metadata(Volume& volume) {
    const uint32_t next_sequence = volume.superblock.sequence + 1u;
    JournalHeader journal = {};
    memcpy(journal.magic, kJournalMagic, sizeof(journal.magic));
    journal.sequence = next_sequence;
    journal.pending = 1;
    journal.metadata_sectors = kJournalMetadataSectors;
    journal.checksum = journal_checksum(journal);

    if (!write_journal_payload(volume) ||
        !block::write(volume.device_index, volume.superblock.journal_lba, 1, &journal) ||
        !write_superblocks(volume, next_sequence, 0) ||
        !write_home_metadata(volume) ||
        !write_superblocks(volume, next_sequence, kFlagClean) ||
        !clear_journal(volume)) {
        return false;
    }

    return true;
}

uint32_t inode_capacity_sectors(const Inode& inode) {
    return svfs_inode_capacity_sectors(&inode);
}

size_t inode_capacity_bytes(const Inode& inode) {
    return static_cast<size_t>(inode_capacity_sectors(inode)) * block::kSectorSize;
}

bool parse_relative_path(Volume& volume, const char* path, char* relative, size_t capacity, bool allow_root = false) {
    if (path == nullptr || relative == nullptr || capacity == 0) {
        return false;
    }
    const size_t prefix = strlen(volume.mount_point);
    if (strncmp(path, volume.mount_point, prefix) != 0 ||
        (path[prefix] != '\0' && path[prefix] != '/')) {
        return false;
    }

    const char* cursor = path + prefix;
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

svfs::FileRecord* find_record_by_inode(Volume& volume, uint32_t inode_id) {
    for (svfs::FileRecord& record : volume.records) {
        if (record.in_use && record.inode_id == inode_id) {
            return &record;
        }
    }
    return nullptr;
}

svfs::FileRecord* find_record_by_relative_path(Volume& volume, const char* relative) {
    if (relative == nullptr || *relative == '\0') {
        return nullptr;
    }
    for (svfs::FileRecord& record : volume.records) {
        if (record.in_use && strcmp(record.path, relative) == 0) {
            return &record;
        }
    }
    return nullptr;
}

svfs::FileRecord* find_record_by_path(Volume& volume, const char* path) {
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
    svfs::FileRecord* parent = find_record_by_relative_path(volume, relative_parent);
    return parent != nullptr && parent->directory ? parent->inode_id : 0;
}

bool make_absolute_path(Volume& volume, const char* relative, char* out, size_t capacity) {
    if (relative == nullptr || out == nullptr) {
        return false;
    }
    const size_t prefix = strlen(volume.mount_point);
    if (*relative == '\0') {
        if (prefix + 1 > capacity) {
            return false;
        }
        strcpy(out, volume.mount_point);
        return true;
    }
    const size_t relative_length = strlen(relative);
    if (prefix + 1 + relative_length + 1 > capacity) {
        return false;
    }
    strcpy(out, volume.mount_point);
    out[prefix] = '/';
    strcpy(out + prefix + 1, relative);
    return true;
}

svfs::FileRecord* allocate_record(Volume& volume) {
    for (svfs::FileRecord& record : volume.records) {
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
    svfs::FileRecord* record = find_record_by_inode(volume, inode_id);
    if (record != nullptr) {
        memset(record, 0, sizeof(*record));
    }
}

void update_descendant_paths(Volume& volume, uint32_t directory_inode, const char* old_path, const char* new_path) {
    const size_t old_length = strlen(old_path);
    const size_t new_length = strlen(new_path);
    for (svfs::FileRecord& record : volume.records) {
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
            const svfs::FileRecord* parent = find_record_by_inode(volume, current);
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

uint32_t find_free_run(Volume& volume, uint32_t minimum_sectors) {
    return svfs_find_free_run(volume.block_bitmap, volume.superblock.data_lba,
                              volume.superblock.total_sectors, minimum_sectors);
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

uint32_t allocated_data_sector_count(Volume& volume) {
    uint32_t count = 0;
    for (uint32_t sector = volume.superblock.data_lba; sector < volume.superblock.total_sectors; ++sector) {
        if (bitmap_test(volume.block_bitmap, sector)) {
            ++count;
        }
    }
    return count;
}

bool allocate_blocks(Volume& volume, Inode& inode, uint32_t additional_sectors) {
    while (additional_sectors != 0) {
        uint32_t run_start = 0;
        uint32_t best = additional_sectors;

        while (best != 0 && run_start == 0) {
            run_start = find_free_run(volume, best);
            if (run_start == 0) {
                --best;
            }
        }
        if (run_start == 0 || best == 0) {
            return false;
        }

        for (uint32_t sector = 0; sector < best; ++sector) {
            bitmap_set(volume.block_bitmap, run_start + sector, true);
        }
        if (!append_extent(inode, run_start, best)) {
            return false;
        }
        additional_sectors -= best;
    }
    return true;
}

void free_inode_blocks(Volume& volume, Inode& inode) {
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
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
    for (uint32_t index = 0; index < inode.extent_count; ++index) {
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

bool ensure_inode_capacity(Volume& volume, Inode& inode, size_t required_size) {
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

    return allocate_blocks(volume, inode, target_sectors - current_sectors);
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

bool read_inode_bytes(Volume& volume, const Inode& inode, size_t offset, void* buffer, size_t count) {
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
            !block::read(volume.device_index, lba, 1, sector)) {
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
        if (!block::write(volume.device_index, lba, 1, sector)) {
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

bool read_dir_entry(Volume& volume, const Inode& directory, size_t offset, DirEntry& entry) {
    if (offset + sizeof(entry) > directory.size) {
        return false;
    }
    return read_inode_bytes(volume, directory, offset, &entry, sizeof(entry));
}

bool write_dir_entry(Volume& volume, const Inode& directory, size_t offset, const DirEntry& entry) {
    return write_inode_bytes(volume, directory, offset, &entry, sizeof(entry));
}

bool find_directory_entry(Volume& volume, const Inode& directory, const char* name, DirEntry& entry, size_t& offset_out) {
    if (directory.type != kInodeTypeDirectory) {
        return false;
    }
    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory.size; offset += sizeof(DirEntry)) {
        DirEntry current = {};
        if (!read_dir_entry(volume, directory, offset, current)) {
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

bool directory_has_children(Volume& volume, const Inode& directory) {
    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory.size; offset += sizeof(DirEntry)) {
        DirEntry entry = {};
        if (!read_dir_entry(volume, directory, offset, entry)) {
            return false;
        }
        if (entry.inode_id != 0 && entry.name_length != 0) {
            return true;
        }
    }
    return false;
}

bool add_directory_entry(Volume& volume, Inode& directory, uint32_t inode_id, uint16_t type, const char* name) {
    DirEntry entry = {};
    const size_t name_length = strlen(name);
    if (name_length == 0 || name_length > kMaxDirNameLength) {
        return false;
    }

    size_t target_offset = directory.size;
    for (size_t offset = 0; offset + sizeof(DirEntry) <= directory.size; offset += sizeof(DirEntry)) {
        DirEntry current = {};
        if (!read_dir_entry(volume, directory, offset, current)) {
            return false;
        }
        if (current.inode_id == 0 || current.name_length == 0) {
            target_offset = offset;
            break;
        }
    }

    const size_t required_size = target_offset + sizeof(DirEntry);
    if (!ensure_inode_capacity(volume, directory, required_size)) {
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
    return write_dir_entry(volume, directory, target_offset, entry);
}

bool remove_directory_entry(Volume& volume, Inode& directory, const char* name) {
    DirEntry entry = {};
    size_t offset = 0;
    if (!find_directory_entry(volume, directory, name, entry, offset)) {
        return false;
    }
    DirEntry empty = {};
    return write_dir_entry(volume, directory, offset, empty);
}

bool build_record_recursive(Volume& volume, uint32_t directory_inode, uint32_t parent_inode, const char* parent_relative) {
    const Inode* directory = inode_for_id(volume, directory_inode);
    if (directory == nullptr || directory->type != kInodeTypeDirectory) {
        return false;
    }

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

        svfs::FileRecord* record = allocate_record(volume);
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

        if (record->directory && !build_record_recursive(volume, record->inode_id, record->inode_id, record->path)) {
            return false;
        }
    }

    return true;
}

bool rebuild_record_cache(Volume& volume) {
    memset(volume.records, 0, sizeof(volume.records));
    return build_record_recursive(volume, kRootInodeId, kRootInodeId, "");
}

void refresh_record_size(Volume& volume, uint32_t inode_id) {
    svfs::FileRecord* record = find_record_by_inode(volume, inode_id);
    Inode* inode = inode_for_id(volume, inode_id);
    if (record != nullptr && inode != nullptr) {
        record->size = inode->size;
        if (record->vnode != nullptr) {
            record->vnode->size = inode->size;
        }
    }
}

void mount_record(Volume& volume, svfs::FileRecord& record) {
    char absolute[262] = {};
    if (!make_absolute_path(volume, record.path, absolute, sizeof(absolute))) {
        return;
    }

    if (record.directory) {
        record.vnode = vfs::ensure_directory(absolute);
    } else {
        record.vnode = vfs::install_external_file(
            absolute,
            vfs::Backend::svfs,
            &record,
            record.size,
            volume.status != svfs::MountStatus::read_only);
    }
}

bool initialize_record_mounts(Volume& volume) {
    if (vfs::ensure_directory(volume.mount_point) == nullptr) {
        return false;
    }

    for (svfs::FileRecord& record : volume.records) {
        if (record.in_use && record.directory) {
            mount_record(volume, record);
            if (record.vnode == nullptr) {
                return false;
            }
        }
    }

    for (svfs::FileRecord& record : volume.records) {
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
        const svfs::FileRecord* record = find_record_by_inode(volume, current);
        current = record != nullptr ? record->parent_inode_id : 0;
    }
    return ancestor_inode == kRootInodeId;
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
    if (!read_home_metadata(volume)) {
        return false;
    }
    const RecoveryState recovery = recover_journal(volume);
    if (recovery == RecoveryState::failed || !rebuild_record_cache(volume)) {
        return false;
    }
    volume.status = recovery == RecoveryState::read_only ? svfs::MountStatus::read_only : svfs::MountStatus::mounted;
    volume.metadata_ready = true;
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
            // No es un SVFS2 (o esta roto): el slot vuelve a quedar libre y
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
    volume->status = MountStatus::mounted;
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

bool owns_path(const char* path) {
    return volume_for_path(path) != nullptr;
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

bool sync(VolumeId id) {
    Volume* volume = volume_for_id(id);
    if (volume == nullptr || !volume->metadata_ready) {
        return false;
    }
    if (volume->status != MountStatus::mounted) {
        return true;
    }
    MutationGuard guard(*volume);
    return write_superblocks(*volume, volume->superblock.sequence, kFlagClean) && clear_journal(*volume);
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

bool write_file(FileRecord& file, size_t offset, const void* buffer, size_t count, bool truncate, size_t& written) {
    written = 0;
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return false;
    }
    if (!file.in_use || file.directory) {
        return false;
    }

    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, file.inode_id);
    if (inode == nullptr || inode->type != kInodeTypeFile) {
        return false;
    }

    const size_t required_size = count == 0 ? (truncate ? 0 : inode->size) : (offset + count);
    const bool needs_metadata = truncate || required_size > inode->size ||
        required_size > inode_capacity_bytes(*inode);

    if (needs_metadata) {
        snapshot_metadata(*volume);
        if (truncate) {
            shrink_inode_to_sectors(*volume, *inode, 0);
            inode->size = 0;
            offset = 0;
        }
        if (!ensure_inode_capacity(*volume, *inode, required_size)) {
            restore_metadata(*volume);
            return false;
        }
        if (offset > inode->size && !zero_inode_range(*volume, *inode, inode->size, offset - inode->size)) {
            restore_metadata(*volume);
            return false;
        }
    } else if (buffer == nullptr && count != 0) {
        return false;
    }

    if (count != 0) {
        if (buffer == nullptr || !write_inode_bytes(*volume, *inode, offset, buffer, count)) {
            if (needs_metadata) {
                restore_metadata(*volume);
            }
            return false;
        }
        written = count;
    }

    if (needs_metadata) {
        const size_t new_size = count == 0
            ? inode->size
            : ((offset + written) > inode->size ? (offset + written) : inode->size);
        inode->size = static_cast<uint32_t>(new_size);
        if (!commit_metadata(*volume)) {
            restore_metadata(*volume);
            return false;
        }
    }

    refresh_record_size(*volume, file.inode_id);
    return true;
}

bool truncate_file(FileRecord& file, size_t size) {
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return false;
    }
    if (!file.in_use || file.directory) {
        return false;
    }

    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, file.inode_id);
    if (inode == nullptr || inode->type != kInodeTypeFile) {
        return false;
    }

    snapshot_metadata(*volume);
    if (size > inode->size) {
        if (!ensure_inode_capacity(*volume, *inode, size) ||
            !zero_inode_range(*volume, *inode, inode->size, size - inode->size)) {
            restore_metadata(*volume);
            return false;
        }
    } else {
        const uint32_t keep_sectors = size == 0
            ? 0
            : static_cast<uint32_t>((size + block::kSectorSize - 1) / block::kSectorSize);
        shrink_inode_to_sectors(*volume, *inode, keep_sectors);
    }

    inode->size = static_cast<uint32_t>(size);
    if (!commit_metadata(*volume)) {
        restore_metadata(*volume);
        return false;
    }
    refresh_record_size(*volume, file.inode_id);
    return true;
}

FileRecord* create_file(const char* path) {
    Volume* volume = volume_for_path(path);
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return nullptr;
    }
    char relative[256] = {};
    char parent_relative[256] = {};
    char leaf[64] = {};
    if (!parse_relative_path(*volume, path, relative, sizeof(relative)) ||
        !split_relative_parent(relative, parent_relative, sizeof(parent_relative), leaf, sizeof(leaf)) ||
        find_record_by_relative_path(*volume, relative) != nullptr) {
        return nullptr;
    }

    const uint32_t parent_inode = parent_inode_for_relative_path(*volume, parent_relative);
    Inode* parent = inode_for_id(*volume, parent_inode);
    if (parent == nullptr || parent->type != kInodeTypeDirectory) {
        return nullptr;
    }

    MutationGuard guard(*volume);
    snapshot_metadata(*volume);

    const uint32_t inode_id = allocate_inode_id(*volume);
    Inode* inode = inode_for_id(*volume, inode_id);
    if (inode_id == 0 || inode == nullptr || !prepare_new_inode(*inode, inode_id, kInodeTypeFile) ||
        !add_directory_entry(*volume, *parent, inode_id, kInodeTypeFile, leaf) ||
        !commit_metadata(*volume)) {
        restore_metadata(*volume);
        return nullptr;
    }

    FileRecord* record = allocate_record(*volume);
    if (record == nullptr) {
        return nullptr;
    }
    record->directory = false;
    record->inode_id = inode_id;
    record->parent_inode_id = parent_inode;
    record->size = 0;
    strcpy(record->name, leaf);
    strcpy(record->path, relative);
    if (mounted(volume_index(*volume))) {
        mount_record(*volume, *record);
    }
    return record;
}

FileRecord* create_directory(const char* path) {
    Volume* volume = volume_for_path(path);
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return nullptr;
    }
    char relative[256] = {};
    char parent_relative[256] = {};
    char leaf[64] = {};
    if (!parse_relative_path(*volume, path, relative, sizeof(relative)) ||
        !split_relative_parent(relative, parent_relative, sizeof(parent_relative), leaf, sizeof(leaf)) ||
        find_record_by_relative_path(*volume, relative) != nullptr) {
        return nullptr;
    }

    const uint32_t parent_inode = parent_inode_for_relative_path(*volume, parent_relative);
    Inode* parent = inode_for_id(*volume, parent_inode);
    if (parent == nullptr || parent->type != kInodeTypeDirectory) {
        return nullptr;
    }

    MutationGuard guard(*volume);
    snapshot_metadata(*volume);

    const uint32_t inode_id = allocate_inode_id(*volume);
    Inode* inode = inode_for_id(*volume, inode_id);
    if (inode_id == 0 || inode == nullptr || !prepare_new_inode(*inode, inode_id, kInodeTypeDirectory) ||
        !add_directory_entry(*volume, *parent, inode_id, kInodeTypeDirectory, leaf) ||
        !commit_metadata(*volume)) {
        restore_metadata(*volume);
        return nullptr;
    }

    FileRecord* record = allocate_record(*volume);
    if (record == nullptr) {
        return nullptr;
    }
    record->directory = true;
    record->inode_id = inode_id;
    record->parent_inode_id = parent_inode;
    record->size = 0;
    strcpy(record->name, leaf);
    strcpy(record->path, relative);
    if (mounted(volume_index(*volume))) {
        mount_record(*volume, *record);
    }
    return record;
}

bool remove_directory(FileRecord& file) {
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return false;
    }
    if (!file.in_use || !file.directory) {
        return false;
    }

    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, file.inode_id);
    Inode* parent = inode_for_id(*volume, file.parent_inode_id);
    if (inode == nullptr || parent == nullptr || inode->type != kInodeTypeDirectory ||
        directory_has_children(*volume, *inode)) {
        return false;
    }

    snapshot_metadata(*volume);
    if (!remove_directory_entry(*volume, *parent, file.name)) {
        restore_metadata(*volume);
        return false;
    }
    free_inode_blocks(*volume, *inode);
    release_inode_id(*volume, file.inode_id);
    if (!commit_metadata(*volume)) {
        restore_metadata(*volume);
        return false;
    }
    remove_record(*volume, file.inode_id);
    return true;
}

bool rename_path(const char* old_path, const char* new_path) {
    Volume* volume = volume_for_path(old_path);
    // Mover entre volumenes seria copiar y borrar, no un rename: se rechaza.
    if (volume == nullptr || volume != volume_for_path(new_path) ||
        volume->status != MountStatus::mounted) {
        return false;
    }
    FileRecord* target = find_record_by_path(*volume, old_path);
    char new_relative[256] = {};
    char new_parent_relative[256] = {};
    char new_leaf[64] = {};
    if (target == nullptr ||
        !parse_relative_path(*volume, new_path, new_relative, sizeof(new_relative)) ||
        !split_relative_parent(new_relative, new_parent_relative, sizeof(new_parent_relative), new_leaf, sizeof(new_leaf)) ||
        strcmp(target->path, new_relative) == 0 ||
        find_record_by_relative_path(*volume, new_relative) != nullptr) {
        return false;
    }

    const uint32_t new_parent_inode = parent_inode_for_relative_path(*volume, new_parent_relative);
    Inode* old_parent = inode_for_id(*volume, target->parent_inode_id);
    Inode* new_parent = inode_for_id(*volume, new_parent_inode);
    if (old_parent == nullptr || new_parent == nullptr || new_parent->type != kInodeTypeDirectory) {
        return false;
    }
    if (target->directory &&
        (target->inode_id == new_parent_inode || record_is_ancestor(*volume, target->inode_id, new_parent_inode))) {
        return false;
    }

    MutationGuard guard(*volume);
    snapshot_metadata(*volume);
    if (!remove_directory_entry(*volume, *old_parent, target->name) ||
        !add_directory_entry(*volume, *new_parent, target->inode_id,
                             target->directory ? kInodeTypeDirectory : kInodeTypeFile, new_leaf) ||
        !commit_metadata(*volume)) {
        restore_metadata(*volume);
        return false;
    }

    char old_relative[256] = {};
    strcpy(old_relative, target->path);
    target->parent_inode_id = new_parent_inode;
    strcpy(target->name, new_leaf);
    strcpy(target->path, new_relative);
    if (target->directory) {
        update_descendant_paths(*volume, target->inode_id, old_relative, target->path);
    }
    return true;
}

bool unlink_file(FileRecord& file) {
    Volume* volume = volume_for_id(file.volume);
    if (volume == nullptr || volume->status != MountStatus::mounted) {
        return false;
    }
    if (!file.in_use || file.directory) {
        return false;
    }

    MutationGuard guard(*volume);
    Inode* inode = inode_for_id(*volume, file.inode_id);
    Inode* parent = inode_for_id(*volume, file.parent_inode_id);
    if (inode == nullptr || parent == nullptr || inode->type != kInodeTypeFile) {
        return false;
    }

    snapshot_metadata(*volume);
    if (!remove_directory_entry(*volume, *parent, file.name)) {
        restore_metadata(*volume);
        return false;
    }
    free_inode_blocks(*volume, *inode);
    release_inode_id(*volume, file.inode_id);
    if (!commit_metadata(*volume)) {
        restore_metadata(*volume);
        return false;
    }
    remove_record(*volume, file.inode_id);
    return true;
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

// unmount queda en nullptr a proposito: vfs:: todavia no sabe desprender un
// subarbol, asi que soltar el volumen dejaria vnodes apuntando a records
// liberados. El instalador cierra con sync(), que si alcanza.
const fs::Driver kDriver = {
    "svfs2",
    100,
    &probe,
    &attach,
    nullptr,
};

} // namespace

const fs::Driver& driver() { return kDriver; }

} // namespace svfs
