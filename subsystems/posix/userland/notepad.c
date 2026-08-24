#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "shared/version.h"

/*
 * Bloc de notas, con la forma del de la era Win95: una ventana que es toda area
 * de texto, menu File/Edit/Search/Help y barra de estado.
 *
 * Es la casa del preview que Files tenia adentro: Files ahora lanza
 * /bin/notepad con el archivo como argv[1] en vez de volcar los primeros bytes
 * en un panel propio.
 *
 * El documento entero vive en un buffer estatico. El limite es deliberado y se
 * avisa al abrir: la arena del SDK es BSS y se mapea entera al exec, asi que
 * cada MiB reservado es RAM residente de la app.
 */

#define NOTEPAD_DOCUMENT_CAPACITY (32 * 1024)
#define NOTEPAD_PATH_CAPACITY 192

#define NOTEPAD_MENU_NEW 1
#define NOTEPAD_MENU_OPEN 2
#define NOTEPAD_MENU_SAVE 3
#define NOTEPAD_MENU_SAVE_AS 4
#define NOTEPAD_MENU_EXIT 5
#define NOTEPAD_MENU_SELECT_ALL 10
#define NOTEPAD_MENU_GO_TOP 20
#define NOTEPAD_MENU_GO_BOTTOM 21
#define NOTEPAD_MENU_ABOUT 30

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[2];

static char g_document[NOTEPAD_DOCUMENT_CAPACITY];
static char g_path[NOTEPAD_PATH_CAPACITY];
static char g_status[128] = "Untitled";
static char g_title[160] = "Untitled";

/* Un solo dialogo de ruta para Open y Save As: un campo de texto, que es el
 * equivalente honesto de un file picker mientras no exista uno. `g_path_mode`
 * dice cual de las dos operaciones confirma el boton. */
#define NOTEPAD_PATH_MODE_OPEN 0
#define NOTEPAD_PATH_MODE_SAVE 1

static struct sxgui_dialog g_open_dialog;
static struct sxgui_widget g_open_widgets[4];
static char g_open_path[NOTEPAD_PATH_CAPACITY];
static int g_path_mode = NOTEPAD_PATH_MODE_OPEN;

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[4];

#define NOTEPAD_EDIT (&g_widgets[0])
#define NOTEPAD_STATUS (&g_widgets[1])

/* ---- estado del documento ------------------------------------------------- */

static const char *notepad_basename(const char *path)
{
    const char *last = path;
    const char *cursor = path;

    if (path == 0 || path[0] == '\0')
    {
        return "Untitled";
    }
    while (*cursor != '\0')
    {
        if (*cursor == '/' && cursor[1] != '\0')
        {
            last = cursor + 1;
        }
        cursor += 1;
    }
    return last;
}

static void notepad_refresh_title(void)
{
    snprintf(
        g_title,
        sizeof(g_title),
        "%s%s",
        NOTEPAD_EDIT->modified ? "*" : "",
        notepad_basename(g_path));
}

static void notepad_set_status(const char *text)
{
    snprintf(g_status, sizeof(g_status), "%s", text != 0 ? text : "");
}

static void notepad_new_document(void)
{
    g_document[0] = '\0';
    g_path[0] = '\0';
    NOTEPAD_EDIT->caret = 0;
    NOTEPAD_EDIT->value = 0;
    NOTEPAD_EDIT->scroll = 0;
    NOTEPAD_EDIT->modified = 0;
    notepad_refresh_title();
    notepad_set_status("Untitled");
}

/* Los bytes que no son texto se muestran como '.', igual que hacia el preview:
 * abrir un binario por accidente no tiene que ensuciar la pantalla con
 * caracteres de control. */
static int notepad_load(const char *path)
{
    FILE *stream = 0;
    struct stat info = {0};
    size_t read_bytes = 0;
    size_t index = 0;
    int truncated = 0;

    if (path == 0 || path[0] == '\0')
    {
        return -1;
    }
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode))
    {
        notepad_set_status("That path is a directory.");
        return -1;
    }

    stream = fopen(path, "r");
    if (stream == 0)
    {
        notepad_set_status("Cannot open that file.");
        return -1;
    }

    read_bytes = fread(g_document, 1, sizeof(g_document) - 1, stream);
    /* Si entro justo todo el buffer, puede haber quedado resto sin leer. */
    if (read_bytes == sizeof(g_document) - 1)
    {
        truncated = 1;
    }
    fclose(stream);
    g_document[read_bytes] = '\0';

    for (index = 0; index < read_bytes; ++index)
    {
        unsigned char value = (unsigned char)g_document[index];
        if (value == '\n')
        {
            continue;
        }
        if (value == '\r')
        {
            /* CRLF: se descarta el CR y la linea queda como la espera el
             * editor, que separa por '\n' solo. */
            memmove(&g_document[index], &g_document[index + 1], read_bytes - index);
            read_bytes -= 1;
            index -= 1;
            continue;
        }
        if (value == '\t')
        {
            g_document[index] = ' ';
            continue;
        }
        if (value < 32 || value > 126)
        {
            g_document[index] = '.';
        }
    }

    snprintf(g_path, sizeof(g_path), "%s", path);
    NOTEPAD_EDIT->caret = 0;
    NOTEPAD_EDIT->value = 0;
    NOTEPAD_EDIT->scroll = 0;
    NOTEPAD_EDIT->modified = 0;
    notepad_refresh_title();
    if (truncated)
    {
        notepad_set_status("File too large: only the first 32 KB were loaded.");
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "%s", path);
    }
    return 0;
}

static void notepad_path_dialog(int mode)
{
    g_path_mode = mode;
    snprintf(g_open_path, sizeof(g_open_path), "%s", g_path);
    g_open_widgets[1].caret = (int)strlen(g_open_path);
    g_open_dialog.title = mode == NOTEPAD_PATH_MODE_SAVE ? "Save As" : "Open";
    g_open_widgets[2].text = mode == NOTEPAD_PATH_MODE_SAVE ? "Save" : "Open";
    sxgui_dialog_begin(&g_app.ui, &g_open_dialog, 340, 118);
}

static int notepad_save(void)
{
    FILE *stream = 0;
    size_t length = strlen(g_document);
    size_t written = 0;

    if (g_path[0] == '\0')
    {
        /* Sin nombre todavia: Save se comporta como Save As, igual que el bloc
         * de notas de siempre. */
        notepad_path_dialog(NOTEPAD_PATH_MODE_SAVE);
        return -1;
    }

    stream = fopen(g_path, "w");
    if (stream == 0)
    {
        notepad_set_status("Cannot write that file.");
        return -1;
    }
    written = length > 0 ? fwrite(g_document, 1, length, stream) : 0;
    fclose(stream);

    if (written != length)
    {
        notepad_set_status("Write failed.");
        return -1;
    }
    NOTEPAD_EDIT->modified = 0;
    notepad_refresh_title();
    snprintf(g_status, sizeof(g_status), "Saved %s", g_path);
    return 0;
}

/* ---- callbacks ------------------------------------------------------------ */

static void on_edit_changed(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    notepad_refresh_title();
}

static void on_open_accept(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (g_open_path[0] == '\0')
    {
        return;
    }
    if (g_path_mode == NOTEPAD_PATH_MODE_SAVE)
    {
        snprintf(g_path, sizeof(g_path), "%s", g_open_path);
        notepad_refresh_title();
        (void)notepad_save();
        return;
    }
    (void)notepad_load(g_open_path);
}

static void on_open_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_about_ok(struct sxgui_widget *widget, void *user)
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
    case NOTEPAD_MENU_NEW:
        notepad_new_document();
        break;
    case NOTEPAD_MENU_OPEN:
        notepad_path_dialog(NOTEPAD_PATH_MODE_OPEN);
        break;
    case NOTEPAD_MENU_SAVE:
        (void)notepad_save();
        break;
    case NOTEPAD_MENU_SAVE_AS:
        notepad_path_dialog(NOTEPAD_PATH_MODE_SAVE);
        break;
    case NOTEPAD_MENU_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    case NOTEPAD_MENU_SELECT_ALL:
        /* Sin seleccion todavia: lo mas util que puede hacer es llevar el caret
         * al final, que es lo que se quiere para seguir escribiendo. */
        NOTEPAD_EDIT->caret = (int)strlen(g_document);
        notepad_set_status("Caret moved to end of document.");
        break;
    case NOTEPAD_MENU_GO_TOP:
        NOTEPAD_EDIT->caret = 0;
        NOTEPAD_EDIT->value = 0;
        NOTEPAD_EDIT->scroll = 0;
        break;
    case NOTEPAD_MENU_GO_BOTTOM:
        NOTEPAD_EDIT->caret = (int)strlen(g_document);
        break;
    case NOTEPAD_MENU_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, 300, 116);
        break;
    default:
        break;
    }
}

/* El editor se come casi todo el teclado, asi que aca solo quedan los atajos
 * que no son texto. */
static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN)
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_F2)
    {
        (void)notepad_save();
        return 1;
    }
    if (event->key == SAVANXP_KEY_F3)
    {
        notepad_path_dialog(NOTEPAD_PATH_MODE_OPEN);
        return 1;
    }
    return 0;
}

/* ---- layout --------------------------------------------------------------- */

#define NOTEPAD_CONTENT_WIDTH 560
#define NOTEPAD_CONTENT_HEIGHT 420
#define NOTEPAD_STATUS_HEIGHT 20

static void notepad_layout(struct sxgui_app *app)
{
    int width = (int)app->gfx.info.width;
    int height = (int)app->gfx.info.height;
    int top = sxgui_menubar_height() + 5;
    int status_y = height - NOTEPAD_STATUS_HEIGHT - 5;
    int edit_height = status_y - top - 5;

    if (edit_height < 60)
    {
        edit_height = 60;
    }
    NOTEPAD_EDIT->rect = sx_rect_make(8, top, width - 16, edit_height);
    NOTEPAD_STATUS->rect = sx_rect_make(8, status_y, width - 16, NOTEPAD_STATUS_HEIGHT);
}

static void on_resize(struct sxgui_app *app)
{
    notepad_layout(app);
}

/* ---- menus ----------------------------------------------------------------- */

static const struct sxgui_menu_item k_file_items[] = {
    {"New", NOTEPAD_MENU_NEW, 0},
    {"Open...", NOTEPAD_MENU_OPEN, 0},
    {"Save", NOTEPAD_MENU_SAVE, 0},
    {"Save As...", NOTEPAD_MENU_SAVE_AS, 0},
    {0, 0, 0},
    {"Exit", NOTEPAD_MENU_EXIT, 0},
};

static const struct sxgui_menu_item k_edit_items[] = {
    {"Go to end", NOTEPAD_MENU_SELECT_ALL, 0},
};

static const struct sxgui_menu_item k_search_items[] = {
    {"Top of document", NOTEPAD_MENU_GO_TOP, 0},
    {"End of document", NOTEPAD_MENU_GO_BOTTOM, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Notepad", NOTEPAD_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Edit", k_edit_items, (int)(sizeof(k_edit_items) / sizeof(k_edit_items[0]))},
    {"Search", k_search_items, (int)(sizeof(k_search_items) / sizeof(k_search_items[0]))},
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

int main(int argc, char **argv)
{
    g_widgets[0] = sxgui_textedit(sx_rect_make(0, 0, 0, 0), g_document, NOTEPAD_DOCUMENT_CAPACITY);
    g_widgets[0].on_action = on_edit_changed;
    g_widgets[1] = sxgui_label(sx_rect_make(0, 0, 0, 0), g_status);
    g_widgets[1].flags |= SXGUI_FLAG_SUNKEN;

    g_open_widgets[0] = sxgui_label(sx_rect_make(12, 10, 300, 16), "File name:");
    g_open_widgets[1] = sxgui_textfield(sx_rect_make(12, 30, 312, 22), g_open_path, sizeof(g_open_path));
    g_open_widgets[2] = sxgui_button(sx_rect_make(120, 62, 96, 26), "Open", on_open_accept, 0);
    g_open_widgets[3] = sxgui_button(sx_rect_make(228, 62, 96, 26), "Cancel", on_open_cancel, 0);
    g_open_dialog.title = "Open";
    g_open_dialog.widgets = g_open_widgets;
    g_open_dialog.widget_count = 4;
    g_open_dialog.initial_focus = 1; /* el campo de la ruta */

    g_about_widgets[0] = sxgui_label(sx_rect_make(10, 8, 280, 16), "SavanXP Notepad");
    g_about_widgets[1] = sxgui_label(sx_rect_make(10, 28, 280, 16), "Version: " SAVANXP_VERSION_STRING);
    g_about_widgets[2] = sxgui_label(sx_rect_make(10, 48, 280, 16), "F2 saves   F3 opens");
    g_about_widgets[3] = sxgui_button(sx_rect_make(100, 76, 100, 26), "OK", on_about_ok, 0);
    g_about_dialog.title = "About Notepad";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 4;

    if (sxgui_app_init(&g_app, "notepad", g_widgets, 2) < 0)
    {
        return 1;
    }
    g_app.on_key = on_key;
    g_app.on_resize = on_resize;
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    (void)sxgui_app_set_content_size(&g_app, NOTEPAD_CONTENT_WIDTH, NOTEPAD_CONTENT_HEIGHT);
    notepad_layout(&g_app);

    notepad_new_document();
    /* argv[1] es el archivo a abrir: es como lo lanza Files. */
    if (argc > 1 && argv != 0 && argv[1] != 0 && argv[1][0] != '\0')
    {
        (void)notepad_load(argv[1]);
    }
    sxgui_focus(&g_app.ui, 0);
    return sxgui_app_run(&g_app);
}
