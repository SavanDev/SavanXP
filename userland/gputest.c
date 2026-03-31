#include "libc.h"

#define GPUTEST_MAX_WIDTH 1920
#define GPUTEST_MAX_HEIGHT 1080
#define GPUTEST_BOX_SIZE 96

static uint32_t g_pixels[GPUTEST_MAX_WIDTH * GPUTEST_MAX_HEIGHT];
static uint32_t g_background[GPUTEST_MAX_WIDTH * GPUTEST_MAX_HEIGHT];

static void draw_static_scene(const struct savanxp_fb_info *info)
{
    const int panel_width = 360;
    const int panel_height = 84;
    const int title_y = 8;
    const int title_x = ((int)info->width - gfx_text_width("SavanXP GPU test")) / 2;

    gfx_clear(g_background, info, gfx_rgb(18, 20, 32));
    gfx_rect(g_background, info, 0, 0, (int)info->width, 36, gfx_rgb(34, 52, 78));
    gfx_hline(g_background, info, 0, 36, (int)info->width, gfx_rgb(84, 132, 192));
    gfx_blit_text(g_background, info, title_x, title_y, "SavanXP GPU test", gfx_rgb(236, 243, 255));

    gfx_rect(g_background, info, 24, 52, panel_width, panel_height, gfx_rgb(23, 31, 46));
    gfx_frame(g_background, info, 24, 52, panel_width, panel_height, gfx_rgb(92, 145, 208));
    gfx_blit_text(g_background, info, 40, 68, "Using /dev/gpu0 directly", gfx_rgb(214, 226, 248));
    gfx_blit_text(g_background, info, 40, 92, "ESC exits   arrows move", gfx_rgb(214, 226, 248));
}

static void draw_box(const struct savanxp_fb_info *info, int box_x, int box_y)
{
    gfx_rect(g_pixels, info, box_x, box_y, GPUTEST_BOX_SIZE, GPUTEST_BOX_SIZE, gfx_rgb(220, 120, 40));
    gfx_frame(g_pixels, info, box_x, box_y, GPUTEST_BOX_SIZE, GPUTEST_BOX_SIZE, gfx_rgb(255, 232, 192));
    gfx_rect(g_pixels, info, box_x + 18, box_y + 18, GPUTEST_BOX_SIZE - 36, GPUTEST_BOX_SIZE - 36, gfx_rgb(98, 48, 18));
}

static void copy_region(uint32_t *destination, const uint32_t *source, const struct savanxp_fb_info *info, int x, int y, int width, int height)
{
    const uint32_t pitch_pixels = gfx_stride_pixels(info);
    int row;

    if (x < 0)
    {
        width += x;
        x = 0;
    }
    if (y < 0)
    {
        height += y;
        y = 0;
    }
    if (width <= 0 || height <= 0 || x >= (int)info->width || y >= (int)info->height)
    {
        return;
    }
    if (x + width > (int)info->width)
    {
        width = (int)info->width - x;
    }
    if (y + height > (int)info->height)
    {
        height = (int)info->height - y;
    }

    for (row = 0; row < height; ++row)
    {
        memcpy(
            destination + ((size_t)(y + row) * pitch_pixels) + (uint32_t)x,
            source + ((size_t)(y + row) * pitch_pixels) + (uint32_t)x,
            (size_t)width * sizeof(uint32_t));
    }
}

static int setup_gpu(struct savanxp_gpu_info* gpu_info, struct savanxp_fb_info* fb_info, long* gpu_fd) {
    *gpu_fd = gpu_open();
    if (*gpu_fd < 0)
    {
        puts_fd(2, "gputest: /dev/gpu0 not available\n");
        return 0;
    }
    if (gpu_get_info((int)*gpu_fd, gpu_info) < 0)
    {
        puts_fd(2, "gputest: GPU_IOC_GET_INFO failed\n");
        close((int)*gpu_fd);
        return 0;
    }
    if (gpu_info->width > GPUTEST_MAX_WIDTH || gpu_info->height > GPUTEST_MAX_HEIGHT || (gpu_info->pitch / 4u) > GPUTEST_MAX_WIDTH)
    {
        puts_fd(2, "gputest: surface too large for static backbuffer\n");
        close((int)*gpu_fd);
        return 0;
    }
    if (gpu_acquire((int)*gpu_fd) < 0)
    {
        puts_fd(2, "gputest: GPU_IOC_ACQUIRE failed\n");
        close((int)*gpu_fd);
        return 0;
    }

    fb_info->width = gpu_info->width;
    fb_info->height = gpu_info->height;
    fb_info->pitch = gpu_info->pitch;
    fb_info->bpp = gpu_info->bpp;
    fb_info->buffer_size = gpu_info->buffer_size;
    return 1;
}

static int validate_stats_progress(const struct savanxp_gpu_stats* before, const struct savanxp_gpu_stats* after) {
    if (after->present_enqueued <= before->present_enqueued ||
        after->present_completed <= before->present_completed ||
        after->transfer_stage_submitted <= before->transfer_stage_submitted ||
        after->flush_stage_submitted <= before->flush_stage_submitted ||
        after->scanout_stage_submitted <= before->scanout_stage_submitted ||
        after->command_completions <= before->command_completions) {
        puts_fd(2, "gputest: GPU stats did not advance as expected\n");
        return 0;
    }
    return 1;
}

static int run_smoke_mode(void) {
    struct savanxp_gpu_info gpu_info = {0};
    struct savanxp_gpu_scanout_state scanouts = {0};
    struct savanxp_gpu_stats stats_before = {0};
    struct savanxp_gpu_stats stats_after = {0};
    struct savanxp_fb_info fb_info = {0};
    long gpu_fd;
    int previous_box_x = 120;
    int previous_box_y = 160;
    static const int positions[][2] = {
        {160, 160},
        {200, 192},
        {264, 216},
        {312, 168},
    };

    if (!setup_gpu(&gpu_info, &fb_info, &gpu_fd)) {
        return 1;
    }
    if (gpu_get_stats((int)gpu_fd, &stats_before) < 0) {
        puts_fd(2, "gputest: GPU_IOC_GET_STATS failed\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }
    if (gpu_get_scanouts((int)gpu_fd, &scanouts) < 0 || scanouts.count == 0) {
        puts_fd(2, "gputest: GPU_IOC_GET_SCANOUTS failed\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }
    if (gpu_refresh_scanouts((int)gpu_fd) < 0) {
        puts_fd(2, "gputest: GPU_IOC_REFRESH_SCANOUTS failed\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }

    draw_static_scene(&fb_info);
    memcpy(g_pixels, g_background, gpu_info.buffer_size);
    draw_box(&fb_info, previous_box_x, previous_box_y);
    if (gpu_present((int)gpu_fd, g_pixels) < 0)
    {
        puts_fd(2, "gputest: initial GPU_IOC_PRESENT failed\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }

    for (size_t index = 0; index < (sizeof(positions) / sizeof(positions[0])); ++index)
    {
        const int box_x = positions[index][0];
        const int box_y = positions[index][1];
        const int dirty_x = previous_box_x < box_x ? previous_box_x : box_x;
        const int dirty_y = previous_box_y < box_y ? previous_box_y : box_y;
        const int dirty_right = (previous_box_x + GPUTEST_BOX_SIZE) > (box_x + GPUTEST_BOX_SIZE)
            ? (previous_box_x + GPUTEST_BOX_SIZE)
            : (box_x + GPUTEST_BOX_SIZE);
        const int dirty_bottom = (previous_box_y + GPUTEST_BOX_SIZE) > (box_y + GPUTEST_BOX_SIZE)
            ? (previous_box_y + GPUTEST_BOX_SIZE)
            : (box_y + GPUTEST_BOX_SIZE);
        const int dirty_width = dirty_right - dirty_x;
        const int dirty_height = dirty_bottom - dirty_y;

        copy_region(g_pixels, g_background, &fb_info, dirty_x, dirty_y, dirty_width, dirty_height);
        draw_box(&fb_info, box_x, box_y);
        if (gpu_present_region((int)gpu_fd, g_pixels, fb_info.pitch, (uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_width, (uint32_t)dirty_height) < 0)
        {
            puts_fd(2, "gputest: GPU_IOC_PRESENT_REGION failed\n");
            gpu_release((int)gpu_fd);
            close((int)gpu_fd);
            return 1;
        }

        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    if (gpu_wait_idle((int)gpu_fd) < 0 || gpu_get_stats((int)gpu_fd, &stats_after) < 0) {
        puts_fd(2, "gputest: unable to fetch final GPU stats\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }
    if (!validate_stats_progress(&stats_before, &stats_after)) {
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }

    gpu_release((int)gpu_fd);
    close((int)gpu_fd);
    return 0;
}

int main(int argc, char** argv)
{
    struct savanxp_gpu_info gpu_info = {0};
    struct savanxp_fb_info fb_info = {0};
    struct savanxp_input_event event = {0};
    long gpu_fd;
    long input_fd;
    int box_x = 120;
    int box_y = 160;
    int previous_box_x;
    int previous_box_y;

    if (argc > 1 && strcmp(argv[1], "--smoke") == 0) {
        return run_smoke_mode();
    }

    (void)argv;
    if (!setup_gpu(&gpu_info, &fb_info, &gpu_fd)) {
        return 1;
    }

    input_fd = open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (input_fd < 0)
    {
        puts_fd(2, "gputest: /dev/input0 open failed\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }

    draw_static_scene(&fb_info);
    memcpy(g_pixels, g_background, gpu_info.buffer_size);
    draw_box(&fb_info, box_x, box_y);
    if (gpu_present((int)gpu_fd, g_pixels) < 0)
    {
        close((int)input_fd);
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        puts_fd(2, "gputest: initial GPU_IOC_PRESENT failed\n");
        return 1;
    }

    previous_box_x = box_x;
    previous_box_y = box_y;

    for (;;)
    {
        int moved = 0;

        while (read((int)input_fd, &event, sizeof(event)) == (long)sizeof(event))
        {
            if (event.type != SAVANXP_INPUT_EVENT_KEY_DOWN)
            {
                continue;
            }
            if (event.key == SAVANXP_KEY_ESC)
            {
                close((int)input_fd);
                gpu_release((int)gpu_fd);
                close((int)gpu_fd);
                return 0;
            }
            if (event.key == SAVANXP_KEY_LEFT)
            {
                box_x -= 8;
                moved = 1;
            }
            else if (event.key == SAVANXP_KEY_RIGHT)
            {
                box_x += 8;
                moved = 1;
            }
            else if (event.key == SAVANXP_KEY_UP)
            {
                box_y -= 8;
                moved = 1;
            }
            else if (event.key == SAVANXP_KEY_DOWN)
            {
                box_y += 8;
                moved = 1;
            }
        }

        if (box_x < 0)
        {
            box_x = 0;
        }
        if (box_y < 40)
        {
            box_y = 40;
        }
        if (box_x + GPUTEST_BOX_SIZE > (int)fb_info.width)
        {
            box_x = (int)fb_info.width - GPUTEST_BOX_SIZE;
        }
        if (box_y + GPUTEST_BOX_SIZE > (int)fb_info.height)
        {
            box_y = (int)fb_info.height - GPUTEST_BOX_SIZE;
        }

        if (!moved && box_x == previous_box_x && box_y == previous_box_y)
        {
            sleep_ms(8);
            continue;
        }

        {
            const int dirty_x = previous_box_x < box_x ? previous_box_x : box_x;
            const int dirty_y = previous_box_y < box_y ? previous_box_y : box_y;
            const int dirty_right = (previous_box_x + GPUTEST_BOX_SIZE) > (box_x + GPUTEST_BOX_SIZE) ? (previous_box_x + GPUTEST_BOX_SIZE) : (box_x + GPUTEST_BOX_SIZE);
            const int dirty_bottom = (previous_box_y + GPUTEST_BOX_SIZE) > (box_y + GPUTEST_BOX_SIZE) ? (previous_box_y + GPUTEST_BOX_SIZE) : (box_y + GPUTEST_BOX_SIZE);
            const int dirty_width = dirty_right - dirty_x;
            const int dirty_height = dirty_bottom - dirty_y;

            copy_region(g_pixels, g_background, &fb_info, dirty_x, dirty_y, dirty_width, dirty_height);
            draw_box(&fb_info, box_x, box_y);
            if (gpu_present_region((int)gpu_fd, g_pixels, fb_info.pitch, (uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_width, (uint32_t)dirty_height) < 0)
            {
                break;
            }
        }

        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    close((int)input_fd);
    gpu_release((int)gpu_fd);
    close((int)gpu_fd);
    puts_fd(2, "gputest: GPU_IOC_PRESENT_REGION failed\n");
    return 1;
}
