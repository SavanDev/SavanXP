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
 *     icon=doom
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
 * Los iconos del .ini se referencian por nombre contra el set horneado en build
 * (desktop_icons.h); un nombre desconocido cae al icono generico.
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
    /* Defaults horneados: no habia archivo, o no aporto ningun item valido. */
    PROGMAN_REGISTRY_SOURCE_DEFAULTS = 0,
    PROGMAN_REGISTRY_SOURCE_FILE = 1,
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
    uint32_t icon_id;      /* enum desktop_icon_id; solo si icon_slot < 0 */
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

/* Carga el registro desde PROGMAN_REGISTRY_PATH; si el archivo falta, no entra
 * en el buffer, o no aporta items validos, cae a los defaults horneados. Nunca
 * deja el registro vacio: el launcher siempre tiene algo que mostrar. */
void progman_registry_load(void);
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
 * A diferencia de progman_registry_load(), esto SI puede dejar el registro
 * vacio -- si de verdad no hay nada lanzable, mostrar nada es lo honesto.
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
