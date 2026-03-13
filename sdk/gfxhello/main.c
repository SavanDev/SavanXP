#include "savanxp/libc.h"

#define GFX_APP_MAX_WIDTH 1920
#define GFX_APP_MAX_HEIGHT 1080
#define BOX_SIZE 88

static uint32_t g_pixels[GFX_APP_MAX_WIDTH * GFX_APP_MAX_HEIGHT];
static uint32_t g_background[GFX_APP_MAX_WIDTH * GFX_APP_MAX_HEIGHT];

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

static void draw_static_ui(const struct savanxp_gfx_context* gfx, uint32_t* pixels) {
    const char* title = "SDK gfxhello";
    const char* subtitle = "ESC exits   arrows move";
    const int title_x = ((int)gfx->info.width - gfx_text_width(title)) / 2;

    gfx_clear(pixels, &gfx->info, gfx_rgb(12, 18, 30));
    gfx_rect(pixels, &gfx->info, 0, 0, (int)gfx->info.width, 40, gfx_rgb(30, 54, 82));
    gfx_hline(pixels, &gfx->info, 0, 40, (int)gfx->info.width, gfx_rgb(90, 145, 196));
    gfx_blit_text(pixels, &gfx->info, title_x, 12, title, gfx_rgb(236, 246, 255));

    gfx_rect(pixels, &gfx->info, 24, 60, 320, 70, gfx_rgb(19, 30, 46));
    gfx_frame(pixels, &gfx->info, 24, 60, 320, 70, gfx_rgb(90, 145, 196));
    gfx_blit_text(pixels, &gfx->info, 40, 76, subtitle, gfx_rgb(198, 226, 255));
}

static void draw_box(const struct savanxp_gfx_context* gfx, uint32_t* pixels, int box_x, int box_y) {
    gfx_rect(pixels, &gfx->info, box_x, box_y, BOX_SIZE, BOX_SIZE, gfx_rgb(53, 194, 141));
    gfx_frame(pixels, &gfx->info, box_x, box_y, BOX_SIZE, BOX_SIZE, gfx_rgb(214, 255, 243));
    gfx_rect(pixels, &gfx->info, box_x + 18, box_y + 18, BOX_SIZE - 36, BOX_SIZE - 36, gfx_rgb(18, 76, 53));
}

static void copy_region(uint32_t* destination, const uint32_t* source, const struct savanxp_fb_info* info, int x, int y, int width, int height) {
    uint32_t pitch_pixels;
    int row;

    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (width <= 0 || height <= 0 || x >= (int)info->width || y >= (int)info->height) {
        return;
    }
    if (x + width > (int)info->width) {
        width = (int)info->width - x;
    }
    if (y + height > (int)info->height) {
        height = (int)info->height - y;
    }

    pitch_pixels = gfx_stride_pixels(info);
    for (row = 0; row < height; ++row) {
        memcpy(
            destination + ((size_t)(y + row) * pitch_pixels) + (uint32_t)x,
            source + ((size_t)(y + row) * pitch_pixels) + (uint32_t)x,
            (size_t)width * sizeof(uint32_t));
    }
}

int main(void) {
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    int box_x = 120;
    int box_y = 160;
    int previous_box_x;
    int previous_box_y;

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

    draw_static_ui(&gfx, g_background);
    memcpy(g_pixels, g_background, gfx_buffer_bytes(&gfx.info));
    draw_box(&gfx, g_pixels, box_x, box_y);
    if (gfx_present(&gfx, g_pixels) < 0) {
        gfx_release(&gfx);
        gfx_close(&gfx);
        puts_err("gfxhello: gfx_present failed\n");
        return 1;
    }

    previous_box_x = box_x;
    previous_box_y = box_y;

    for (;;) {
        int moved = 0;
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
                moved = 1;
            } else if (event.key == SAVANXP_KEY_RIGHT) {
                box_x += 8;
                moved = 1;
            } else if (event.key == SAVANXP_KEY_UP) {
                box_y -= 8;
                moved = 1;
            } else if (event.key == SAVANXP_KEY_DOWN) {
                box_y += 8;
                moved = 1;
            }
        }

        clamp_box(&gfx.info, &box_x, &box_y);
        if (!moved && box_x == previous_box_x && box_y == previous_box_y) {
            sleep_ms(8);
            continue;
        }

        {
            int dirty_x = previous_box_x < box_x ? previous_box_x : box_x;
            int dirty_y = previous_box_y < box_y ? previous_box_y : box_y;
            int dirty_right = (previous_box_x + BOX_SIZE) > (box_x + BOX_SIZE) ? (previous_box_x + BOX_SIZE) : (box_x + BOX_SIZE);
            int dirty_bottom = (previous_box_y + BOX_SIZE) > (box_y + BOX_SIZE) ? (previous_box_y + BOX_SIZE) : (box_y + BOX_SIZE);
            int dirty_width = dirty_right - dirty_x;
            int dirty_height = dirty_bottom - dirty_y;

            copy_region(g_pixels, g_background, &gfx.info, dirty_x, dirty_y, dirty_width, dirty_height);
            draw_box(&gfx, g_pixels, box_x, box_y);
            if (gfx_present_region(&gfx, g_pixels, (uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_width, (uint32_t)dirty_height) < 0) {
                break;
            }
        }

        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    gfx_release(&gfx);
    gfx_close(&gfx);
    puts_err("gfxhello: gfx_present failed\n");
    return 1;
}
