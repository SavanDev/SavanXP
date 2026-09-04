#include "libc.h"
#include "savanxp/sxgui.h"

#include <stdio.h>

#include "shared/version.h"

static struct sxgui_app g_app;

static char g_version_line[96];
static char g_uptime_line[96];
static char g_process_line[96];
static char g_memory_line[96];
static char g_disk_line[96];
static char g_time_line[96];

static void refresh_info(void)
{
    struct savanxp_system_info info = {0};
    struct savanxp_realtime now = {0};
    struct savanxp_process_info proc = {0};
    unsigned long process_count = 0;
    unsigned long proc_index = 0;

    (void)system_info(&info);
    while (proc_info(proc_index, &proc) > 0)
    {
        if (proc.state != SAVANXP_PROC_UNUSED)
        {
            process_count += 1;
        }
        proc_index += 1;
    }

    snprintf(g_version_line, sizeof(g_version_line), "Version: %s", SAVANXP_VERSION_STRING);
    snprintf(g_uptime_line, sizeof(g_uptime_line), "Uptime: %llu ms", (unsigned long long)info.uptime_ms);
    snprintf(g_process_line, sizeof(g_process_line), "Processes: %lu active", process_count);
    snprintf(
        g_memory_line,
        sizeof(g_memory_line),
        "Memory: %llu MiB usable, %llu MiB reclaimable",
        (unsigned long long)(info.memory_usable_bytes / (1024ULL * 1024ULL)),
        (unsigned long long)(info.memory_reclaimable_bytes / (1024ULL * 1024ULL)));
    snprintf(
        g_disk_line,
        sizeof(g_disk_line),
        "Persistent disk: %llu / %llu MiB used",
        (unsigned long long)(info.sxfs_used_bytes / (1024ULL * 1024ULL)),
        (unsigned long long)(info.sxfs_total_bytes / (1024ULL * 1024ULL)));
    if (realtime(&now) == 0 && now.valid != 0)
    {
        snprintf(g_time_line, sizeof(g_time_line), "Clock: %02u:%02u:%02u", now.hour, now.minute, now.second);
    }
    else
    {
        snprintf(g_time_line, sizeof(g_time_line), "Clock: unavailable");
    }
}

static void on_refresh(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    refresh_info();
    sxgui_app_request_repaint((struct sxgui_app *)user);
}

static void on_close(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    sxgui_app_quit((struct sxgui_app *)user, 0);
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && event->key == SAVANXP_KEY_F5)
    {
        refresh_info();
        return 1;
    }
    return 0;
}

/* Grilla de la ventana. El margen es SXGUI_CONTENT_MARGIN porque es el que
 * sxgui_app_autosize deja del otro lado: usar cualquier otro deja la ventana
 * despareja sin que se note en el codigo.
 *
 * ABOUT_ROW es el paso entre lineas de texto y ABOUT_INDENT la sangria de lo
 * que va adentro de un group box. El alto de rotulo es el del tipo, no 16: un
 * rect mas bajo que el texto lo centraba medio pixel arriba. */
#define ABOUT_MARGIN SXGUI_CONTENT_MARGIN
#define ABOUT_WIDTH 424
#define ABOUT_LABEL_HEIGHT 18
#define ABOUT_ROW (ABOUT_LABEL_HEIGHT + 4)
#define ABOUT_INDENT 12
/* Del borde de arriba del group box a su primera linea: la mitad del rotulo
 * que se monta sobre el marco, mas una fila. */
#define ABOUT_GROUP_TOP (ABOUT_LABEL_HEIGHT / 2 + ABOUT_ROW / 2 + 4)

/* Alto de un group box de `rows` lineas, cerrando abajo con el mismo aire. */
#define ABOUT_GROUP_HEIGHT(rows) (ABOUT_GROUP_TOP + (rows) * ABOUT_ROW + ABOUT_INDENT - 4)

int main(void)
{
    struct sxgui_widget widgets[18];
    int text_width = ABOUT_WIDTH - ABOUT_INDENT * 2;
    int y = ABOUT_MARGIN;
    int group_y;

    refresh_info();

    widgets[0] = sxgui_label(sx_rect_make(ABOUT_MARGIN, y, ABOUT_WIDTH, ABOUT_LABEL_HEIGHT), "About SavanXP");
    y += ABOUT_ROW;
    widgets[1] = sxgui_label(
        sx_rect_make(ABOUT_MARGIN, y, ABOUT_WIDTH, ABOUT_LABEL_HEIGHT),
        "Experimental desktop OS with compositor-first GUI");
    y += ABOUT_ROW + SXGUI_GAP;

    group_y = y;
    widgets[2] = sxgui_groupbox(
        sx_rect_make(ABOUT_MARGIN, group_y, ABOUT_WIDTH, ABOUT_GROUP_HEIGHT(6)), "System");
    y = group_y + ABOUT_GROUP_TOP;
    widgets[3] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT), g_version_line);
    y += ABOUT_ROW;
    widgets[4] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT), g_uptime_line);
    y += ABOUT_ROW;
    widgets[5] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT), g_process_line);
    y += ABOUT_ROW;
    widgets[6] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT), g_memory_line);
    y += ABOUT_ROW;
    widgets[7] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT), g_disk_line);
    y += ABOUT_ROW;
    widgets[8] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT), g_time_line);
    y = group_y + ABOUT_GROUP_HEIGHT(6) + SXGUI_GAP;

    group_y = y;
    widgets[9] = sxgui_groupbox(
        sx_rect_make(ABOUT_MARGIN, group_y, ABOUT_WIDTH, ABOUT_GROUP_HEIGHT(3)), "Shell");
    y = group_y + ABOUT_GROUP_TOP;
    widgets[10] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT),
                              "Program Manager launches apps from its groups");
    y += ABOUT_ROW;
    widgets[11] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT),
                              "Task List switches between or ends running tasks");
    y += ABOUT_ROW;
    widgets[12] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT),
                              "Window controls: minimize, maximize, close");
    y = group_y + ABOUT_GROUP_HEIGHT(3) + SXGUI_GAP;

    group_y = y;
    widgets[13] = sxgui_groupbox(
        sx_rect_make(ABOUT_MARGIN, group_y, ABOUT_WIDTH, ABOUT_GROUP_HEIGHT(2)), "Keyboard");
    y = group_y + ABOUT_GROUP_TOP;
    widgets[14] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT),
                              "CTRL+ESC opens the Task List   ESC closes this window");
    y += ABOUT_ROW;
    widgets[15] = sxgui_label(sx_rect_make(ABOUT_MARGIN + ABOUT_INDENT, y, text_width, ABOUT_LABEL_HEIGHT),
                              "F5 or the Refresh button updates the values");
    y = group_y + ABOUT_GROUP_HEIGHT(2) + SXGUI_GAP + SXGUI_GAP;

    /* Los botones se apoyan en el borde derecho del contenido, que es donde
     * los busca la vista despues de leer los group boxes. */
    widgets[16] = sxgui_button(
        sx_rect_make(ABOUT_MARGIN + ABOUT_WIDTH - SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP, y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Refresh", on_refresh, &g_app);
    widgets[17] = sxgui_button(
        sx_rect_make(ABOUT_MARGIN + ABOUT_WIDTH - SXGUI_BUTTON_WIDTH, y,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Close", on_close, &g_app);

    if (sxgui_app_init(&g_app, "aboutapp", widgets, 18) < 0)
    {
        return 1;
    }
    /* Layout fijo: la ventana es exactamente lo que ocupan los widgets. */
    (void)sxgui_app_autosize(&g_app);
    g_app.on_key = on_key;
    return sxgui_app_run(&g_app);
}
