/*
 * Agregar o quitar programas.
 *
 * La contracara del alta automatica del launcher (docs/SXE_FORMAT.md, "All
 * Programs"): ahi instalar es copiar el binario a /disk/bin y aparece solo, y
 * aca desinstalar es borrarlo y desaparece solo. No hay ninguna entrada de
 * registro que dar de baja, porque no hay ninguna que se haya dado de alta.
 *
 * Que se puede desinstalar y por que lo decide appwiz_catalog.h; esto es la
 * ventana.
 */

#include "appwiz_catalog.h"
#include "savanxp/sxgui.h"

#include <stdio.h>

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[8];

#define APPWIZ_TITLE_LABEL (&g_widgets[0])
#define APPWIZ_LIST (&g_widgets[1])
#define APPWIZ_DETAIL (&g_widgets[2])
#define APPWIZ_DATA_CHECK (&g_widgets[3])
#define APPWIZ_REMOVE_BUTTON (&g_widgets[4])
#define APPWIZ_WIDGET_COUNT ((int)(sizeof(g_widgets) / sizeof(g_widgets[0])))

/* Filas de la lista, con las celdas separadas por tabulacion. */
static char g_rows[APPWIZ_MAX_ENTRIES][APPWIZ_NAME_CAPACITY + 32];
static const char *g_row_ptrs[APPWIZ_MAX_ENTRIES];
static const struct sx_bitmap *g_row_icons[APPWIZ_MAX_ENTRIES];

static char g_detail[APPWIZ_PATH_CAPACITY + APPWIZ_DESC_CAPACITY + 16];
static char g_data_label[APPWIZ_PATH_CAPACITY + 48];
static char g_status[APPWIZ_NAME_CAPACITY + 96];

#define APPWIZ_COLUMN_SIZE_WIDTH 90

/*
 * El ancho de Program se calcula en main a partir del area util de la lista, no
 * se clava: es la misma cuenta que hace filesapp -- el rect menos los dos
 * biseles hundidos y menos la columna del scrollbar, que se reserva siempre
 * este o no, para que las columnas no salten de ancho al agregar una fila. Esa
 * columna reservada es la franja que queda a la derecha de la cabecera.
 */
static struct sxgui_column g_columns[] = {
    {"Program", 0, 0},
    {"Size", APPWIZ_COLUMN_SIZE_WIDTH, SXGUI_COLUMN_RIGHT},
};
#define APPWIZ_COLUMN_COUNT ((int)(sizeof(g_columns) / sizeof(g_columns[0])))

static struct sxgui_dialog g_confirm_dialog;
static struct sxgui_widget g_confirm_widgets[4];
static char g_confirm_line[APPWIZ_NAME_CAPACITY + 48];
static char g_confirm_data_line[APPWIZ_PATH_CAPACITY + 32];

/* ---- grilla ---------------------------------------------------------------
 *
 * Layout fijo y autosize, como el Acerca de: esta ventana es una lista corta y
 * tres botones, no un contenedor que crezca. El margen es SXGUI_CONTENT_MARGIN
 * porque es el que autosize deja del otro lado. */
#define APPWIZ_MARGIN SXGUI_CONTENT_MARGIN
#define APPWIZ_WIDTH 360
#define APPWIZ_LABEL_HEIGHT 18
#define APPWIZ_ROW (APPWIZ_LABEL_HEIGHT + 4)
#define APPWIZ_LIST_HEIGHT 176

#define APPWIZ_DLG_WIDTH 340
#define APPWIZ_DLG_HEIGHT 132
#define APPWIZ_DLG_MARGIN SXGUI_DIALOG_MARGIN
#define APPWIZ_DLG_ROW (APPWIZ_LABEL_HEIGHT + 4)
#define APPWIZ_DLG_BUTTON_ROW (APPWIZ_DLG_HEIGHT - APPWIZ_DLG_MARGIN - SXGUI_BUTTON_HEIGHT)
#define APPWIZ_DLG_CENTRED(count, index) \
    ((APPWIZ_DLG_WIDTH - (count) * SXGUI_BUTTON_WIDTH - ((count) - 1) * SXGUI_GAP) / 2 + \
     (index) * (SXGUI_BUTTON_WIDTH + SXGUI_GAP))

static void format_size(uint32_t bytes, char *out, size_t capacity)
{
    if (bytes >= 1024u * 1024u)
    {
        snprintf(out, capacity, "%u.%u MB", (unsigned)(bytes / (1024u * 1024u)),
                 (unsigned)((bytes % (1024u * 1024u)) / (1024u * 1024u / 10u)));
    }
    else if (bytes >= 1024u)
    {
        snprintf(out, capacity, "%u KB", (unsigned)(bytes / 1024u));
    }
    else
    {
        snprintf(out, capacity, "%u B", (unsigned)bytes);
    }
}

static const struct appwiz_entry *selected_entry(void)
{
    return appwiz_entry_at(APPWIZ_LIST->value);
}

/*
 * La casilla de datos se apaga cuando no hay nada que borrar. Deshabilitada y
 * no escondida a proposito: que el programa no declare un directorio de datos
 * es informacion sobre el programa, y esconder el control lo unico que logra
 * es que la ventana cambie de forma segun la fila.
 */
static void refresh_details(void)
{
    const struct appwiz_entry *entry = selected_entry();

    if (entry == 0)
    {
        snprintf(g_detail, sizeof(g_detail), "No programs installed outside the system image.");
        snprintf(g_data_label, sizeof(g_data_label), "Also remove the program's data");
        APPWIZ_DATA_CHECK->value = 0;
        APPWIZ_DATA_CHECK->flags |= SXGUI_FLAG_DISABLED;
        APPWIZ_REMOVE_BUTTON->flags |= SXGUI_FLAG_DISABLED;
        return;
    }

    snprintf(g_detail, sizeof(g_detail), "%s  -  %s",
             entry->description[0] != '\0' ? entry->description : "No description",
             entry->path);
    APPWIZ_REMOVE_BUTTON->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;

    if (entry->data_removable)
    {
        snprintf(g_data_label, sizeof(g_data_label), "Also remove the program's data (%s)", entry->data_dir);
        APPWIZ_DATA_CHECK->flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
    else
    {
        if (entry->data_dir[0] != '\0')
        {
            /* Declaro un directorio que la validacion rechazo. Se dice, en vez
             * de mostrarlo igual que a un programa que no declaro nada: es un
             * manifiesto mal escrito y alguien tiene que enterarse. */
            snprintf(g_data_label, sizeof(g_data_label), "Data directory not removable (%s)", entry->data_dir);
        }
        else
        {
            snprintf(g_data_label, sizeof(g_data_label), "This program declares no data of its own");
        }
        APPWIZ_DATA_CHECK->value = 0;
        APPWIZ_DATA_CHECK->flags |= SXGUI_FLAG_DISABLED;
    }
}

static void reload_catalog(void)
{
    int count = appwiz_catalog_scan(0);
    int index;

    for (index = 0; index < count; ++index)
    {
        const struct appwiz_entry *entry = appwiz_entry_at(index);
        char size_cell[24];

        format_size(entry->size_bytes, size_cell, sizeof(size_cell));
        snprintf(g_rows[index], sizeof(g_rows[index]), "%s%c%s",
                 entry->name, SXGUI_COLUMN_SEPARATOR, size_cell);
        g_row_ptrs[index] = g_rows[index];
        g_row_icons[index] = appwiz_entry_icon(entry);
    }

    APPWIZ_LIST->items = g_row_ptrs;
    APPWIZ_LIST->item_icons = g_row_icons;
    APPWIZ_LIST->item_count = count;
    if (APPWIZ_LIST->value >= count)
    {
        APPWIZ_LIST->value = count > 0 ? count - 1 : 0;
    }
    APPWIZ_LIST->scroll = 0;
    refresh_details();
}

static void on_list(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    refresh_details();
}

static void on_confirm_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

static void on_confirm_remove(struct sxgui_widget *widget, void *user)
{
    const struct appwiz_entry *entry = selected_entry();
    char name[APPWIZ_NAME_CAPACITY];
    int remove_data;
    int result;

    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    if (entry == 0)
    {
        return;
    }

    /* El nombre se copia ANTES de desinstalar: el rescan de abajo reordena el
     * catalogo y `entry` deja de ser valido. */
    snprintf(name, sizeof(name), "%s", entry->name);
    remove_data = APPWIZ_DATA_CHECK->value != 0;
    result = appwiz_uninstall(entry, remove_data);

    switch (result)
    {
    case APPWIZ_OK:
        snprintf(g_status, sizeof(g_status), "%s was removed.", name);
        break;
    case APPWIZ_ERR_DATA:
        /* El programa se fue igual: decir solo "fallo" seria mentir y dejar a
         * alguien buscando un ejecutable que ya no existe. */
        snprintf(g_status, sizeof(g_status), "%s was removed, but its data could not be deleted.", name);
        break;
    default:
        snprintf(g_status, sizeof(g_status), "%s could not be removed.", name);
        break;
    }
    reload_catalog();
}

static void on_remove(struct sxgui_widget *widget, void *user)
{
    const struct appwiz_entry *entry = selected_entry();

    (void)widget;
    (void)user;
    if (entry == 0)
    {
        return;
    }

    snprintf(g_confirm_line, sizeof(g_confirm_line), "Remove %s from this system?", entry->name);
    if (APPWIZ_DATA_CHECK->value != 0 && entry->data_removable)
    {
        snprintf(g_confirm_data_line, sizeof(g_confirm_data_line), "%s will be deleted as well.", entry->data_dir);
    }
    else
    {
        snprintf(g_confirm_data_line, sizeof(g_confirm_data_line), "Its data will be left in place.");
    }
    sxgui_dialog_begin(&g_app.ui, &g_confirm_dialog, APPWIZ_DLG_WIDTH, APPWIZ_DLG_HEIGHT);
}

static void on_refresh(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    reload_catalog();
    snprintf(g_status, sizeof(g_status), "%d program(s) installed outside the system image.", appwiz_entry_count());
}

static void on_close(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    sxgui_app_quit((struct sxgui_app *)user, 0);
}

int main(int argc, char **argv)
{
    int y = APPWIZ_MARGIN;
    int buttons_y;

    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        int failures = appwiz_selftest();

        if (failures != 0)
        {
            printf("APPWIZ SMOKE FAIL %d checks\n", failures);
            return 1;
        }
        printf("APPWIZ SMOKE PASS entries=%d\n", appwiz_entry_count());
        return 0;
    }

    *APPWIZ_TITLE_LABEL = sxgui_label(
        sx_rect_make(APPWIZ_MARGIN, y, APPWIZ_WIDTH, APPWIZ_LABEL_HEIGHT),
        "Programs installed outside the system image:");
    y += APPWIZ_ROW;

    *APPWIZ_LIST = sxgui_listbox(
        sx_rect_make(APPWIZ_MARGIN, y, APPWIZ_WIDTH, APPWIZ_LIST_HEIGHT), g_row_ptrs, 0);
    g_columns[0].width = APPWIZ_WIDTH - SXGUI_BORDER_SUNKEN * 2 - SXGUI_SCROLLBAR_THICKNESS -
        APPWIZ_COLUMN_SIZE_WIDTH;
    APPWIZ_LIST->columns = g_columns;
    APPWIZ_LIST->column_count = APPWIZ_COLUMN_COUNT;
    APPWIZ_LIST->on_action = on_list;
    y += APPWIZ_LIST_HEIGHT + SXGUI_GAP;

    *APPWIZ_DETAIL = sxgui_label(
        sx_rect_make(APPWIZ_MARGIN, y, APPWIZ_WIDTH, SXGUI_STATUS_HEIGHT), g_detail);
    APPWIZ_DETAIL->flags |= SXGUI_FLAG_SUNKEN;
    y += SXGUI_STATUS_HEIGHT + SXGUI_GAP;

    *APPWIZ_DATA_CHECK = sxgui_checkbox(
        sx_rect_make(APPWIZ_MARGIN, y, APPWIZ_WIDTH, APPWIZ_LABEL_HEIGHT), g_data_label, 0);
    y += APPWIZ_ROW + SXGUI_GAP;

    buttons_y = y;
    *APPWIZ_REMOVE_BUTTON = sxgui_button(
        sx_rect_make(APPWIZ_MARGIN, buttons_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Remove", on_remove, 0);
    g_widgets[5] = sxgui_button(
        sx_rect_make(APPWIZ_MARGIN + APPWIZ_WIDTH - SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Refresh", on_refresh, 0);
    g_widgets[6] = sxgui_button(
        sx_rect_make(APPWIZ_MARGIN + APPWIZ_WIDTH - SXGUI_BUTTON_WIDTH, buttons_y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Close", on_close, &g_app);
    y += SXGUI_BUTTON_HEIGHT + SXGUI_GAP;

    g_widgets[7] = sxgui_label(
        sx_rect_make(APPWIZ_MARGIN, y, APPWIZ_WIDTH, SXGUI_STATUS_HEIGHT), g_status);
    g_widgets[7].flags |= SXGUI_FLAG_SUNKEN;

    g_confirm_widgets[0] = sxgui_label(
        sx_rect_make(APPWIZ_DLG_MARGIN, APPWIZ_DLG_MARGIN,
                     APPWIZ_DLG_WIDTH - APPWIZ_DLG_MARGIN * 2, APPWIZ_LABEL_HEIGHT),
        g_confirm_line);
    g_confirm_widgets[1] = sxgui_label(
        sx_rect_make(APPWIZ_DLG_MARGIN, APPWIZ_DLG_MARGIN + APPWIZ_DLG_ROW,
                     APPWIZ_DLG_WIDTH - APPWIZ_DLG_MARGIN * 2, APPWIZ_LABEL_HEIGHT),
        g_confirm_data_line);
    g_confirm_widgets[2] = sxgui_button(
        sx_rect_make(APPWIZ_DLG_CENTRED(2, 0), APPWIZ_DLG_BUTTON_ROW, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Remove", on_confirm_remove, 0);
    g_confirm_widgets[3] = sxgui_button(
        sx_rect_make(APPWIZ_DLG_CENTRED(2, 1), APPWIZ_DLG_BUTTON_ROW, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_confirm_cancel, 0);
    g_confirm_dialog.title = "Add or Remove Programs";
    g_confirm_dialog.widgets = g_confirm_widgets;
    g_confirm_dialog.widget_count = 4;
    g_confirm_dialog.initial_focus = 3;
    /* Enter cae en Cancel a proposito: el boton por defecto de un dialogo que
     * borra tiene que ser el que no borra. */
    g_confirm_dialog.default_button = 3;

    reload_catalog();
    snprintf(g_status, sizeof(g_status), "%d program(s) installed outside the system image.", appwiz_entry_count());

    if (sxgui_app_init(&g_app, "appwiz", g_widgets, APPWIZ_WIDGET_COUNT) < 0)
    {
        return 1;
    }
    (void)sxgui_app_autosize(&g_app);
    return sxgui_app_run(&g_app);
}
