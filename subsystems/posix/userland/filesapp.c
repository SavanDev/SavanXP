#include "libc.h"
#include "savanxp/sxgui.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "shared/version.h"

#include "file_assoc.h"
#include "mime_icon.h"

/*
 * Explorador de archivos, con la forma del explorador de la era Win95: barra de
 * menu, barra de direccion con "Up One Level", lista de detalles (Name / Size /
 * Type) con cabecera de columnas, y barra de estado de dos paneles.
 *
 * NO muestra el contenido de los archivos: eso es trabajo de un editor. Abrir
 * un archivo que no es un programa lo manda al programa ASOCIADO a su
 * extension (file_assoc, docs/SXE_FORMAT.md fase 5); sin asociacion cae a
 * /bin/notepad, que es donde vive ahora el preview que esta app tenia adentro.
 */

#define FILESAPP_PATH_CAPACITY 256
#define FILESAPP_MAX_ENTRIES 128
#define FILESAPP_LABEL_LENGTH 96

#define FILESAPP_MENU_OPEN 1
#define FILESAPP_MENU_UP 2
#define FILESAPP_MENU_REFRESH 3
#define FILESAPP_MENU_ABOUT 8
#define FILESAPP_MENU_CLOSE 9

struct filesapp_entry
{
    char name[256];
    unsigned int size;
    int is_dir;
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[5];

static struct filesapp_entry g_entries[FILESAPP_MAX_ENTRIES];
static char g_item_labels[FILESAPP_MAX_ENTRIES][FILESAPP_LABEL_LENGTH];
static const char *g_item_ptrs[FILESAPP_MAX_ENTRIES];
/* Paralelo a g_item_ptrs: el icono que le toca a cada fila, o 0. */
static const struct sx_bitmap *g_item_icons[FILESAPP_MAX_ENTRIES];
static char g_current_path[FILESAPP_PATH_CAPACITY] = "/";
static char g_count_pane[64] = "0 object(s)";
static char g_size_pane[64] = "";
static int g_entry_count = 0;
/* Las asociaciones se cargan a demanda; ver filesapp_activate_selected. */
static int g_assoc_loaded = 0;
static int g_mime_icons_loaded = 0;

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[4];

#define FILESAPP_ADDRESS (&g_widgets[0])
#define FILESAPP_UP_BUTTON (&g_widgets[1])
#define FILESAPP_LIST_INDEX 2
#define FILESAPP_LIST (&g_widgets[FILESAPP_LIST_INDEX])

/* Anchos fijos; la columna Name se lleva el resto y se recalcula en el layout,
 * que es lo que hace que la lista se estire con la ventana. */
static struct sxgui_column g_columns[] = {
    {"Name", 240, 0},
    {"Size", 80, SXGUI_COLUMN_RIGHT},
    {"Type", 130, 0},
};

#define FILESAPP_COLUMN_COUNT ((int)(sizeof(g_columns) / sizeof(g_columns[0])))
#define FILESAPP_EDITOR_PATH "/bin/notepad"

#define FILESAPP_COLUMN_SIZE_WIDTH 80
#define FILESAPP_COLUMN_TYPE_WIDTH 130

/* Grilla del Acerca de: la misma que usan los dialogos de notepad. */
#define FILESAPP_ABOUT_WIDTH 320
#define FILESAPP_ABOUT_HEIGHT 128
#define FILESAPP_DLG_ROW (18 + 4)

/* ---- helpers de path ------------------------------------------------------ */

static int filesapp_is_root_path(const char *path)
{
    return path != 0 && path[0] == '/' && path[1] == '\0';
}

static int filesapp_path_is_launchable(const char *path)
{
    return path != 0 &&
        (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/disk/bin/", 10) == 0);
}

static int filesapp_join_path(const char *base, const char *name, char *buffer, size_t capacity)
{
    if (buffer == 0 || capacity == 0 || name == 0 || name[0] == '\0')
    {
        return 0;
    }
    if (base == 0 || base[0] == '\0' || strcmp(base, "/") == 0)
    {
        return snprintf(buffer, capacity, "/%s", name) < (int)capacity;
    }
    return snprintf(buffer, capacity, "%s/%s", base, name) < (int)capacity;
}

static void filesapp_parent_path(const char *path, char *buffer, size_t capacity)
{
    size_t length;

    if (buffer == 0 || capacity == 0)
    {
        return;
    }
    if (path == 0 || filesapp_is_root_path(path))
    {
        snprintf(buffer, capacity, "/");
        return;
    }

    snprintf(buffer, capacity, "%s", path);
    length = strlen(buffer);
    while (length > 1 && buffer[length - 1] == '/')
    {
        buffer[length - 1] = '\0';
        length -= 1;
    }
    while (length > 1 && buffer[length - 1] != '/')
    {
        buffer[length - 1] = '\0';
        length -= 1;
    }
    if (length > 1)
    {
        buffer[length - 1] = '\0';
    }
    if (buffer[0] == '\0')
    {
        snprintf(buffer, capacity, "/");
    }
}

/* ---- formato de las celdas ------------------------------------------------ */

static void filesapp_set_status(const char *text)
{
    snprintf(g_count_pane, sizeof(g_count_pane), "%s", text != 0 ? text : "");
}

/* Como el explorador de la epoca: el tamano se muestra en KB redondeados para
 * arriba, y los directorios no muestran tamano. */
static void filesapp_format_size(unsigned int bytes, char *buffer, size_t capacity)
{
    snprintf(buffer, capacity, "%u KB", (bytes + 1023u) / 1024u);
}

/* "File Folder" / "Application" / "TXT File" / "File", el vocabulario del
 * explorador clasico para la columna Type. */
static void filesapp_format_type(
    const char *name,
    int is_dir,
    int launchable,
    char *buffer,
    size_t capacity)
{
    const char *dot = 0;
    size_t index;

    if (is_dir)
    {
        snprintf(buffer, capacity, "File Folder");
        return;
    }
    if (launchable)
    {
        snprintf(buffer, capacity, "Application");
        return;
    }

    for (index = 0; name != 0 && name[index] != '\0'; ++index)
    {
        if (name[index] == '.' && index > 0 && name[index + 1] != '\0')
        {
            dot = &name[index + 1];
        }
    }
    if (dot == 0)
    {
        snprintf(buffer, capacity, "File");
        return;
    }

    /* La extension va en mayusculas, como la mostraba el explorador. */
    {
        char extension[16];
        size_t length = 0;

        while (dot[length] != '\0' && length + 1 < sizeof(extension))
        {
            char value = dot[length];
            if (value >= 'a' && value <= 'z')
            {
                value = (char)(value - 'a' + 'A');
            }
            extension[length] = value;
            length += 1;
        }
        extension[length] = '\0';
        snprintf(buffer, capacity, "%s File", extension);
    }
}

static void filesapp_format_total(unsigned int bytes, char *buffer, size_t capacity)
{
    if (bytes < 1024u)
    {
        snprintf(buffer, capacity, "%u bytes", bytes);
        return;
    }
    if (bytes < 1024u * 1024u)
    {
        snprintf(buffer, capacity, "%u KB", bytes / 1024u);
        return;
    }
    /* Un decimal por aritmetica entera: el printf de userland no lleva float. */
    snprintf(
        buffer,
        capacity,
        "%u.%u MB",
        bytes / (1024u * 1024u),
        (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u));
}

/* ---- listado -------------------------------------------------------------- */

static void filesapp_sort_entries(int start_index)
{
    int left;

    for (left = start_index; left < g_entry_count; ++left)
    {
        int right;
        for (right = left + 1; right < g_entry_count; ++right)
        {
            int swap = 0;
            if (g_entries[left].is_dir != g_entries[right].is_dir)
            {
                swap = g_entries[right].is_dir > g_entries[left].is_dir;
            }
            else if (strcmp(g_entries[right].name, g_entries[left].name) < 0)
            {
                swap = 1;
            }

            if (swap)
            {
                struct filesapp_entry temp = g_entries[left];
                g_entries[left] = g_entries[right];
                g_entries[right] = temp;
            }
        }
    }
}

/* Arma las filas "nombre TAB tamano TAB tipo" que parte el listbox. */
static void filesapp_rebuild_labels(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    char size_cell[32];
    char type_cell[32];
    int index;

    /* Una sola lectura de /disk/mimeicon.ini por proceso. Es barata --un
     * ini chico, sin escanear binarios como file_assoc--, pero repetirla en
     * cada cambio de directorio no aporta nada. */
    if (!g_mime_icons_loaded)
    {
        (void)mime_icon_load();
        g_mime_icons_loaded = 1;
    }

    for (index = 0; index < g_entry_count; ++index)
    {
        const struct filesapp_entry *entry = &g_entries[index];
        int launchable = 0;

        if (!entry->is_dir &&
            filesapp_join_path(g_current_path, entry->name, full_path, sizeof(full_path)))
        {
            launchable = filesapp_path_is_launchable(full_path);
        }

        size_cell[0] = '\0';
        if (!entry->is_dir)
        {
            filesapp_format_size(entry->size, size_cell, sizeof(size_cell));
        }
        if (strcmp(entry->name, "..") == 0)
        {
            snprintf(type_cell, sizeof(type_cell), "File Folder");
        }
        else
        {
            filesapp_format_type(entry->name, entry->is_dir, launchable, type_cell, sizeof(type_cell));
        }

        snprintf(
            g_item_labels[index],
            sizeof(g_item_labels[index]),
            "%s%c%s%c%s",
            entry->name,
            SXGUI_COLUMN_SEPARATOR,
            size_cell,
            SXGUI_COLUMN_SEPARATOR,
            type_cell);
        g_item_ptrs[index] = g_item_labels[index];
        g_item_icons[index] = mime_icon_for_file(entry->name, entry->is_dir, launchable);
    }
    FILESAPP_LIST->items = g_item_ptrs;
    FILESAPP_LIST->item_icons = g_item_icons;
    FILESAPP_LIST->item_count = g_entry_count;
    FILESAPP_LIST->value = 0;
    FILESAPP_LIST->scroll = 0;
}

static void filesapp_update_status(void)
{
    unsigned int total = 0;
    int index;
    int objects = 0;

    for (index = 0; index < g_entry_count; ++index)
    {
        if (strcmp(g_entries[index].name, "..") == 0)
        {
            continue;
        }
        objects += 1;
        if (!g_entries[index].is_dir)
        {
            total += g_entries[index].size;
        }
    }

    snprintf(g_count_pane, sizeof(g_count_pane), "%d object(s)", objects);
    filesapp_format_total(total, g_size_pane, sizeof(g_size_pane));
}

static int filesapp_load_directory(const char *path)
{
    DIR *directory = 0;
    struct dirent *entry = 0;
    char full_path[FILESAPP_PATH_CAPACITY];
    int insert_parent = 0;

    directory = opendir(path);
    if (directory == 0)
    {
        filesapp_set_status("Unable to open directory.");
        return -1;
    }

    g_entry_count = 0;
    insert_parent = !filesapp_is_root_path(path);
    if (insert_parent && g_entry_count < FILESAPP_MAX_ENTRIES)
    {
        memset(&g_entries[g_entry_count], 0, sizeof(g_entries[g_entry_count]));
        strcpy(g_entries[g_entry_count].name, "..");
        g_entries[g_entry_count].is_dir = 1;
        g_entry_count += 1;
    }

    while ((entry = readdir(directory)) != 0 && g_entry_count < FILESAPP_MAX_ENTRIES)
    {
        struct stat info = {0};
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        memset(&g_entries[g_entry_count], 0, sizeof(g_entries[g_entry_count]));
        snprintf(g_entries[g_entry_count].name, sizeof(g_entries[g_entry_count].name), "%s", entry->d_name);
        if (filesapp_join_path(path, entry->d_name, full_path, sizeof(full_path)) && stat(full_path, &info) == 0)
        {
            g_entries[g_entry_count].is_dir = S_ISDIR(info.st_mode);
            g_entries[g_entry_count].size = info.st_size;
        }
        else
        {
            g_entries[g_entry_count].is_dir = entry->d_type == DT_DIR;
            g_entries[g_entry_count].size = 0;
        }
        g_entry_count += 1;
    }
    closedir(directory);

    snprintf(g_current_path, sizeof(g_current_path), "%s", path);
    filesapp_sort_entries(insert_parent ? 1 : 0);
    filesapp_rebuild_labels();
    filesapp_update_status();

    /* En la raiz no hay a donde subir: el boton se apaga en vez de no hacer
     * nada al apretarlo. */
    if (filesapp_is_root_path(g_current_path))
    {
        FILESAPP_UP_BUTTON->flags |= SXGUI_FLAG_DISABLED;
    }
    else
    {
        FILESAPP_UP_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    return 0;
}

/* ---- navegacion ----------------------------------------------------------- */

static void filesapp_go_up(void)
{
    char parent_path[FILESAPP_PATH_CAPACITY];

    if (filesapp_is_root_path(g_current_path))
    {
        return;
    }
    filesapp_parent_path(g_current_path, parent_path, sizeof(parent_path));
    (void)filesapp_load_directory(parent_path);
}

static void filesapp_activate_selected(void)
{
    char full_path[FILESAPP_PATH_CAPACITY];
    int selected = FILESAPP_LIST->value;

    if (g_entry_count <= 0 || selected < 0 || selected >= g_entry_count)
    {
        return;
    }
    if (strcmp(g_entries[selected].name, "..") == 0)
    {
        filesapp_go_up();
        return;
    }
    if (!filesapp_join_path(g_current_path, g_entries[selected].name, full_path, sizeof(full_path)))
    {
        filesapp_set_status("Path too long.");
        return;
    }
    if (g_entries[selected].is_dir)
    {
        (void)filesapp_load_directory(full_path);
        return;
    }
    if (filesapp_path_is_launchable(full_path))
    {
        if (gfx_desktop_launch(&g_app.gfx, full_path) < 0)
        {
            filesapp_set_status("Launch failed.");
            return;
        }
        filesapp_set_status("Launch requested.");
        return;
    }
    /*
     * Un archivo que no es programa lo abre quien este asociado a su extension
     * (docs/SXE_FORMAT.md, fase 5): el binario declara que puede abrirla y
     * /disk/assoc.ini decide cual gana. Sin asociacion queda el bloc de notas,
     * que es donde vive el preview que esta app tenia adentro -- no tener
     * asociacion no es un error, es el caso comun.
     */
    {
        const char *program = 0;
        int associated = 0;

        /* Perezoso: el escaneo abre el .sxmeta de cada ejecutable instalado, y
         * una sesion que solo navega directorios no tiene por que pagarlo. Se
         * hace una sola vez, en el momento en que de verdad hace falta. */
        if (!g_assoc_loaded)
        {
            (void)file_assoc_load();
            g_assoc_loaded = 1;
        }
        program = file_assoc_program_for_file(full_path);
        associated = (program != 0);

        if (program == 0)
        {
            program = FILESAPP_EDITOR_PATH;
        }
        if (gfx_desktop_launch_arg(&g_app.gfx, program, full_path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0)
        {
            filesapp_set_status(associated ? "Cannot open associated program." : "Cannot open Notepad.");
            return;
        }
        filesapp_set_status(associated ? "Opening with associated program..." : "Opening in Notepad...");
    }
}

/* ---- callbacks ------------------------------------------------------------ */

static void on_list(struct sxgui_widget *widget, void *user)
{
    (void)user;
    if (widget->action == SXGUI_ACTION_ACTIVATE)
    {
        filesapp_activate_selected();
    }
}

static void on_up_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    filesapp_go_up();
}

static void on_dialog_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case FILESAPP_MENU_OPEN:
        filesapp_activate_selected();
        break;
    case FILESAPP_MENU_UP:
        filesapp_go_up();
        break;
    case FILESAPP_MENU_REFRESH:
        (void)filesapp_load_directory(g_current_path);
        break;
    case FILESAPP_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, FILESAPP_ABOUT_WIDTH, FILESAPP_ABOUT_HEIGHT);
        break;
    case FILESAPP_MENU_CLOSE:
        sxgui_app_quit(&g_app, 0);
        break;
    default:
        break;
    }
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_BACKSPACE)
    {
        filesapp_go_up();
        return 1;
    }
    if (event->key == SAVANXP_KEY_F5)
    {
        (void)filesapp_load_directory(g_current_path);
        return 1;
    }
    return 0;
}

/* ---- layout --------------------------------------------------------------- */

/* La lista se estira con la ventana, asi que no hay bounding box del que
 * deducir el tamano: este es un explorador comodo sin comerse la pantalla. */
#define FILESAPP_CONTENT_WIDTH 560
#define FILESAPP_CONTENT_HEIGHT 400

/* La banda de la barra de direccion lleva UNA altura para los dos controles que
 * viven ahi: si el campo y el boton no miden lo mismo, quedan desalineados por
 * arriba o por abajo y no hay margen que lo tape. */
#define FILESAPP_TOOLBAR_HEIGHT SXGUI_FIELD_HEIGHT
#define FILESAPP_UP_BUTTON_WIDTH 110
/* Separacion entre los dos paneles de la barra de estado. Es menor que el GAP
 * general a proposito: son dos mitades de la misma barra, no dos controles. */
#define FILESAPP_STATUS_GAP 2

static void filesapp_layout(struct sxgui_app *app)
{
    int width = (int)app->gfx.info.width;
    int height = (int)app->gfx.info.height;
    int toolbar_y = sxgui_menubar_height() + SXGUI_MARGIN;
    int list_y = toolbar_y + FILESAPP_TOOLBAR_HEIGHT + SXGUI_GAP;
    int status_y = height - SXGUI_MARGIN - SXGUI_STATUS_HEIGHT;
    int list_height = status_y - SXGUI_GAP - list_y;
    int list_width = width - SXGUI_MARGIN * 2;
    int address_width = list_width - FILESAPP_UP_BUTTON_WIDTH - SXGUI_GAP;
    int name_width;
    int count_pane_width;

    if (list_height < 60)
    {
        list_height = 60;
    }
    if (address_width < 80)
    {
        address_width = 80;
    }
    if (list_width < 120)
    {
        list_width = 120;
    }

    FILESAPP_ADDRESS->rect = sx_rect_make(SXGUI_MARGIN, toolbar_y, address_width, FILESAPP_TOOLBAR_HEIGHT);
    FILESAPP_UP_BUTTON->rect = sx_rect_make(
        SXGUI_MARGIN + address_width + SXGUI_GAP, toolbar_y, FILESAPP_UP_BUTTON_WIDTH, FILESAPP_TOOLBAR_HEIGHT);
    FILESAPP_LIST->rect = sx_rect_make(SXGUI_MARGIN, list_y, list_width, list_height);

    /* Name se queda con lo que sobra: es la columna que crece al agrandar la
     * ventana, igual que en el explorador. Lo que se descuenta es el area util
     * REAL de la lista -- los dos biseles hundidos y la columna del scrollbar,
     * que se reserva siempre: si solo se descontara cuando la barra esta, las
     * columnas saltarian de ancho al agregar un archivo. */
    name_width = list_width - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS -
        FILESAPP_COLUMN_SIZE_WIDTH - FILESAPP_COLUMN_TYPE_WIDTH;
    if (name_width < 90)
    {
        name_width = 90;
    }
    g_columns[0].width = name_width;

    count_pane_width = (list_width - FILESAPP_STATUS_GAP) / 2;
    if (count_pane_width < 80)
    {
        count_pane_width = 80;
    }
    g_widgets[3].rect = sx_rect_make(SXGUI_MARGIN, status_y, count_pane_width, SXGUI_STATUS_HEIGHT);
    g_widgets[4].rect = sx_rect_make(
        SXGUI_MARGIN + count_pane_width + FILESAPP_STATUS_GAP, status_y,
        list_width - count_pane_width - FILESAPP_STATUS_GAP, SXGUI_STATUS_HEIGHT);
}

static void on_resize(struct sxgui_app *app)
{
    filesapp_layout(app);
}

/* ---- menus ----------------------------------------------------------------- */

static const struct sxgui_menu_item k_file_items[] = {
    {"Open", FILESAPP_MENU_OPEN, 0},
    {"Up One Level", FILESAPP_MENU_UP, 0},
    {0, 0, 0},
    {"Close", FILESAPP_MENU_CLOSE, 0},
};

static const struct sxgui_menu_item k_view_items[] = {
    {"Refresh", FILESAPP_MENU_REFRESH, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Files", FILESAPP_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"View", k_view_items, (int)(sizeof(k_view_items) / sizeof(k_view_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menubar g_menubar = {
    k_menus,
    (int)(sizeof(k_menus) / sizeof(k_menus[0])),
    -1,
    -1,
    on_menu_command,
    0
};

/* ---- selftest ------------------------------------------------------------- */

static int filesapp_selftest(void)
{
    int failures = 0;
    int index;

    /* La tabla se vuelca SIEMPRE, tambien al fallar: un assert que dice "no
     * resolvio a notepad" sin mostrar a que resolvio no alcanza para
     * diagnosticar nada. */
    {
        /*
         * El costo del escaneo se MIDE, no se estima: la decision del
         * documento fue no poner cache hasta saber cuanto duele. Estos dos
         * numeros -- ejecutables abiertos y milisegundos -- son la evidencia
         * para revisarla.
         */
        unsigned long started_ms = uptime_ms();

        (void)file_assoc_load();
        printf("FILESAPP SMOKE assoc entries=%d examined=%d ms=%lu\n",
            file_assoc_count(),
            file_assoc_scan_examined(),
            uptime_ms() - started_ms);
    }
    for (index = 0; index < file_assoc_count(); ++index)
    {
        const struct file_assoc_entry *entry = file_assoc_at(index);
        if (entry != 0)
        {
            printf("FILESAPP SMOKE assoc %s -> %s (%s)\n",
                entry->extension,
                entry->program,
                entry->from_policy ? "politica" : "declarada");
        }
    }

    /*
     * Capa de iconos por tipo. Se reporta la cuenta del mapeo real de la
     * imagen y si el catalogo de /disk/icons responde: un mapeo cargado con
     * cero iconos que abren significa que el build no emitio los blobs, y eso
     * desde afuera se ve igual que "la lista no tiene iconos".
     */
    {
        int mapped = mime_icon_load();
        int resolved = 0;
        static const char *const k_probe[] = {"notas.txt", "captura.png", "tema.wav", "doom.sxe"};
        int probe;

        for (probe = 0; probe < (int)(sizeof(k_probe) / sizeof(k_probe[0])); ++probe)
        {
            if (mime_icon_for_file(k_probe[probe], 0, 0) != 0)
            {
                resolved += 1;
            }
        }
        printf("FILESAPP SMOKE mimeicon mapped=%d probes=%d resolved=%d\n",
            mapped,
            (int)(sizeof(k_probe) / sizeof(k_probe[0])),
            resolved);
    }

    /* El harness corta la corrida en la PRIMERA linea con el token de fallo,
     * asi que el volcado tiene que salir antes del selftest o no se ve nunca
     * cuando algo falla. */
    failures = file_assoc_selftest();
    failures += mime_icon_selftest();

    if (failures != 0)
    {
        printf("FILESAPP SMOKE FAIL %d checks\n", failures);
        return 1;
    }

    /*
     * Lo que los fixtures no pueden fingir: la resolucion contra los binarios
     * REALES de esta imagen, y cuanto cuesta. El numero de ejecutables abiertos
     * es la magnitud a mirar antes de decidir si hace falta una cache -- la
     * decision del documento es medir primero.
     */
    if (file_assoc_count() <= 0)
    {
        printf("FILESAPP SMOKE FAIL ningun binario declara extensiones\n");
        return 1;
    }

    printf("FILESAPP SMOKE PASS entries=%d\n", file_assoc_count());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return filesapp_selftest();
    }

    g_widgets[0] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_current_path);
    g_widgets[0].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[1] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Up One Level", on_up_clicked, 0);
    g_widgets[2] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_item_ptrs, 0);
    g_widgets[2].columns = g_columns;
    g_widgets[2].column_count = FILESAPP_COLUMN_COUNT;
    g_widgets[2].on_action = on_list;
    g_widgets[3] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_count_pane);
    g_widgets[3].flags |= SXGUI_FLAG_SUNKEN;
    g_widgets[4] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_size_pane);
    g_widgets[4].flags |= SXGUI_FLAG_SUNKEN;

    g_about_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                     FILESAPP_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "SavanXP Files");
    g_about_widgets[1] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW,
                     FILESAPP_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "Version: " SAVANXP_VERSION_STRING);
    g_about_widgets[2] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + FILESAPP_DLG_ROW * 2,
                     FILESAPP_ABOUT_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "Browse directories and launch programs");
    g_about_widgets[3] = sxgui_button(
        sx_rect_make((FILESAPP_ABOUT_WIDTH - SXGUI_BUTTON_WIDTH) / 2,
                     FILESAPP_ABOUT_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    g_about_dialog.title = "About Files";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 4;
    g_about_dialog.default_button = 3;

    if (sxgui_app_init(&g_app, "filesapp", g_widgets, 5) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_resize = on_resize;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    (void)sxgui_app_set_content_size(&g_app, FILESAPP_CONTENT_WIDTH, FILESAPP_CONTENT_HEIGHT);
    filesapp_layout(&g_app);

    if (filesapp_load_directory("/") < 0)
    {
        sxgui_app_quit(&g_app, 1);
    }
    /* El contenido de Files ES la lista: sin esto el foco arranca en ninguna
     * parte y las flechas no mueven la seleccion hasta que alguien tabula. */
    sxgui_focus(&g_app.ui, FILESAPP_LIST_INDEX);
    return sxgui_app_run(&g_app);
}
