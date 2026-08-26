#pragma once

#include "libc.h"

/*
 * Asociaciones de archivo (docs/SXE_FORMAT.md, fase 5).
 *
 * Resuelve "que programa abre este archivo" con la division que fija el
 * documento:
 *
 *   - El BINARIO declara CAPACIDAD, en el tag EXT_OPEN de su .sxmeta: "yo se
 *     abrir .txt". Declararlo no lo convierte en el que los abre.
 *   - El REGISTRO resuelve la ASOCIACION: /disk/assoc.ini dice cual gana. Es
 *     politica del usuario, igual que en Windows vive en el registro y no en
 *     el EXE.
 *
 * Precedencia: politica del usuario > primer binario que declare la extension
 * > nada (y ahi el llamador decide su propio default).
 *
 * SOLO POR EXTENSION EN ESTA ETAPA. El tag MIME_OPEN se sigue estampando y se
 * puede leer, pero resolver por tipo de contenido necesita una capa de
 * deteccion -- extension a mime, o sniffing -- que el sistema todavia no
 * tiene. Hornear aca una tabla de ".txt es text/plain" seria reintroducir
 * exactamente la clase de tabla central que este diseno vino a sacar.
 *
 * Sin malloc: capacidades fijas y truncado, igual que progman_registry.
 */

/* Con punto y en minusculas: ".txt". Alcanza para lo que declara un .sxres. */
#define FILE_ASSOC_EXT_CAPACITY 16
#define FILE_ASSOC_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define FILE_ASSOC_MAX_ENTRIES 32

/* Archivo de politica del usuario. Formato: una linea `\.ext=/ruta/programa`
 * por asociacion, con '#' y ';' como comentario. */
#define FILE_ASSOC_POLICY_PATH "/disk/assoc.ini"
#define FILE_ASSOC_POLICY_MAX_BYTES 4096

/*
 * Directorios que se escanean en busca de programas que declaren extensiones,
 * en orden de prioridad. El primero que declare una extension se la queda: sin
 * un criterio de desempate declarado, el orden estable del escaneo es mas
 * predecible que cualquier heuristica.
 */
#define FILE_ASSOC_SCAN_DIR_PRIMARY "/bin"
#define FILE_ASSOC_SCAN_DIR_SECONDARY "/disk/bin"

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
