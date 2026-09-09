#pragma once

#include "libc.h"

#include "savanxp/gfx2d.h"

/*
 * Catalogo de programas desinstalables (Agregar o quitar programas).
 *
 * QUE ES DESINSTALABLE, que es la unica decision de fondo de este modulo:
 *
 *     esta en /disk/bin  Y  NO esta en /bin
 *
 * No es una lista negra ni una heuristica sobre el nombre: es exactamente el
 * conjunto de lo que se instalo aparte. El motivo es del sistema de archivos y
 * no del gusto de nadie -- /bin es el ramdisk del initramfs (Backend::memory en
 * kernel/vfs.cpp), asi que borrar ahi solo invalida el vnode en RAM y el
 * archivo vuelve al reiniciar. La unica desinstalacion que persiste es sobre
 * /disk, que es SxFS. Y como el build copia /bin entero a /disk/bin
 * (build.ps1), un programa del sistema aparece en los dos lados: la resta es
 * la que separa "vino con el SO" de "lo instalo alguien".
 *
 * De ahi que un programa del sistema no se pueda desinstalar desde aca. No es
 * una politica de permisos, es que no se puede: el proximo build lo repone.
 *
 * La identidad de cada entrada sale del .sxmeta del propio binario, igual que
 * en el launcher (docs/SXE_FORMAT.md). Un binario sin recursos no es un error:
 * se muestra con su basename y se desinstala igual.
 *
 * Sin malloc: capacidades fijas y truncado, igual que progman_registry.
 */

#define APPWIZ_MAX_ENTRIES 24
#define APPWIZ_NAME_CAPACITY 32
#define APPWIZ_DESC_CAPACITY 64
#define APPWIZ_PATH_CAPACITY 192

#define APPWIZ_INSTALL_DIR "/disk/bin"
#define APPWIZ_SYSTEM_DIR "/bin"

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
 * Si un directorio declarado con data_dir= se puede borrar. ES LA VALIDACION
 * QUE HACE QUE SXE_TAG_DATA_DIR SEA UNA DECLARACION Y NO UN PERMISO: el path
 * lo escribio quien compilo el programa, y sin este filtro un manifiesto con
 * "data_dir=/disk" convertiria un clic en Remove en un borrado del disco
 * entero.
 *
 * Exige, y el selftest lo verifica caso por caso:
 *   - absoluto y bajo /disk/, con al menos un segmento propio despues;
 *   - ningun segmento "." ni "..", asi no se puede salir por arriba;
 *   - no ser uno de los directorios del sistema (/disk/bin, /disk/icons).
 *
 * Devuelve 1 si se puede borrar, 0 si no. Es funcion pura del texto.
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
