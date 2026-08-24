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
        (unsigned long long)(info.svfs_used_bytes / (1024ULL * 1024ULL)),
        (unsigned long long)(info.svfs_total_bytes / (1024ULL * 1024ULL)));
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

int main(void)
{
    struct sxgui_widget widgets[18];

    refresh_info();

    widgets[0] = sxgui_label(sx_rect_make(16, 10, 320, 16), "About SavanXP");
    widgets[1] = sxgui_label(sx_rect_make(16, 30, 420, 16), "Experimental desktop OS with compositor-first GUI");

    widgets[2] = sxgui_groupbox(sx_rect_make(16, 56, 424, 148), "System");
    widgets[3] = sxgui_label(sx_rect_make(28, 76, 400, 16), g_version_line);
    widgets[4] = sxgui_label(sx_rect_make(28, 96, 400, 16), g_uptime_line);
    widgets[5] = sxgui_label(sx_rect_make(28, 116, 400, 16), g_process_line);
    widgets[6] = sxgui_label(sx_rect_make(28, 136, 400, 16), g_memory_line);
    widgets[7] = sxgui_label(sx_rect_make(28, 156, 400, 16), g_disk_line);
    widgets[8] = sxgui_label(sx_rect_make(28, 176, 400, 16), g_time_line);

    widgets[9] = sxgui_groupbox(sx_rect_make(16, 214, 424, 88), "Shell");
    widgets[10] = sxgui_label(sx_rect_make(28, 234, 400, 16), "Program Manager launches apps from its groups");
    widgets[11] = sxgui_label(sx_rect_make(28, 254, 400, 16), "Task List switches between or ends running tasks");
    widgets[12] = sxgui_label(sx_rect_make(28, 274, 400, 16), "Window controls: minimize, maximize, close");

    widgets[13] = sxgui_groupbox(sx_rect_make(16, 312, 424, 68), "Keyboard");
    widgets[14] = sxgui_label(sx_rect_make(28, 332, 400, 16), "CTRL+ESC opens the Task List   ESC closes this window");
    widgets[15] = sxgui_label(sx_rect_make(28, 352, 400, 16), "F5 or the Refresh button updates the values");

    widgets[16] = sxgui_button(sx_rect_make(16, 392, 100, 26), "Refresh", on_refresh, &g_app);
    widgets[17] = sxgui_button(sx_rect_make(128, 392, 100, 26), "Close", on_close, &g_app);

    if (sxgui_app_init(&g_app, "aboutapp", widgets, 18) < 0)
    {
        return 1;
    }
    /* Layout fijo: la ventana es exactamente lo que ocupan los widgets. */
    (void)sxgui_app_autosize(&g_app);
    g_app.on_key = on_key;
    return sxgui_app_run(&g_app);
}
