#include "savanxp/libc.h"

#define GFX_APP_MAX_WIDTH 1920
#define GFX_APP_MAX_HEIGHT 1080
#define BOX_SIZE 88

static uint32_t g_pixels[GFX_APP_MAX_WIDTH * GFX_APP_MAX_HEIGHT];

static void clamp_box(const struct savanxp_fb_info* info, int* box_x, int* box_y) {
    if (*box_x < 24) {
        *box_x = 24;
    }
    if (*box_y < 84) {
        *box_y = 84;
    }
    if (*box_x + BOX_SIZE > (int)info->width - 24) {
        *box_x = (int)info->width - 24 - BOX_SIZE;
    }
    if (*box_y + BOX_SIZE > (int)info->height - 24) {
        *box_y = (int)info->height - 24 - BOX_SIZE;
    }
}

static void draw_ui(const struct savanxp_gfx_context* gfx, int box_x, int box_y) {
    const char* title = "SDK gfxhello";
    const char* subtitle = "ESC exits   arrows move";
    const int title_x = ((int)gfx->info.width - gfx_text_width(title)) / 2;

    gfx_clear(g_pixels, &gfx->info, gfx_rgb(12, 18, 30));
    gfx_rect(g_pixels, &gfx->info, 0, 0, (int)gfx->info.width, 40, gfx_rgb(30, 54, 82));
    gfx_hline(g_pixels, &gfx->info, 0, 40, (int)gfx->info.width, gfx_rgb(90, 145, 196));
    gfx_blit_text(g_pixels, &gfx->info, title_x, 12, title, gfx_rgb(236, 246, 255));

    gfx_rect(g_pixels, &gfx->info, 24, 60, 320, 70, gfx_rgb(19, 30, 46));
    gfx_frame(g_pixels, &gfx->info, 24, 60, 320, 70, gfx_rgb(90, 145, 196));
    gfx_blit_text(g_pixels, &gfx->info, 40, 76, subtitle, gfx_rgb(198, 226, 255));

    gfx_rect(g_pixels, &gfx->info, box_x, box_y, BOX_SIZE, BOX_SIZE, gfx_rgb(53, 194, 141));
    gfx_frame(g_pixels, &gfx->info, box_x, box_y, BOX_SIZE, BOX_SIZE, gfx_rgb(214, 255, 243));
    gfx_rect(g_pixels, &gfx->info, box_x + 18, box_y + 18, BOX_SIZE - 36, BOX_SIZE - 36, gfx_rgb(18, 76, 53));
}

int main(void) {
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    int box_x = 120;
    int box_y = 160;

    if (gfx_open(&gfx) < 0) {
        puts_err("gfxhello: gfx_open failed\n");
        return 1;
    }
    if (gfx.info.width > GFX_APP_MAX_WIDTH || gfx.info.height > GFX_APP_MAX_HEIGHT || gfx_buffer_pixels(&gfx.info) > (sizeof(g_pixels) / sizeof(g_pixels[0]))) {
        puts_err("gfxhello: framebuffer too large for static backbuffer\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0) {
        puts_err("gfxhello: gfx_acquire failed\n");
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

        clamp_box(&gfx.info, &box_x, &box_y);
        draw_ui(&gfx, box_x, box_y);
        if (gfx_present(&gfx, g_pixels) < 0) {
            break;
        }
        sleep_ms(16);
    }

    gfx_release(&gfx);
    gfx_close(&gfx);
    puts_err("gfxhello: gfx_present failed\n");
    return 1;
}
