#pragma once

#include "libc.h"

#include "savanxp/gfx2d.h"

/*
 * The catalog of programs that can be uninstalled (Add or Remove Programs).
 *
 * WHAT CAN BE UNINSTALLED, which is this module's one real decision:
 *
 *     it is in /bin  AND  it is NOT in /rescue/bin
 *
 * Not a blocklist and not a guess from the name: it is exactly the set of what
 * was installed separately. /bin is on the system disk, and the build stages
 * the system's programs there; the initramfs, published under /rescue, carries
 * the same programs (docs/kernel/FILESYSTEMS.md). The subtraction is what tells
 * "came with the OS" from "someone installed it".
 *
 * That holds while the initramfs carries every system program. Once it is cut
 * down to a rescue set, the subtraction needs another source for "came with
 * the OS" (a manifest of the system's programs), or it will offer to remove
 * the desktop.
 *
 * Each entry's identity comes from the binary's own .sxmeta, as in the launcher
 * (docs/userland/SXE_FORMAT.md). A binary without resources is not an error: it
 * is shown by its basename and uninstalls all the same.
 *
 * No malloc: fixed capacities and truncation, as in progman_registry.
 */

#define APPWIZ_MAX_ENTRIES 24
#define APPWIZ_NAME_CAPACITY 32
#define APPWIZ_DESC_CAPACITY 64
#define APPWIZ_PATH_CAPACITY 192

#define APPWIZ_INSTALL_DIR "/bin"
#define APPWIZ_SYSTEM_DIR "/rescue/bin"

/* Los iconos se dibujan en la lista, que es de 16. */
#define APPWIZ_ICON_SIZE 16
#define APPWIZ_ICON_SLOT_NONE (-1)

struct appwiz_entry
{
    char name[APPWIZ_NAME_CAPACITY];
    char path[APPWIZ_PATH_CAPACITY];
    char description[APPWIZ_DESC_CAPACITY];
    /* Vacio si el binario no declaro SXE_TAG_DATA_DIR. */
    char data_dir[APPWIZ_PATH_CAPACITY];
    uint32_t size_bytes;
    /* 1 solo si data_dir esta declarado Y paso appwiz_data_dir_is_removable().
     * Un manifiesto que declara una barbaridad se muestra igual, con la casilla
     * de datos apagada: la entrada sigue siendo desinstalable. */
    int data_removable;
    int icon_slot;
};

/* Predicado de existencia inyectable, igual que en progman_registry: la app
 * pasa el que abre el path y el selftest puede pasar uno falso. */
typedef int (*appwiz_path_exists_fn)(const char *path);

/*
 * Recorre APPWIZ_INSTALL_DIR y arma el catalogo, salteando todo lo que tambien
 * exista en APPWIZ_SYSTEM_DIR. Devuelve cuantas entradas quedaron.
 *
 * Las entradas quedan ordenadas alfabeticamente por nombre visible: el orden
 * de readdir es el de la imagen, y esta es una lista para buscar algo en ella.
 */
int appwiz_catalog_scan(appwiz_path_exists_fn exists);

int appwiz_entry_count(void);
const struct appwiz_entry *appwiz_entry_at(int index);
/* Icono propio del binario, o 0 si no trajo uno que entre en el slot. */
const struct sx_bitmap *appwiz_entry_icon(const struct appwiz_entry *entry);

/*
 * Whether a directory declared with data_dir= may be deleted. THIS IS THE CHECK
 * THAT MAKES SXE_TAG_DATA_DIR A DECLARATION AND NOT A PERMISSION: whoever built
 * the program wrote that path, and without this filter a manifest saying
 * "data_dir=/" would turn a click on Remove into deleting the whole disk.
 *
 * It requires, and the selftest checks each case:
 *   - absolute, with at least one segment after the root. The legacy /disk
 *     prefix is accepted and dropped first, because binaries built before the
 *     disk became the root (an installed Doom) declare data_dir=/disk/...;
 *   - no "." or ".." segment and no empty one, so it cannot climb out;
 *   - not one of the system's directories, nor anything below one: /bin, /lib,
 *     /icons, /tmp, and the in-memory /dev and /rescue.
 *
 * Returns 1 if it may be deleted, 0 if not. A pure function of the text.
 */
int appwiz_data_dir_is_removable(const char *path);

/*
 * Borra un arbol completo. Devuelve 0 si quedo vacio y borrado, -1 si algo
 * fallo (y ahi puede haber borrado una parte: no hay transacciones en SxFS).
 *
 * NO valida el path: eso es appwiz_data_dir_is_removable, y quien llama tiene
 * que haberla consultado antes. Estan separadas porque la validacion es pura y
 * testeable sin disco, y esto es la parte que toca el disco.
 */
int appwiz_remove_tree(const char *path);

enum appwiz_result
{
    APPWIZ_OK = 0,
    /* No se pudo borrar el ejecutable: no se toco nada mas. */
    APPWIZ_ERR_BINARY = 1,
    /* El ejecutable se fue, los datos no. La entrada ya no existe, asi que se
     * informa distinto: el programa esta desinstalado y quedo basura. */
    APPWIZ_ERR_DATA = 2,
};

/*
 * Desinstala una entrada: borra el ejecutable y, si remove_data y la entrada
 * lo permite, tambien su directorio de datos.
 *
 * El binario va PRIMERO. Si se fuera al reves y fallara el borrado del
 * ejecutable, el programa quedaria instalado y sin sus datos, que es el peor
 * de los desenlaces posibles.
 */
int appwiz_uninstall(const struct appwiz_entry *entry, int remove_data);

/* Valida la validacion de data_dir, el orden del catalogo, el descarte de los
 * programas del sistema y una instalacion/desinstalacion real contra el disco.
 * Devuelve la cantidad de checks fallados; 0 si todo pasa. */
int appwiz_selftest(void);
