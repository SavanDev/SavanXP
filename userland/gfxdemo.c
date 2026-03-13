#include "libc.h"

#define GFX_MAX_WIDTH 1920
#define GFX_MAX_HEIGHT 1080
#define BOX_SIZE 96

static uint32_t g_backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];
static uint32_t g_background[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];

static void draw_static_scene(struct savanxp_gfx_context* gfx, uint32_t* pixels) {
    const int panel_width = 320;
    const int panel_height = 84;
    const int title_y = 8;
    const int title_x = ((int)gfx->info.width - gfx_text_width("SavanXP gfx demo")) / 2;

    gfx_clear(pixels, &gfx->info, gfx_rgb(16, 24, 36));
    gfx_rect(pixels, &gfx->info, 0, 0, (int)gfx->info.width, 36, gfx_rgb(37, 61, 82));
    gfx_hline(pixels, &gfx->info, 0, 36, (int)gfx->info.width, gfx_rgb(72, 115, 145));
    gfx_blit_text(pixels, &gfx->info, title_x, title_y, "SavanXP gfx demo", gfx_rgb(213, 244, 223));

    gfx_rect(pixels, &gfx->info, 24, 52, panel_width, panel_height, gfx_rgb(21, 36, 54));
    gfx_frame(pixels, &gfx->info, 24, 52, panel_width, panel_height, gfx_rgb(82, 131, 166));
    gfx_blit_text(pixels, &gfx->info, 40, 68, "ARROWS move the box", gfx_rgb(213, 244, 223));
    gfx_blit_text(pixels, &gfx->info, 40, 92, "ESC returns to shell", gfx_rgb(213, 244, 223));
}

static void draw_box(struct savanxp_gfx_context* gfx, uint32_t* pixels, int box_x, int box_y) {
    gfx_rect(pixels, &gfx->info, box_x, box_y, BOX_SIZE, BOX_SIZE, gfx_rgb(39, 211, 123));
    gfx_frame(pixels, &gfx->info, box_x, box_y, BOX_SIZE, BOX_SIZE, gfx_rgb(199, 255, 225));
    gfx_rect(pixels, &gfx->info, box_x + 20, box_y + 20, BOX_SIZE - 40, BOX_SIZE - 40, gfx_rgb(19, 59, 45));
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
    int box_y = 140;
    int previous_box_x;
    int previous_box_y;

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

    draw_static_scene(&gfx, g_background);
    memcpy(g_backbuffer, g_background, gfx_buffer_bytes(&gfx.info));
    draw_box(&gfx, g_backbuffer, box_x, box_y);
    if (gfx_present(&gfx, g_backbuffer) < 0) {
        gfx_release(&gfx);
        gfx_close(&gfx);
        puts_fd(2, "gfxdemo: present failed\n");
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

        if (box_x < 0) {
            box_x = 0;
        }
        if (box_y < 40) {
            box_y = 40;
        }
        if (box_x + BOX_SIZE > (int)gfx.info.width) {
            box_x = (int)gfx.info.width - BOX_SIZE;
        }
        if (box_y + BOX_SIZE > (int)gfx.info.height) {
            box_y = (int)gfx.info.height - BOX_SIZE;
        }

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

            copy_region(g_backbuffer, g_background, &gfx.info, dirty_x, dirty_y, dirty_width, dirty_height);
            draw_box(&gfx, g_backbuffer, box_x, box_y);
            if (gfx_present_region(&gfx, g_backbuffer, (uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_width, (uint32_t)dirty_height) < 0) {
                break;
            }
        }

        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    gfx_release(&gfx);
    gfx_close(&gfx);
    puts_fd(2, "gfxdemo: present failed\n");
    return 1;
}
