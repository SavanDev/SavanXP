#pragma once

#include "libc.h"

/*
 * File associations (docs/userland/SXE_FORMAT.md, phase 5).
 *
 * Resolves Open and Edit with the capability/policy split in the document:
 *
 *   - The BINARY declares a CAPABILITY, in the EXT_OPEN tag of its .sxmeta:
 *     "I can open .txt". Declaring it does not make it the one that opens them.
 *   - The REGISTRY resolves each ACTION: Settings/assoc.ini overrides the
 *     shipped /system/assoc.ini defaults independently for Open and Edit.
 *     Policy lives outside the executable.
 *
 * Precedence: user policy > /system/assoc.ini > the first binary declaring
 * the extension > nothing. Capabilities provide Open only; Edit is policy.
 *
 * BY EXTENSION ONLY AT THIS STAGE. The MIME_OPEN tag is still stamped and can
 * be read, but resolving by content type needs a detection layer -- extension
 * to mime, or sniffing -- that the system does not have yet. Baking a ".txt is
 * text/plain" table in here would bring back exactly the kind of central table
 * this design came to remove.
 *
 * No malloc: fixed capacities and truncation, as in progman_registry.
 */

/* Con punto y en minusculas: ".txt". Alcanza para lo que declara un .sxres. */
#define FILE_ASSOC_EXT_CAPACITY 16
#define FILE_ASSOC_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define FILE_ASSOC_MAX_ENTRIES 64

/* Policy lines: `.ext=/path/program` (Open), or `edit:.ext=/path/editor`.
 * `open:.ext` is also accepted; '#' and ';' start comments. */
#define FILE_ASSOC_POLICY_NAME "assoc.ini" /* in the profile: savanxp_settings_path */
#define FILE_ASSOC_POLICY_MAX_BYTES 4096
#define FILE_ASSOC_DEFAULTS_PATH "/system/assoc.ini"

/*
 * The first directory scanned for programs that declare extensions; /system
 * follows. The first declaration wins: without a declared tie-break rule, the stable
 * order of the scan is more predictable than any heuristic.
 */
#define FILE_ASSOC_SCAN_DIR "/bin"

struct file_assoc_entry
{
    char extension[FILE_ASSOC_EXT_CAPACITY];
    char program[FILE_ASSOC_PATH_CAPACITY];
    char editor[FILE_ASSOC_PATH_CAPACITY];
    /* Non-zero for an explicit user choice, independently for each action. */
    int from_policy;
    int edit_from_policy;
};

/* Load user policy, shipped defaults, then executable Open capabilities.
 * The caller keeps this in memory for its lifetime; there is no disk cache. */
int file_assoc_load(void);

/* Solo la politica, sin escanear. El escaneo se agrega encima. */
int file_assoc_load_policy(void);

/* Parseo puro del texto de politica: el punto de entrada del selftest, que asi
 * ejercita el formato sin depender de que haya un assoc.ini en el disco. */
int file_assoc_parse_policy(const char *text, size_t length);

/*
 * Escanea los directorios de programas y agrega lo que declaren, sin pisar lo
 * que ya fijo la politica. Devuelve cuantas asociaciones agrego.
 *
 * `exists` es inyectable por el mismo motivo que en progman_registry: permite
 * testear el escaneo sin depender de que haya en el disco. 0 = usar el real.
 */
typedef int (*file_assoc_path_exists_fn)(const char *path);
int file_assoc_scan_programs(file_assoc_path_exists_fn exists);

/*
 * Programa asociado al archivo, o 0 si ninguno lo reclama. El llamador decide
 * que hacer con el 0 -- filesapp cae a su editor por defecto --, porque "no
 * hay asociacion" no es un error.
 */
const char *file_assoc_program_for_file(const char *file_path);
/* Edit has no implicit fallback. A missing association returns NULL. */
const char *file_assoc_editor_for_file(const char *file_path);

/* Extension de un path, con punto y en minusculas. Devuelve 0 si no tiene.
 * Expuesto porque el llamador suele querer mostrarla. */
const char *file_assoc_extension_of(const char *file_path, char *out, size_t capacity);

/* Cuantos ejecutables abrio el ultimo escaneo. Es la magnitud que hay que
 * mirar antes de decidir si hace falta una cache en disco. */
int file_assoc_scan_examined(void);

int file_assoc_count(void);
const struct file_assoc_entry *file_assoc_at(int index);

/* Valida parseo, precedencia, truncado, limites y el escaneo. 0 si todo pasa. */
int file_assoc_selftest(void);
