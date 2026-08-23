/*
 * svfs_core.h -- Core portable de SVFS2 sobre el formato de svfs_format.h.
 *
 * Implementa la logica de allocacion, inodos, extents y directorios de SVFS2 de
 * forma agnostica del backend: todo el I/O de bloques pasa por un par de
 * callbacks (read/write) que el consumidor inyecta, mas un cookie opaco. Asi el
 * mismo core sirve tanto para un tool de host (callbacks sobre un archivo con
 * fseek/fread/fwrite) como, a futuro, para el driver del kernel (callbacks
 * sobre block::read/write).
 *
 * Freestanding-friendly: solo depende de svfs_format.h, <stdint.h>, <stddef.h>
 * y <string.h> (mem*). No hace I/O propio, no usa malloc: la metadata (bitmaps
 * + tabla de inodos + superblock) vive dentro de struct svfs_ctx, acotada. Los
 * datos de archivos y directorios se leen/escriben por callback contra la
 * region de datos.
 *
 * Asume host little-endian (igual que el kernel, que block::write-ea los structs
 * on-disk directo). Los enteros on-disk son LE y los structs se copian tal cual.
 */
#ifndef SVFS_CORE_H
#define SVFS_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "svfs/svfs_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Codigos de resultado. 0 = ok, negativos = error. */
enum {
    SVFS_OK = 0,
    SVFS_ERR_IO = -1,        /* fallo un callback de bloque */
    SVFS_ERR_NO_SPACE = -2,  /* sin corrida contigua de sectores */
    SVFS_ERR_NO_INODES = -3, /* tabla de inodos llena */
    SVFS_ERR_INVALID = -4,   /* argumento/ruta invalida */
    SVFS_ERR_EXISTS = -5,    /* colision de tipo (p.ej. archivo vs dir) */
    SVFS_ERR_NOT_FOUND = -6, /* componente de ruta inexistente */
    SVFS_ERR_TOO_LONG = -7,  /* nombre/ruta excede el limite del formato */
};

/* Lee/escribe `count` sectores desde/hacia `lba` en el backend. Devuelve 0 en
 * exito, distinto de 0 en error. `buffer` mide count * SVFS_SECTOR_SIZE. */
typedef int (*svfs_read_fn)(void* cookie, uint32_t lba, uint32_t count, void* buffer);
typedef int (*svfs_write_fn)(void* cookie, uint32_t lba, uint32_t count, const void* buffer);

struct svfs_ctx {
    void* cookie;
    svfs_read_fn read;
    svfs_write_fn write;
    uint32_t total_sectors;

    /* Metadata en memoria (fuente de verdad durante la construccion; se vuelca
     * al backend con svfs_flush). Espeja los globals del kernel. */
    struct svfs_superblock superblock;
    uint8_t block_bitmap[SVFS_BLOCK_BITMAP_SECTORS * SVFS_SECTOR_SIZE];
    uint8_t inode_bitmap[SVFS_INODE_BITMAP_SECTORS * SVFS_SECTOR_SIZE];
    struct svfs_inode inodes[SVFS_MAX_INODES];
};

/* Ata el ctx a un backend. No toca el disco. */
void svfs_ctx_init(struct svfs_ctx* ctx, void* cookie, svfs_read_fn read, svfs_write_fn write);

/* mkfs: inicializa la metadata en memoria para un volumen de `total_sectors`
 * (marca la region de metadata como usada y crea el inodo raiz vacio). No
 * escribe nada al backend todavia; llamar svfs_flush para persistir. */
int svfs_format(struct svfs_ctx* ctx, uint32_t total_sectors);

/* Monta una imagen existente: lee el superblock (elige el de mayor secuencia
 * entre primario y secundario), lo valida contra el layout del formato y carga
 * bitmaps y tabla de inodos en el ctx. Preserva el contenido (persistencia).
 * Devuelve SVFS_ERR_INVALID si no hay un superblock SVFS2 valido. */
int svfs_open(struct svfs_ctx* ctx);

/* Persiste superblocks (primario+secundario), bitmaps y tabla de inodos al
 * backend. Los datos de archivos/directorios ya se escribieron in situ. */
int svfs_flush(struct svfs_ctx* ctx);

/* Crea el directorio `relpath` (relativo a la raiz, separado por '/'),
 * creando los componentes intermedios que falten (mkdir -p). Idempotente si ya
 * existe como directorio. */
int svfs_mkdir_p(struct svfs_ctx* ctx, const char* relpath);

/* Crea o sobrescribe el archivo `relpath` con `size` bytes de `data`, creando
 * los directorios padre que falten. */
int svfs_write_file(struct svfs_ctx* ctx, const char* relpath, const void* data, uint32_t size);

/* Borra `relpath` (relativo a la raiz): libera los extents y el inodo, y saca
 * la entrada del directorio padre. Llamar svfs_flush para persistir.
 *
 * Solo archivos y directorios VACIOS. Un directorio con contenido devuelve
 * SVFS_ERR_EXISTS: el borrado recursivo es politica del llamador, no del core.
 * La raiz no se puede borrar (SVFS_ERR_INVALID).
 *
 * A diferencia de svfs_write_file, NO crea los padres que falten: borrar algo
 * bajo un directorio inexistente es SVFS_ERR_NOT_FOUND. */
int svfs_remove(struct svfs_ctx* ctx, const char* relpath);

#ifdef __cplusplus
}
#endif

#endif /* SVFS_CORE_H */
