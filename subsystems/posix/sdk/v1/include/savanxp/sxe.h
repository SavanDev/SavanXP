#pragma once

/*
 * Lector de recursos SXE (fase 1 de docs/userland/SXE_FORMAT.md).
 *
 * El formato en si vive en include/sxe/sxe_format.h; esto es la API para
 * leerlo. Dos capas separadas a proposito:
 *
 *   1. PARSEO PURO -- sxe_meta_open / sxe_icons_open trabajan sobre un buffer
 *      en memoria y no tocan disco. Es lo que el selftest ejercita, y lo que
 *      permite testear todos los casos degradados sin fabricar archivos.
 *   2. CAPA DE DISCO -- sxe_load_meta / sxe_load_icons abren el ELF, buscan la
 *      seccion por nombre y la leen al buffer que trae el llamador.
 *
 * Es el mismo corte que progman_registry: parsear es una funcion pura del
 * contenido y tocar el disco es politica aparte.
 *
 * SIN MALLOC. El llamador es dueno de la memoria en todos los casos: las
 * vistas (struct sxe_meta / struct sxe_icons) apuntan DENTRO de su buffer y
 * solo son validas mientras ese buffer viva y no se modifique.
 *
 * CONTRATO DE ERRORES: cualquier resultado distinto de SXE_OK significa lo
 * mismo para un llamador de produccion -- "este binario no trajo recursos" --
 * y se responde con los defaults (basename, icono generico, accent default).
 * Los codigos se distinguen para el selftest y para diagnostico, nunca para
 * negarse a lanzar un programa: un ELF sin metadata es un ejecutable de
 * primera clase.
 */

#include <stddef.h>
#include <stdint.h>

#include "sxe/sxe_format.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sxe_result {
    SXE_OK = 0,
    /* No hay seccion, o el archivo no abre. El caso normal, no un error. */
    SXE_ABSENT = 1,
    SXE_BAD_MAGIC = 2,
    /* Version mayor a la soportada: se rechaza entero (ver sxe_format.h). */
    SXE_BAD_VERSION = 3,
    /* El blob declara mas bytes de los que hay, o un registro se pasa del fin. */
    SXE_TRUNCATED = 4,
    /* Excede SXE_META_MAX_BYTES / SXE_ICON_MAX_BYTES, o la capacidad dada. */
    SXE_TOO_LARGE = 5,
    /* Header incoherente, desalineado, o cuentas que no cierran. */
    SXE_MALFORMED = 6,
    /* Un registro con SXE_RECORD_REQUIRED cuyo tag este lector no conoce. */
    SXE_REQUIRED_UNKNOWN = 7,
};

const char* sxe_result_string(int result);

/* Si este lector conoce el tag. Los privados (>= SXE_TAG_PRIVATE_FIRST) nunca
 * lo son: por eso un tag privado marcado REQUIRED invalida el blob. */
int sxe_tag_is_known(uint16_t tag);

/* --- Buffers ------------------------------------------------------------- */

/*
 * Los blobs se parsean en sitio, asi que el buffer tiene que estar alineado a
 * 4: sxe_icons_open devuelve punteros a struct sxe_icon_entry y a pixeles
 * uint32 que apuntan adentro. Un buffer desalineado se RECHAZA con
 * SXE_MALFORMED en vez de producir accesos mal alineados en silencio.
 *
 * Estos structs son la forma comoda de cumplirlo. No son obligatorios: la API
 * toma (void*, capacity) justamente para que un llamador traiga exactamente lo
 * que necesita.
 *
 * OJO CON LA RAM: struct sxe_icon_buffer mide 64 KiB. Declarar uno estatico es
 * 64 KiB de BSS residente por proceso (la BSS se mapea eager al exec). Si solo
 * hace falta el icono de 16x16, alcanza con un buffer mucho mas chico.
 */
struct sxe_meta_buffer {
    _Alignas(4) uint8_t bytes[SXE_META_MAX_BYTES];
    size_t length;
};

struct sxe_icon_buffer {
    _Alignas(4) uint8_t bytes[SXE_ICON_MAX_BYTES];
    size_t length;
};

/* --- .sxmeta: parseo puro ------------------------------------------------ */

/*
 * Vista validada sobre el buffer del llamador. No copia nada: `records` apunta
 * adentro del blob.
 */
struct sxe_meta {
    const uint8_t* records;
    uint32_t records_bytes;
    uint32_t record_count;
    uint16_t version;
};

/*
 * Valida el header y RECORRE TODOS LOS REGISTROS una vez, chequeando limites,
 * alineacion y REQUIRED. Que la validacion completa pase aca es lo que permite
 * que los accessors de abajo no puedan toparse con datos malformados despues.
 *
 * Devuelve SXE_OK o un enum sxe_result. En cualquier caso distinto de SXE_OK
 * deja *out en cero.
 */
int sxe_meta_open(const void* blob, size_t length, struct sxe_meta* out);

/* Primer registro con ese tag. `payload`/`length` pueden ser 0 si solo se
 * quiere saber si esta. Devuelve 1 si lo encontro, 0 si no. */
int sxe_meta_find(const struct sxe_meta* meta, uint16_t tag, const void** payload, uint32_t* length);

/*
 * Copia un payload de texto a `out`, TRUNCANDO a capacity - 1 y terminando
 * siempre en NUL. Se corta en el primer NUL embebido (los payloads no
 * garantizan terminador, pero se tolera que lo traigan).
 *
 * Devuelve la cantidad de bytes escritos sin contar el NUL; 0 si el tag no
 * esta o el payload esta vacio. Con capacity 0 no escribe nada.
 */
size_t sxe_meta_string(const struct sxe_meta* meta, uint16_t tag, char* out, size_t capacity);

/* Enteros. Devuelven 1 si el tag esta y el payload mide exactamente lo que
 * corresponde (4 y 1 bytes); 0 si no -- un payload de tamano incorrecto se
 * trata como ausente, no como error del blob. */
int sxe_meta_u32(const struct sxe_meta* meta, uint16_t tag, uint32_t* out);
int sxe_meta_u8(const struct sxe_meta* meta, uint16_t tag, uint8_t* out);

/* SXE_TAG_VERSION: major, minor, patch, build. Devuelve 1 si estaba. */
int sxe_meta_version(const struct sxe_meta* meta, uint16_t out[SXE_VERSION_COMPONENTS]);

/* Listas separadas por NUL (SXE_TAG_MIME_OPEN, SXE_TAG_EXT_OPEN). Las
 * entradas vacias no se cuentan, asi que un NUL final sobrante es inofensivo. */
int sxe_meta_list_count(const struct sxe_meta* meta, uint16_t tag);
size_t sxe_meta_list_at(const struct sxe_meta* meta, uint16_t tag, int index, char* out, size_t capacity);

/* --- .sxicon: parseo puro ------------------------------------------------ */

struct sxe_icons {
    const uint8_t* blob;
    const struct sxe_icon_entry* entries;
    uint32_t blob_bytes;
    uint32_t image_count;
    uint16_t version;
};

/*
 * Valida el header y TODAS las entradas (formato, tamanos, offsets dentro del
 * blob, longitud coherente con width*height*4, alineacion a 4 de cada offset).
 * Igual que sxe_meta_open: si vuelve SXE_OK, los accessors ya no pueden
 * encontrarse con basura.
 */
int sxe_icons_open(const void* blob, size_t length, struct sxe_icons* out);

/*
 * Mejor imagen para un tamano pedido: exacta si esta, si no la menor que sea
 * >= al pedido (agrandar se ve peor que achicar), si no la mas grande que haya.
 * Devuelve 0 solo si no hay ninguna imagen.
 */
const struct sxe_icon_entry* sxe_icons_best(const struct sxe_icons* icons, uint32_t wanted_size);

/* Pixeles de una entrada, listos para un struct sx_bitmap en
 * SX_PIXEL_FORMAT_BGRA8888 sin conversion. */
const uint32_t* sxe_icons_pixels(const struct sxe_icons* icons, const struct sxe_icon_entry* entry);

/* --- Capa de disco ------------------------------------------------------- */

/*
 * Reads one section of the ELF at `path` into the caller's buffer. It is
 * sxe_file_open + sxe_file_read_section + sxe_file_close in one call.
 *
 * Returns SXE_ABSENT if the file does not open, is not a valid ELF64, or has
 * no such section -- the three are the same case for the caller.
 * SXE_TOO_LARGE if the section does not fit in `capacity`.
 */
int sxe_read_section(const char* path, const char* section, void* buffer, size_t capacity, size_t* out_length);

/*
 * An ELF opened once to read several sections from it, so a caller that wants
 * both .sxmeta and .sxicon pays for one open and one walk of the headers.
 *
 * Opening reads the ELF header, the whole section header table and the whole
 * .shstrtab in one read each, and keeps them here: finding a section is then
 * a search in memory plus one read for its data. Every disk read is a real
 * device request (SxFS has no read cache), so this is what keeps a scan of
 * /bin cheap. A table or name table larger than the cache below is still
 * read correctly, one header and one name at a time.
 *
 * About 3.6 KiB, fine on the stack. Not shared between threads.
 */
#define SXE_FILE_SHDR_CACHE 48u
#define SXE_FILE_STRTAB_CACHE 512u

struct sxe_file {
    int fd;
    uint32_t section_count;
    uint64_t section_header_offset;
    uint64_t strtab_offset;
    uint64_t strtab_size;
    int shdrs_cached;
    int strtab_cached;
    _Alignas(8) uint8_t shdrs[SXE_FILE_SHDR_CACHE * 64u];
    char strtab[SXE_FILE_STRTAB_CACHE];
};

/* SXE_OK, or SXE_ABSENT if `path` does not open or is not an ELF64. On
 * failure nothing stays open and sxe_file_close is harmless. */
int sxe_file_open(struct sxe_file* file, const char* path);
/* Same results as sxe_read_section, against an open file. */
int sxe_file_read_section(struct sxe_file* file, const char* section, void* buffer, size_t capacity, size_t* out_length);
int sxe_file_load_meta(struct sxe_file* file, void* buffer, size_t capacity, struct sxe_meta* out);
int sxe_file_load_icons(struct sxe_file* file, void* buffer, size_t capacity, struct sxe_icons* out);
void sxe_file_close(struct sxe_file* file);

/* Conveniencia: leer + parsear en un paso. El buffer sigue siendo del
 * llamador y tiene que sobrevivir a la vista devuelta. */
int sxe_load_meta(const char* path, void* buffer, size_t capacity, struct sxe_meta* out);
int sxe_load_icons(const char* path, void* buffer, size_t capacity, struct sxe_icons* out);

/*
 * Like sxe_load_icons, but the file IS the blob: no ELF or sections around it.
 * It is what lets an icon exist in the image without a program carrying it --
 * the file-type catalog in /icons lives that way (see diskfs/mimeicon.ini).
 *
 * SXE_ABSENT if it does not open or does not parse; SXE_TOO_LARGE if the file
 * does not fit in `capacity`, told apart from ABSENT by reading one byte more
 * when filling the buffer -- without that, a blob too large would look the
 * same as a truncated one.
 */
int sxe_load_icon_file(const char* path, void* buffer, size_t capacity, struct sxe_icons* out);

/*
 * Si el path termina en SXE_FILE_EXTENSION. Es la PISTA para saltear I/O en un
 * escaneo de directorio, no la autoridad: un .sxe puede no tener recursos y un
 * .elf puede tenerlos. Usarlo para evitar aperturas, jamas para decidir si un
 * programa es lanzable.
 */
int sxe_path_has_extension(const char* path);

/* --- Selftest ------------------------------------------------------------ */

/* Valida parseo, headers rotos, truncados, tags desconocidos, REQUIRED,
 * limites de capacidad, alineacion y seleccion de iconos. Devuelve la cantidad
 * de checks fallados; 0 si todo pasa. */
int sxe_selftest(void);

#ifdef __cplusplus
}
#endif
