/*
 * sxfs_format.h -- Definicion canonica del formato on-disk de SxFS.
 *
 * FUENTE DE VERDAD UNICA del layout persistente de SxFS. Todo lo que lee o
 * escribe una imagen de disco SxFS (el driver del kernel en kernel/sxfs.cpp,
 * el futuro tool nativo del host, y --por verificacion-- el instalador
 * host-side en tools/UserAppCommon.ps1) debe derivar de aqui, no reimplementar
 * offsets, tamanos ni bit-math por su cuenta. Historicamente el formato vivia
 * duplicado a mano entre el kernel (C++) y el host (PowerShell), y las dos
 * copias se desincronizaban: ver el bug del indexado de bitmap ([int]($Bit/8)
 * redondeaba en PowerShell mientras el kernel hacia floor).
 *
 * Freestanding a proposito: solo depende de <stdint.h> y <stddef.h>, sin libc
 * ni headers del kernel, para compilar igual en el kernel y en un tool de host.
 * Es C y C++ compatible.
 *
 * Todos los enteros on-disk son little-endian. Los structs usan padding
 * reservado explicito y estan verificados por SXFS_STATIC_ASSERT abajo; no hace
 * falta (ni conviene) #pragma pack porque cada campo esta alineado natural.
 */
#ifndef SXFS_SXFS_FORMAT_H
#define SXFS_SXFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define SXFS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SXFS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* --- Geometria basica ---------------------------------------------------- */

#define SXFS_SECTOR_SIZE 512u
#define SXFS_VERSION 1u
#define SXFS_FLAG_CLEAN 1u

/* Tipos de inodo (campo sxfs_inode.type). */
#define SXFS_INODE_UNUSED 0u
#define SXFS_INODE_FILE 1u
#define SXFS_INODE_DIRECTORY 2u

/* Tamanos fijos de los registros on-disk (bytes). */
#define SXFS_INODE_SIZE 128u
#define SXFS_DIR_ENTRY_SIZE 80u
#define SXFS_MAX_EXTENTS 8u
#define SXFS_INODE_NAME_CAPACITY 64u /* incluye el NUL: nombre util <= 63 */

/* --- Layout de LBAs (sectores) ------------------------------------------- */
/* El orden on-disk es: [SB primario][SB secundario][journal][block bitmap]  */
/* [inode bitmap][inode table][datos...]. Las composiciones de abajo son la  */
/* fuente de verdad de las posiciones; cualquier consumidor debe derivarlas  */
/* con estas mismas formulas.                                                */

#define SXFS_PRIMARY_SB_LBA 0u
#define SXFS_SECONDARY_SB_LBA 1u
#define SXFS_JOURNAL_LBA 2u

#define SXFS_BLOCK_BITMAP_SECTORS 32u
#define SXFS_INODE_BITMAP_SECTORS 1u
#define SXFS_INODE_TABLE_SECTORS 64u

/* El journal guarda una copia fija de la metadata (bitmaps + tabla de inodos) */
/* mas su propio header de 1 sector. */
#define SXFS_JOURNAL_METADATA_SECTORS \
    (SXFS_BLOCK_BITMAP_SECTORS + SXFS_INODE_BITMAP_SECTORS + SXFS_INODE_TABLE_SECTORS)
#define SXFS_JOURNAL_SECTORS (1u + SXFS_JOURNAL_METADATA_SECTORS)

#define SXFS_BLOCK_BITMAP_LBA (SXFS_JOURNAL_LBA + SXFS_JOURNAL_SECTORS)
#define SXFS_INODE_BITMAP_LBA (SXFS_BLOCK_BITMAP_LBA + SXFS_BLOCK_BITMAP_SECTORS)
#define SXFS_INODE_TABLE_LBA (SXFS_INODE_BITMAP_LBA + SXFS_INODE_BITMAP_SECTORS)
#define SXFS_DATA_LBA (SXFS_INODE_TABLE_LBA + SXFS_INODE_TABLE_SECTORS)

/* Inodos: id 0 es invalido, id 1 es la raiz; hay SXFS_MAX_INODES entradas en  */
/* la tabla (indice = id - 1) y por lo tanto SXFS_MAX_RECORDS archivos/dirs.   */
#define SXFS_MAX_INODES 256u
#define SXFS_MAX_RECORDS (SXFS_MAX_INODES - 1u)
#define SXFS_ROOT_INODE 1u

/* Checksum: FNV-1a de 32 bits sobre el registro con su campo checksum en 0. */
#define SXFS_CHECKSUM_BASIS 2166136261u
#define SXFS_CHECKSUM_PRIME 16777619u

/* --- Magias -------------------------------------------------------------- */
/* Campos de 8 bytes rellenados con NUL. Se exponen como arrays para poder    */
/* compararlos con memcmp sobre los 8 bytes completos (incluidos los ceros    */
/* de cola), no como string literal (que leeria fuera del literal).           */

static const char sxfs_superblock_magic[8] = {'S', 'X', 'F', 'S', '\0', '\0', '\0', '\0'};
static const char sxfs_journal_magic[8] = {'S', 'X', 'J', 'N', 'L', '\0', '\0', '\0'};

/* --- Registros on-disk --------------------------------------------------- */

struct sxfs_extent {
    uint32_t start_lba;
    uint32_t sector_count;
};

struct sxfs_inode {
    uint32_t inode_id;
    uint16_t type;
    uint16_t reserved0;
    uint32_t size;
    uint32_t link_count;
    uint32_t extent_count;
    struct sxfs_extent extents[SXFS_MAX_EXTENTS];
    uint8_t reserved[44];
};

struct sxfs_superblock {
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

struct sxfs_journal_header {
    char magic[8];
    uint32_t checksum;
    uint32_t sequence;
    uint32_t pending;
    uint32_t metadata_sectors;
    uint32_t reserved[122];
};

struct sxfs_dir_entry {
    uint32_t inode_id;
    uint16_t type;
    uint16_t name_length;
    char name[SXFS_INODE_NAME_CAPACITY];
    uint8_t reserved[8];
};

SXFS_STATIC_ASSERT(sizeof(struct sxfs_inode) == SXFS_INODE_SIZE, "sxfs_inode debe medir 128 bytes");
SXFS_STATIC_ASSERT(sizeof(struct sxfs_superblock) == SXFS_SECTOR_SIZE, "sxfs_superblock debe medir 1 sector");
SXFS_STATIC_ASSERT(sizeof(struct sxfs_journal_header) == SXFS_SECTOR_SIZE, "sxfs_journal_header debe medir 1 sector");
SXFS_STATIC_ASSERT(sizeof(struct sxfs_dir_entry) == SXFS_DIR_ENTRY_SIZE, "sxfs_dir_entry debe medir 80 bytes");

/* --- Primitivas de formato ----------------------------------------------- */
/* Bit-math y checksum compartidos. Son la unica implementacion valida; no     */
/* reimplementar (el bug historico de bitmap salio de una segunda copia).      */

/* FNV-1a de 32 bits sobre count bytes. */
static inline uint32_t sxfs_checksum(const void* data, size_t count) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t value = SXFS_CHECKSUM_BASIS;
    for (size_t index = 0; index < count; ++index) {
        value ^= bytes[index];
        value *= SXFS_CHECKSUM_PRIME;
    }
    return value;
}

/* Indexado floor: byte = bit / 8, shift = bit % 8 (LSB primero). */
static inline int sxfs_bitmap_test(const uint8_t* bitmap, uint32_t bit) {
    const uint32_t byte = bit / 8u;
    const uint32_t shift = bit % 8u;
    return (bitmap[byte] & (uint8_t)(1u << shift)) != 0;
}

static inline void sxfs_bitmap_set(uint8_t* bitmap, uint32_t bit, int value) {
    const uint32_t byte = bit / 8u;
    const uint32_t shift = bit % 8u;
    const uint8_t mask = (uint8_t)(1u << shift);
    if (value) {
        bitmap[byte] |= mask;
    } else {
        bitmap[byte] &= (uint8_t)(~mask);
    }
}

/* Compara dos campos de magia de 8 bytes (incluidos los NUL de cola) sin
 * depender de memcmp, que no esta garantizado en freestanding. */
static inline int sxfs_magic_equals(const char a[8], const char b[8]) {
    for (int i = 0; i < 8; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* --- Validacion y helpers de metadata (puros, sin estado ni I/O) ---------- */
/* Estos son la logica compartida entre el driver del kernel y el builder de
 * host: validacion de superblock/journal, checksum y allocacion first-fit. No
 * tocan disco ni globals; operan sobre lo que se les pasa. */

/* Checksum de un superblock: FNV-1a sobre el registro con su campo checksum=0. */
static inline uint32_t sxfs_superblock_checksum(const struct sxfs_superblock* sb) {
    struct sxfs_superblock copy = *sb;
    copy.checksum = 0;
    return sxfs_checksum(&copy, sizeof(copy));
}

/* Checksum de un header de journal (misma convencion que el superblock). */
static inline uint32_t sxfs_journal_checksum(const struct sxfs_journal_header* header) {
    struct sxfs_journal_header copy = *header;
    copy.checksum = 0;
    return sxfs_checksum(&copy, sizeof(copy));
}

/* Valida un superblock completo: magia, version, layout (todos los LBAs/tamanos
 * contra las constantes del formato), total_sectors > data_lba, y checksum. */
static inline int sxfs_superblock_valid(const struct sxfs_superblock* sb) {
    return sxfs_magic_equals(sb->magic, sxfs_superblock_magic) &&
        sb->version == SXFS_VERSION &&
        sb->journal_lba == SXFS_JOURNAL_LBA &&
        sb->journal_sectors == SXFS_JOURNAL_SECTORS &&
        sb->block_bitmap_lba == SXFS_BLOCK_BITMAP_LBA &&
        sb->block_bitmap_sectors == SXFS_BLOCK_BITMAP_SECTORS &&
        sb->inode_bitmap_lba == SXFS_INODE_BITMAP_LBA &&
        sb->inode_bitmap_sectors == SXFS_INODE_BITMAP_SECTORS &&
        sb->inode_table_lba == SXFS_INODE_TABLE_LBA &&
        sb->inode_table_sectors == SXFS_INODE_TABLE_SECTORS &&
        sb->data_lba == SXFS_DATA_LBA &&
        sb->max_inodes == SXFS_MAX_INODES &&
        sb->root_inode == SXFS_ROOT_INODE &&
        sb->total_sectors > sb->data_lba &&
        sb->checksum == sxfs_superblock_checksum(sb);
}

/* Valida un header de journal: magia, checksum y metadata_sectors esperados. */
static inline int sxfs_journal_valid(const struct sxfs_journal_header* header) {
    return sxfs_magic_equals(header->magic, sxfs_journal_magic) &&
        header->checksum == sxfs_journal_checksum(header) &&
        header->metadata_sectors == SXFS_JOURNAL_METADATA_SECTORS;
}

/* Suma de sectores cubiertos por los extents de un inodo (su capacidad). */
static inline uint32_t sxfs_inode_capacity_sectors(const struct sxfs_inode* inode) {
    uint32_t sectors = 0;
    for (uint32_t i = 0; i < inode->extent_count && i < SXFS_MAX_EXTENTS; ++i) {
        sectors += inode->extents[i].sector_count;
    }
    return sectors;
}

/* First-fit: primer LBA de una corrida contigua de `required` sectores libres
 * en [data_lba, total_sectors). Devuelve 0 si no hay (0 nunca es un LBA de
 * datos: es el superblock primario). */
static inline uint32_t sxfs_find_free_run(const uint8_t* block_bitmap, uint32_t data_lba,
                                          uint32_t total_sectors, uint32_t required) {
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t sector = data_lba; sector < total_sectors; ++sector) {
        if (!sxfs_bitmap_test(block_bitmap, sector)) {
            if (run_length == 0) {
                run_start = sector;
            }
            if (++run_length >= required) {
                return run_start;
            }
        } else {
            run_length = 0;
        }
    }
    return 0;
}

#endif /* SXFS_SXFS_FORMAT_H */
