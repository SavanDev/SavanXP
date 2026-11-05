/*
 * sxdisk.h -- partition tables for Disk Management and setup.
 *
 * Reads, edits and writes GUID partition tables (protective MBR, primary and
 * backup headers and entry arrays, both CRC32s) through I/O callbacks, so the
 * same code runs in a SUN program over the storage calls and on the host
 * over a file. Freestanding: <stdint.h>, <stddef.h> and <string.h> (mem*), no
 * malloc; a table lives in a caller-owned struct sxdisk_gpt (about 17 KiB).
 *
 * The kernel's reader (kernel/partition.cpp) is the other half of the
 * contract: a table this library writes is one it slices. The host test
 * (tests/host/sxdisk_test.cpp) runs the two against each other.
 *
 * Writing MBR tables, extended partitions and resizing are out of scope
 * (docs/userland/DISK_MANAGEMENT.md).
 */
#ifndef SXDISK_SXDISK_H
#define SXDISK_SXDISK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXDISK_SECTOR_SIZE 512u
#define SXDISK_GPT_ENTRY_COUNT 128u
#define SXDISK_GPT_ENTRY_SIZE 128u
#define SXDISK_GPT_ARRAY_SECTORS (SXDISK_GPT_ENTRY_COUNT * SXDISK_GPT_ENTRY_SIZE / SXDISK_SECTOR_SIZE)
/* Partitions start on 1 MiB boundaries, as every current tool does. */
#define SXDISK_ALIGN_SECTORS 2048u
#define SXDISK_GUID_SIZE 16u
#define SXDISK_NAME_UNITS 36u
/* A disk smaller than this has no room for a GPT and one aligned partition. */
#define SXDISK_MIN_DISK_SECTORS (2u * SXDISK_ALIGN_SECTORS + 2u * (1u + SXDISK_GPT_ARRAY_SECTORS) + 1u)

enum {
    SXDISK_OK = 0,
    SXDISK_ERR_IO = -1,       /* a callback failed */
    SXDISK_ERR_INVALID = -2,  /* bad argument, or no valid table */
    SXDISK_ERR_NO_SPACE = -3, /* the range is not free or not inside the usable area */
    SXDISK_ERR_FULL = -4,     /* all 128 entries are in use */
    SXDISK_ERR_NOT_FOUND = -5,/* no partition with that number */
};

/* Sector I/O on the whole disk. Return 0 on success. A caller whose device
 * limits the transfer size splits inside the callback. */
typedef int (*sxdisk_read_fn)(void* cookie, uint64_t lba, uint32_t count, void* buffer);
typedef int (*sxdisk_write_fn)(void* cookie, uint64_t lba, uint32_t count, const void* buffer);
/* Fills `size` bytes with random data, for new GUIDs. */
typedef void (*sxdisk_random_fn)(void* cookie, void* out, size_t size);

struct sxdisk_io {
    void* cookie;
    sxdisk_read_fn read;
    sxdisk_write_fn write;
    sxdisk_random_fn random;
};

/* One GPT entry, exactly as on disk (128 bytes, little-endian). */
struct sxdisk_gpt_entry {
    uint8_t type_guid[SXDISK_GUID_SIZE];
    uint8_t unique_guid[SXDISK_GUID_SIZE];
    uint64_t first_lba;
    uint64_t last_lba; /* inclusive */
    uint64_t attributes;
    uint16_t name[SXDISK_NAME_UNITS]; /* UTF-16LE */
};

struct sxdisk_gpt {
    uint64_t disk_sectors;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[SXDISK_GUID_SIZE];
    /* sxdisk_gpt_read found the primary damaged and used the backup. The next
     * sxdisk_gpt_write repairs it. */
    int from_backup;
    struct sxdisk_gpt_entry entries[SXDISK_GPT_ENTRY_COUNT];
};

/* A free range, aligned to SXDISK_ALIGN_SECTORS at its start. */
struct sxdisk_extent {
    uint64_t first_lba;
    uint64_t sector_count;
};

/* An empty table for a disk of `disk_sectors`, with a fresh disk GUID. Writes
 * nothing. */
int sxdisk_gpt_init(struct sxdisk_gpt* gpt, uint64_t disk_sectors, const struct sxdisk_io* io);

/* Loads the table: the primary header if it passes both CRCs, otherwise the
 * backup on the last sector. SXDISK_ERR_INVALID when neither is valid. */
int sxdisk_gpt_read(struct sxdisk_gpt* gpt, uint64_t disk_sectors, const struct sxdisk_io* io);

/* Writes the whole table: protective MBR (keeping any boot code in its first
 * 440 bytes), backup entries and header, then primary entries and header. An
 * interruption leaves at least one complete copy, the old or the new. */
int sxdisk_gpt_write(const struct sxdisk_gpt* gpt, const struct sxdisk_io* io);

/* The free ranges of at least SXDISK_ALIGN_SECTORS, in disk order. Returns how
 * many there are (it may exceed `capacity`; only `capacity` are stored). */
size_t sxdisk_gpt_free_extents(const struct sxdisk_gpt* gpt, struct sxdisk_extent* out, size_t capacity);

/* Adds a partition over [first_lba, first_lba + sector_count) in the first
 * empty entry, with a fresh unique GUID and an ASCII `name` (may be NULL).
 * `*out_number`, if given, receives its 1-based entry number. */
int sxdisk_gpt_add(struct sxdisk_gpt* gpt, uint64_t first_lba, uint64_t sector_count,
                   const uint8_t type_guid[SXDISK_GUID_SIZE], const char* name,
                   const struct sxdisk_io* io, uint32_t* out_number);

/* Empties entry `number` (1-based). */
int sxdisk_gpt_remove(struct sxdisk_gpt* gpt, uint32_t number);

/* Entry `number` (1-based) if it is in use, else NULL. */
struct sxdisk_gpt_entry* sxdisk_gpt_entry_at(struct sxdisk_gpt* gpt, uint32_t number);

int sxdisk_entry_in_use(const struct sxdisk_gpt_entry* entry);
void sxdisk_entry_set_name(struct sxdisk_gpt_entry* entry, const char* name);
/* The entry's name as ASCII ('?' for anything else); `out` holds 37 bytes. */
void sxdisk_entry_get_name(const struct sxdisk_gpt_entry* entry, char* out, size_t capacity);

/* Partition types, as stored on disk. */
extern const uint8_t sxdisk_type_esp[SXDISK_GUID_SIZE];
extern const uint8_t sxdisk_type_sxfs[SXDISK_GUID_SIZE];
extern const uint8_t sxdisk_type_basic_data[SXDISK_GUID_SIZE];
extern const uint8_t sxdisk_type_linux[SXDISK_GUID_SIZE];

/* "EFI System", "SavanXP SxFS", "Basic data", "Linux filesystem", or NULL. */
const char* sxdisk_type_name(const uint8_t guid[SXDISK_GUID_SIZE]);
/* A name for an MBR partition type, or NULL. */
const char* sxdisk_mbr_type_name(uint8_t type);
/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"; `out` holds 37 bytes. */
void sxdisk_guid_format(const uint8_t guid[SXDISK_GUID_SIZE], char* out);
int sxdisk_guid_is_zero(const uint8_t guid[SXDISK_GUID_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SXDISK_SXDISK_H */
