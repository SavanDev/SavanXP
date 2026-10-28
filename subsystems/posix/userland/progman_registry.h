#pragma once

#include "libc.h"
#include "desktop_icons.h"

/*
 * Program Manager's program registry (A2.3, see docs/desktop/WM_SUBSYSTEM.md).
 *
 * It replaces the hard-coded k_menu_items table of windowd_menu.c: the groups
 * and items live in a text file on the disk, editable without recompiling the
 * OS. The format follows the idea of the original PROGMAN.INI: [group]/[item]
 * sections with key=value pairs.
 *
 *     # comment (';' too)
 *     [group]
 *     name=Main
 *
 *     [item]
 *     name=Doom
 *     path=/bin/doomgeneric
 *     desc=Classic FPS test port
 *     icon=/bin/doomgeneric
 *     flags=fullscreen
 *
 * Parser rules:
 *   - Unknown keys and sections are ignored (forward compatibility).
 *   - An item without a path, or with a path not starting at '/', is dropped.
 *   - Items declared before the first [group] fall into an implicit group.
 *   - It tolerates CRLF and spaces around keys and values.
 *   - No malloc (this libc has none): everything fits in fixed-capacity arrays
 *     and strings are truncated instead of overflowing.
 *
 * SINCE SXE PHASE 3 (docs/userland/SXE_FORMAT.md) this file changed role: it
 * is NO LONGER the catalog of the programs' identity, it is the user's
 * ARRANGEMENT. Which groups there are, what goes in each, in what order, and
 * one-off overrides. What a program is called, what it does and which icon it
 * has is said by the binary itself, through its .sxmeta/.sxicon section.
 *
 * Each field's precedence, from highest to lowest:
 *
 *   1. The key written in the .ini      -- what the USER decided
 *   2. The binary's .sxmeta/.sxicon     -- what the PROGRAM declares of itself
 *   3. The baked-in default below       -- safety net
 *   4. Generic (desktop icon)           -- last resort
 *
 * Step 2 is applied by progman_registry_apply_sxe(). Binaries without
 * resources -- still first-class executables -- simply skip that step.
 *
 * AND THE FILE IS NO LONGER THE ONLY SOURCE OF ITEMS.
 * progman_registry_scan_programs() walks /bin and adds every executable that
 * declares SXE_TAG_CATEGORY, which is how a program asks to appear in the
 * menu. The .ini stopped being required for something to show: it is the
 * ARRANGEMENT (which groups, in what order, what to rename) on top of a
 * catalog that discovers itself. Installing is copying the binary;
 * uninstalling is deleting it.
 *
 * `icon=` in an [item] NO LONGER picks from a catalog baked in by name: it
 * points at a PROGRAM whose .sxicon to borrow (icon=/bin/notepad). The
 * referenced binary does not have to be the one this item launches -- asking
 * for another program's icon is exactly the use case, e.g. a second entry for
 * the same executable under another name. Short names from before this change
 * (shell, notepad, gfxdemo, keytest, mousetest) still work for compatibility:
 * they resolve to the PATH of the program that draws that icon today, no
 * longer to a baked-in id. "desktop" and any name without an alias fall to the
 * generic icon, as does a path that cannot be read.
 */

#define PROGMAN_MAX_GROUPS 8
#define PROGMAN_MAX_ITEMS 48
#define PROGMAN_NAME_CAPACITY 32
#define PROGMAN_DESC_CAPACITY 64
#define PROGMAN_PATH_CAPACITY SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY
#define PROGMAN_REGISTRY_PATH "/progman.ini"
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
 * The directory the scan walks, the same one file_assoc (file_assoc.h) scans.
 * System programs and installed ones share it since the disk became the root,
 * so each program appears once.
 */
#define PROGMAN_SCAN_DIR "/bin"

/*
 * "All Programs": adds to the registry every installed executable that ASKS
 * to appear, that is, one that declares SXE_TAG_CATEGORY in its .sxmeta. The
 * category is the group name; the rest of the identity (name, description,
 * icon, flags) is filled in by progman_registry_apply_sxe(), as for any other
 * item. Returns how many items it added.
 *
 * The category being OPT-IN is the design decision: the program asks to be in
 * the menu, as in the XP era its installer did by creating a shortcut. The
 * opposite -- listing everything stamped -- would need a baked-in exclusion
 * list for the command-line tools (ls, cat, cp, ...) and the diagnostic
 * binaries, and that list drifts the moment someone adds a new *test.
 *
 * Rules:
 *   - It overrides NOTHING already in the registry: if an existing item
 *     already points at a binary with the same BASENAME, the candidate is
 *     skipped. So the user's .ini always wins.
 *   - The items it adds carry no overrides, so apply_sxe fills in their
 *     presentation from the binary itself.
 *   - It runs AFTER progman_registry_prune_missing() and BEFORE
 *     progman_registry_apply_sxe(): pruning reorders items, and apply_sxe
 *     assigns icon slots against the final indices.
 *   - The groups it creates and the items it adds are sorted alphabetically,
 *     after the ones already there. readdir order is the image's order, not
 *     one that helps anybody find something in a list.
 *
 * `exists` is the same injectable predicate as the pruning's. With 0 it uses
 * its own.
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

/*
 * The item's own 16x16 icon, when its .sxicon carries one at exactly that
 * size. Returns 0 otherwise; the caller then shrinks progman_item_icon().
 */
const struct desktop_embedded_bitmap *progman_item_icon_small(const struct progman_item *item);

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
