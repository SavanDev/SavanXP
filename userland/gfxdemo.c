#include "libc.h"

#define GFX_MAX_WIDTH 1920
#define GFX_MAX_HEIGHT 1080

static uint32_t g_backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];

static void draw_scene(struct savanxp_gfx_context* gfx, int box_x, int box_y) {
    const int panel_width = 320;
    const int panel_height = 84;
    const int title_y = 8;
    const int title_x = ((int)gfx->info.width - gfx_text_width("SavanXP gfx demo")) / 2;

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(16, 24, 36));
    gfx_rect(g_backbuffer, &gfx->info, 0, 0, (int)gfx->info.width, 36, gfx_rgb(37, 61, 82));
    gfx_hline(g_backbuffer, &gfx->info, 0, 36, (int)gfx->info.width, gfx_rgb(72, 115, 145));
    gfx_blit_text(g_backbuffer, &gfx->info, title_x, title_y, "SavanXP gfx demo", gfx_rgb(213, 244, 223));

    gfx_rect(g_backbuffer, &gfx->info, 24, 52, panel_width, panel_height, gfx_rgb(21, 36, 54));
    gfx_frame(g_backbuffer, &gfx->info, 24, 52, panel_width, panel_height, gfx_rgb(82, 131, 166));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 68, "ARROWS move the box", gfx_rgb(213, 244, 223));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 92, "ESC returns to shell", gfx_rgb(213, 244, 223));

    gfx_rect(g_backbuffer, &gfx->info, box_x, box_y, 96, 96, gfx_rgb(39, 211, 123));
    gfx_frame(g_backbuffer, &gfx->info, box_x, box_y, 96, 96, gfx_rgb(199, 255, 225));
    gfx_rect(g_backbuffer, &gfx->info, box_x + 20, box_y + 20, 56, 56, gfx_rgb(19, 59, 45));
}

int main(void) {
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    int box_x = 120;
    int box_y = 140;

    if (gfx_open(&gfx) < 0) {
        puts_fd(2, "gfxdemo: open failed\n");
        return 1;
    }
    if (gfx.info.width > GFX_MAX_WIDTH || gfx.info.height > GFX_MAX_HEIGHT || (gfx.info.pitch / 4u) > GFX_MAX_WIDTH) {
        puts_fd(2, "gfxdemo: framebuffer too large\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0) {
        puts_fd(2, "gfxdemo: acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    for (;;) {
        while (gfx_poll_event(&gfx, &event) > 0) {
            if (event.type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
                continue;
            }
            if (event.key == SAVANXP_KEY_ESC) {
                gfx_release(&gfx);
                gfx_close(&gfx);
                return 0;
            }
            if (event.key == SAVANXP_KEY_LEFT) {
                box_x -= 8;
            } else if (event.key == SAVANXP_KEY_RIGHT) {
                box_x += 8;
            } else if (event.key == SAVANXP_KEY_UP) {
                box_y -= 8;
            } else if (event.key == SAVANXP_KEY_DOWN) {
                box_y += 8;
            }
        }

        if (box_x < 0) {
            box_x = 0;
        }
        if (box_y < 40) {
            box_y = 40;
        }
        if (box_x + 96 > (int)gfx.info.width) {
            box_x = (int)gfx.info.width - 96;
        }
        if (box_y + 96 > (int)gfx.info.height) {
            box_y = (int)gfx.info.height - 96;
        }

        draw_scene(&gfx, box_x, box_y);
        if (gfx_present(&gfx, g_backbuffer) < 0) {
            break;
        }
        sleep_ms(16);
    }

    gfx_release(&gfx);
    gfx_close(&gfx);
    puts_fd(2, "gfxdemo: present failed\n");
    return 1;
}
