#pragma once

#include "libc.h"
#include "desktop_icons.h"

/*
 * Registro de programas del Program Manager (A2.3, ver docs/WM_SUBSYSTEM.md).
 *
 * Reemplaza la tabla hardcodeada k_menu_items de desktop_menu.c: los grupos y
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
 * Los iconos se referencian por nombre contra el set horneado en build
 * (desktop_icons.h); un nombre desconocido cae al icono generico. Traer iconos
 * propios por programa necesitaria un formato+loader de iconos: trabajo aparte.
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

struct progman_item
{
    char name[PROGMAN_NAME_CAPACITY];
    char path[PROGMAN_PATH_CAPACITY];
    char description[PROGMAN_DESC_CAPACITY];
    uint32_t icon_id;      /* enum desktop_icon_id */
    uint32_t launch_flags; /* SAVANXP_DESKTOP_LAUNCH_FLAG_* */
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
