#pragma once

#include "libc.h"

#include "savanxp/gfx2d.h"

/*
 * Iconos por tipo de archivo.
 *
 * Resuelve "que icono le toca a este archivo" con la misma division que usa
 * file_assoc para "que programa lo abre":
 *
 *   - El MAPEO es un dato de la imagen, /disk/mimeicon.ini, no una tabla
 *     compilada. docs/SXE_FORMAT.md decide que el sistema no lleva una tabla
 *     central de "que tipo es cada extension"; aca esa tabla existe pero es
 *     editable y no entra en ningun binario.
 *   - Los PIXELES son archivos .sxicon sueltos en /disk/icons, emitidos en
 *     build por tools/gen_mime_icons.py. Por el mismo motivo: el documento
 *     decide que los KiB de iconos no viajan en un segmento cargable, porque
 *     la BSS se mapea eager al exec y cada KiB horneado es RAM residente por
 *     proceso.
 *
 * Los nombres de icono siguen la Icon Naming Specification de freedesktop
 * ("text-x-generic", "image-x-generic"). Es lo que permite cambiar el origen
 * del arte sin tocar el mapeo.
 *
 * Sin malloc: capacidades fijas y truncado, igual que file_assoc.
 */

/* Con punto y en minusculas: ".txt". */
#define MIME_ICON_EXT_CAPACITY 16
#define MIME_ICON_NAME_CAPACITY 40
#define MIME_ICON_MAX_ENTRIES 64

#define MIME_ICON_POLICY_PATH "/disk/mimeicon.ini"
#define MIME_ICON_POLICY_MAX_BYTES 4096
#define MIME_ICON_DIRECTORY "/disk/icons"

/*
 * Cache de bitmaps ya cargados.
 *
 * PRESUPUESTO DE RAM, que es lo que fija estos numeros: la cache guarda solo
 * el tamano chico (16x16 = 1 KiB por icono), asi que MIME_ICON_CACHE_ENTRIES
 * iconos son 12 KiB de BSS residente. El scratch para parsear el blob entra
 * aparte y es uno solo, reusado: un .sxicon con los dos tamanos mide ~5 KiB.
 *
 * Que la cache sea del tamano chico y no de los dos es deliberado: hoy el
 * unico consumidor es la lista de filesapp, que dibuja a 16. El blob igual
 * lleva los dos tamanos porque un .sxicon incompleto obligaria a escalar en
 * runtime; el que elige cual usar es el llamador.
 */
#define MIME_ICON_CACHE_ENTRIES 12
#define MIME_ICON_SMALL_SIZE 16
#define MIME_ICON_SCRATCH_BYTES 8192

struct mime_icon_entry
{
    char extension[MIME_ICON_EXT_CAPACITY];
    char icon[MIME_ICON_NAME_CAPACITY];
};

/* Carga /disk/mimeicon.ini. Sin archivo no hay error: se queda sin mapeo y
 * todo resuelve a 0, que es "este archivo no muestra icono". Devuelve cuantas
 * extensiones quedaron mapeadas. */
int mime_icon_load(void);

/* Parseo puro del texto, punto de entrada del selftest. */
int mime_icon_parse_policy(const char *text, size_t length);

/*
 * Nombre de icono para un archivo, o 0 si ninguna regla aplica.
 *
 * Precedencia: directorio > lanzable > extension > default. Que el flag de
 * lanzable gane sobre la extension es lo que hace que un programa sin
 * extension igual muestre el icono de programa.
 */
const char *mime_icon_name_for(const char *file_name, int is_dir, int launchable);

/*
 * Bitmap de 16x16 para un nombre de icono, cargado de /disk/icons/<nombre>.sxicon
 * la primera vez y cacheado despues. Devuelve 0 si no existe o no parsea.
 *
 * El puntero es estable mientras viva el proceso: apunta a la cache, no al
 * scratch. Los fallos tambien se cachean -- un icono que no esta no se
 * reintenta en cada repintado.
 */
const struct sx_bitmap *mime_icon_small(const char *icon_name);

/* Atajo: las dos llamadas de arriba en una. 0 si el archivo no tiene icono. */
const struct sx_bitmap *mime_icon_for_file(const char *file_name, int is_dir, int launchable);

int mime_icon_count(void);
const struct mime_icon_entry *mime_icon_at(int index);

/* Valida parseo, reservadas, precedencia, truncado y limites. 0 si todo pasa. */
int mime_icon_selftest(void);
