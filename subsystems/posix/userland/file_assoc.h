#pragma once

#include "libc.h"

/*
 * File associations (docs/userland/SXE_FORMAT.md, phase 5).
 *
 * Answers "which program opens this file" with the split the document sets:
 *
 *   - The BINARY declares a CAPABILITY, in the EXT_OPEN tag of its .sxmeta:
 *     "I can open .txt". Declaring it does not make it the one that opens them.
 *   - The REGISTRY resolves the ASSOCIATION: /assoc.ini says which one wins.
 *     It is the user's policy, as on Windows it lives in the registry and not
 *     in the EXE.
 *
 * Precedence: the user's policy > the first binary that declares the extension
 * > nothing (and then the caller decides its own default).
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
#define FILE_ASSOC_MAX_ENTRIES 32

/* The user's policy file. Format: one `.ext=/path/program` line per
 * association, with '#' and ';' starting a comment. */
#define FILE_ASSOC_POLICY_NAME "assoc.ini" /* in the profile: savanxp_settings_path */
#define FILE_ASSOC_POLICY_MAX_BYTES 4096

/*
 * The directory scanned for programs that declare extensions. The first one to
 * declare an extension keeps it: without a declared tie-break rule, the stable
 * order of the scan is more predictable than any heuristic.
 */
#define FILE_ASSOC_SCAN_DIR "/bin"

struct file_assoc_entry
{
    char extension[FILE_ASSOC_EXT_CAPACITY];
    char program[FILE_ASSOC_PATH_CAPACITY];
    /* 1 = lo fijo el usuario en assoc.ini; 0 = lo declaro un binario. */
    int from_policy;
};

/*
 * Carga la politica y escanea los binarios instalados. Es caro (abre el
 * .sxmeta de cada ejecutable), asi que se llama UNA vez y el resultado queda
 * en memoria mientras viva el proceso.
 *
 * Deliberadamente sin cache en disco: la decision del documento es medir
 * primero. Un indice persistente agrega bugs de invalidacion que no se pagan
 * hasta saber que el escaneo se nota.
 *
 * Devuelve cuantas asociaciones quedaron.
 */
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
