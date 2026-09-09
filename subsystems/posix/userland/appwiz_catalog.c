#include "appwiz_catalog.h"

#include "savanxp/sxe.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static struct appwiz_entry g_entries[APPWIZ_MAX_ENTRIES];
static int g_entry_count = 0;

/*
 * Pool de iconos. APPWIZ_MAX_ENTRIES * 16 * 16 * 4 = 24 KiB de BSS, contra los
 * 192 KiB que el launcher paga por los suyos a 32: aca la lista dibuja a 16 y
 * no hay motivo para guardar el tamano grande.
 */
static uint32_t g_icon_pixels[APPWIZ_MAX_ENTRIES][APPWIZ_ICON_SIZE * APPWIZ_ICON_SIZE];
static struct sx_bitmap g_icon_bitmaps[APPWIZ_MAX_ENTRIES];

/* --- helpers -------------------------------------------------------------- */

static void copy_field(char *destination, size_t capacity, const char *source)
{
    size_t index = 0;

    if (destination == 0 || capacity == 0)
    {
        return;
    }
    if (source != 0)
    {
        while (source[index] != '\0' && index + 1u < capacity)
        {
            destination[index] = source[index];
            index += 1u;
        }
    }
    destination[index] = '\0';
}

static const char *path_basename(const char *path)
{
    const char *base = path;

    if (path == 0)
    {
        return "";
    }
    while (*path != '\0')
    {
        if (*path == '/')
        {
            base = path + 1;
        }
        path += 1;
    }
    return base;
}

static int default_path_exists(const char *path)
{
    long fd;

    if (path == 0 || path[0] == '\0')
    {
        return 0;
    }
    fd = savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0)
    {
        return 0;
    }
    (void)savanxp_close((int)fd);
    return 1;
}

/* --- validacion de data_dir ----------------------------------------------- */

/* Directorios de /disk que son del sistema y no de ningun programa. Que la
 * lista sea corta es a proposito: el filtro de verdad es el prefijo /disk/ mas
 * la prohibicion de "..", y esto solo tapa los tres nombres que el build
 * escribe en cada imagen. */
static const char *const k_reserved_data_dirs[] = {
    "/disk/bin",
    "/disk/icons",
    "/disk/tmp",
};

int appwiz_data_dir_is_removable(const char *path)
{
    static const char prefix[] = "/disk/";
    size_t index;
    size_t length;
    size_t segment_start;

    if (path == 0)
    {
        return 0;
    }
    length = strlen(path);
    /* Una barra al final es cosmetica; el resto de la validacion trabaja sobre
     * el path sin ella para no ver un ultimo segmento vacio. */
    while (length > 0u && path[length - 1u] == '/')
    {
        length -= 1u;
    }
    if (length < sizeof(prefix)) /* "/disk/" + al menos un caracter */
    {
        return 0;
    }
    if (strncmp(path, prefix, sizeof(prefix) - 1u) != 0)
    {
        return 0;
    }

    /* Ningun segmento "." ni ".." -- con uno solo se sale de /disk y toda la
     * garantia del prefijo deja de valer. */
    segment_start = sizeof(prefix) - 1u;
    for (index = segment_start; index <= length; ++index)
    {
        if (index == length || path[index] == '/')
        {
            size_t segment_length = index - segment_start;

            if (segment_length == 0u)
            {
                return 0; /* "//" */
            }
            if (segment_length == 1u && path[segment_start] == '.')
            {
                return 0;
            }
            if (segment_length == 2u && path[segment_start] == '.' && path[segment_start + 1u] == '.')
            {
                return 0;
            }
            segment_start = index + 1u;
        }
    }

    for (index = 0; index < sizeof(k_reserved_data_dirs) / sizeof(k_reserved_data_dirs[0]); ++index)
    {
        const char *reserved = k_reserved_data_dirs[index];
        size_t reserved_length = strlen(reserved);

        if (length == reserved_length && strncmp(path, reserved, reserved_length) == 0)
        {
            return 0;
        }
    }
    return 1;
}

/* --- borrado de arboles ---------------------------------------------------- */

/*
 * Tope de anidamiento. No es una restriccion del formato: es que este borrado
 * es recursivo y cada nivel abre un DIR, asi que un arbol patologico no puede
 * llevarse el stack por delante.
 */
#define APPWIZ_TREE_MAX_DEPTH 8

static int remove_directory_contents(char *path, size_t capacity, int depth)
{
    /*
     * Una entrada por vuelta, reabriendo el directorio cada vez, en vez de un
     * solo readdir borrando a medida que avanza. Borrar mientras se itera deja
     * el offset del directorio apuntando a otro lado y se saltean entradas en
     * silencio; con n del orden de los archivos de un juego, reabrir es gratis
     * al lado de tener que confiar en eso.
     */
    for (;;)
    {
        DIR *handle = opendir(path);
        struct dirent *entry = 0;
        char name[256];
        size_t path_length = strlen(path);
        int is_directory = 0;
        int found = 0;

        if (handle == 0)
        {
            return -1;
        }
        while ((entry = readdir(handle)) != 0)
        {
            if (entry->d_name[0] == '\0')
            {
                continue;
            }
            if (entry->d_name[0] == '.' &&
                (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            {
                continue;
            }
            copy_field(name, sizeof(name), entry->d_name);
            is_directory = entry->d_type == DT_DIR;
            found = 1;
            break;
        }
        closedir(handle);
        if (!found)
        {
            return 0;
        }

        {
            int written = snprintf(path + path_length, capacity - path_length, "/%s", name);
            if (written < 0 || (size_t)written >= capacity - path_length)
            {
                path[path_length] = '\0';
                return -1;
            }
        }

        if (is_directory)
        {
            if (depth + 1 >= APPWIZ_TREE_MAX_DEPTH ||
                remove_directory_contents(path, capacity, depth + 1) != 0 ||
                rmdir(path) != 0)
            {
                path[path_length] = '\0';
                return -1;
            }
        }
        else if (unlink(path) != 0)
        {
            path[path_length] = '\0';
            return -1;
        }
        path[path_length] = '\0';
    }
}

int appwiz_remove_tree(const char *path)
{
    /* Estatico y no en el stack: es el buffer que la recursion va extendiendo
     * y truncando, uno solo para todo el arbol. */
    static char buffer[APPWIZ_PATH_CAPACITY + 256];

    if (path == 0 || path[0] != '/')
    {
        return -1;
    }
    copy_field(buffer, sizeof(buffer), path);
    if (remove_directory_contents(buffer, sizeof(buffer), 0) != 0)
    {
        return -1;
    }
    return rmdir(buffer) == 0 ? 0 : -1;
}

/* --- catalogo -------------------------------------------------------------- */

static int adopt_icon(struct appwiz_entry *entry, int slot, const struct sxe_icons *icons)
{
    const struct sxe_icon_entry *image = sxe_icons_best(icons, APPWIZ_ICON_SIZE);
    const uint32_t *pixels = 0;

    if (image == 0 || image->width != APPWIZ_ICON_SIZE || image->height != APPWIZ_ICON_SIZE)
    {
        return 0;
    }
    pixels = sxe_icons_pixels(icons, image);
    if (pixels == 0)
    {
        return 0;
    }

    memcpy(g_icon_pixels[slot], pixels, (size_t)APPWIZ_ICON_SIZE * APPWIZ_ICON_SIZE * sizeof(uint32_t));
    g_icon_bitmaps[slot].pixels = g_icon_pixels[slot];
    g_icon_bitmaps[slot].info.width = APPWIZ_ICON_SIZE;
    g_icon_bitmaps[slot].info.height = APPWIZ_ICON_SIZE;
    g_icon_bitmaps[slot].info.pitch = APPWIZ_ICON_SIZE * (uint32_t)sizeof(uint32_t);
    g_icon_bitmaps[slot].info.bpp = 32u;
    g_icon_bitmaps[slot].format = SX_PIXEL_FORMAT_BGRA8888;
    entry->icon_slot = slot;
    return 1;
}

/* Completa una entrada con lo que declare el binario. Que no declare nada no
 * es un error: queda el basename y se desinstala igual. */
static void describe_entry(struct appwiz_entry *entry, int slot, void *meta_buffer, size_t meta_capacity,
                           void *icon_buffer, size_t icon_capacity)
{
    struct sxe_meta meta;
    struct sxe_icons icons;
    char text[APPWIZ_PATH_CAPACITY];
    struct stat info;

    if (stat(entry->path, &info) == 0)
    {
        entry->size_bytes = (uint32_t)info.st_size;
    }

    if (sxe_load_meta(entry->path, meta_buffer, meta_capacity, &meta) == SXE_OK)
    {
        if (sxe_meta_string(&meta, SXE_TAG_NAME, text, sizeof(text)) > 0u)
        {
            copy_field(entry->name, sizeof(entry->name), text);
        }
        if (sxe_meta_string(&meta, SXE_TAG_DESCRIPTION, text, sizeof(text)) > 0u)
        {
            copy_field(entry->description, sizeof(entry->description), text);
        }
        if (sxe_meta_string(&meta, SXE_TAG_DATA_DIR, text, sizeof(text)) > 0u)
        {
            copy_field(entry->data_dir, sizeof(entry->data_dir), text);
            entry->data_removable = appwiz_data_dir_is_removable(entry->data_dir);
        }
    }

    if (sxe_load_icons(entry->path, icon_buffer, icon_capacity, &icons) == SXE_OK)
    {
        (void)adopt_icon(entry, slot, &icons);
    }
}

static void sort_entries(void)
{
    static struct appwiz_entry pivot;
    int index;

    for (index = 1; index < g_entry_count; ++index)
    {
        int hole = index;

        pivot = g_entries[index];
        while (hole > 0 && strcmp(g_entries[hole - 1].name, pivot.name) > 0)
        {
            g_entries[hole] = g_entries[hole - 1];
            hole -= 1;
        }
        g_entries[hole] = pivot;
    }
}

int appwiz_catalog_scan(appwiz_path_exists_fn exists)
{
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    _Alignas(4) static uint8_t icon_buffer[SXE_ICON_MAX_BYTES];
    DIR *handle = 0;
    struct dirent *entry = 0;
    int index;

    g_entry_count = 0;
    for (index = 0; index < APPWIZ_MAX_ENTRIES; ++index)
    {
        g_icon_bitmaps[index].pixels = 0;
    }
    if (exists == 0)
    {
        exists = default_path_exists;
    }

    handle = opendir(APPWIZ_INSTALL_DIR);
    if (handle == 0)
    {
        return 0;
    }
    while ((entry = readdir(handle)) != 0 && g_entry_count < APPWIZ_MAX_ENTRIES)
    {
        struct appwiz_entry *slot = &g_entries[g_entry_count];
        char system_path[APPWIZ_PATH_CAPACITY];

        if (entry->d_name[0] == '.' || entry->d_type == DT_DIR)
        {
            continue;
        }

        memset(slot, 0, sizeof(*slot));
        slot->icon_slot = APPWIZ_ICON_SLOT_NONE;
        {
            int written = snprintf(slot->path, sizeof(slot->path), "%s/%s", APPWIZ_INSTALL_DIR, entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(slot->path))
            {
                continue;
            }
            written = snprintf(system_path, sizeof(system_path), "%s/%s", APPWIZ_SYSTEM_DIR, entry->d_name);
            if (written < 0 || (size_t)written >= sizeof(system_path))
            {
                continue;
            }
        }
        /* La resta con /bin: si tambien esta ahi, vino con el SO. */
        if (exists(system_path))
        {
            continue;
        }
        if (!exists(slot->path))
        {
            continue;
        }

        copy_field(slot->name, sizeof(slot->name), entry->d_name);
        g_entry_count += 1;
    }
    closedir(handle);

    /* Los recursos se leen DESPUES de cerrar el directorio y en una pasada
     * aparte: describe_entry abre cada binario, y hacerlo con el readdir en
     * curso mezclaria el recorrido con I/O sobre otros archivos. */
    for (index = 0; index < g_entry_count; ++index)
    {
        describe_entry(&g_entries[index], index, meta_buffer, sizeof(meta_buffer), icon_buffer, sizeof(icon_buffer));
    }
    /* Ordenar despues de asignar los slots no los invalida: icon_slot es un
     * campo de la entrada y viaja con ella. El pool no se reordena porque no
     * hace falta -- nadie lo recorre por indice. */
    sort_entries();
    return g_entry_count;
}

int appwiz_entry_count(void)
{
    return g_entry_count;
}

const struct appwiz_entry *appwiz_entry_at(int index)
{
    if (index < 0 || index >= g_entry_count)
    {
        return 0;
    }
    return &g_entries[index];
}

const struct sx_bitmap *appwiz_entry_icon(const struct appwiz_entry *entry)
{
    if (entry == 0 || entry->icon_slot < 0 || entry->icon_slot >= APPWIZ_MAX_ENTRIES)
    {
        return 0;
    }
    if (g_icon_bitmaps[entry->icon_slot].pixels == 0)
    {
        return 0;
    }
    return &g_icon_bitmaps[entry->icon_slot];
}

int appwiz_uninstall(const struct appwiz_entry *entry, int remove_data)
{
    if (entry == 0 || entry->path[0] != '/')
    {
        return APPWIZ_ERR_BINARY;
    }
    if (unlink(entry->path) != 0)
    {
        return APPWIZ_ERR_BINARY;
    }
    if (remove_data && entry->data_removable && entry->data_dir[0] != '\0')
    {
        /* Se revalida aca aunque el catalogo ya lo haya hecho: entre el
         * escaneo y el clic pasa tiempo, y esta es la llamada que borra. */
        if (!appwiz_data_dir_is_removable(entry->data_dir) || appwiz_remove_tree(entry->data_dir) != 0)
        {
            return APPWIZ_ERR_DATA;
        }
    }
    return APPWIZ_OK;
}

/* --- selftest -------------------------------------------------------------- */

static int g_selftest_failures = 0;

static void expect(int condition, const char *label)
{
    if (!condition)
    {
        printf("APPWIZ SMOKE FAIL %s\n", label);
        g_selftest_failures += 1;
    }
}

/*
 * La fixture vive en /disk/tmp y no en /disk/bin ni en el data_dir de nadie: el
 * selftest tiene que ejercitar un borrado REAL, y hacerlo sobre datos de un
 * programa instalado significaria que correr un smoke te desinstala algo.
 */
#define APPWIZ_FIXTURE_TREE "/disk/tmp/appwiz-selftest"
#define APPWIZ_FIXTURE_APP APPWIZ_INSTALL_DIR "/appwiz-selftest-app"

static int write_file(const char *path, const char *contents)
{
    long fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    long written;

    if (fd < 0)
    {
        return -1;
    }
    written = savanxp_write((int)fd, contents, strlen(contents));
    (void)savanxp_close((int)fd);
    return written == (long)strlen(contents) ? 0 : -1;
}

static int catalog_has_basename(const char *basename)
{
    int index;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(path_basename(g_entries[index].path), basename) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void selftest_data_dir(void)
{
    /* Lo que un manifiesto mal escrito -- o malicioso -- no puede lograr. */
    static const char *const rejected[] = {
        "",
        "/",
        "/disk",
        "/disk/",
        "disk/games/doom",  /* relativo */
        "/bin",
        "/bin/notepad",
        "/games/doom",      /* fuera de /disk */
        "/disk/../bin",
        "/disk/games/../../bin",
        "/disk/./games",
        "/disk//games",
        "/disk/bin",
        "/disk/icons",
        "/disk/tmp",
    };
    static const char *const accepted[] = {
        "/disk/games/doom",
        "/disk/games/doom/",
        "/disk/myapp",
        "/disk/a/b/c/d",
        "/disk/binaries", /* prefijo de /disk/bin, pero no es /disk/bin */
    };
    size_t index;

    expect(appwiz_data_dir_is_removable(0) == 0, "data_dir: NULL");
    for (index = 0; index < sizeof(rejected) / sizeof(rejected[0]); ++index)
    {
        if (appwiz_data_dir_is_removable(rejected[index]))
        {
            printf("APPWIZ SMOKE FAIL data_dir acepto '%s'\n", rejected[index]);
            g_selftest_failures += 1;
        }
    }
    for (index = 0; index < sizeof(accepted) / sizeof(accepted[0]); ++index)
    {
        if (!appwiz_data_dir_is_removable(accepted[index]))
        {
            printf("APPWIZ SMOKE FAIL data_dir rechazo '%s'\n", accepted[index]);
            g_selftest_failures += 1;
        }
    }
}

static void selftest_remove_tree(void)
{
    int ok = 1;

    ok = ok && savanxp_mkdir(APPWIZ_FIXTURE_TREE) >= 0;
    ok = ok && write_file(APPWIZ_FIXTURE_TREE "/data.bin", "payload") == 0;
    ok = ok && savanxp_mkdir(APPWIZ_FIXTURE_TREE "/sub") >= 0;
    ok = ok && write_file(APPWIZ_FIXTURE_TREE "/sub/deep.bin", "deeper") == 0;
    expect(ok, "remove_tree: no se pudo armar la fixture");
    if (!ok)
    {
        return;
    }

    expect(default_path_exists(APPWIZ_FIXTURE_TREE "/sub/deep.bin"), "remove_tree: la fixture existe antes");
    expect(appwiz_remove_tree(APPWIZ_FIXTURE_TREE) == 0, "remove_tree: borrado del arbol");
    /* Los dos niveles: un borrado que se lleva el archivo de arriba y deja el
     * subdirectorio es exactamente el bug que la recursion existe para no
     * tener. */
    expect(!default_path_exists(APPWIZ_FIXTURE_TREE "/data.bin"), "remove_tree: no queda el archivo de arriba");
    expect(!default_path_exists(APPWIZ_FIXTURE_TREE "/sub/deep.bin"), "remove_tree: no queda el archivo anidado");
    expect(opendir(APPWIZ_FIXTURE_TREE) == 0, "remove_tree: no queda el directorio raiz");
}

static void selftest_catalog(void)
{
    const struct appwiz_entry *entry = 0;
    int index;

    (void)appwiz_catalog_scan(0);
    printf("APPWIZ SMOKE catalog entries=%d\n", appwiz_entry_count());
    for (index = 0; index < appwiz_entry_count(); ++index)
    {
        entry = appwiz_entry_at(index);
        if (entry != 0)
        {
            printf("APPWIZ SMOKE entry %s -> %s bytes=%u datos=%s\n",
                entry->name,
                entry->path,
                (unsigned)entry->size_bytes,
                entry->data_dir[0] != '\0' ? entry->data_dir : "(ninguno)");
        }
    }

    /*
     * La resta con /bin, que es la unica decision de fondo del modulo. Estos
     * tres estan en /disk/bin en TODA imagen -- el build copia /bin entero --,
     * asi que si alguno aparece, la resta no se esta haciendo y el
     * desinstalador estaria ofreciendo borrar el sistema operativo.
     */
    expect(!catalog_has_basename("notepad"), "catalogo: notepad es del sistema");
    expect(!catalog_has_basename("shellapp"), "catalogo: shellapp es del sistema");
    expect(!catalog_has_basename("busybox"), "catalogo: busybox es del sistema");

    /* Ida y vuelta real contra el disco: instalar algo que NO esta en /bin,
     * verlo aparecer, desinstalarlo y verlo desaparecer. Es el ciclo entero
     * que la app expone con un boton. */
    if (write_file(APPWIZ_FIXTURE_APP, "no soy un ELF, y aun asi se desinstala") != 0)
    {
        expect(0, "catalogo: no se pudo instalar la fixture");
        return;
    }
    (void)appwiz_catalog_scan(0);
    expect(catalog_has_basename("appwiz-selftest-app"), "catalogo: la fixture instalada aparece");

    entry = 0;
    for (index = 0; index < appwiz_entry_count(); ++index)
    {
        const struct appwiz_entry *candidate = appwiz_entry_at(index);
        if (candidate != 0 && strcmp(path_basename(candidate->path), "appwiz-selftest-app") == 0)
        {
            entry = candidate;
            break;
        }
    }
    if (entry == 0)
    {
        (void)unlink(APPWIZ_FIXTURE_APP);
        return;
    }
    /* Sin recursos: el nombre cae al basename y se desinstala igual, que es el
     * contrato de "un binario sin .sxmeta sigue siendo de primera clase". */
    expect(strcmp(entry->name, "appwiz-selftest-app") == 0, "catalogo: sin recursos queda el basename");
    expect(entry->data_removable == 0, "catalogo: sin data_dir declarado no hay datos que borrar");

    expect(appwiz_uninstall(entry, 0) == APPWIZ_OK, "desinstalar: la fixture se va");
    expect(!default_path_exists(APPWIZ_FIXTURE_APP), "desinstalar: el ejecutable ya no esta");
    (void)appwiz_catalog_scan(0);
    expect(!catalog_has_basename("appwiz-selftest-app"), "desinstalar: sale del catalogo");
}

int appwiz_selftest(void)
{
    g_selftest_failures = 0;
    selftest_data_dir();
    selftest_remove_tree();
    selftest_catalog();
    return g_selftest_failures;
}
