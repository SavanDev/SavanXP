#include "libc.h"

#include <stdio.h>

#include "shared/version.h"

#define ABOUTAPP_MAX_WIDTH 1920
#define ABOUTAPP_MAX_HEIGHT 1080

static uint32_t g_backbuffer[ABOUTAPP_MAX_WIDTH * ABOUTAPP_MAX_HEIGHT];

static void draw_panel(struct savanxp_gfx_context *gfx, const struct savanxp_system_info *info)
{
    char version_line[96];
    char uptime_line[96];
    char process_line[96];
    char memory_line[96];
    char disk_line[96];
    char time_line[96];
    struct savanxp_realtime now = {0};
    struct savanxp_process_info proc = {0};
    unsigned long process_count = 0;
    unsigned long proc_index = 0;

    while (proc_info(proc_index, &proc) > 0)
    {
        if (proc.state != SAVANXP_PROC_UNUSED)
        {
            process_count += 1;
        }
        proc_index += 1;
    }

    snprintf(version_line, sizeof(version_line), "Version: %s", SAVANXP_VERSION_STRING);
    snprintf(uptime_line, sizeof(uptime_line), "Uptime: %llu ms", (unsigned long long)info->uptime_ms);
    snprintf(process_line, sizeof(process_line), "Processes: %lu active", process_count);
    snprintf(
        memory_line,
        sizeof(memory_line),
        "Memory: %llu MiB usable, %llu MiB reclaimable",
        (unsigned long long)(info->memory_usable_bytes / (1024ULL * 1024ULL)),
        (unsigned long long)(info->memory_reclaimable_bytes / (1024ULL * 1024ULL)));
    snprintf(
        disk_line,
        sizeof(disk_line),
        "Persistent disk: %llu / %llu MiB used",
        (unsigned long long)(info->svfs_used_bytes / (1024ULL * 1024ULL)),
        (unsigned long long)(info->svfs_total_bytes / (1024ULL * 1024ULL)));
    if (realtime(&now) == 0 && now.valid != 0)
    {
        snprintf(time_line, sizeof(time_line), "Clock: %02u:%02u:%02u", now.hour, now.minute, now.second);
    }
    else
    {
        snprintf(time_line, sizeof(time_line), "Clock: unavailable");
    }

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(12, 92, 134));
    gfx_rect(g_backbuffer, &gfx->info, 0, 0, (int)gfx->info.width, 36, gfx_rgb(7, 56, 89));
    gfx_hline(g_backbuffer, &gfx->info, 0, 36, (int)gfx->info.width, gfx_rgb(177, 216, 236));
    gfx_blit_text(g_backbuffer, &gfx->info, 20, 10, "About SavanXP", gfx_rgb(255, 255, 255));

    gfx_rect(g_backbuffer, &gfx->info, 24, 56, (int)gfx->info.width - 48, (int)gfx->info.height - 112, gfx_rgb(224, 228, 232));
    gfx_frame(g_backbuffer, &gfx->info, 24, 56, (int)gfx->info.width - 48, (int)gfx->info.height - 112, gfx_rgb(64, 70, 76));
    gfx_rect(g_backbuffer, &gfx->info, 40, 76, 240, 64, gfx_rgb(0, 106, 72));
    gfx_blit_text(g_backbuffer, &gfx->info, 56, 92, "Experimental desktop OS", gfx_rgb(255, 255, 255));
    gfx_blit_text(g_backbuffer, &gfx->info, 56, 112, "with compositor-first GUI", gfx_rgb(232, 244, 238));

    gfx_blit_text(g_backbuffer, &gfx->info, 320, 82, version_line, gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 320, 104, uptime_line, gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 320, 126, process_line, gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 320, 148, memory_line, gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 320, 170, disk_line, gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 320, 192, time_line, gfx_rgb(28, 32, 36));

    gfx_blit_text(g_backbuffer, &gfx->info, 48, 170, "Desktop shortcuts", gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 48, 194, "Start menu launches GUI apps in overlay windows", gfx_rgb(58, 64, 72));
    gfx_blit_text(g_backbuffer, &gfx->info, 48, 214, "Taskbar buttons restore or minimize windows", gfx_rgb(58, 64, 72));
    gfx_blit_text(g_backbuffer, &gfx->info, 48, 234, "Window controls: minimize, maximize, close", gfx_rgb(58, 64, 72));

    gfx_blit_text(g_backbuffer, &gfx->info, 48, 286, "Keyboard", gfx_rgb(28, 32, 36));
    gfx_blit_text(g_backbuffer, &gfx->info, 48, 308, "SUPER toggles Start   ESC closes this window", gfx_rgb(58, 64, 72));
    gfx_blit_text(g_backbuffer, &gfx->info, 48, 328, "Desktop icon double click launches the app", gfx_rgb(58, 64, 72));

    gfx_blit_text(g_backbuffer, &gfx->info, 48, (int)gfx->info.height - 44, "SavanXP desktop session: ESC exits this app", gfx_rgb(38, 42, 48));
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct savanxp_system_info info = {0};

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "aboutapp: gfx_open failed\n");
        return 1;
    }
    if (gfx.info.width > ABOUTAPP_MAX_WIDTH || gfx.info.height > ABOUTAPP_MAX_HEIGHT || (gfx.info.pitch / 4u) > ABOUTAPP_MAX_WIDTH)
    {
        puts_fd(2, "aboutapp: framebuffer too large\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "aboutapp: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    (void)system_info(&info);
    draw_panel(&gfx, &info);
    if (gfx_present(&gfx, g_backbuffer) < 0)
    {
        gfx_release(&gfx);
        gfx_close(&gfx);
        puts_fd(2, "aboutapp: present failed\n");
        return 1;
    }

    for (;;)
    {
        while (gfx_poll_event(&gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &event);
                draw_panel(&gfx, &info);
                if (gfx_present(&gfx, g_backbuffer) < 0)
                {
                    gfx_release(&gfx);
                    gfx_close(&gfx);
                    puts_fd(2, "aboutapp: present failed\n");
                    return 1;
                }
                continue;
            }
            if (event.type != SAVANXP_INPUT_EVENT_KEY_DOWN)
            {
                continue;
            }
            if (event.key == SAVANXP_KEY_ESC)
            {
                gfx_release(&gfx);
                gfx_close(&gfx);
                return 0;
            }
            if (event.key == SAVANXP_KEY_F5)
            {
                (void)system_info(&info);
                draw_panel(&gfx, &info);
                if (gfx_present(&gfx, g_backbuffer) < 0)
                {
                    gfx_release(&gfx);
                    gfx_close(&gfx);
                    puts_fd(2, "aboutapp: present failed\n");
                    return 1;
                }
            }
        }
        sleep_ms(16);
    }
}
