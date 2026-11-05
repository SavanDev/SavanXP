/*
 * sxdisk.c -- GUID partition tables (include/sxdisk/sxdisk.h).
 *
 * Assumes a little-endian host, like the kernel and libsxfs: entries are
 * copied to and from disk as they are.
 */
#include "sxdisk/sxdisk.h"

#include <string.h>

#include "shared/crc32.h"

#if defined(__cplusplus)
static_assert(sizeof(struct sxdisk_gpt_entry) == SXDISK_GPT_ENTRY_SIZE, "GPT entry is 128 bytes");
#else
_Static_assert(sizeof(struct sxdisk_gpt_entry) == SXDISK_GPT_ENTRY_SIZE, "GPT entry is 128 bytes");
#endif

#define GPT_HEADER_SIZE 92u
#define GPT_REVISION 0x00010000u
#define MBR_TABLE_OFFSET 446u
#define MBR_BOOT_CODE_SIZE 440u

/* All four as stored on disk: the first three fields little-endian. */
/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
const uint8_t sxdisk_type_esp[SXDISK_GUID_SIZE] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};
/* 277C8079-900E-477E-99D8-4FEE1496D370, chosen for SavanXP on 2026-11-05
 * (docs/userland/DISK_MANAGEMENT.md). Never change it: it is how an installed
 * system's root is recognised. */
const uint8_t sxdisk_type_sxfs[SXDISK_GUID_SIZE] = {
    0x79, 0x80, 0x7c, 0x27, 0x0e, 0x90, 0x7e, 0x47, 0x99, 0xd8, 0x4f, 0xee, 0x14, 0x96, 0xd3, 0x70,
};
/* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
const uint8_t sxdisk_type_basic_data[SXDISK_GUID_SIZE] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};
/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
const uint8_t sxdisk_type_linux[SXDISK_GUID_SIZE] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};

static void put16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t* out, uint64_t value) {
    put32(out, (uint32_t)value);
    put32(out + 4, (uint32_t)(value >> 32));
}

static uint32_t get32(const uint8_t* in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static uint64_t get64(const uint8_t* in) {
    return (uint64_t)get32(in) | ((uint64_t)get32(in + 4) << 32);
}

int sxdisk_guid_is_zero(const uint8_t guid[SXDISK_GUID_SIZE]) {
    for (size_t index = 0; index < SXDISK_GUID_SIZE; ++index) {
        if (guid[index] != 0) {
            return 0;
        }
    }
    return 1;
}

/* A version 4 GUID. The version sits in the high nibble of the third field,
 * which is little-endian on disk (byte 7); the variant in byte 8. */
static void new_guid(const struct sxdisk_io* io, uint8_t out[SXDISK_GUID_SIZE]) {
    memset(out, 0, SXDISK_GUID_SIZE);
    if (io != NULL && io->random != NULL) {
        io->random(io->cookie, out, SXDISK_GUID_SIZE);
    }
    out[7] = (uint8_t)((out[7] & 0x0f) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
}

static uint64_t array_sectors_end(uint64_t entries_lba) {
    return entries_lba + SXDISK_GPT_ARRAY_SECTORS;
}

int sxdisk_gpt_init(struct sxdisk_gpt* gpt, uint64_t disk_sectors, const struct sxdisk_io* io) {
    if (gpt == NULL || disk_sectors < SXDISK_MIN_DISK_SECTORS) {
        return SXDISK_ERR_INVALID;
    }
    memset(gpt, 0, sizeof(*gpt));
    gpt->disk_sectors = disk_sectors;
    gpt->first_usable = array_sectors_end(2);
    gpt->last_usable = disk_sectors - 2 - SXDISK_GPT_ARRAY_SECTORS;
    new_guid(io, gpt->disk_guid);
    return SXDISK_OK;
}

/* Reads and checks the header at `lba` and its array into gpt->entries. */
static int load_copy(struct sxdisk_gpt* gpt, uint64_t disk_sectors, uint64_t lba, const struct sxdisk_io* io) {
    uint8_t sector[SXDISK_SECTOR_SIZE];
    if (io->read(io->cookie, lba, 1, sector) != 0) {
        return SXDISK_ERR_IO;
    }
    if (memcmp(sector, "EFI PART", 8) != 0) {
        return SXDISK_ERR_INVALID;
    }
    const uint32_t header_size = get32(sector + 12);
    if (header_size < GPT_HEADER_SIZE || header_size > SXDISK_SECTOR_SIZE) {
        return SXDISK_ERR_INVALID;
    }
    const uint32_t stored = get32(sector + 16);
    put32(sector + 16, 0);
    if (sx_crc32(sector, header_size) != stored || get64(sector + 24) != lba) {
        return SXDISK_ERR_INVALID;
    }
    const uint64_t first_usable = get64(sector + 40);
    const uint64_t last_usable = get64(sector + 48);
    const uint64_t entries_lba = get64(sector + 72);
    const uint32_t entry_count = get32(sector + 80);
    const uint32_t entry_size = get32(sector + 84);
    const uint32_t entries_crc = get32(sector + 88);
    /* This library edits only the layout every tool writes: 128 entries of
     * 128 bytes. Anything else is read as invalid rather than half-understood. */
    if (entry_count != SXDISK_GPT_ENTRY_COUNT || entry_size != SXDISK_GPT_ENTRY_SIZE ||
        entries_lba < 2 || array_sectors_end(entries_lba) > disk_sectors ||
        first_usable > last_usable || last_usable >= disk_sectors) {
        return SXDISK_ERR_INVALID;
    }
    if (io->read(io->cookie, entries_lba, SXDISK_GPT_ARRAY_SECTORS, gpt->entries) != 0) {
        return SXDISK_ERR_IO;
    }
    if (sx_crc32(gpt->entries, sizeof(gpt->entries)) != entries_crc) {
        return SXDISK_ERR_INVALID;
    }
    gpt->disk_sectors = disk_sectors;
    gpt->first_usable = first_usable;
    gpt->last_usable = last_usable;
    memcpy(gpt->disk_guid, sector + 56, SXDISK_GUID_SIZE);
    return SXDISK_OK;
}

int sxdisk_gpt_read(struct sxdisk_gpt* gpt, uint64_t disk_sectors, const struct sxdisk_io* io) {
    if (gpt == NULL || io == NULL || io->read == NULL || disk_sectors < SXDISK_MIN_DISK_SECTORS) {
        return SXDISK_ERR_INVALID;
    }
    memset(gpt, 0, sizeof(*gpt));
    int rc = load_copy(gpt, disk_sectors, 1, io);
    if (rc == SXDISK_ERR_IO) {
        return rc;
    }
    if (rc == SXDISK_OK) {
        return SXDISK_OK;
    }
    rc = load_copy(gpt, disk_sectors, disk_sectors - 1, io);
    if (rc != SXDISK_OK) {
        memset(gpt, 0, sizeof(*gpt));
        return rc;
    }
    gpt->from_backup = 1;
    return SXDISK_OK;
}

static void build_header(const struct sxdisk_gpt* gpt, uint64_t lba, uint64_t alternate, uint64_t entries_lba,
                         uint32_t entries_crc, uint8_t* sector) {
    memset(sector, 0, SXDISK_SECTOR_SIZE);
    memcpy(sector, "EFI PART", 8);
    put32(sector + 8, GPT_REVISION);
    put32(sector + 12, GPT_HEADER_SIZE);
    put64(sector + 24, lba);
    put64(sector + 32, alternate);
    put64(sector + 40, gpt->first_usable);
    put64(sector + 48, gpt->last_usable);
    memcpy(sector + 56, gpt->disk_guid, SXDISK_GUID_SIZE);
    put64(sector + 72, entries_lba);
    put32(sector + 80, SXDISK_GPT_ENTRY_COUNT);
    put32(sector + 84, SXDISK_GPT_ENTRY_SIZE);
    put32(sector + 88, entries_crc);
    put32(sector + 16, sx_crc32(sector, GPT_HEADER_SIZE));
}

int sxdisk_gpt_write(const struct sxdisk_gpt* gpt, const struct sxdisk_io* io) {
    if (gpt == NULL || io == NULL || io->read == NULL || io->write == NULL ||
        gpt->disk_sectors < SXDISK_MIN_DISK_SECTORS) {
        return SXDISK_ERR_INVALID;
    }
    const uint64_t last = gpt->disk_sectors - 1;
    const uint64_t backup_entries = last - SXDISK_GPT_ARRAY_SECTORS;
    const uint32_t entries_crc = sx_crc32(gpt->entries, sizeof(gpt->entries));
    uint8_t sector[SXDISK_SECTOR_SIZE];

    /* Protective MBR: one 0xEE entry over the whole disk (capped at 32 bits).
     * Boot code a legacy loader put in the first 440 bytes is kept. */
    if (io->read(io->cookie, 0, 1, sector) != 0) {
        return SXDISK_ERR_IO;
    }
    memset(sector + MBR_BOOT_CODE_SIZE, 0, SXDISK_SECTOR_SIZE - MBR_BOOT_CODE_SIZE);
    uint8_t* entry = sector + MBR_TABLE_OFFSET;
    entry[1] = 0x00; /* CHS of LBA 1: head 0, sector 2, cylinder 0 */
    entry[2] = 0x02;
    entry[3] = 0x00;
    entry[4] = 0xee;
    entry[5] = 0xff;
    entry[6] = 0xff;
    entry[7] = 0xff;
    put32(entry + 8, 1);
    put32(entry + 12, last > 0xffffffffu ? 0xffffffffu : (uint32_t)last);
    put16(sector + 510, 0xaa55);
    if (io->write(io->cookie, 0, 1, sector) != 0) {
        return SXDISK_ERR_IO;
    }

    if (io->write(io->cookie, backup_entries, SXDISK_GPT_ARRAY_SECTORS, gpt->entries) != 0) {
        return SXDISK_ERR_IO;
    }
    build_header(gpt, last, 1, backup_entries, entries_crc, sector);
    if (io->write(io->cookie, last, 1, sector) != 0) {
        return SXDISK_ERR_IO;
    }
    if (io->write(io->cookie, 2, SXDISK_GPT_ARRAY_SECTORS, gpt->entries) != 0) {
        return SXDISK_ERR_IO;
    }
    build_header(gpt, 1, last, 2, entries_crc, sector);
    if (io->write(io->cookie, 1, 1, sector) != 0) {
        return SXDISK_ERR_IO;
    }
    return SXDISK_OK;
}

int sxdisk_entry_in_use(const struct sxdisk_gpt_entry* entry) {
    return entry != NULL && !sxdisk_guid_is_zero(entry->type_guid);
}

struct sxdisk_gpt_entry* sxdisk_gpt_entry_at(struct sxdisk_gpt* gpt, uint32_t number) {
    if (gpt == NULL || number == 0 || number > SXDISK_GPT_ENTRY_COUNT) {
        return NULL;
    }
    struct sxdisk_gpt_entry* entry = &gpt->entries[number - 1];
    return sxdisk_entry_in_use(entry) ? entry : NULL;
}

static uint64_t align_up(uint64_t value) {
    return (value + SXDISK_ALIGN_SECTORS - 1) / SXDISK_ALIGN_SECTORS * SXDISK_ALIGN_SECTORS;
}

/* The used entries sorted by first LBA (insertion sort over 128 at most). */
static size_t sorted_entries(const struct sxdisk_gpt* gpt, const struct sxdisk_gpt_entry** out) {
    size_t count = 0;
    for (size_t index = 0; index < SXDISK_GPT_ENTRY_COUNT; ++index) {
        const struct sxdisk_gpt_entry* entry = &gpt->entries[index];
        if (!sxdisk_entry_in_use(entry)) {
            continue;
        }
        size_t at = count++;
        while (at > 0 && out[at - 1]->first_lba > entry->first_lba) {
            out[at] = out[at - 1];
            --at;
        }
        out[at] = entry;
    }
    return count;
}

size_t sxdisk_gpt_free_extents(const struct sxdisk_gpt* gpt, struct sxdisk_extent* out, size_t capacity) {
    if (gpt == NULL) {
        return 0;
    }
    const struct sxdisk_gpt_entry* used[SXDISK_GPT_ENTRY_COUNT];
    const size_t used_count = sorted_entries(gpt, used);
    size_t found = 0;
    uint64_t cursor = gpt->first_usable;
    for (size_t index = 0; index <= used_count; ++index) {
        /* The gap ends right before the next partition, or at last_usable. */
        const uint64_t limit = index < used_count ? used[index]->first_lba : gpt->last_usable + 1;
        const uint64_t start = align_up(cursor);
        if (limit > start && limit - start >= SXDISK_ALIGN_SECTORS) {
            if (found < capacity && out != NULL) {
                out[found].first_lba = start;
                out[found].sector_count = limit - start;
            }
            ++found;
        }
        if (index < used_count && used[index]->last_lba + 1 > cursor) {
            cursor = used[index]->last_lba + 1;
        }
    }
    return found;
}

void sxdisk_entry_set_name(struct sxdisk_gpt_entry* entry, const char* name) {
    memset(entry->name, 0, sizeof(entry->name));
    if (name == NULL) {
        return;
    }
    for (size_t index = 0; index < SXDISK_NAME_UNITS && name[index] != 0; ++index) {
        const unsigned char c = (unsigned char)name[index];
        entry->name[index] = (uint16_t)(c < 0x80 ? c : '?');
    }
}

void sxdisk_entry_get_name(const struct sxdisk_gpt_entry* entry, char* out, size_t capacity) {
    if (out == NULL || capacity == 0) {
        return;
    }
    size_t length = 0;
    for (size_t index = 0; index < SXDISK_NAME_UNITS && length + 1 < capacity; ++index) {
        const uint16_t unit = entry->name[index];
        if (unit == 0) {
            break;
        }
        out[length++] = (unit >= 0x20 && unit < 0x7f) ? (char)unit : '?';
    }
    out[length] = 0;
}

int sxdisk_gpt_add(struct sxdisk_gpt* gpt, uint64_t first_lba, uint64_t sector_count,
                   const uint8_t type_guid[SXDISK_GUID_SIZE], const char* name,
                   const struct sxdisk_io* io, uint32_t* out_number) {
    if (gpt == NULL || type_guid == NULL || sxdisk_guid_is_zero(type_guid) || sector_count == 0) {
        return SXDISK_ERR_INVALID;
    }
    const uint64_t last_lba = first_lba + sector_count - 1;
    if (last_lba < first_lba || first_lba < gpt->first_usable || last_lba > gpt->last_usable) {
        return SXDISK_ERR_NO_SPACE;
    }
    size_t slot = SXDISK_GPT_ENTRY_COUNT;
    for (size_t index = 0; index < SXDISK_GPT_ENTRY_COUNT; ++index) {
        const struct sxdisk_gpt_entry* entry = &gpt->entries[index];
        if (!sxdisk_entry_in_use(entry)) {
            if (slot == SXDISK_GPT_ENTRY_COUNT) {
                slot = index;
            }
            continue;
        }
        if (first_lba <= entry->last_lba && entry->first_lba <= last_lba) {
            return SXDISK_ERR_NO_SPACE;
        }
    }
    if (slot == SXDISK_GPT_ENTRY_COUNT) {
        return SXDISK_ERR_FULL;
    }
    struct sxdisk_gpt_entry* entry = &gpt->entries[slot];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->type_guid, type_guid, SXDISK_GUID_SIZE);
    new_guid(io, entry->unique_guid);
    entry->first_lba = first_lba;
    entry->last_lba = last_lba;
    sxdisk_entry_set_name(entry, name);
    if (out_number != NULL) {
        *out_number = (uint32_t)(slot + 1);
    }
    return SXDISK_OK;
}

int sxdisk_gpt_remove(struct sxdisk_gpt* gpt, uint32_t number) {
    struct sxdisk_gpt_entry* entry = sxdisk_gpt_entry_at(gpt, number);
    if (entry == NULL) {
        return SXDISK_ERR_NOT_FOUND;
    }
    memset(entry, 0, sizeof(*entry));
    return SXDISK_OK;
}

const char* sxdisk_type_name(const uint8_t guid[SXDISK_GUID_SIZE]) {
    if (memcmp(guid, sxdisk_type_esp, SXDISK_GUID_SIZE) == 0) {
        return "EFI System";
    }
    if (memcmp(guid, sxdisk_type_sxfs, SXDISK_GUID_SIZE) == 0) {
        return "SavanXP SxFS";
    }
    if (memcmp(guid, sxdisk_type_basic_data, SXDISK_GUID_SIZE) == 0) {
        return "Basic data";
    }
    if (memcmp(guid, sxdisk_type_linux, SXDISK_GUID_SIZE) == 0) {
        return "Linux filesystem";
    }
    return NULL;
}

const char* sxdisk_mbr_type_name(uint8_t type) {
    switch (type) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0e: return "FAT16";
    case 0x0b: case 0x0c: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xee: return "GPT protective";
    case 0xef: return "EFI System";
    default: return NULL;
    }
}

void sxdisk_guid_format(const uint8_t guid[SXDISK_GUID_SIZE], char* out) {
    static const char digits[] = "0123456789ABCDEF";
    /* Byte order of each group as printed: three little-endian fields, then
     * the last eight bytes as stored. */
    static const uint8_t order[SXDISK_GUID_SIZE] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    size_t cursor = 0;
    for (size_t index = 0; index < SXDISK_GUID_SIZE; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            out[cursor++] = '-';
        }
        const uint8_t value = guid[order[index]];
        out[cursor++] = digits[value >> 4];
        out[cursor++] = digits[value & 0x0f];
    }
    out[cursor] = 0;
}
