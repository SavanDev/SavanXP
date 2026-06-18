#include "libc.h"

#define GPUTEST_MAX_WIDTH 1920
#define GPUTEST_MAX_HEIGHT 1080
#define GPUTEST_BOX_SIZE 96
#define GPUTEST_SOAK_DEFAULT_ITERATIONS 96u
#define GPUTEST_SOAK_MAX_ITERATIONS 4096u

static uint32_t g_pixels[GPUTEST_MAX_WIDTH * GPUTEST_MAX_HEIGHT];
static uint32_t g_background[GPUTEST_MAX_WIDTH * GPUTEST_MAX_HEIGHT];

struct gputest_imported_surface {
    long section_fd;
    void* view;
    uint32_t surface_id;
};

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

static int parse_soak_iterations(const char* text, size_t* iterations) {
    size_t value = 0;
    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        value = value * 10u + (size_t)(text[index] - '0');
        if (value > GPUTEST_SOAK_MAX_ITERATIONS) {
            return 0;
        }
    }
    if (value == 0) {
        return 0;
    }
    *iterations = value;
    return 1;
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
    const uint64_t scanout_delta = after->scanout_stage_submitted - before->scanout_stage_submitted;

    if (after->present_enqueued <= before->present_enqueued ||
        after->present_completed <= before->present_completed ||
        after->transfer_stage_submitted <= before->transfer_stage_submitted ||
        after->flush_stage_submitted <= before->flush_stage_submitted ||
        after->command_completions <= before->command_completions ||
        after->present_end_to_end_samples <= before->present_end_to_end_samples ||
        after->present_end_to_end_max_ticks == 0) {
        puts_fd(2, "gputest: GPU stats did not advance as expected\n");
        return 0;
    }

    // Partial updates on the active front resource no longer need to burn a
    // SET_SCANOUT stage every frame; full presents may still do so.
    if (scanout_delta == 0 &&
        (after->transfer_stage_submitted - before->transfer_stage_submitted) < 2u) {
        puts_fd(2, "gputest: expected either scanout work or multiple partial transfers\n");
        return 0;
    }

    return 1;
}

static int wait_for_latest_present(int gpu_fd) {
    struct savanxp_gpu_present_timeline timeline = {0};
    struct savanxp_gpu_present_wait wait_request = {0};

    if (gpu_get_present_timeline(gpu_fd, &timeline) < 0) {
        puts_fd(2, "gputest: GPU_IOC_GET_PRESENT_TIMELINE failed\n");
        return 0;
    }
    if (timeline.submitted_sequence == 0) {
        return 1;
    }

    wait_request.target_sequence = timeline.submitted_sequence;
    if (gpu_wait_present(gpu_fd, &wait_request) < 0) {
        puts_fd(2, "gputest: GPU_IOC_WAIT_PRESENT failed\n");
        return 0;
    }
    if ((wait_request.flags & SAVANXP_GPU_PRESENT_TIMELINE_FLAG_TARGET_FAILED) != 0) {
        puts_fd(2, "gputest: present wait target failed\n");
        return 0;
    }
    return wait_request.retired_sequence >= wait_request.target_sequence;
}

static int wait_for_handle_signal(int handle, const char* label) {
    long result = wait_one(handle, 1000);
    if (result < 0) {
        eprintf("gputest: %s wait failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int reset_handle_signal(int handle, const char* label) {
    long result = event_reset(handle);
    if (result < 0) {
        eprintf("gputest: %s reset failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_handle_unsignaled(int handle, const char* label) {
    long result = wait_one(handle, 0);
    if (!result_is_error(result) || result_error_code(result) != SAVANXP_ETIMEDOUT) {
        eprintf("gputest: %s expected unsignaled handle\n", label);
        return 0;
    }
    return 1;
}

static void cleanup_gpu_test_session(long* gpu_fd, long* present_event_fd, long* scanout_event_fd) {
    if (scanout_event_fd != 0 && *scanout_event_fd >= 0) {
        close((int)*scanout_event_fd);
        *scanout_event_fd = -1;
    }
    if (present_event_fd != 0 && *present_event_fd >= 0) {
        close((int)*present_event_fd);
        *present_event_fd = -1;
    }
    if (gpu_fd != 0 && *gpu_fd >= 0) {
        gpu_release((int)*gpu_fd);
        close((int)*gpu_fd);
        *gpu_fd = -1;
    }
}

static void cleanup_imported_test_surface(int gpu_fd, struct gputest_imported_surface* imported) {
    if (imported == 0) {
        return;
    }
    if (imported->surface_id != 0 && gpu_fd >= 0) {
        (void)gpu_release_surface(gpu_fd, imported->surface_id);
    }
    if (imported->view != 0 && !result_is_error((long)imported->view)) {
        (void)unmap_view(imported->view);
    }
    if (imported->section_fd >= 0) {
        close((int)imported->section_fd);
    }
    imported->section_fd = -1;
    imported->view = 0;
    imported->surface_id = 0;
}

static void cleanup_soak_session(long* gpu_fd, long* present_event_fd, long* scanout_event_fd, struct gputest_imported_surface* imported) {
    cleanup_imported_test_surface(gpu_fd != 0 ? (int)*gpu_fd : -1, imported);
    cleanup_gpu_test_session(gpu_fd, present_event_fd, scanout_event_fd);
}

static int setup_imported_test_surface(
    int gpu_fd,
    const struct savanxp_gpu_info* gpu_info,
    const struct savanxp_fb_info* fb_info,
    struct gputest_imported_surface* imported) {
    struct savanxp_gpu_surface_import import_request = {0};
    long result = 0;

    imported->section_fd = -1;
    imported->view = 0;
    imported->surface_id = 0;

    imported->section_fd = section_create(gpu_info->buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (imported->section_fd < 0) {
        eprintf("gputest: section_create for imported surface failed (%s)\n", result_error_string(imported->section_fd));
        return 0;
    }

    imported->view = map_view((int)imported->section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)imported->view)) {
        eprintf("gputest: map_view for imported surface failed (%s)\n", result_error_string((long)imported->view));
        cleanup_imported_test_surface(gpu_fd, imported);
        return 0;
    }
    memset(imported->view, 0, gpu_info->buffer_size);

    import_request.section_handle = (int32_t)imported->section_fd;
    import_request.width = fb_info->width;
    import_request.height = fb_info->height;
    import_request.pitch = fb_info->pitch;
    import_request.bpp = fb_info->bpp;
    import_request.buffer_size = fb_info->buffer_size;
    import_request.flags = SAVANXP_GPU_SURFACE_FLAG_NONE;

    result = gpu_import_section(gpu_fd, &import_request);
    if (result < 0) {
        eprintf("gputest: GPU_IOC_IMPORT_SECTION failed (%s)\n", result_error_string(result));
        cleanup_imported_test_surface(gpu_fd, imported);
        return 0;
    }

    imported->surface_id = (uint32_t)import_request.surface_id;
    return imported->surface_id != 0;
}

static int open_gpu_test_events(int gpu_fd, long* present_event_fd, long* scanout_event_fd) {
    *present_event_fd = gpu_create_present_event(gpu_fd);
    if (*present_event_fd < 0) {
        puts_fd(2, "gputest: GPU_IOC_CREATE_PRESENT_EVENT failed\n");
        return 0;
    }

    *scanout_event_fd = gpu_create_scanout_event(gpu_fd);
    if (*scanout_event_fd < 0) {
        puts_fd(2, "gputest: GPU_IOC_CREATE_SCANOUT_EVENT failed\n");
        close((int)*present_event_fd);
        *present_event_fd = -1;
        return 0;
    }
    return 1;
}

static int refresh_scanouts_and_wait(int gpu_fd, int scanout_event_fd, const char* label) {
    if (gpu_refresh_scanouts(gpu_fd) < 0) {
        puts_fd(2, "gputest: GPU_IOC_REFRESH_SCANOUTS failed\n");
        return 0;
    }
    return wait_for_handle_signal(scanout_event_fd, label) &&
        reset_handle_signal(scanout_event_fd, label) &&
        expect_handle_unsignaled(scanout_event_fd, label);
}

static int wait_present_event_and_retire(int gpu_fd, int present_event_fd, const char* label) {
    if (!wait_for_handle_signal(present_event_fd, label)) {
        return 0;
    }
    if (!reset_handle_signal(present_event_fd, label)) {
        return 0;
    }
    if (!expect_handle_unsignaled(present_event_fd, label)) {
        return 0;
    }
    if (!wait_for_latest_present(gpu_fd)) {
        eprintf("gputest: %s retire wait failed\n", label);
        return 0;
    }
    return 1;
}

static int present_imported_test_surface(
    int gpu_fd,
    int present_event_fd,
    const struct savanxp_gpu_info* gpu_info,
    const struct gputest_imported_surface* imported,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    struct savanxp_gpu_present_timeline timeline = {0};
    struct savanxp_gpu_surface_present_batch batch = {0};

    if (imported == 0 || imported->surface_id == 0 || imported->view == 0) {
        return 0;
    }
    if (gpu_get_present_timeline(gpu_fd, &timeline) < 0) {
        puts_fd(2, "gputest: imported GPU_IOC_GET_PRESENT_TIMELINE failed\n");
        return 0;
    }

    memcpy(imported->view, g_pixels, gpu_info->buffer_size);
    batch.surface_id = imported->surface_id;
    batch.rect_count = 1;
    batch.flags = SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_NONE;
    batch.present_cookie = timeline.submitted_sequence + 1u;
    batch.rects[0].x = x;
    batch.rects[0].y = y;
    batch.rects[0].width = width;
    batch.rects[0].height = height;

    if (gpu_present_surface_batch(gpu_fd, &batch) < 0) {
        puts_fd(2, "gputest: GPU_IOC_PRESENT_SURFACE_BATCH failed\n");
        return 0;
    }
    return wait_present_event_and_retire(gpu_fd, present_event_fd, "present event soak imported");
}

static int run_soak_mode(size_t iteration_count) {
    struct savanxp_gpu_info gpu_info = {0};
    struct savanxp_gpu_stats stats_before = {0};
    struct savanxp_gpu_stats stats_after = {0};
    struct savanxp_fb_info fb_info = {0};
    struct gputest_imported_surface imported = {-1, 0, 0};
    long gpu_fd = -1;
    long present_event_fd = -1;
    long scanout_event_fd = -1;
    int previous_box_x = 96;
    int previous_box_y = 144;
    int usable_width = 0;
    int usable_height = 0;

    puts("SOAK START\n");

    if (!setup_gpu(&gpu_info, &fb_info, &gpu_fd)) {
        return 1;
    }
    if (gpu_get_stats((int)gpu_fd, &stats_before) < 0) {
        puts_fd(2, "gputest: GPU_IOC_GET_STATS failed\n");
        cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
        return 1;
    }
    if (!open_gpu_test_events((int)gpu_fd, &present_event_fd, &scanout_event_fd) ||
        !refresh_scanouts_and_wait((int)gpu_fd, (int)scanout_event_fd, "scanout event soak initial")) {
        cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
        return 1;
    }
    if (!setup_imported_test_surface((int)gpu_fd, &gpu_info, &fb_info, &imported)) {
        cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
        return 1;
    }
    usable_width = (int)fb_info.width - GPUTEST_BOX_SIZE - 32;
    usable_height = (int)fb_info.height - GPUTEST_BOX_SIZE - 56;

    draw_static_scene(&fb_info);
    memcpy(g_pixels, g_background, gpu_info.buffer_size);
    draw_box(&fb_info, previous_box_x, previous_box_y);
    if (gpu_present((int)gpu_fd, g_pixels) < 0 ||
        !wait_present_event_and_retire((int)gpu_fd, (int)present_event_fd, "present event soak initial")) {
        puts_fd(2, "gputest: initial soak present failed\n");
        cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
        return 1;
    }

    for (size_t iteration = 0; iteration < iteration_count; ++iteration) {
        const int max_x = usable_width > 0 ? usable_width : 1;
        const int max_y = usable_height > 0 ? usable_height : 1;
        const int box_x = 32 + (int)((iteration * 37u) % (uint32_t)max_x);
        const int box_y = 56 + (int)((iteration * 23u) % (uint32_t)max_y);
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

        if ((iteration % 11u) == 5u) {
            if (!present_imported_test_surface(
                    (int)gpu_fd,
                    (int)present_event_fd,
                    &gpu_info,
                    &imported,
                    (uint32_t)dirty_x,
                    (uint32_t)dirty_y,
                    (uint32_t)dirty_width,
                    (uint32_t)dirty_height)) {
                puts_fd(2, "gputest: soak imported present failed\n");
                cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
                return 1;
            }
        } else if ((iteration % 7u) == 0u) {
            if (gpu_present((int)gpu_fd, g_pixels) < 0 ||
                !wait_present_event_and_retire((int)gpu_fd, (int)present_event_fd, "present event soak full")) {
                puts_fd(2, "gputest: soak full present failed\n");
                cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
                return 1;
            }
        } else {
            if (gpu_present_region((int)gpu_fd, g_pixels, fb_info.pitch, (uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_width, (uint32_t)dirty_height) < 0) {
                puts_fd(2, "gputest: soak GPU_IOC_PRESENT_REGION failed\n");
                cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
                return 1;
            }
            if (!wait_present_event_and_retire((int)gpu_fd, (int)present_event_fd, "present event soak partial")) {
                puts_fd(2, "gputest: soak partial present failed\n");
                cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
                return 1;
            }
        }

        if ((iteration % 5u) == 4u) {
            if (!refresh_scanouts_and_wait((int)gpu_fd, (int)scanout_event_fd, "scanout event soak refresh")) {
                puts_fd(2, "gputest: soak refresh failed\n");
                cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
                return 1;
            }
        }

        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    if (gpu_wait_idle((int)gpu_fd) < 0 || gpu_get_stats((int)gpu_fd, &stats_after) < 0) {
        puts_fd(2, "gputest: soak final stats failed\n");
        cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
        return 1;
    }
    if (!validate_stats_progress(&stats_before, &stats_after)) {
        cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
        return 1;
    }

    cleanup_soak_session(&gpu_fd, &present_event_fd, &scanout_event_fd, &imported);
    puts("SOAK PASS\n");
    return 0;
}

static int run_smoke_mode(void) {
    struct savanxp_gpu_info gpu_info = {0};
    struct savanxp_gpu_scanout_state scanouts = {0};
    struct savanxp_gpu_stats stats_before = {0};
    struct savanxp_gpu_stats stats_after = {0};
    struct savanxp_fb_info fb_info = {0};
    long gpu_fd;
    long present_event_fd = -1;
    long scanout_event_fd = -1;
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
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
        return 1;
    }
    if (!open_gpu_test_events((int)gpu_fd, &present_event_fd, &scanout_event_fd)) {
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
        return 1;
    }
    if (!refresh_scanouts_and_wait((int)gpu_fd, (int)scanout_event_fd, "scanout event initial")) {
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
        return 1;
    }

    draw_static_scene(&fb_info);
    memcpy(g_pixels, g_background, gpu_info.buffer_size);
    draw_box(&fb_info, previous_box_x, previous_box_y);
    if (gpu_present((int)gpu_fd, g_pixels) < 0)
    {
        puts_fd(2, "gputest: initial GPU_IOC_PRESENT failed\n");
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
        return 1;
    }
    if (!wait_present_event_and_retire((int)gpu_fd, (int)present_event_fd, "present event initial")) {
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
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
            cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
            return 1;
        }
        if (!wait_present_event_and_retire((int)gpu_fd, (int)present_event_fd, "present event partial")) {
            cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
            return 1;
        }

        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    for (size_t iteration = 0; iteration < 16; ++iteration) {
        const int box_x = 160 + (int)(iteration * 12);
        const int box_y = 160 + (int)((iteration % 3) * 18);
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
        if (gpu_present_region((int)gpu_fd, g_pixels, fb_info.pitch, (uint32_t)dirty_x, (uint32_t)dirty_y, (uint32_t)dirty_width, (uint32_t)dirty_height) < 0) {
            puts_fd(2, "gputest: stress GPU_IOC_PRESENT_REGION failed\n");
            cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
            return 1;
        }
        if (!wait_present_event_and_retire((int)gpu_fd, (int)present_event_fd, "present event stress")) {
            cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
            return 1;
        }
        if ((iteration % 2u) == 1u) {
            if (!refresh_scanouts_and_wait((int)gpu_fd, (int)scanout_event_fd, "scanout event refresh")) {
                puts_fd(2, "gputest: scanout refresh stress failed\n");
                cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
                return 1;
            }
        }
        previous_box_x = box_x;
        previous_box_y = box_y;
    }

    if (gpu_wait_idle((int)gpu_fd) < 0 || gpu_get_stats((int)gpu_fd, &stats_after) < 0) {
        puts_fd(2, "gputest: unable to fetch final GPU stats\n");
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
        return 1;
    }
    if (!validate_stats_progress(&stats_before, &stats_after)) {
        cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
        return 1;
    }

    cleanup_gpu_test_session(&gpu_fd, &present_event_fd, &scanout_event_fd);
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
    if (argc > 1 && strcmp(argv[1], "--soak") == 0) {
        size_t soak_iterations = GPUTEST_SOAK_DEFAULT_ITERATIONS;
        if (argc > 2 && !parse_soak_iterations(argv[2], &soak_iterations)) {
            eprintf("gputest: invalid soak iteration count '%s'\n", argv[2]);
            return 1;
        }
        return run_soak_mode(soak_iterations);
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
