#pragma once

#include "libc.h"

#include "savanxp/gfx2d.h"

/*
 * Icons by file type.
 *
 * Answers "which icon does this file get" with the same split file_assoc uses
 * for "which program opens it":
 *
 *   - The MAPPING is data in the image, /mimeicon.ini, not a compiled table.
 *     docs/userland/SXE_FORMAT.md decides that the system carries no central
 *     table of "which type is each extension"; here that table exists but is
 *     editable and goes into no binary.
 *   - The PIXELS are loose .sxicon files in /icons, emitted at build time by
 *     tools/gen_mime_icons.py. For the same reason: the document decides that
 *     the KiB of icons do not travel in a loadable segment, because BSS is
 *     mapped eagerly at exec and every baked-in KiB is resident RAM per
 *     process.
 *
 * Icon names follow freedesktop's Icon Naming Specification
 * ("text-x-generic", "image-x-generic"). That is what allows changing where
 * the art comes from without touching the mapping.
 *
 * No malloc: fixed capacities and truncation, as in file_assoc.
 */

/* Con punto y en minusculas: ".txt". */
#define MIME_ICON_EXT_CAPACITY 16
#define MIME_ICON_NAME_CAPACITY 40
#define MIME_ICON_MAX_ENTRIES 64

#define MIME_ICON_POLICY_NAME "mimeicon.ini" /* in the profile: savanxp_settings_path */
#define MIME_ICON_POLICY_MAX_BYTES 4096
#define MIME_ICON_DIRECTORY "/icons"

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

/* Loads /mimeicon.ini. No file is not an error: it stays without a mapping and
 * everything resolves to 0, which is "this file shows no icon". Returns how
 * many extensions ended up mapped. */
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
 * The 16x16 bitmap for an icon name, loaded from /icons/<name>.sxicon the
 * first time and cached afterwards. Returns 0 if it does not exist or does not
 * parse.
 *
 * The pointer is stable for the life of the process: it points at the cache,
 * not at the scratch buffer. Failures are cached too -- an icon that is not
 * there is not retried on every repaint.
 */
const struct sx_bitmap *mime_icon_small(const char *icon_name);

/* Atajo: las dos llamadas de arriba en una. 0 si el archivo no tiene icono. */
const struct sx_bitmap *mime_icon_for_file(const char *file_name, int is_dir, int launchable);

int mime_icon_count(void);
const struct mime_icon_entry *mime_icon_at(int index);

/* Valida parseo, reservadas, precedencia, truncado y limites. 0 si todo pasa. */
int mime_icon_selftest(void);
