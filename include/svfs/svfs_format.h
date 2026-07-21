/*
 * svfs_format.h -- Definicion canonica del formato on-disk de SVFS2.
 *
 * FUENTE DE VERDAD UNICA del layout persistente de SVFS2. Todo lo que lee o
 * escribe una imagen de disco SVFS2 (el driver del kernel en kernel/svfs.cpp,
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
 * reservado explicito y estan verificados por SVFS_STATIC_ASSERT abajo; no hace
 * falta (ni conviene) #pragma pack porque cada campo esta alineado natural.
 */
#ifndef SVFS_SVFS_FORMAT_H
#define SVFS_SVFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define SVFS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SVFS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* --- Geometria basica ---------------------------------------------------- */

#define SVFS_SECTOR_SIZE 512u
#define SVFS_VERSION 2u
#define SVFS_FLAG_CLEAN 1u

/* Tipos de inodo (campo svfs_inode.type). */
#define SVFS_INODE_UNUSED 0u
#define SVFS_INODE_FILE 1u
#define SVFS_INODE_DIRECTORY 2u

/* Tamanos fijos de los registros on-disk (bytes). */
#define SVFS_INODE_SIZE 128u
#define SVFS_DIR_ENTRY_SIZE 80u
#define SVFS_MAX_EXTENTS 8u
#define SVFS_INODE_NAME_CAPACITY 64u /* incluye el NUL: nombre util <= 63 */

/* --- Layout de LBAs (sectores) ------------------------------------------- */
/* El orden on-disk es: [SB primario][SB secundario][journal][block bitmap]  */
/* [inode bitmap][inode table][datos...]. Las composiciones de abajo son la  */
/* fuente de verdad de las posiciones; cualquier consumidor debe derivarlas  */
/* con estas mismas formulas.                                                */

#define SVFS_PRIMARY_SB_LBA 0u
#define SVFS_SECONDARY_SB_LBA 1u
#define SVFS_JOURNAL_LBA 2u

#define SVFS_BLOCK_BITMAP_SECTORS 32u
#define SVFS_INODE_BITMAP_SECTORS 1u
#define SVFS_INODE_TABLE_SECTORS 64u

/* El journal guarda una copia fija de la metadata (bitmaps + tabla de inodos) */
/* mas su propio header de 1 sector. */
#define SVFS_JOURNAL_METADATA_SECTORS \
    (SVFS_BLOCK_BITMAP_SECTORS + SVFS_INODE_BITMAP_SECTORS + SVFS_INODE_TABLE_SECTORS)
#define SVFS_JOURNAL_SECTORS (1u + SVFS_JOURNAL_METADATA_SECTORS)

#define SVFS_BLOCK_BITMAP_LBA (SVFS_JOURNAL_LBA + SVFS_JOURNAL_SECTORS)
#define SVFS_INODE_BITMAP_LBA (SVFS_BLOCK_BITMAP_LBA + SVFS_BLOCK_BITMAP_SECTORS)
#define SVFS_INODE_TABLE_LBA (SVFS_INODE_BITMAP_LBA + SVFS_INODE_BITMAP_SECTORS)
#define SVFS_DATA_LBA (SVFS_INODE_TABLE_LBA + SVFS_INODE_TABLE_SECTORS)

/* Inodos: id 0 es invalido, id 1 es la raiz; hay SVFS_MAX_INODES entradas en  */
/* la tabla (indice = id - 1) y por lo tanto SVFS_MAX_RECORDS archivos/dirs.   */
#define SVFS_MAX_INODES 256u
#define SVFS_MAX_RECORDS (SVFS_MAX_INODES - 1u)
#define SVFS_ROOT_INODE 1u

/* Checksum: FNV-1a de 32 bits sobre el registro con su campo checksum en 0. */
#define SVFS_CHECKSUM_BASIS 2166136261u
#define SVFS_CHECKSUM_PRIME 16777619u

/* --- Magias -------------------------------------------------------------- */
/* Campos de 8 bytes rellenados con NUL. Se exponen como arrays para poder    */
/* compararlos con memcmp sobre los 8 bytes completos (incluidos los ceros    */
/* de cola), no como string literal (que leeria fuera del literal).           */

static const char svfs_superblock_magic[8] = {'S', 'V', 'F', 'S', '2', '\0', '\0', '\0'};
static const char svfs_journal_magic[8] = {'S', 'V', 'J', 'N', 'L', '2', '\0', '\0'};

/* --- Registros on-disk --------------------------------------------------- */

struct svfs_extent {
    uint32_t start_lba;
    uint32_t sector_count;
};

struct svfs_inode {
    uint32_t inode_id;
    uint16_t type;
    uint16_t reserved0;
    uint32_t size;
    uint32_t link_count;
    uint32_t extent_count;
    struct svfs_extent extents[SVFS_MAX_EXTENTS];
    uint8_t reserved[44];
};

struct svfs_superblock {
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

struct svfs_journal_header {
    char magic[8];
    uint32_t checksum;
    uint32_t sequence;
    uint32_t pending;
    uint32_t metadata_sectors;
    uint32_t reserved[122];
};

struct svfs_dir_entry {
    uint32_t inode_id;
    uint16_t type;
    uint16_t name_length;
    char name[SVFS_INODE_NAME_CAPACITY];
    uint8_t reserved[8];
};

SVFS_STATIC_ASSERT(sizeof(struct svfs_inode) == SVFS_INODE_SIZE, "svfs_inode debe medir 128 bytes");
SVFS_STATIC_ASSERT(sizeof(struct svfs_superblock) == SVFS_SECTOR_SIZE, "svfs_superblock debe medir 1 sector");
SVFS_STATIC_ASSERT(sizeof(struct svfs_journal_header) == SVFS_SECTOR_SIZE, "svfs_journal_header debe medir 1 sector");
SVFS_STATIC_ASSERT(sizeof(struct svfs_dir_entry) == SVFS_DIR_ENTRY_SIZE, "svfs_dir_entry debe medir 80 bytes");

/* --- Primitivas de formato ----------------------------------------------- */
/* Bit-math y checksum compartidos. Son la unica implementacion valida; no     */
/* reimplementar (el bug historico de bitmap salio de una segunda copia).      */

/* FNV-1a de 32 bits sobre count bytes. */
static inline uint32_t svfs_checksum(const void* data, size_t count) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t value = SVFS_CHECKSUM_BASIS;
    for (size_t index = 0; index < count; ++index) {
        value ^= bytes[index];
        value *= SVFS_CHECKSUM_PRIME;
    }
    return value;
}

/* Indexado floor: byte = bit / 8, shift = bit % 8 (LSB primero). */
static inline int svfs_bitmap_test(const uint8_t* bitmap, uint32_t bit) {
    const uint32_t byte = bit / 8u;
    const uint32_t shift = bit % 8u;
    return (bitmap[byte] & (uint8_t)(1u << shift)) != 0;
}

static inline void svfs_bitmap_set(uint8_t* bitmap, uint32_t bit, int value) {
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
static inline int svfs_magic_equals(const char a[8], const char b[8]) {
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
static inline uint32_t svfs_superblock_checksum(const struct svfs_superblock* sb) {
    struct svfs_superblock copy = *sb;
    copy.checksum = 0;
    return svfs_checksum(&copy, sizeof(copy));
}

/* Checksum de un header de journal (misma convencion que el superblock). */
static inline uint32_t svfs_journal_checksum(const struct svfs_journal_header* header) {
    struct svfs_journal_header copy = *header;
    copy.checksum = 0;
    return svfs_checksum(&copy, sizeof(copy));
}

/* Valida un superblock completo: magia, version, layout (todos los LBAs/tamanos
 * contra las constantes del formato), total_sectors > data_lba, y checksum. */
static inline int svfs_superblock_valid(const struct svfs_superblock* sb) {
    return svfs_magic_equals(sb->magic, svfs_superblock_magic) &&
        sb->version == SVFS_VERSION &&
        sb->journal_lba == SVFS_JOURNAL_LBA &&
        sb->journal_sectors == SVFS_JOURNAL_SECTORS &&
        sb->block_bitmap_lba == SVFS_BLOCK_BITMAP_LBA &&
        sb->block_bitmap_sectors == SVFS_BLOCK_BITMAP_SECTORS &&
        sb->inode_bitmap_lba == SVFS_INODE_BITMAP_LBA &&
        sb->inode_bitmap_sectors == SVFS_INODE_BITMAP_SECTORS &&
        sb->inode_table_lba == SVFS_INODE_TABLE_LBA &&
        sb->inode_table_sectors == SVFS_INODE_TABLE_SECTORS &&
        sb->data_lba == SVFS_DATA_LBA &&
        sb->max_inodes == SVFS_MAX_INODES &&
        sb->root_inode == SVFS_ROOT_INODE &&
        sb->total_sectors > sb->data_lba &&
        sb->checksum == svfs_superblock_checksum(sb);
}

/* Valida un header de journal: magia, checksum y metadata_sectors esperados. */
static inline int svfs_journal_valid(const struct svfs_journal_header* header) {
    return svfs_magic_equals(header->magic, svfs_journal_magic) &&
        header->checksum == svfs_journal_checksum(header) &&
        header->metadata_sectors == SVFS_JOURNAL_METADATA_SECTORS;
}

/* Suma de sectores cubiertos por los extents de un inodo (su capacidad). */
static inline uint32_t svfs_inode_capacity_sectors(const struct svfs_inode* inode) {
    uint32_t sectors = 0;
    for (uint32_t i = 0; i < inode->extent_count && i < SVFS_MAX_EXTENTS; ++i) {
        sectors += inode->extents[i].sector_count;
    }
    return sectors;
}

/* First-fit: primer LBA de una corrida contigua de `required` sectores libres
 * en [data_lba, total_sectors). Devuelve 0 si no hay (0 nunca es un LBA de
 * datos: es el superblock primario). */
static inline uint32_t svfs_find_free_run(const uint8_t* block_bitmap, uint32_t data_lba,
                                          uint32_t total_sectors, uint32_t required) {
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t sector = data_lba; sector < total_sectors; ++sector) {
        if (!svfs_bitmap_test(block_bitmap, sector)) {
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

#endif /* SVFS_SVFS_FORMAT_H */
