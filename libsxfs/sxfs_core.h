/*
 * sxfs_core.h -- Core portable de SxFS sobre el formato de sxfs_format.h.
 *
 * Implementa la logica de allocacion, inodos, extents y directorios de SxFS de
 * forma agnostica del backend: todo el I/O de bloques pasa por un par de
 * callbacks (read/write) que el consumidor inyecta, mas un cookie opaco. Asi el
 * mismo core sirve tanto para un tool de host (callbacks sobre un archivo con
 * fseek/fread/fwrite) como, a futuro, para el driver del kernel (callbacks
 * sobre block::read/write).
 *
 * Freestanding-friendly: solo depende de sxfs_format.h, <stdint.h>, <stddef.h>
 * y <string.h> (mem*). No hace I/O propio, no usa malloc: la metadata (bitmaps
 * + tabla de inodos + superblock) vive dentro de struct sxfs_ctx, acotada. Los
 * datos de archivos y directorios se leen/escriben por callback contra la
 * region de datos.
 *
 * Asume host little-endian (igual que el kernel, que block::write-ea los structs
 * on-disk directo). Los enteros on-disk son LE y los structs se copian tal cual.
 */
#ifndef SXFS_CORE_H
#define SXFS_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "sxfs/sxfs_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Codigos de resultado. 0 = ok, negativos = error. */
enum {
    SXFS_OK = 0,
    SXFS_ERR_IO = -1,        /* fallo un callback de bloque */
    SXFS_ERR_NO_SPACE = -2,  /* sin corrida contigua de sectores */
    SXFS_ERR_NO_INODES = -3, /* tabla de inodos llena */
    SXFS_ERR_INVALID = -4,   /* argumento/ruta invalida */
    SXFS_ERR_EXISTS = -5,    /* colision de tipo (p.ej. archivo vs dir) */
    SXFS_ERR_NOT_FOUND = -6, /* componente de ruta inexistente */
    SXFS_ERR_TOO_LONG = -7,  /* nombre/ruta excede el limite del formato */
};

/* sxfs_ctx.recovery. */
enum {
    SXFS_RECOVERY_UNCLEAN = 1u,         /* the superblock is not marked clean */
    SXFS_RECOVERY_REPLAYED = 2u,        /* a pending journal was replayed in memory */
    SXFS_RECOVERY_JOURNAL_INVALID = 4u, /* the journal header is not one; ignored */
};

/* Lee/escribe `count` sectores desde/hacia `lba` en el backend. Devuelve 0 en
 * exito, distinto de 0 en error. `buffer` mide count * SXFS_SECTOR_SIZE. */
typedef int (*sxfs_read_fn)(void* cookie, uint32_t lba, uint32_t count, void* buffer);
typedef int (*sxfs_write_fn)(void* cookie, uint32_t lba, uint32_t count, const void* buffer);

struct sxfs_ctx {
    void* cookie;
    sxfs_read_fn read;
    sxfs_write_fn write;
    uint32_t total_sectors;
    /* SXFS_RECOVERY_* bits set by sxfs_open: what the image on the backend
     * still needs before it matches the metadata in memory. */
    uint32_t recovery;

    /* Metadata en memoria (fuente de verdad durante la construccion; se vuelca
     * al backend con sxfs_flush). Espeja los globals del kernel. */
    struct sxfs_superblock superblock;
    uint8_t block_bitmap[SXFS_BLOCK_BITMAP_SECTORS * SXFS_SECTOR_SIZE];
    uint8_t inode_bitmap[SXFS_INODE_BITMAP_SECTORS * SXFS_SECTOR_SIZE];
    struct sxfs_inode inodes[SXFS_MAX_INODES];

    /* Scratch para las operaciones que necesitan recorrer TODAS las entradas de
     * un directorio (resolver una ruta, agregar una entrada, quitar la ultima,
     * validar el arbol). Antes vivia en la pila de cada una de esas funciones,
     * y eso estaba bien mientras un directorio tenia 255 entradas y mal en
     * cuanto la tabla de inodos crecio: 4095 entradas son 320 KiB, y
     * walk_directory es recursiva con 32 niveles de profundidad, o sea 10 MiB de
     * pila garantizados. El buffer es del ctx, que el CLI pide en el heap, y lo
     * comparten todas ellas. No es reentrante a proposito: el ctx ya es de un
     * solo hilo. */
    struct sxfs_dir_entry dir_scratch[SXFS_MAX_RECORDS];
};

/* Ata el ctx a un backend. No toca el disco. */
void sxfs_ctx_init(struct sxfs_ctx* ctx, void* cookie, sxfs_read_fn read, sxfs_write_fn write);

/* mkfs: inicializa la metadata en memoria para un volumen de `total_sectors`
 * (marca la region de metadata como usada y crea el inodo raiz vacio). No
 * escribe nada al backend todavia; llamar sxfs_flush para persistir. */
int sxfs_format(struct sxfs_ctx* ctx, uint32_t total_sectors);

/* Opens an existing image: reads the superblock (the higher sequence of
 * primary and secondary), validates it against the format's layout and loads
 * the bitmaps and the inode table. SXFS_ERR_INVALID without a valid SxFS
 * superblock.
 *
 * It never writes. An interrupted volume is recovered IN MEMORY the way the
 * kernel recovers it at mount: a pending journal is replayed over the loaded
 * metadata. `ctx->recovery` (SXFS_RECOVERY_*) says what the image still needs,
 * and every read through the ctx sees the recovered state. The caller
 * validates that state (the tree walk) before trusting or persisting it. */
int sxfs_open(struct sxfs_ctx* ctx);
/* Persists what sxfs_open recovered: replayed metadata home, clean
 * superblocks, journal header cleared, in that order. Idempotent, and safe to
 * interrupt: opening the image again recovers to the same state. A no-op on an
 * image that needs nothing. */
int sxfs_recover(struct sxfs_ctx* ctx);
/* Writes bitmaps, inode table and both superblocks (clean, next sequence).
 * File and directory data were already written in place. SXFS_ERR_INVALID
 * while the image needs sxfs_recover: a journal left pending would be replayed
 * over the new metadata. */
int sxfs_flush(struct sxfs_ctx* ctx);
/* Formats a volume on the backend in one go: sxfs_format, the root directory
 * owned by `root_uid`/`root_gid` with `root_mode` (0 leaves it to the path
 * defaults, which make the root the system's), sxfs_flush, and a zeroed
 * journal header. The last step is what sxfs_flush alone does not do: on a
 * device that held an SxFS before, a leftover journal with `pending` set would
 * be replayed over the new volume at its first mount. */
int sxfs_mkfs(struct sxfs_ctx* ctx, uint32_t total_sectors,
              uint32_t root_uid, uint32_t root_gid, uint16_t root_mode);

/* Crea el directorio `relpath` (relativo a la raiz, separado por '/'),
 * creando los componentes intermedios que falten (mkdir -p). Idempotente si ya
 * existe como directorio. */
int sxfs_mkdir_p(struct sxfs_ctx* ctx, const char* relpath);

/* Crea o sobrescribe el archivo `relpath` con `size` bytes de `data`, creando
 * los directorios padre que falten. */
int sxfs_write_file(struct sxfs_ctx* ctx, const char* relpath, const void* data, uint32_t size);

/* Borra `relpath` (relativo a la raiz): libera los extents y el inodo, y saca
 * la entrada del directorio padre. Llamar sxfs_flush para persistir.
 *
 * Solo archivos y directorios VACIOS. Un directorio con contenido devuelve
 * SXFS_ERR_EXISTS: el borrado recursivo es politica del llamador, no del core.
 * La raiz no se puede borrar (SXFS_ERR_INVALID).
 *
 * A diferencia de sxfs_write_file, NO crea los padres que falten: borrar algo
 * bajo un directorio inexistente es SXFS_ERR_NOT_FOUND. */
int sxfs_remove(struct sxfs_ctx* ctx, const char* relpath);

/* Lee el contenido logico de un inodo. `dst_capacity` debe ser al menos
 * inode->size; no se trunca silenciosamente. El callback de `read` sigue
 * siendo la unica forma de acceder a los datos. */
int sxfs_read_inode(const struct sxfs_ctx* ctx, uint32_t inode_id,
                    void* dst, uint32_t dst_capacity, uint32_t* out_size);

/* Recorre el arbol de directorios en orden de profundidad. El callback recibe
 * rutas relativas a la raiz (sin `/` inicial), el inodo y el tipo on-disk.
 * Es una API de mantenimiento host-only y aplica el limite de ruta de 255
 * bytes. Un alias/ciclo o una entrada invalida devuelve SXFS_ERR_INVALID: las
 * operaciones de mantenimiento deben fallar cerradas, no duplicar datos. */
typedef int (*sxfs_walk_fn)(void* cookie, const char* relpath,
                            uint32_t inode_id, uint16_t type);
int sxfs_walk(const struct sxfs_ctx* ctx, sxfs_walk_fn visit, void* cookie);

/* What sxfs_repair did, and what it left because the image does not say
 * whose it is (docs/fs/SXFS_CHECK.md). */
struct sxfs_repair_report {
    /* Repaired. */
    uint32_t inodes_released;       /* marked allocated, with no record */
    uint32_t stale_records_cleared; /* a record left in a free inode */
    uint32_t entries_removed;       /* malformed, dangling, duplicate or alias entries */
    uint32_t entries_fixed;         /* an entry whose type disagreed with its inode */
    uint32_t sectors_marked;        /* claimed by an inode, free in the bitmap */
    uint32_t sectors_freed;         /* marked in the bitmap, claimed by nothing */
    uint32_t deletions_finished;    /* doomed inodes freed */
    uint32_t files_rescued;         /* unreachable files named under lost+found */
    uint32_t directories_rescued;   /* unreachable directories, with their contents */
    /* Left as found. */
    uint32_t damaged_inodes;        /* a record that contradicts itself */
    uint32_t cross_linked_sectors;  /* a sector two inodes claim */
    uint32_t leaked_sectors_kept;   /* unclaimed, kept because an inode is damaged */
    uint32_t orphans_left;          /* unreachable, and lost+found could not take them */
};

static inline uint32_t sxfs_repair_changes(const struct sxfs_repair_report* report) {
    return report->inodes_released + report->stale_records_cleared + report->entries_removed +
        report->entries_fixed + report->sectors_marked + report->sectors_freed +
        report->deletions_finished + report->files_rescued + report->directories_rescued;
}

static inline uint32_t sxfs_repair_unresolved(const struct sxfs_repair_report* report) {
    return report->damaged_inodes + report->cross_linked_sectors + report->leaked_sectors_kept +
        report->orphans_left;
}

/* Repairs the metadata in memory and the directories on the backend; the
 * caller persists with sxfs_flush. It never frees or overwrites contents it
 * can still reach: an unreachable file or directory is named under
 * /lost+found (inode-<id>, dir-<id>) before any block is reclaimed, and
 * damage whose owner the image does not identify is counted and left. Run it
 * on a copy: directories are rewritten in place, so an interrupted repair is
 * resumed by starting again from the original. SXFS_ERR_INVALID while the
 * image needs sxfs_recover, or without a usable root directory. */
int sxfs_repair(struct sxfs_ctx* ctx, struct sxfs_repair_report* report);

/* Ownership (SXFS_FEATURE_OWNERSHIP, docs/kernel/USERS_AND_PERMISSIONS.md).
 * New inodes get the default of their path on their own; these are for what
 * was there before. */

/* Gives every reachable inode with mode 0 the default of its path
 * (sxfs/sxfs_ownership.h) and sets the feature. The host's answer to an image
 * written before ownership existed. */
int sxfs_stamp_ownership(struct sxfs_ctx* ctx);
/* Records uid, gid and mode for an existing path. The mode's type bits have to
 * match the inode's type. */
int sxfs_set_ownership(struct sxfs_ctx* ctx, const char* relpath,
                       uint32_t uid, uint32_t gid, uint16_t mode);
int sxfs_get_ownership(const struct sxfs_ctx* ctx, uint32_t inode_id,
                       uint32_t* uid, uint32_t* gid, uint16_t* mode);
/* The modification time (seconds since 1970 UTC) of an existing path. */
int sxfs_set_mtime(struct sxfs_ctx* ctx, const char* relpath, uint64_t mtime);

#ifdef __cplusplus
}
#endif

#endif /* SXFS_CORE_H */
