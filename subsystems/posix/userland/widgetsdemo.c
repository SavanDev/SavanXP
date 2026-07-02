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

#define WIDGETSDEMO_MENU_EXIT 9

static void on_menu_command(int id, void *user)
{
    (void)user;
    if (id == WIDGETSDEMO_MENU_EXIT)
    {
        sxgui_app_quit(&g_app, 0);
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
    {"About widgets", 8, 0},
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

int main(void)
{
    static struct sxgui_widget widgets[13];
    int item_count = (int)(sizeof(g_list_items) / sizeof(g_list_items[0]));
    int top = sxgui_menubar_height() + 6;

    widgets[0] = sxgui_label(sx_rect_make(16, top, 320, 16), "sxgui widget gallery");
    widgets[1] = sxgui_button(sx_rect_make(16, top + 28, 100, 26), "OK", on_ok, 0);
    widgets[2] = sxgui_button(sx_rect_make(128, top + 28, 100, 26), "Cancel", on_cancel, 0);
    widgets[3] = sxgui_checkbox(sx_rect_make(16, top + 70, 220, 18), "Enable option", 1);
    widgets[4] = sxgui_textfield(sx_rect_make(16, top + 100, 220, 22), g_text_buffer, (int)sizeof(g_text_buffer));
    widgets[5] = sxgui_listbox(sx_rect_make(16, top + 134, 220, 112), g_list_items, item_count);
    widgets[5].on_action = on_list;
    widgets[6] = sxgui_scrollbar(sx_rect_make(252, top + 134, 16, 112), 0, 99, 10, 50);
    widgets[6].on_action = on_scroll;
    widgets[7] = sxgui_label(sx_rect_make(280, top + 134, 100, 16), g_scroll_status);
    widgets[8] = sxgui_label(sx_rect_make(16, top + 256, 320, 16), g_status);
    widgets[9] = sxgui_radio(sx_rect_make(252, top + 64, 120, 16), "Small", 1, 1);
    widgets[9].on_action = on_radio;
    widgets[10] = sxgui_radio(sx_rect_make(252, top + 84, 120, 16), "Medium", 1, 0);
    widgets[10].on_action = on_radio;
    widgets[11] = sxgui_radio(sx_rect_make(252, top + 104, 120, 16), "Large", 1, 0);
    widgets[11].on_action = on_radio;
    widgets[12] = sxgui_combobox(
        sx_rect_make(252, top + 28, 130, 22),
        g_theme_items,
        (int)(sizeof(g_theme_items) / sizeof(g_theme_items[0])),
        0);
    widgets[12].on_action = on_theme;

    if (sxgui_app_init(&g_app, "widgetsdemo", widgets, 13) < 0)
    {
        return 1;
    }
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    return sxgui_app_run(&g_app);
}
