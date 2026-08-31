#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>
#include <string.h>

static struct sxgui_app g_app;

static char g_text_buffer[48] = "type here";
static char g_status[64] = "Ready";
static char g_scroll_status[32] = "Scroll: 50";
static int g_ok_clicks = 0;

static const char *const g_list_items[] = {
    "Documents",
    "Pictures",
    "Music",
    "Programs",
    "Network",
    "Recycle Bin",
    "Control Panel",
    "Printers",
    "Fonts",
    "Games",
    "Accessories",
    "System Tools",
    "Startup",
    "Downloads"
};

static void on_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    g_ok_clicks += 1;
    snprintf(g_status, sizeof(g_status), "OK pressed %d time(s)", g_ok_clicks);
}

static void on_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    snprintf(g_status, sizeof(g_status), "Cancel pressed");
}

static void on_list(struct sxgui_widget *widget, void *user)
{
    const char *item = widget->items != 0 ? widget->items[widget->value] : "?";
    (void)user;
    if (widget->action == SXGUI_ACTION_ACTIVATE)
    {
        snprintf(g_status, sizeof(g_status), "Activated: %s", item);
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "Selected: %s", item);
    }
}

static void on_scroll(struct sxgui_widget *widget, void *user)
{
    (void)user;
    snprintf(g_scroll_status, sizeof(g_scroll_status), "Scroll: %d", widget->value);
}

static void on_radio(struct sxgui_widget *widget, void *user)
{
    (void)user;
    snprintf(g_status, sizeof(g_status), "Radio: %s", widget->text != 0 ? widget->text : "?");
}

static const char *const g_theme_items[] = {
    "Classic",
    "Teal",
    "Plum",
    "Desert",
    "High Contrast",
    "Rainy Day",
    "Slate",
    "Rose",
    "Maple",
    "Wheat"
};

static void on_theme(struct sxgui_widget *widget, void *user)
{
    (void)user;
    snprintf(g_status, sizeof(g_status), "Theme: %s", widget->items[widget->value]);
}

#define WIDGETSDEMO_MENU_ABOUT 8
#define WIDGETSDEMO_MENU_EXIT 9

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[3];

static void on_dialog_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    snprintf(g_status, sizeof(g_status), "About closed with OK");
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    if (id == WIDGETSDEMO_MENU_EXIT)
    {
        sxgui_app_quit(&g_app, 0);
        return;
    }
    if (id == WIDGETSDEMO_MENU_ABOUT)
    {
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, 260, 104);
        return;
    }
    snprintf(g_status, sizeof(g_status), "Menu command %d", id);
}

static const struct sxgui_menu_item k_file_items[] = {
    {"Reset status", 1, 0},
    {"Say hello", 2, 0},
    {0, 0, 0},
    {"Exit", WIDGETSDEMO_MENU_EXIT, 0},
};

static const struct sxgui_menu_item k_edit_items[] = {
    {"Cut", 3, SXGUI_MENU_DISABLED},
    {"Copy", 4, 0},
    {"Word wrap", 5, SXGUI_MENU_CHECKED},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About widgets", WIDGETSDEMO_MENU_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Edit", k_edit_items, (int)(sizeof(k_edit_items) / sizeof(k_edit_items[0]))},
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

/* Grilla de la galeria: dos columnas, el mismo margen que deja autosize del
 * otro lado y las alturas estandar de cada control. Es la ventana que existe
 * para mostrar como se ve el toolkit, asi que es la que menos puede permitirse
 * medidas propias. */
#define DEMO_MARGIN SXGUI_CONTENT_MARGIN
#define DEMO_ROW 22
#define DEMO_COL_WIDTH 220
/* La barra de scroll suelta va entre las dos columnas, asi que la segunda
 * arranca despues de ELLA y no despues de la lista: contarla de menos las
 * superponia. */
#define DEMO_SCROLL_X (DEMO_MARGIN + DEMO_COL_WIDTH + SXGUI_GAP * 2)
#define DEMO_COL2 (DEMO_SCROLL_X + SXGUI_SCROLLBAR_THICKNESS + SXGUI_GAP * 2)

int main(void)
{
    static struct sxgui_widget widgets[13];
    int item_count = (int)(sizeof(g_list_items) / sizeof(g_list_items[0]));
    int top = sxgui_menubar_height() + DEMO_MARGIN;
    int left_y;
    int right_y;

    left_y = top;
    widgets[0] = sxgui_label(sx_rect_make(DEMO_MARGIN, left_y, 320, 18), "sxgui widget gallery");
    left_y += DEMO_ROW + SXGUI_GAP;
    widgets[1] = sxgui_button(
        sx_rect_make(DEMO_MARGIN, left_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT), "OK", on_ok, 0);
    widgets[2] = sxgui_button(
        sx_rect_make(DEMO_MARGIN + SXGUI_BUTTON_WIDTH + SXGUI_GAP, left_y, SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Cancel", on_cancel, 0);
    left_y += SXGUI_BUTTON_HEIGHT + SXGUI_GAP * 2;
    widgets[3] = sxgui_checkbox(sx_rect_make(DEMO_MARGIN, left_y, DEMO_COL_WIDTH, 20), "Enable option", 1);
    left_y += DEMO_ROW + SXGUI_GAP;
    widgets[4] = sxgui_textfield(
        sx_rect_make(DEMO_MARGIN, left_y, DEMO_COL_WIDTH, SXGUI_FIELD_HEIGHT),
        g_text_buffer, (int)sizeof(g_text_buffer));
    left_y += SXGUI_FIELD_HEIGHT + SXGUI_GAP * 2;
    widgets[5] = sxgui_listbox(sx_rect_make(DEMO_MARGIN, left_y, DEMO_COL_WIDTH, 112), g_list_items, item_count);
    widgets[5].on_action = on_list;

    /* La barra suelta y su rotulo comparten la fila de la lista, que es lo que
     * hace que las dos columnas terminen a la misma altura. */
    widgets[6] = sxgui_scrollbar(
        sx_rect_make(DEMO_SCROLL_X, left_y, SXGUI_SCROLLBAR_THICKNESS, 112),
        0, 99, 10, 50);
    widgets[6].on_action = on_scroll;
    widgets[7] = sxgui_label(sx_rect_make(DEMO_COL2, left_y, 120, 18), g_scroll_status);
    left_y += 112 + SXGUI_GAP * 2;
    widgets[8] = sxgui_label(sx_rect_make(DEMO_MARGIN, left_y, 320, 18), g_status);

    right_y = top + DEMO_ROW + SXGUI_GAP;
    widgets[12] = sxgui_combobox(
        sx_rect_make(DEMO_COL2, right_y, 130, SXGUI_FIELD_HEIGHT),
        g_theme_items,
        (int)(sizeof(g_theme_items) / sizeof(g_theme_items[0])),
        0);
    widgets[12].on_action = on_theme;
    right_y += SXGUI_FIELD_HEIGHT + SXGUI_GAP * 2;
    widgets[9] = sxgui_radio(sx_rect_make(DEMO_COL2, right_y, 120, 20), "Small", 1, 1);
    widgets[9].on_action = on_radio;
    right_y += DEMO_ROW;
    widgets[10] = sxgui_radio(sx_rect_make(DEMO_COL2, right_y, 120, 20), "Medium", 1, 0);
    widgets[10].on_action = on_radio;
    right_y += DEMO_ROW;
    widgets[11] = sxgui_radio(sx_rect_make(DEMO_COL2, right_y, 120, 20), "Large", 1, 0);
    widgets[11].on_action = on_radio;

    g_about_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, 260 - SXGUI_DIALOG_MARGIN * 2, 18),
        "sxgui widget gallery");
    g_about_widgets[1] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + DEMO_ROW, 260 - SXGUI_DIALOG_MARGIN * 2, 18),
        "Win9x-flavoured toolkit for SavanXP");
    g_about_widgets[2] = sxgui_button(
        sx_rect_make((260 - SXGUI_BUTTON_WIDTH) / 2, 104 - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_dialog_ok, 0);
    g_about_dialog.title = "About";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = 3;

    if (sxgui_app_init(&g_app, "widgetsdemo", widgets, 13) < 0)
    {
        return 1;
    }
    /* Layout fijo: la ventana es exactamente lo que ocupa la galeria. */
    (void)sxgui_app_autosize(&g_app);
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    return sxgui_app_run(&g_app);
}
