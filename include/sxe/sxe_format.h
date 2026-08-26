/*
 * sxe_format.h -- Definicion canonica del formato SXE.
 *
 * FUENTE DE VERDAD UNICA del layout de las secciones .sxmeta y .sxicon. Todo
 * lo que lea o escriba recursos SXE (el lector del SDK en
 * subsystems/posix/sdk/v1/runtime/sxe.c, y el generador de blobs host-side de
 * la fase 2) debe derivar de aca, no reimplementar offsets ni tamanos por su
 * cuenta. Es el mismo criterio que include/svfs/svfs_format.h: cuando el
 * formato vive duplicado a mano, las copias se desincronizan y la corrupcion
 * es silenciosa.
 *
 * Ver docs/SXE_FORMAT.md para el diseno y el razonamiento.
 *
 * Freestanding a proposito: solo depende de <stdint.h> y <stddef.h>, sin libc
 * ni headers del SDK, para compilar igual en userland y en un tool de host. Es
 * C y C++ compatible.
 *
 * Todos los enteros son little-endian. Cada campo esta alineado natural, asi
 * que no hace falta (ni conviene) #pragma pack; los SXE_STATIC_ASSERT del
 * final verifican los tamanos.
 *
 * SXE NO es un contenedor: estas secciones viven dentro de un ELF normal,
 * marcadas como NO-alloc para que el kernel nunca las mapee. El archivo sigue
 * empezando en 7F 45 4C 46 y lo carga el loader de siempre.
 */
#ifndef SXE_SXE_FORMAT_H
#define SXE_SXE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define SXE_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SXE_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* --- Nombres de seccion ELF ---------------------------------------------- */

#define SXE_SECTION_META ".sxmeta"
#define SXE_SECTION_ICON ".sxicon"

/* Extension de archivo. Es una PISTA para saltear I/O, no la autoridad: el
 * blob manda. Un .sxe puede no tener metadata valida y un .elf puede tenerla. */
#define SXE_FILE_EXTENSION ".sxe"

/* --- Topes ---------------------------------------------------------------
 *
 * Sin malloc en userland, los lectores usan buffers fijos: un blob mas grande
 * que esto se RECHAZA en vez de leerse a medias. 4 KiB sobran para texto y
 * 64 KiB entran 16+32+48 en BGRA8888 con margen (~16 KiB de uso real).
 */
#define SXE_META_MAX_BYTES 4096u
#define SXE_ICON_MAX_BYTES 65536u

/* --- .sxmeta -------------------------------------------------------------- */

#define SXE_META_MAGIC0 'S'
#define SXE_META_MAGIC1 'X'
#define SXE_META_MAGIC2 'M'
#define SXE_META_MAGIC3 'E'

/*
 * La version se sube SOLO para cambios incompatibles. El crecimiento
 * compatible se hace con tags nuevos (los desconocidos se ignoran) y con
 * header_bytes (un lector viejo saltea el header por su tamano declarado en
 * vez de asumir sizeof). Por eso un blob de version futura se rechaza entero:
 * si se pudo resolver con tags, la version no deberia haber subido.
 */
#define SXE_META_VERSION 1u

/*
 * Los tamanos on-blob se declaran como numeros ademas de derivarse de los
 * structs. El generador host-side (tools/gen_sxe_resources.py) los lee de aca
 * en vez de hardcodearlos, y los SXE_STATIC_ASSERT del final verifican que
 * sigan coincidiendo con los structs reales: si alguien cambia un campo, rompe
 * el build en vez de emitir blobs que el lector no entiende.
 */
#define SXE_META_HEADER_BYTES 16u
#define SXE_RECORD_HEADER_BYTES 8u

struct sxe_meta_header {
    uint8_t magic[4];      /* 'S','X','M','E' */
    uint16_t version;      /* SXE_META_VERSION */
    uint16_t header_bytes; /* sizeof(struct sxe_meta_header); permite crecer */
    uint32_t blob_bytes;   /* total del blob, header incluido */
    uint32_t record_count;
};

/*
 * Registro TLV. El payload arranca justo despues y se rellena con ceros hasta
 * multiplo de SXE_RECORD_ALIGNMENT; el padding NO cuenta en length.
 */
struct sxe_record {
    uint16_t tag;    /* SXE_TAG_* */
    uint16_t flags;  /* SXE_RECORD_* */
    uint32_t length; /* bytes de payload, sin el padding */
};

#define SXE_RECORD_ALIGNMENT 4u

/*
 * La regla general es "tag desconocido se ignora": asi se agregan campos sin
 * romper binarios viejos. REQUIRED es la valvula de escape para el dia que se
 * agregue algo que cambie el significado del resto -- si el lector no conoce
 * el tag, descarta el blob entero y cae a los defaults. Ningun tag de v1 lo
 * usa; esta definido ahora porque despues ya es tarde.
 */
#define SXE_RECORD_NONE 0x0000u
#define SXE_RECORD_REQUIRED 0x0001u

/* --- Espacio de tags -----------------------------------------------------
 *
 *   0x0001-0x00FF  Identidad
 *   0x0100-0x01FF  Presentacion
 *   0x0200-0x02FF  Ejecucion
 *   0x0300-0x03FF  Capacidades
 *   0x0400-0x7FFF  Reservado para el sistema
 *   0x8000-0xFFFF  Privado / experimental -- el sistema nunca los define, y
 *                  por lo tanto nunca los "conoce": un tag privado con
 *                  REQUIRED invalida el blob para cualquier lector estandar.
 */
#define SXE_TAG_PRIVATE_FIRST 0x8000u

/* Identidad. */
#define SXE_TAG_NAME 0x0001u           /* utf8 */
#define SXE_TAG_DESCRIPTION 0x0002u    /* utf8 */
#define SXE_TAG_VERSION 0x0003u        /* uint16[4]: major, minor, patch, build */
#define SXE_TAG_VERSION_STRING 0x0004u /* utf8, para mostrar */
#define SXE_TAG_VENDOR 0x0005u         /* utf8 */
#define SXE_TAG_COPYRIGHT 0x0006u      /* utf8 */
#define SXE_TAG_BUILD_ID 0x0007u       /* utf8 */

/* Presentacion. */
#define SXE_TAG_ACCENT 0x0101u       /* uint32 0x00RRGGBB, formato de gfx_rgb */
#define SXE_TAG_LAUNCH_FLAGS 0x0102u /* uint32 SAVANXP_DESKTOP_LAUNCH_FLAG_* */

/* Ejecucion. */
#define SXE_TAG_INTERPRETER 0x0201u /* utf8; ausente/vacio = lo corre el kernel */
#define SXE_TAG_SUBSYSTEM 0x0202u   /* uint8, espejo INFORMATIVO de EI_OSABI */

/* Capacidades: listas de entradas separadas por NUL. */
#define SXE_TAG_MIME_OPEN 0x0301u /* utf8: "text/plain\0text/markdown" */
#define SXE_TAG_EXT_OPEN 0x0302u  /* utf8 con punto: ".txt\0.md" */

/* Longitudes fijas de los payloads que no son texto. */
#define SXE_VERSION_COMPONENTS 4u
#define SXE_VERSION_BYTES (SXE_VERSION_COMPONENTS * 2u)

/* --- .sxicon -------------------------------------------------------------- */

#define SXE_ICON_MAGIC0 'S'
#define SXE_ICON_MAGIC1 'X'
#define SXE_ICON_MAGIC2 'I'
#define SXE_ICON_MAGIC3 'C'

#define SXE_ICON_VERSION 1u

#define SXE_ICON_HEADER_BYTES 16u
#define SXE_ICON_ENTRY_BYTES 16u

struct sxe_icon_header {
    uint8_t magic[4]; /* 'S','X','I','C' */
    uint16_t version; /* SXE_ICON_VERSION */
    uint16_t header_bytes;
    uint32_t blob_bytes;
    uint32_t image_count;
    /* struct sxe_icon_entry entries[image_count]; */
    /* pixeles, apuntados por entry.offset */
};

struct sxe_icon_entry {
    uint16_t width;
    uint16_t height;
    uint32_t format; /* SXE_ICON_FORMAT_* */
    uint32_t offset; /* desde el inicio del blob; multiplo de 4 */
    uint32_t length; /* = width * height * 4 en BGRA8888 */
};

/*
 * uint32 por pixel con valor 0xAARRGGBB (en memoria: B,G,R,A), filas de arriba
 * hacia abajo y sin padding de fila.
 *
 * El valor 2 NO es arbitrario: coincide con SX_PIXEL_FORMAT_BGRA8888 de
 * gfx2d.h a proposito, para que los pixeles de un .sxicon se le pasen a un
 * struct sx_bitmap sin traducir nada. sxe.c tiene un static assert que
 * verifica que los dos numeros sigan siendo el mismo.
 *
 * Es tambien exactamente lo que ya emite tools/gen_desktop_icon_assets.py
 * ((a << 24) | (r << 16) | (g << 8) | b) y lo que consume
 * struct desktop_embedded_bitmap: cero conversion en todo el camino.
 */
#define SXE_ICON_FORMAT_INVALID 0u
#define SXE_ICON_FORMAT_BGRA8888 2u

#define SXE_ICON_BYTES_PER_PIXEL 4u

/* Tamanos que el sistema usa hoy (desktop_icon_small / desktop_icon_large).
 * Todo .sxe deberia traer los dos; otros tamanos son validos y opcionales. */
#define SXE_ICON_SIZE_SMALL 16u
#define SXE_ICON_SIZE_LARGE 32u

/* Tope de imagenes por blob: acota el walk de validacion sin malloc. */
#define SXE_ICON_MAX_IMAGES 16u

/* --- Verificacion de layout ---------------------------------------------- */

SXE_STATIC_ASSERT(sizeof(struct sxe_meta_header) == SXE_META_HEADER_BYTES,
    "sxe_meta_header no coincide con SXE_META_HEADER_BYTES");
SXE_STATIC_ASSERT(sizeof(struct sxe_record) == SXE_RECORD_HEADER_BYTES,
    "sxe_record no coincide con SXE_RECORD_HEADER_BYTES");
SXE_STATIC_ASSERT(sizeof(struct sxe_icon_header) == SXE_ICON_HEADER_BYTES,
    "sxe_icon_header no coincide con SXE_ICON_HEADER_BYTES");
SXE_STATIC_ASSERT(sizeof(struct sxe_icon_entry) == SXE_ICON_ENTRY_BYTES,
    "sxe_icon_entry no coincide con SXE_ICON_ENTRY_BYTES");

#endif /* SXE_SXE_FORMAT_H */
