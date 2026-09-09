#pragma once

#include "libc.h"
#include "desktop_icons.h"

/*
 * Registro de programas del Program Manager (A2.3, ver docs/WM_SUBSYSTEM.md).
 *
 * Reemplaza la tabla hardcodeada k_menu_items de windowd_menu.c: los grupos y
 * items viven en un archivo de texto en /disk, editable sin recompilar el SO.
 * El formato sigue la idea del PROGMAN.INI original: secciones [group]/[item]
 * con pares clave=valor.
 *
 *     # comentario (tambien ';')
 *     [group]
 *     name=Main
 *
 *     [item]
 *     name=Doom
 *     path=/disk/bin/doomgeneric
 *     desc=Classic FPS test port
 *     icon=/disk/bin/doomgeneric
 *     flags=fullscreen
 *
 * Reglas del parser:
 *   - Claves y secciones desconocidas se ignoran (compatibilidad hacia adelante).
 *   - Un item sin path, o con path que no empieza en '/', se descarta.
 *   - Items declarados antes del primer [group] caen en un grupo implicito.
 *   - Tolera CRLF y espacios alrededor de claves y valores.
 *   - Sin malloc (esta libc no tiene): todo entra en arrays de capacidad fija y
 *     los strings se truncan en vez de desbordar.
 *
 * A PARTIR DE LA FASE 3 DE SXE (docs/SXE_FORMAT.md) este archivo cambio de rol:
 * ya NO es el catalogo de identidad de los programas, es el ARREGLO del
 * usuario. Que grupos hay, que entra en cada uno, en que orden, y overrides
 * puntuales. Quien dice como se llama un programa, que hace y que icono tiene
 * es el propio binario, via su seccion .sxmeta/.sxicon.
 *
 * Precedencia de cada campo, de mayor a menor:
 *
 *   1. La clave escrita en el .ini      -- lo que el USUARIO decidio
 *   2. El .sxmeta/.sxicon del binario   -- lo que el PROGRAMA declara de si
 *   3. El default horneado de abajo     -- red de seguridad
 *   4. Generico (icono desktop)         -- ultimo recurso
 *
 * El paso 2 lo aplica progman_registry_apply_sxe(). Los binarios sin recursos
 * -- que siguen siendo ejecutables de primera clase -- simplemente se saltean
 * ese escalon.
 *
 * Y EL ARCHIVO YA NO ES LA UNICA FUENTE DE ITEMS. progman_registry_scan_programs()
 * recorre /bin y /disk/bin y da de alta cada ejecutable que declare
 * SXE_TAG_CATEGORY, que es como un programa pide aparecer en el menu. El .ini
 * dejo de ser obligatorio para que algo se vea: es el ARREGLO (que grupos hay,
 * en que orden, que renombrar) por encima de un catalogo que se descubre solo.
 * Instalar es copiar el binario; desinstalar es borrarlo.
 *
 * `icon=` en un [item] YA NO elige de un catalogo horneado por nombre: apunta
 * a un PROGRAMA cuyo .sxicon tomar prestado (icon=/bin/notepad). El binario
 * referenciado no tiene que ser el que este item lanza -- pedir el icono de
 * otro programa es exactamente el caso de uso, p.ej. una segunda entrada del
 * mismo ejecutable con otro nombre. Nombres cortos previos a este cambio
 * (shell, notepad, gfxdemo, keytest, mousetest) siguen andando por
 * compatibilidad: resuelven al PATH del programa que hoy dibuja ese icono, ya
 * no a un id horneado. "desktop" y cualquier nombre sin alias caen al icono
 * generico, igual que un path que no se puede leer.
 */

#define PROGMAN_MAX_GROUPS 8
#define PROGMAN_MAX_ITEMS 48
#define PROGMAN_NAME_CAPACITY 32
#define PROGMAN_DESC_CAPACITY 64
#define PROGMAN_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define PROGMAN_REGISTRY_PATH "/disk/progman.ini"
/* Tope del archivo que se lee a un buffer estatico. */
#define PROGMAN_REGISTRY_MAX_BYTES 8192

enum progman_registry_source
{
    /* Defaults horneados: ni el archivo ni el escaneo aportaron un solo item. */
    PROGMAN_REGISTRY_SOURCE_DEFAULTS = 0,
    PROGMAN_REGISTRY_SOURCE_FILE = 1,
    /* No habia .ini utilizable y el catalogo lo armo el escaneo del disco. */
    PROGMAN_REGISTRY_SOURCE_SCAN = 2,
};

/*
 * Campos que el .ini declaro EXPLICITAMENTE. Sin esto no se puede distinguir
 * "el usuario eligio este nombre" de "quedo el valor por defecto", y el .sxe
 * pisaria decisiones del usuario o al reves.
 */
#define PROGMAN_OVERRIDE_NONE 0x00000000u
#define PROGMAN_OVERRIDE_NAME 0x00000001u
#define PROGMAN_OVERRIDE_DESCRIPTION 0x00000002u
#define PROGMAN_OVERRIDE_ICON 0x00000004u
#define PROGMAN_OVERRIDE_FLAGS 0x00000008u

/* icon_slot cuando el binario no trajo icono propio: se usa icon_id. */
#define PROGMAN_ICON_SLOT_NONE (-1)

struct progman_item
{
    char name[PROGMAN_NAME_CAPACITY];
    char path[PROGMAN_PATH_CAPACITY];
    char description[PROGMAN_DESC_CAPACITY];
    /* Vacio salvo que icon= haya resuelto a un programa (propio o de un alias
     * legado): ahi apply_sxe lee el .sxicon de ESTE path en vez del de
     * `path`. Vacio con PROGMAN_OVERRIDE_ICON puesto = nombre sin alias
     * (p.ej. "desktop") -- se queda en icon_id generico, sin leer nada. */
    char icon_borrow_path[PROGMAN_PATH_CAPACITY];
    uint32_t icon_id;      /* enum desktop_icon_id; ultimo recurso si nada mas resolvio */
    uint32_t launch_flags; /* SAVANXP_DESKTOP_LAUNCH_FLAG_* */
    uint32_t overrides;    /* PROGMAN_OVERRIDE_* */
    int icon_slot;         /* indice en el pool de iconos .sxicon, o NONE */
    int group_index;
};

struct progman_group
{
    char name[PROGMAN_NAME_CAPACITY];
    int item_count;
};

/*
 * Carga el registro desde PROGMAN_REGISTRY_PATH. Devuelve la cantidad de items
 * que aporto el archivo; si falta, no entra en el buffer o no tiene ningun item
 * valido, devuelve 0 y DEJA EL REGISTRO VACIO.
 *
 * Antes esta funcion caia sola a los defaults horneados. Ya no: con el escaneo
 * (progman_registry_scan_programs) los defaults pasaron a ser la red de
 * seguridad de ULTIMO recurso, y solo tienen sentido despues de que el escaneo
 * TAMBIEN se haya venido con las manos vacias. Quien orquesta esa secuencia es
 * el llamador -- progman.c --, porque es el unico que puede decidir entre las
 * dos fuentes con el disco delante.
 */
int progman_registry_load_file(void);
/* Carga solo los defaults horneados (instalacion fresca / recuperacion). */
void progman_registry_load_defaults(void);
/* Parsea desde memoria. Devuelve la cantidad de items validos cargados. Es el
 * punto de entrada que usa el selftest para ejercitar el parser sin disco. */
int progman_registry_parse(const char *text, size_t length);

/*
 * Predicado de existencia, inyectable para poder testear el pruning sin disco:
 * progman pasa el que abre el path, el selftest uno falso.
 */
typedef int (*progman_path_exists_fn)(const char *path);

/*
 * Descarta los items cuyo path no existe y, con ellos, los grupos que quedan
 * vacios; los items que sobreviven quedan reapuntados a los indices nuevos.
 * Devuelve cuantos items se descartaron.
 *
 * Es una etapa APARTE del parseo a proposito: parsear es una funcion pura del
 * texto (y asi la ejercita el selftest), y esto es la politica que toca el
 * disco. Aplica igual a los defaults horneados y a lo que venga del .ini: una
 * entrada que no se puede lanzar es ruido venga de donde venga.
 *
 * Esto SI puede dejar el registro vacio -- si de verdad no hay nada lanzable,
 * mostrar nada es lo honesto. El escaneo corre despues y tiene su chance.
 */
int progman_registry_prune_missing(progman_path_exists_fn exists);

/*
 * Rellena los items con lo que declara el .sxmeta/.sxicon de su binario, sin
 * pisar lo que el .ini haya declarado explicitamente. Devuelve cuantos items
 * tomaron algo del ejecutable.
 *
 * Va DESPUES de progman_registry_prune_missing(): no tiene sentido abrir el
 * binario de un item que se va a descartar, y el pruning reordena los items,
 * lo que invalidaria los slots de icono ya asignados.
 *
 * Que un binario no tenga recursos, no abra, o traiga un blob invalido no es
 * un error: ese item simplemente se queda con sus valores previos.
 */
int progman_registry_apply_sxe(void);

/*
 * Directorios que recorre el escaneo, en este orden. Espejan los de file_assoc
 * (file_assoc.h) por el mismo motivo: /disk/bin es una COPIA de /bin, asi que
 * cada programa del sistema aparece dos veces y hace falta una regla estable
 * de desempate en vez de una heuristica.
 */
#define PROGMAN_SCAN_DIR_PRIMARY "/bin"
#define PROGMAN_SCAN_DIR_SECONDARY "/disk/bin"

/*
 * "Todos los programas": agrega al registro cada ejecutable instalado que PIDA
 * aparecer, o sea que declare SXE_TAG_CATEGORY en su .sxmeta. La categoria es
 * el nombre del grupo; el resto de la identidad (nombre, descripcion, icono,
 * flags) la completa progman_registry_apply_sxe() como para cualquier otro
 * item. Devuelve cuantos items agrego.
 *
 * Que la categoria sea OPT-IN es la decision de diseno: el alta en el menu la
 * pide el programa, igual que en la era XP la pedia su instalador creando un
 * acceso directo. Lo contrario -- listar todo lo estampado -- obligaria a una
 * lista de exclusion horneada para busybox (30 copias del MISMO binario bajo
 * nombres distintos, ver build.ps1) y para los binarios de diagnostico, y esa
 * lista se desincroniza sola en cuanto alguien agrega un *test nuevo.
 *
 * Reglas:
 *   - No pisa NADA de lo que ya haya en el registro: si un item existente ya
 *     apunta a un binario con el mismo BASENAME, el candidato se saltea. Asi
 *     el .ini del usuario siempre gana, y /disk/bin no duplica a /bin.
 *   - Los items que agrega entran sin overrides, para que apply_sxe complete
 *     su presentacion desde el propio binario.
 *   - Va DESPUES de progman_registry_prune_missing() y ANTES de
 *     progman_registry_apply_sxe(): el pruning reordena items y apply_sxe
 *     asigna los slots de icono contra los indices finales.
 *   - Los grupos que crea y los items que agrega quedan ordenados
 *     alfabeticamente, atras de los que ya estaban. El orden de readdir es el
 *     de la imagen, no uno que le sirva a nadie para buscar en una lista.
 *
 * `exists` es el mismo predicado inyectable del pruning. Con 0 usa el propio.
 */
int progman_registry_scan_programs(progman_path_exists_fn exists);

/* Cuantos ejecutables abrio el ultimo escaneo. Lo reporta el smoke: es la
 * magnitud a mirar antes de decidir si hace falta una cache. */
int progman_registry_scan_examined(void);

/*
 * Icono propio del item, traido de su .sxicon. Devuelve 0 si el binario no
 * trajo uno; ahi el llamador cae a desktop_icon_large(item->icon_id).
 */
const struct desktop_embedded_bitmap *progman_item_icon(const struct progman_item *item);

int progman_registry_source(void);
int progman_group_count(void);
const struct progman_group *progman_group_at(int index);
int progman_item_count(void);
const struct progman_item *progman_item_at(int index);
/* Item i-esimo dentro de un grupo (los items guardan su group_index). */
const struct progman_item *progman_group_item_at(int group_index, int item_index);

/* Valida parser, defaults, mapeo de iconos/flags, truncado y limites de
 * capacidad. Devuelve 0 si todo pasa. */
int progman_registry_selftest(void);
