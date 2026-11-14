#include "libc.h"

#include "savanxp/syscall.h"

/* Minimal i386 desktop, launched after sh32 exits (no scheduler yet).
 * One fullscreen /dev/gpu0 client renders 64-row strips to keep its working
 * buffer at 256 KiB. The kernel composes the cursor over every presentation.
 * The window and taskbar are informational; there is no window manager. */

#define DESKTOP_MAX_WIDTH 1024
#define DESKTOP_STRIP_ROWS 64
#define DESKTOP_FRAME_MS 250

static uint32_t g_strip[DESKTOP_MAX_WIDTH * DESKTOP_STRIP_ROWS];

static const char* const kTitle = "SavanXP Legacy";
static const char* const kSubtitle = "i386 legacy target";
static const char* const kBody[] = {
    "Framebuffer via /dev/gpu0",
    "PIT 100 Hz, PS/2 keyboard and mouse",
    "SxFS root mounted read-only by the kernel",
};
static const uint32_t kBodyCount = sizeof(kBody) / sizeof(kBody[0]);

static void draw_desktop(uint32_t* pixels, const struct savanxp_fb_info* info, int y_offset,
                         int width, int height) {
    const uint32_t wallpaper = gfx_rgb(18, 26, 44);
    const uint32_t panel = gfx_rgb(28, 42, 62);
    const uint32_t accent = gfx_rgb(90, 145, 196);
    const uint32_t window_body = gfx_rgb(232, 236, 242);
    const uint32_t window_edge = gfx_rgb(120, 134, 152);
    const uint32_t title_bar = gfx_rgb(44, 79, 124);
    const uint32_t title_text = gfx_rgb(236, 246, 255);
    const uint32_t body_text = gfx_rgb(40, 52, 68);
    const int window_x = 80;
    const int window_y = 72;
    const int window_width = 560;
    const int window_height = 340;
    const int taskbar_height = 36;

    gfx_clear(pixels, info, wallpaper);

    /* Top panel and accent line. */
    gfx_rect(pixels, info, 0, 0 - y_offset, width, 28, panel);
    gfx_hline(pixels, info, 0, 27 - y_offset, width, accent);
    gfx_blit_text_title(pixels, info, 12, 4 - y_offset, kTitle, title_text);

    /* Informational window: shadow, frame and title bar. */
    gfx_rect(pixels, info, window_x + 6, window_y + 6 - y_offset, window_width, window_height,
             gfx_rgb(10, 14, 22));
    gfx_rect(pixels, info, window_x, window_y - y_offset, window_width, window_height,
             window_body);
    gfx_frame(pixels, info, window_x, window_y - y_offset, window_width, window_height,
              window_edge);
    gfx_rect(pixels, info, window_x + 1, window_y + 1 - y_offset, window_width - 2, 26,
             title_bar);
    gfx_blit_text_title(pixels, info, window_x + 12, window_y + 4 - y_offset, kTitle,
                        title_text);
    gfx_rect(pixels, info, window_x + window_width - 20, window_y + 8 - y_offset, 10, 10,
             gfx_rgb(214, 92, 92));

    gfx_blit_text(pixels, info, window_x + 16, window_y + 44 - y_offset, kSubtitle,
                  body_text);
    for (uint32_t index = 0; index < kBodyCount; ++index) {
        const int line_y = window_y + 76 + (int)index * 22;
        if (line_y + 16 < y_offset || line_y > y_offset + height) {
            continue;
        }
        gfx_blit_text_mono(pixels, info, window_x + 16, line_y - y_offset, kBody[index],
                           body_text);
    }

    /* Taskbar. */
    gfx_rect(pixels, info, 0, height - taskbar_height - y_offset, width, taskbar_height, panel);
    gfx_hline(pixels, info, 0, height - taskbar_height - y_offset, width, accent);
    gfx_blit_text(pixels, info, 12, height - taskbar_height + 10 - y_offset, kTitle,
                  gfx_rgb(214, 226, 240));
}

static void format_size(char* buffer, int capacity, unsigned int width, unsigned int height,
                        unsigned int bpp) {
    int offset = 0;
    const unsigned int values[3] = {width, height, bpp};
    for (int index = 0; index < 3; ++index) {
        char digits[12];
        int count = 0;
        unsigned int value = values[index];
        if (value == 0) {
            digits[count++] = '0';
        }
        while (value != 0 && count < (int)sizeof(digits)) {
            digits[count++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        while (count > 0 && offset + 1 < capacity) {
            buffer[offset++] = digits[--count];
        }
        if (index == 0 && offset + 1 < capacity) {
            buffer[offset++] = 'x';
        }
        if (index == 1 && offset + 1 < capacity) {
            buffer[offset++] = 'x';
        }
    }
    buffer[offset] = '\0';
}

int main(void) {
    struct savanxp_gpu_info gpu;
    struct savanxp_fb_info screen;
    char geometry[24];
    int fd;

    memset(&gpu, 0, sizeof(gpu));
    fd = (int)gpu_open();
    if (fd < 0) {
        puts_out("desktop: cannot open /dev/gpu0\n");
        return 1;
    }
    if (gpu_get_info(fd, &gpu) < 0 || gpu.width == 0 || gpu.width > DESKTOP_MAX_WIDTH ||
        gpu.height == 0 || gpu.bpp != 32) {
        puts_out("desktop: unsupported /dev/gpu0 geometry\n");
        savanxp_close(fd);
        return 1;
    }

    if (gpu_acquire(fd) < 0) {
        puts_out("desktop: cannot acquire video\n");
        savanxp_close(fd);
        return 1;
    }
    memset(&screen, 0, sizeof(screen));
    screen.width = gpu.width;
    screen.height = DESKTOP_STRIP_ROWS;
    screen.pitch = gpu.width * 4u;
    screen.bpp = gpu.bpp;
    screen.buffer_size = screen.pitch * screen.height;

    format_size(geometry, sizeof(geometry), gpu.width, gpu.height, gpu.bpp);
    puts_out("desktop: gpu ");
    puts_out(geometry);
    puts_out(", framebuffer backend, 64-row strips\n");

    puts_out("desktop: ready\n");
    for (;;) {
        int y = 0;
        while (y < (int)gpu.height) {
            const int rows = ((int)gpu.height - y) < DESKTOP_STRIP_ROWS
                                 ? (int)gpu.height - y
                                 : DESKTOP_STRIP_ROWS;
            draw_desktop(g_strip, &screen, y, (int)gpu.width, (int)gpu.height);
            if (gpu_present_region(fd, g_strip, gpu.width, 0, (uint32_t)y,
                                   gpu.width, (uint32_t)rows) < 0) {
                puts_out("desktop: present failed\n");
                savanxp_close(fd);
                return 1;
            }
            y += rows;
        }
        sleep_ms(DESKTOP_FRAME_MS);
    }
}
