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
 * Caches of loaded bitmaps, one per size.
 *
 * The RAM BUDGET is what sets these numbers. The small cache (16x16 = 1 KiB
 * per icon) is MIME_ICON_CACHE_ENTRIES icons, 12 KiB of resident BSS; the
 * large one (32x32 = 4 KiB per icon) is 48 KiB more. The large one lives in
 * mime_icon_large.c, which only the programs that show large icons (Files)
 * link: --gc-sections cannot drop it from the others, because a PIE program
 * exports every global function (--export-dynamic) and so keeps it. The
 * parse scratch counts apart and is one, reused: an .sxicon with both sizes
 * measures ~5 KiB.
 *
 * The blob carries both sizes because an incomplete .sxicon would force
 * scaling at runtime; the caller picks which one it draws.
 */
#define MIME_ICON_CACHE_ENTRIES 12
#define MIME_ICON_SMALL_SIZE 16
#define MIME_ICON_LARGE_SIZE 32
#define MIME_ICON_SCRATCH_BYTES 8192

/* One cached icon of one size. */
struct mime_icon_cache_slot
{
    char name[MIME_ICON_NAME_CAPACITY];
    /* 0 = it was tried and failed. A failure is cached like a success:
     * without that, a missing icon is retried on every repaint of the list. */
    int valid;
    struct sx_bitmap bitmap;
};

/* A cache of icons of one size; the pixels are the owner's array of
 * MIME_ICON_CACHE_ENTRIES * size * size, so each size's RAM is in the file
 * that defines it. */
struct mime_icon_cache
{
    int size;
    int count;
    struct mime_icon_cache_slot *slots;
    uint32_t *pixels;
};

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
/* The same at 32x32, from its own cache: Files' Large Icons view. Defined in
 * mime_icon_large.c, which a program links only if it draws large icons. */
const struct sx_bitmap *mime_icon_large(const char *icon_name);

/* The loader behind both: the icon from `cache`, loading it the first time. */
const struct sx_bitmap *mime_icon_cache_load(struct mime_icon_cache *cache, const char *icon_name);

/* Shortcuts: mime_icon_name_for and the bitmap of one size in one call. 0 if
 * the file has no icon. */
const struct sx_bitmap *mime_icon_for_file(const char *file_name, int is_dir, int launchable);
const struct sx_bitmap *mime_icon_large_for_file(const char *file_name, int is_dir, int launchable);

int mime_icon_count(void);
const struct mime_icon_entry *mime_icon_at(int index);

/* Valida parseo, reservadas, precedencia, truncado y limites. 0 si todo pasa. */
int mime_icon_selftest(void);
