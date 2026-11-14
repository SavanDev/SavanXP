#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compositor_protocol.h"
#include "sxn/sxn.h"
#include "compositord_scene.h"
/* After the SavanXP headers: it uses the cursor shape enum. */
#include "cursor_catalog.h"
#include "shadow.h"
#include "effects_settings.h"

/*
 * compositord is a native process (EI_OSABI 0x53) and a session service
 * (docs/desktop/DESKTOP_ARCHITECTURE.md#composition-in-compositord): sessiond
 * starts it before windowd, it takes the GPU and allocates the display memory
 * at startup, and serves one window manager through its port
 * (compositor_protocol.h). It composes the scene the WM commits
 * (compositord_scene.c) and presents it. It speaks only SXN: the GPU is a device it opens by
 * name, every GPU request is the control code the POSIX side sends through
 * ioctl. Nothing here may call into the POSIX runtime: from a native process
 * those calls are ENOSYS.
 */

/* The GPU requests, as native device control. Same codes and structs as the
 * POSIX gpu_* wrappers in the SDK. */
static long gpu_open(void)
{
    return sxn_open_device("gpu0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
}

static long gpu_control(int fd, unsigned long code, const void *argument)
{
    return sxn_device_control(fd, code, (unsigned long)argument);
}

#define gpu_get_info(fd, info) gpu_control((fd), GPU_IOC_GET_INFO, (info))
#define gpu_get_connector_properties(fd, properties) \
    gpu_control((fd), GPU_IOC_GET_CONNECTOR_PROPERTIES, (properties))
#define gpu_acquire(fd) gpu_control((fd), GPU_IOC_ACQUIRE, 0)
#define gpu_release(fd) gpu_control((fd), GPU_IOC_RELEASE, 0)
#define gpu_set_mode(fd, mode) gpu_control((fd), GPU_IOC_SET_MODE, (mode))
#define gpu_import_section(fd, request) gpu_control((fd), GPU_IOC_IMPORT_SECTION, (request))
#define gpu_release_surface(fd, id) sxn_device_control((fd), GPU_IOC_RELEASE_SURFACE, (unsigned long)(id))
#define gpu_create_present_event(fd) gpu_control((fd), GPU_IOC_CREATE_PRESENT_EVENT, 0)
#define gpu_get_present_timeline(fd, timeline) gpu_control((fd), GPU_IOC_GET_PRESENT_TIMELINE, (timeline))
#define gpu_present_surface_batch(fd, batch) gpu_control((fd), GPU_IOC_PRESENT_SURFACE_BATCH, (batch))
#define gpu_wait_present(fd, request) gpu_control((fd), GPU_IOC_WAIT_PRESENT, (request))
#define gpu_set_cursor(fd, image) gpu_control((fd), GPU_IOC_SET_CURSOR, (image))
#define gpu_move_cursor(fd, position) gpu_control((fd), GPU_IOC_MOVE_CURSOR, (position))

static unsigned long long monotonic_ns(void)
{
    return (unsigned long long)sxn_clock_ns();
}

/* Time since `mark`, 0 if the clock is not calibrated (monotonic_ns returns
 * 0) or went backwards: the consumer reads 0 as "no sample". */
static uint64_t elapsed_since(unsigned long long mark)
{
    unsigned long long now = monotonic_ns();
    if (mark == 0ull || now <= mark)
    {
        return 0ull;
    }
    return (uint64_t)(now - mark);
}

struct compositor_state
{
    int gpu_fd;
    int present_event_fd;
    uint32_t display_surface_id;
    struct savanxp_fb_info display_info;
    struct savanxp_gpu_info gpu_info;
    int initialized;
    /* The cursor plane is in use; without one the scene draws the cursor. */
    int hw_cursor;
    struct scene_cursor cursor_applied;
    /* compositord's own mapping of the display section, which it composes
     * into. */
    uint32_t *display_view;
    /* The last present not yet on the screen (its timeline cookie), 0 when
     * none: composing waits for it, so a frame never changes under a transfer
     * in progress. */
    uint64_t in_flight;
    /* What the request being served measured (savanxp_compositor_reply). */
    struct savanxp_compositor_service_timing timing;
    uint64_t damage_px;
    uint64_t bounds_px;
    uint32_t damage_rects;
    /* The session's submit event, handed to every client with its welcome:
     * set after a frame, it wakes compositord, never windowd. */
    long frame_event;
    /* The display memory, allocated at startup for the largest mode, and its
     * size: a mode whose buffer does not fit is refused. */
    long display_section;
    uint64_t display_capacity;
    /* The largest mode the section holds, for the WM (the INIT reply). */
    uint32_t max_width;
    uint32_t max_height;
};

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        (void)sxn_close(*fd);
        *fd = -1;
    }
}

static int read_exact(int fd, void *buffer, size_t size)
{
    unsigned char *cursor = (unsigned char *)buffer;
    size_t offset = 0;

    while (offset < size)
    {
        long result = sxn_read(fd, cursor + offset, size - offset);
        if (result < 0)
        {
            return (int)result;
        }
        if (result == 0)
        {
            return -SAVANXP_EPIPE;
        }
        offset += (size_t)result;
    }
    return 0;
}

static int write_exact(int fd, const void *buffer, size_t size)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t offset = 0;

    while (offset < size)
    {
        long result = sxn_write(fd, cursor + offset, size - offset);
        if (result < 0)
        {
            return (int)result;
        }
        if (result == 0)
        {
            return -SAVANXP_EPIPE;
        }
        offset += (size_t)result;
    }
    return 0;
}

static void compositor_state_init(struct compositor_state *state)
{
    memset(state, 0, sizeof(*state));
    state->gpu_fd = -1;
    state->present_event_fd = -1;
    state->display_section = -1;
    state->frame_event = -1;
}

static void compositor_state_close(struct compositor_state *state)
{
    if (state == 0)
    {
        return;
    }

    if (state->display_surface_id != 0 && state->gpu_fd >= 0)
    {
        (void)gpu_release_surface(state->gpu_fd, state->display_surface_id);
        state->display_surface_id = 0;
    }
    if (state->gpu_fd >= 0)
    {
        (void)gpu_release(state->gpu_fd);
        close_fd_if_needed(&state->gpu_fd);
    }
    close_fd_if_needed(&state->present_event_fd);
    if (state->frame_event >= 0)
    {
        (void)sxn_close(state->frame_event);
        state->frame_event = -1;
    }
    scene_reset();
    if (state->display_view != 0)
    {
        (void)sxn_unmap_view((uint64_t)(uintptr_t)state->display_view);
        state->display_view = 0;
    }
    if (state->display_section >= 0)
    {
        (void)sxn_close(state->display_section);
        state->display_section = -1;
    }
    state->initialized = 0;
    state->hw_cursor = 0;
}

/* (Re)imports the display section as the scanout surface with the geometry in
 * state->display_info. It is always the same section, sized for the largest
 * mode, so a smaller mode fits without reallocating anything. */
static int compositor_import_display(struct compositor_state *state)
{
    struct savanxp_gpu_surface_import import_request;
    long result;

    if (state->display_section < 0 ||
        (uint64_t)state->display_info.buffer_size > state->display_capacity)
    {
        return -SAVANXP_EINVAL;
    }
    memset(&import_request, 0, sizeof(import_request));
    import_request.section_handle = (int32_t)state->display_section;
    import_request.width = state->display_info.width;
    import_request.height = state->display_info.height;
    import_request.pitch = state->display_info.pitch;
    import_request.bpp = state->display_info.bpp;
    import_request.buffer_size = state->display_info.buffer_size;
    import_request.flags = SAVANXP_GPU_SURFACE_FLAG_SCANOUT;
    import_request.pixels_offset = 0;
    result = gpu_import_section(state->gpu_fd, &import_request);
    if (result < 0)
    {
        return (int)result;
    }

    state->display_surface_id = (uint32_t)import_request.surface_id;
    return 0;
}

static int finish_present(struct compositor_state *state, int wait);
static void setup_cursor(struct compositor_state *state);

/* Programs a mode and leaves the display section imported against it. All or
 * nothing: on failure no surface is left imported, and the caller decides
 * whether to restore the previous mode. No surface may be live (SET_MODE
 * rebuilds the scanout). */
static int compositor_apply_mode(struct compositor_state *state, uint32_t width, uint32_t height)
{
    struct savanxp_gpu_mode mode;
    long result;

    memset(&mode, 0, sizeof(mode));
    mode.width = width;
    mode.height = height;
    mode.bpp = 32u;
    result = gpu_set_mode(state->gpu_fd, &mode);
    if (result < 0)
    {
        return (int)result;
    }

    state->display_info.width = mode.width;
    state->display_info.height = mode.height;
    state->display_info.pitch = mode.pitch;
    state->display_info.bpp = mode.bpp;
    state->display_info.buffer_size = mode.buffer_size;
    state->gpu_info.width = mode.width;
    state->gpu_info.height = mode.height;
    state->gpu_info.pitch = mode.pitch;
    state->gpu_info.bpp = mode.bpp;
    state->gpu_info.buffer_size = mode.buffer_size;

    result = compositor_import_display(state);
    if (result == 0 && state->display_view != 0)
    {
        scene_set_display(state->display_view, &state->display_info);
    }
    return (int)result;
}

/* Cambio de modo en caliente: suelta la superficie importada (apunta al modo
 * viejo), reprograma y reimporta. Si algo falla repone el modo anterior, para
 * no dejar al shell sin scanout donde presentar. */
static int compositor_set_mode(
    struct compositor_state *state,
    const struct savanxp_fb_info *requested_info,
    struct savanxp_compositor_reply *reply)
{
    struct savanxp_fb_info previous_info;
    int result;

    if (state == 0 || requested_info == 0 || reply == 0 || !state->initialized)
    {
        return -SAVANXP_ENODEV;
    }
    if (requested_info->width == 0 || requested_info->height == 0)
    {
        return -SAVANXP_EINVAL;
    }
    if (requested_info->width == state->display_info.width &&
        requested_info->height == state->display_info.height)
    {
        reply->fb_info = state->display_info;
        reply->gpu_info = state->gpu_info;
        return 0;
    }

    previous_info = state->display_info;
    /* The scanout is rebuilt: nothing may still be reading the old one. */
    (void)finish_present(state, 1);
    if (state->display_surface_id != 0)
    {
        (void)gpu_release_surface(state->gpu_fd, state->display_surface_id);
        state->display_surface_id = 0;
    }

    result = compositor_apply_mode(state, requested_info->width, requested_info->height);
    if (result < 0)
    {
        if (state->display_surface_id != 0)
        {
            (void)gpu_release_surface(state->gpu_fd, state->display_surface_id);
            state->display_surface_id = 0;
        }
        state->display_info = previous_info;
        (void)compositor_apply_mode(state, previous_info.width, previous_info.height);
        return result;
    }

    reply->fb_info = state->display_info;
    reply->gpu_info = state->gpu_info;
    return 0;
}

/*
 * Takes the GPU and the display at startup, before the port exists: a
 * compositord that cannot drive the display never offers itself to a WM. The
 * mode is the one the firmware left (the native one). The display section is
 * sized for the largest mode this session may switch to: the boot mode, or the
 * adapter's maximum up to SAVANXP_DISPLAY_SESSION_MAX_*, whichever is larger.
 */
static void choose_max_mode(struct compositor_state *state, uint32_t boot_width, uint32_t boot_height)
{
    struct savanxp_gpu_connector_properties properties;
    uint32_t width = boot_width;
    uint32_t height = boot_height;

    memset(&properties, 0, sizeof(properties));
    if (gpu_get_connector_properties(state->gpu_fd, &properties) == 0 &&
        (properties.flags & SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING) != 0u)
    {
        uint32_t adapter_width = properties.max_width < SAVANXP_DISPLAY_SESSION_MAX_WIDTH
            ? properties.max_width
            : SAVANXP_DISPLAY_SESSION_MAX_WIDTH;
        uint32_t adapter_height = properties.max_height < SAVANXP_DISPLAY_SESSION_MAX_HEIGHT
            ? properties.max_height
            : SAVANXP_DISPLAY_SESSION_MAX_HEIGHT;

        if (adapter_width > width)
        {
            width = adapter_width;
        }
        if (adapter_height > height)
        {
            height = adapter_height;
        }
    }
    state->max_width = width;
    state->max_height = height;
}

static int compositor_startup(struct compositor_state *state)
{
    struct sxn_gfx_info boot;
    uint32_t width = 1024u;
    uint32_t height = 768u;
    long result;

    if (sxn_gfx_info(&boot) == 0 && boot.width != 0 && boot.height != 0)
    {
        width = boot.width;
        height = boot.height;
    }

    state->gpu_fd = (int)gpu_open();
    if (state->gpu_fd < 0)
    {
        return state->gpu_fd;
    }
    result = gpu_get_info(state->gpu_fd, &state->gpu_info);
    if (result < 0)
    {
        return (int)result;
    }
    result = gpu_acquire(state->gpu_fd);
    if (result < 0)
    {
        return (int)result;
    }

    choose_max_mode(state, width, height);
    state->display_capacity = ((uint64_t)state->max_width * 4u * state->max_height) + (4ull * 1024ull * 1024ull);
    state->display_section = sxn_section_create(state->display_capacity, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (state->display_section < 0)
    {
        return (int)state->display_section;
    }

    {
        long view = sxn_map_view(state->display_section, SXN_SECTION_READ | SXN_SECTION_WRITE);

        if (view < 0 && view > -4096)
        {
            return (int)view;
        }
        state->display_view = (uint32_t *)(uintptr_t)view;
    }

    state->present_event_fd = (int)gpu_create_present_event(state->gpu_fd);
    if (state->present_event_fd < 0)
    {
        state->present_event_fd = -1;
    }
    state->frame_event = sxn_event_create(SXN_EVENT_MANUAL_RESET);
    if (state->frame_event < 0)
    {
        return (int)state->frame_event;
    }

    result = compositor_apply_mode(state, width, height);
    if (result < 0)
    {
        return (int)result;
    }
    setup_cursor(state);
    state->initialized = 1;
    return 0;
}

/* INIT: the WM's scene section (queued before the request), and in return
 * what compositord set up at startup, the display section and the frame
 * event. */
static int compositor_init(
    struct compositor_state *state,
    int request_fd,
    int reply_fd,
    struct savanxp_compositor_reply *reply)
{
    long scene_section;

    if (!state->initialized)
    {
        return -SAVANXP_ENODEV;
    }
    scene_section = sxn_pipe_receive_handle(request_fd);
    if (scene_section < 0 || scene_map_scene_section(scene_section) < 0)
    {
        return -SAVANXP_EINVAL;
    }
    if (sxn_pipe_send_handle(reply_fd, state->display_section) < 0 ||
        sxn_pipe_send_handle(reply_fd, state->frame_event) < 0)
    {
        return -SAVANXP_EIO;
    }
    reply->fb_info = state->display_info;
    reply->gpu_info = state->gpu_info;
    reply->max_width = state->max_width;
    reply->max_height = state->max_height;
    return 0;
}

/* The last present is on the screen, or (wait = 0) not yet. With `wait` it
 * blocks until it is. 1 when nothing is in flight any more. */
static int finish_present(struct compositor_state *state, int wait)
{
    struct savanxp_gpu_present_timeline timeline;
    struct savanxp_gpu_present_wait wait_request;

    if (state->in_flight == 0)
    {
        return 1;
    }
    if (!wait)
    {
        memset(&timeline, 0, sizeof(timeline));
        if (gpu_get_present_timeline(state->gpu_fd, &timeline) < 0 ||
            timeline.retired_sequence < state->in_flight)
        {
            return 0;
        }
    }
    memset(&wait_request, 0, sizeof(wait_request));
    wait_request.target_sequence = state->in_flight;
    (void)gpu_wait_present(state->gpu_fd, &wait_request);
    if (state->present_event_fd >= 0)
    {
        (void)sxn_event_reset(state->present_event_fd);
    }
    state->in_flight = 0;
    scene_retire();
    return 1;
}

/* Hands a composed region to the GPU, in batches of its rectangle limit. */
static int present_region(struct compositor_state *state, const struct sx_region *region)
{
    struct savanxp_gpu_surface_present_batch batch;
    size_t count = sx_region_rect_count(region);
    size_t index = 0;
    struct sx_rect bounds = sx_region_bounds(region);

    state->bounds_px += (uint64_t)bounds.width * (uint64_t)bounds.height;
    while (index < count)
    {
        struct savanxp_gpu_present_timeline timeline;
        unsigned long long mark;
        long result;

        memset(&batch, 0, sizeof(batch));
        while (index < count && batch.rect_count < SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS)
        {
            struct sx_rect rect;

            if (sx_region_rect_at(region, index++, &rect) && rect.width > 0 && rect.height > 0)
            {
                batch.rects[batch.rect_count].x = (uint32_t)rect.x;
                batch.rects[batch.rect_count].y = (uint32_t)rect.y;
                batch.rects[batch.rect_count].width = (uint32_t)rect.width;
                batch.rects[batch.rect_count].height = (uint32_t)rect.height;
                batch.rect_count += 1u;
                state->damage_px += (uint64_t)rect.width * (uint64_t)rect.height;
                state->damage_rects += 1u;
            }
        }
        if (batch.rect_count == 0)
        {
            continue;
        }

        memset(&timeline, 0, sizeof(timeline));
        mark = monotonic_ns();
        result = gpu_get_present_timeline(state->gpu_fd, &timeline);
        state->timing.timeline_ns += elapsed_since(mark);
        if (result < 0)
        {
            return (int)result;
        }
        batch.surface_id = state->display_surface_id;
        batch.present_cookie = timeline.submitted_sequence + 1u;
        mark = monotonic_ns();
        result = gpu_present_surface_batch(state->gpu_fd, &batch);
        state->timing.gpu_ns += elapsed_since(mark);
        if (result < 0)
        {
            return (int)result;
        }
        state->in_flight = batch.present_cookie;
    }
    return 0;
}

/*
 * compositord-stats (docs/desktop/GRAPHICS_PERF.md): one line on /dev/serial
 * every two seconds in which a frame was presented. Never on stderr, which the
 * kernel also paints on the framebuffer being measured; without the serial
 * device it is a no-op.
 */
#define COMPOSITOR_STATS_INTERVAL_NS 2000000000ull

struct compositor_stats
{
    long serial;
    unsigned long long window_begin_ns;
    unsigned long frames;
    uint64_t compose_ns_total;
    uint64_t compose_ns_max;
    uint64_t present_ns_total;
    uint64_t present_ns_max;
    uint64_t gpu_ns_total;
    uint64_t timeline_ns_total;
    uint64_t damage_px_total;
    uint64_t bounds_px_total;
    uint64_t rects_total;
};

static struct compositor_stats g_stats = {-1, 0ull, 0ul, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static void stats_open(void)
{
    g_stats.serial = sxn_open_device("serial", SXN_DEVICE_WRITE);
    g_stats.window_begin_ns = monotonic_ns();
}

static void stats_frame(uint64_t compose_ns, uint64_t gpu_ns, uint64_t timeline_ns, uint64_t damage_px, uint64_t bounds_px, uint64_t rects)
{
    uint64_t present_ns = gpu_ns + timeline_ns;

    if (g_stats.serial < 0 || compose_ns == 0)
    {
        return; /* no clock yet: not a sample */
    }
    g_stats.frames += 1u;
    g_stats.compose_ns_total += compose_ns;
    g_stats.present_ns_total += present_ns;
    if (compose_ns > g_stats.compose_ns_max)
    {
        g_stats.compose_ns_max = compose_ns;
    }
    if (present_ns > g_stats.present_ns_max)
    {
        g_stats.present_ns_max = present_ns;
    }
    g_stats.gpu_ns_total += gpu_ns;
    g_stats.timeline_ns_total += timeline_ns;
    g_stats.damage_px_total += damage_px;
    g_stats.bounds_px_total += bounds_px;
    g_stats.rects_total += rects;
}

static void stats_report(void)
{
    char line[320];
    unsigned long long now = monotonic_ns();
    unsigned long long elapsed = now > g_stats.window_begin_ns ? now - g_stats.window_begin_ns : 0ull;
    unsigned long frames = g_stats.frames;
    unsigned long fps_tenths;
    unsigned long rects_tenths;
    int length;

    if (g_stats.serial < 0 || elapsed < COMPOSITOR_STATS_INTERVAL_NS)
    {
        return;
    }
    if (frames != 0)
    {
        fps_tenths = (unsigned long)(((unsigned long long)frames * 10000000000ull) / elapsed);
        rects_tenths = (unsigned long)((g_stats.rects_total * 10u) / frames);
        length = snprintf(
            line, sizeof(line),
            "compositord-stats: window_ms=%lu frames=%lu fps=%lu.%lu compose_us=%lu/%lu present_us=%lu/%lu "
            "gpu_us=%lu tl_us=%lu damage_px=%lu bounds_px=%lu rects=%lu.%lu\n",
            (unsigned long)(elapsed / 1000000ull),
            frames,
            fps_tenths / 10ul, fps_tenths % 10ul,
            (unsigned long)(g_stats.compose_ns_total / frames / 1000u),
            (unsigned long)(g_stats.compose_ns_max / 1000u),
            (unsigned long)(g_stats.present_ns_total / frames / 1000u),
            (unsigned long)(g_stats.present_ns_max / 1000u),
            (unsigned long)(g_stats.gpu_ns_total / frames / 1000u),
            (unsigned long)(g_stats.timeline_ns_total / frames / 1000u),
            (unsigned long)(g_stats.damage_px_total / frames),
            (unsigned long)(g_stats.bounds_px_total / frames),
            rects_tenths / 10ul, rects_tenths % 10ul);
        if (length > 0)
        {
            (void)sxn_write(g_stats.serial, line, (unsigned long)length);
        }
    }
    memset(&g_stats.frames, 0, sizeof(g_stats) - offsetof(struct compositor_stats, frames));
    g_stats.window_begin_ns = now;
}

/* Composes and presents what is damaged, once the last present is on the
 * screen. `wait`: block for that, and for this frame to be on the screen too
 * (a COMMIT_WAIT). */
static int produce_frame(struct compositor_state *state, int wait)
{
    static struct sx_region presented;
    unsigned long long mark;
    uint64_t compose_ns;
    uint64_t gpu_before = state->timing.gpu_ns;
    uint64_t timeline_before = state->timing.timeline_ns;
    uint64_t damage_before = state->damage_px;
    uint64_t bounds_before = state->bounds_px;
    uint32_t rects_before = state->damage_rects;
    int result;

    if (!finish_present(state, wait))
    {
        return 0;
    }
    if (!scene_has_damage())
    {
        return 0;
    }
    mark = monotonic_ns();
    scene_compose(&presented);
    compose_ns = elapsed_since(mark);
    state->timing.compose_ns += compose_ns;
    if (sx_region_is_empty(&presented))
    {
        return 0;
    }
    result = present_region(state, &presented);
    if (result < 0)
    {
        return result;
    }
    stats_frame(
        compose_ns,
        state->timing.gpu_ns - gpu_before,
        state->timing.timeline_ns - timeline_before,
        state->damage_px - damage_before,
        state->bounds_px - bounds_before,
        state->damage_rects - rects_before);
    if (wait)
    {
        (void)finish_present(state, 1);
    }
    return 0;
}

static int compositor_get_timeline(
    struct compositor_state *state,
    struct savanxp_compositor_reply *reply)
{
    long result;

    if (state == 0 || reply == 0 || !state->initialized)
    {
        return -SAVANXP_ENODEV;
    }

    result = gpu_get_present_timeline(state->gpu_fd, &reply->timeline);
    return result < 0 ? (int)result : 0;
}

static uint32_t g_effects = SXN_EFFECTS_ALL;
static long g_effects_save_status;

static void effects_load(void) {
    char text[256];
    long fd = sxn_open_file(SXN_EFFECTS_PATH, SXN_FILE_READ);
    if (fd >= 0) {
        long count = sxn_read(fd, text, sizeof(text) - 1);
        (void)sxn_close(fd);
        if (count >= 0) { text[count] = 0; g_effects = effects_settings_parse(text); }
        else g_effects_save_status = count;
    } else if (fd != -SAVANXP_ENOENT) g_effects_save_status = fd;
    scene_set_effects(g_effects);
}

static int upload_cursor_shape(struct compositor_state *state, uint32_t shape)
{
    static uint32_t cursor_pixels[64 * 64];
    struct savanxp_gpu_cursor_image image;
    const struct cursor_image *asset;
    if (state == 0 || (state->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) == 0)
    {
        return -SAVANXP_ENOSYS;
    }
    if (shape >= (uint32_t)SAVANXP_CURSOR_SHAPE_COUNT)
    {
        shape = SAVANXP_CURSOR_ARROW;
    }
    asset = cursor_catalog_get(shape);

    if (cursor_shadow_pack(asset,
            (g_effects & SXN_EFFECTS_CURSOR) ? cursor_catalog_shadow(shape) : 0, cursor_pixels) < 0)
        return -SAVANXP_E2BIG;

    image.pixels = (uint64_t)(unsigned long)cursor_pixels;
    image.width = 64;
    image.height = 64;
    image.pitch = 64u * sizeof(uint32_t);
    image.hotspot_x = (uint32_t)asset->hotspot_x + ((g_effects & SXN_EFFECTS_CURSOR) ? 3 : 0);
    image.hotspot_y = (uint32_t)asset->hotspot_y + ((g_effects & SXN_EFFECTS_CURSOR) ? 3 : 0);
    {
        long result = gpu_set_cursor(state->gpu_fd, &image);
        if (result < 0) {
            char log[96];
            snprintf(log, sizeof(log), "compositord: cursor hardware upload shape=%u failed: %ld", shape, result);
            (void)sxn_log(log);
        }
        return (int)result;
    }
}

/* The cursor of the last commit on the hardware plane: the shape when it
 * changed, then where and whether it shows. Without a plane the scene draws
 * it, and this does nothing. */
static void apply_cursor(struct compositor_state *state)
{
    struct scene_cursor cursor = scene_current_cursor();
    struct savanxp_gpu_cursor_position position;

    if (!state->hw_cursor)
    {
        return;
    }
    if (cursor.shape != state->cursor_applied.shape)
    {
        /* Hide before replacing the resource: a failed upload may discard
         * it, leaving no image that a later MOVE_CURSOR could hide. */
        memset(&position, 0, sizeof(position));
        if (gpu_move_cursor(state->gpu_fd, &position) < 0) {
            (void)sxn_log("compositord: cannot hide cursor plane before upload");
            sxn_exit(1);
        }
        if (upload_cursor_shape(state, (uint32_t)cursor.shape) < 0) {
            state->hw_cursor = 0;
            scene_set_software_cursor(1);
            (void)sxn_log("compositord: cursor upload failed; using software");
            return;
        }
    }
    if (cursor.x != state->cursor_applied.x || cursor.y != state->cursor_applied.y ||
        cursor.visible != state->cursor_applied.visible || cursor.shape != state->cursor_applied.shape)
    {
        memset(&position, 0, sizeof(position));
        position.x = (uint32_t)(cursor.x < 0 ? 0 : cursor.x);
        position.y = (uint32_t)(cursor.y < 0 ? 0 : cursor.y);
        position.visible = cursor.visible ? 1u : 0u;
        position.shape = (uint32_t)cursor.shape;
        if (gpu_move_cursor(state->gpu_fd, &position) < 0) {
            position.visible = 0;
            if (gpu_move_cursor(state->gpu_fd, &position) < 0) sxn_exit(1);
            state->hw_cursor = 0;
            scene_set_software_cursor(1);
            return;
        }
    }
    state->cursor_applied = cursor;
}

/* The plane if the GPU has one, else the scene's software cursor. Hidden until
 * the first commit says where. */
static void setup_cursor(struct compositor_state *state)
{
    struct savanxp_gpu_cursor_position position;

    state->hw_cursor = 0;
    state->cursor_applied.x = 0;
    state->cursor_applied.y = 0;
    state->cursor_applied.shape = SAVANXP_CURSOR_ARROW;
    state->cursor_applied.visible = 0;
    if (!cursor_catalog_needs_background() &&
        (!(g_effects & SXN_EFFECTS_CURSOR) || cursor_catalog_shadow_fits()) &&
        (state->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) != 0 &&
        upload_cursor_shape(state, SAVANXP_CURSOR_ARROW) == 0)
    {
        memset(&position, 0, sizeof(position));
        position.shape = SAVANXP_CURSOR_ARROW;
        if (gpu_move_cursor(state->gpu_fd, &position) >= 0)
        {
            state->hw_cursor = 1;
        }
    }
    {
        char log[128];
        snprintf(log, sizeof(log), "compositord: cursor renderer=%s gpu_flags=%x background=%d",
            state->hw_cursor ? "hardware" : "software", state->gpu_info.flags, cursor_catalog_needs_background());
        (void)sxn_log(log);
    }
    scene_set_software_cursor(!state->hw_cursor);
}

/* Cursor settings have their own bounded, nonblocking connections. A client
 * that connects without sending a complete request cannot stall the WM. */
struct cursor_control_client {
    long input, output;
    size_t received;
    uint64_t deadline;
    uint32_t uid, euid;
    struct sxn_cursor_request request;
};
static struct cursor_control_client cursor_clients[4];

static int change_cursor_theme(struct compositor_state *state, const char *theme) {
    struct savanxp_gpu_cursor_position hidden = {0};
    long result = cursor_catalog_select(theme);
    if (result < 0) return (int)result;
    (void)finish_present(state, 1);
    if (state->hw_cursor && gpu_move_cursor(state->gpu_fd, &hidden) < 0) sxn_exit(1);
    /* Full damage restores the old software pointer even when the new image
     * is smaller or has a different hotspot. setup_cursor may recover a plane. */
    scene_set_display(state->display_view, &state->display_info);
    setup_cursor(state);
    state->cursor_applied.shape = -1;
    apply_cursor(state);
    return produce_frame(state, 1);
}

struct effects_control_client {
    long input, output;
    uint64_t deadline;
    uint32_t uid, euid;
    size_t received;
    struct sxn_effects_request request;
};
static struct effects_control_client effects_clients[2];

static void effects_close(struct effects_control_client *client) {
    if (client->input >= 0) (void)sxn_close(client->input);
    if (client->output >= 0) (void)sxn_close(client->output);
    client->input = client->output = -1;
}
static void effects_service(struct compositor_state *state, long port) {
    unsigned i;
    for (i = 0; i < 2; ++i) {
        struct effects_control_client *client = &effects_clients[i];
        struct sxn_effects_reply reply = {SXN_EFFECTS_MAGIC, SXN_EFFECTS_VERSION, 0, 0, 0};
        long got;
        if (client->input < 0) continue;
        if (sxn_clock_ns() >= client->deadline) { effects_close(client); continue; }
        got = sxn_read(client->input, (unsigned char *)&client->request + client->received,
            sizeof(client->request) - client->received);
        if (got == -SAVANXP_EAGAIN) continue;
        if (got <= 0) { effects_close(client); continue; }
        client->received += (size_t)got;
        if (client->received < sizeof(client->request)) continue;
        if (client->euid != 0 && (client->uid != 1000 || client->euid != 1000)) reply.status = -SAVANXP_EACCES;
        else if (client->request.magic != SXN_EFFECTS_MAGIC || client->request.version != SXN_EFFECTS_VERSION ||
            (client->request.effects & ~SXN_EFFECTS_ALL) ||
            (client->request.operation != SXN_EFFECTS_QUERY && client->request.operation != SXN_EFFECTS_SET))
            reply.status = -SAVANXP_EINVAL;
        else if (client->request.operation == SXN_EFFECTS_SET) {
            char text[64];
            int size;
            /* Drain the old frame before changing the plane or its bounds. */
            (void)finish_present(state, 1);
            struct savanxp_gpu_cursor_position hidden = {0};
            if (state->hw_cursor && gpu_move_cursor(state->gpu_fd, &hidden) < 0) sxn_exit(1);
            g_effects = client->request.effects;
            /* Until both presentation and persistence succeed, a query must
             * keep the preference retryable even after an interrupted apply. */
            g_effects_save_status = -SAVANXP_EIO;
            scene_set_effects(g_effects);
            setup_cursor(state);
            state->cursor_applied.shape = -1;
            apply_cursor(state);
            reply.status = produce_frame(state, 1);
            if (!reply.status) {
                size = snprintf(text, sizeof(text), "window_shadows=%u\ncursor_shadow=%u\n",
                    !!(g_effects & SXN_EFFECTS_WINDOWS), !!(g_effects & SXN_EFFECTS_CURSOR));
                g_effects_save_status = sxn_write_file(SXN_EFFECTS_PATH, text, size);
            }
        }
        if (reply.status != -SAVANXP_EACCES) {
            reply.effects = g_effects;
            reply.save_status = (int32_t)g_effects_save_status;
        }
        (void)sxn_write(client->output, &reply, sizeof(reply));
        effects_close(client);
    }
    if (port >= 0 && sxn_wait_one(port, 0) == 0) {
        struct sxn_port_connection_ex connection;
        if (sxn_port_accept_ex(port, &connection) < 0) return;
        for (i = 0; i < 2 && effects_clients[i].input >= 0; ++i) {}
        if (i == 2) { (void)sxn_close(connection.read_handle); (void)sxn_close(connection.write_handle); return; }
        effects_clients[i] = (struct effects_control_client){.input=connection.read_handle, .output=connection.write_handle,
            .deadline=sxn_clock_ns()+2000000000ull, .uid=connection.client.uid, .euid=connection.client.euid};
        (void)sxn_set_nonblocking(connection.read_handle, 1);
        (void)sxn_set_nonblocking(connection.write_handle, 1);
    }
}

static void cursor_control_close(struct cursor_control_client *client) {
    if (client->input >= 0) (void)sxn_close(client->input);
    if (client->output >= 0) (void)sxn_close(client->output);
    client->input = client->output = -1;
}
static void cursor_control_service(struct compositor_state *state, long port) {
    unsigned i;
    for (i = 0; i < 4; ++i) {
        struct cursor_control_client *client = &cursor_clients[i];
        struct sxn_cursor_reply reply = {SXN_CURSOR_MAGIC, SXN_CURSOR_VERSION, 0, 0, {0}};
        long got;
        if (client->input < 0) continue;
        if (sxn_clock_ns() >= client->deadline) { cursor_control_close(client); continue; }
        got = sxn_read(client->input, (unsigned char *)&client->request + client->received,
            sizeof(client->request) - client->received);
        if (got == -SAVANXP_EAGAIN) continue;
        if (got <= 0) { cursor_control_close(client); continue; }
        client->received += (size_t)got;
        if (client->received < sizeof(client->request)) continue;
        /* The current desktop profile is uid 1000, as in the account table.
         * Other users cannot change or inspect this session's preferences. */
        if (client->euid != 0 && (client->uid != 1000 || client->euid != 1000)) reply.status = -SAVANXP_EACCES;
        else if (client->request.magic != SXN_CURSOR_MAGIC || client->request.version != SXN_CURSOR_VERSION ||
            client->request.reserved || client->request.theme[SXN_CURSOR_ID_SIZE-1] ||
            (client->request.operation != SXN_CURSOR_QUERY && client->request.operation != SXN_CURSOR_SELECT))
            reply.status = -SAVANXP_EINVAL;
        else if (client->request.operation == SXN_CURSOR_SELECT) {
            reply.status = change_cursor_theme(state, client->request.theme);
            if (!reply.status) (void)cursor_catalog_save();
        }
        if (reply.status != -SAVANXP_EACCES) reply.save_status = (int32_t)cursor_catalog_save_status();
        if (reply.status != -SAVANXP_EACCES) snprintf(reply.theme, sizeof(reply.theme), "%s", cursor_catalog_theme());
        (void)sxn_write(client->output, &reply, sizeof(reply));
        cursor_control_close(client);
    }
    if (port >= 0 && sxn_wait_one(port, 0) == 0) {
        struct sxn_port_connection_ex connection;
        if (sxn_port_accept_ex(port, &connection) < 0) return;
        for (i = 0; i < 4 && cursor_clients[i].input >= 0; ++i) {}
        if (i == 4) { (void)sxn_close(connection.read_handle); (void)sxn_close(connection.write_handle); return; }
        cursor_clients[i] = (struct cursor_control_client){.input=connection.read_handle, .output=connection.write_handle,
            .deadline=sxn_clock_ns()+2000000000ull, .uid=connection.client.uid, .euid=connection.client.euid};
        (void)sxn_set_nonblocking(connection.read_handle, 1);
        (void)sxn_set_nonblocking(connection.write_handle, 1);
    }
}

static int compositor_attach(struct compositor_state *state, int request_fd, const struct savanxp_compositor_request *request)
{
    long section = sxn_pipe_receive_handle(request_fd);
    long wake = -1;

    (void)state;
    if (section < 0)
    {
        return -SAVANXP_EINVAL;
    }
    if ((request->surface.flags & SAVANXP_COMPOSITOR_SURFACE_CLIENT) != 0)
    {
        wake = sxn_pipe_receive_handle(request_fd);
        if (wake < 0)
        {
            (void)sxn_close(section);
            return -SAVANXP_EINVAL;
        }
    }
    return scene_attach(&request->surface, section, wake);
}

static int compositor_commit(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request,
    struct savanxp_compositor_reply *reply)
{
    int result = scene_commit();

    if (result < 0)
    {
        return result;
    }
    apply_cursor(state);
    result = produce_frame(state, (request->flags & SAVANXP_COMPOSITOR_COMMIT_WAIT) != 0);
    reply->broken_surfaces = scene_take_broken();
    return result;
}

static int handle_request(
    struct compositor_state *state,
    int request_fd,
    int reply_fd,
    const struct savanxp_compositor_request *request,
    struct savanxp_compositor_reply *reply)
{
    if (request->magic != SAVANXP_COMPOSITOR_PROTOCOL_MAGIC ||
        request->version != SAVANXP_COMPOSITOR_PROTOCOL_VERSION)
    {
        return -SAVANXP_EINVAL;
    }

    switch (request->type)
    {
    case SAVANXP_COMPOSITOR_MSG_INIT:
        return compositor_init(state, request_fd, reply_fd, reply);
    case SAVANXP_COMPOSITOR_MSG_GET_TIMELINE:
        return compositor_get_timeline(state, reply);
    case SAVANXP_COMPOSITOR_MSG_SET_MODE:
        return compositor_set_mode(state, &request->fb_info, reply);
    case SAVANXP_COMPOSITOR_MSG_ATTACH:
        return compositor_attach(state, request_fd, request);
    case SAVANXP_COMPOSITOR_MSG_DETACH:
        scene_detach(request->surface_id);
        return 0;
    case SAVANXP_COMPOSITOR_MSG_COMMIT:
        return compositor_commit(state, request, reply);
    case SAVANXP_COMPOSITOR_MSG_SHUTDOWN:
        return 0;
    default:
        return -SAVANXP_EINVAL;
    }
}

/* Serves one request read from `request_fd`: 1 to go on, 0 after SHUTDOWN,
 * negative when the WM is gone. */
static int serve_request(struct compositor_state *state, int request_fd, int reply_fd)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    unsigned long long service_begin;
    int result = read_exact(request_fd, &request, sizeof(request));

    if (result < 0)
    {
        return result;
    }
    /* The clock starts once the request is in: what is measured is the
     * service, not the wait for a request that had not arrived. */
    service_begin = monotonic_ns();
    memset(&reply, 0, sizeof(reply));
    reply.magic = SAVANXP_COMPOSITOR_PROTOCOL_MAGIC;
    reply.version = SAVANXP_COMPOSITOR_PROTOCOL_VERSION;
    reply.type = request.type;
    reply.serial = request.serial;
    memset(&state->timing, 0, sizeof(state->timing));
    state->damage_px = 0;
    state->bounds_px = 0;
    state->damage_rects = 0;
    reply.status = handle_request(state, request_fd, reply_fd, &request, &reply);
    reply.service_timing = state->timing;
    reply.service_timing.service_ns = elapsed_since(service_begin);
    reply.damage_px = state->damage_px;
    reply.bounds_px = state->bounds_px;
    reply.damage_rects = state->damage_rects;
    if (write_exact(reply_fd, &reply, sizeof(reply)) < 0)
    {
        return -SAVANXP_EPIPE;
    }
    return request.type == SAVANXP_COMPOSITOR_MSG_SHUTDOWN ? 0 : 1;
}

/* Whether a client may be this compositord's window manager: the system's
 * (the desktop is a sibling too, and it is the user's), and either the
 * session's windowd (a sibling, started by the same parent) or a windowd that
 * started this compositord for its tests (the parent itself). */
static int may_be_wm(long pid, const struct sxn_identity *client)
{
    static long parent = -1;
    struct sxn_process_query query;

    if (parent < 0)
    {
        struct sxn_native_info self;

        if (sxn_info(&self) != 0 || sxn_process_query((long)self.pid, &query) != 0)
        {
            return 0;
        }
        parent = (long)query.parent_pid;
    }
    if (client->euid != 0u)
    {
        return 0;
    }
    if (pid == parent)
    {
        return 1;
    }
    return sxn_process_query(pid, &query) == 0 && (long)query.parent_pid == parent;
}

/*
 * Waits for the WM: a connection from a process that may be it, whose first
 * request is INIT. Anything else is closed and the wait goes on -- a
 * connection that hangs up at once is sessiond probing for the port.
 */
static int accept_wm(struct compositor_state *state, long port, int *request_fd, int *reply_fd)
{
    for (;;)
    {
        struct sxn_port_connection_ex connection;
        struct savanxp_compositor_request request;
        struct savanxp_compositor_reply reply;

        if (sxn_wait_one(port, -1) < 0)
        {
            return -1;
        }
        memset(&connection, 0, sizeof(connection));
        if (sxn_port_accept_ex(port, &connection) < 0)
        {
            continue;
        }
        if (may_be_wm((long)connection.client_pid, &connection.client) &&
            read_exact(connection.read_handle, &request, sizeof(request)) == 0 &&
            request.magic == SAVANXP_COMPOSITOR_PROTOCOL_MAGIC &&
            request.version == SAVANXP_COMPOSITOR_PROTOCOL_VERSION &&
            request.type == SAVANXP_COMPOSITOR_MSG_INIT)
        {
            memset(&reply, 0, sizeof(reply));
            reply.magic = SAVANXP_COMPOSITOR_PROTOCOL_MAGIC;
            reply.version = SAVANXP_COMPOSITOR_PROTOCOL_VERSION;
            reply.type = request.type;
            reply.serial = request.serial;
            reply.status = compositor_init(state, connection.read_handle, connection.write_handle, &reply);
            if (write_exact(connection.write_handle, &reply, sizeof(reply)) == 0 && reply.status == 0)
            {
                *request_fd = connection.read_handle;
                *reply_fd = connection.write_handle;
                return 0;
            }
        }
        (void)sxn_close(connection.read_handle);
        (void)sxn_close(connection.write_handle);
    }
}

/* Standalone visual diagnostic: the real file catalog and software painter,
 * shown on both backgrounds. The host callback verifies the scanout pixels. */
static void cursor_selftest(struct compositor_state *state, int emergency) {
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_region region;
    struct savanxp_gpu_cursor_position position;
    unsigned shape, row, theme;
    static const char *themes[] = {"classic", "retrosmart", "hackneyed"};
    int failures = 0;
    g_effects = 0;
    scene_set_effects(0);
    setup_cursor(state);
    if ((state->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) &&
        !cursor_catalog_needs_background() && !state->hw_cursor) failures++;
    memset(&position, 0, sizeof(position));
    if (state->hw_cursor && gpu_move_cursor(state->gpu_fd, &position) < 0) failures++;
    sx_bitmap_wrap(&bitmap, state->display_view, &state->display_info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_init(&painter, &bitmap);
    sx_painter_fill_rect(&painter, sx_rect_make(0, 0, state->display_info.width, state->display_info.height), 0xff808080u);
    for (theme = 0; theme < (emergency ? 1u : 3u); ++theme) {
        if (!emergency && cursor_catalog_select(themes[theme]) < 0) failures++;
        for (row = 0; row < 2; ++row) for (shape = 0; shape < SAVANXP_CURSOR_SHAPE_COUNT; ++shape) {
            const struct cursor_image *asset = cursor_catalog_get(shape);
            if (emergency && (asset->width != 12 || asset->height != 18)) failures++;
            struct scene_cursor cursor = {32 + (int)shape * 96 + asset->hotspot_x,
                32 + (int)theme * 176 + (int)row * 80 + asset->hotspot_y, (int)shape, 1};
            sx_painter_fill_rect(&painter, sx_rect_make(32 + shape * 96, 32 + theme * 176 + row * 80, 64, 64),
                row ? 0xff000000u : 0xffffffffu);
            scene_paint_cursor(&painter, &cursor);
            if (state->hw_cursor && !cursor_catalog_needs_background() && upload_cursor_shape(state, shape) < 0) failures++;
        }
    }
    {
        static struct cursor_image classic;
        classic.width = classic.height = 2;
        classic.classic = classic.needs_background = 1;
        classic.xor_rgb[1] = classic.xor_rgb[3] = 0x00ffffffu;
        classic.and_mask[2] = classic.and_mask[3] = 0x00ffffffu;
        sx_painter_fill_rect(&painter, sx_rect_make(32, 600, 64, 64), 0xff123456u);
        scene_paint_cursor_image(&painter, &classic, 32, 600);
        sx_painter_push_clip(&painter, sx_rect_make(64, 600, 1, 2));
        scene_paint_cursor_image(&painter, &classic, 64, 600);
        sx_painter_pop_clip(&painter);
        sx_painter_fill_rect(&painter, sx_rect_make(0, 632, 2, 2), 0xff123456u);
        scene_paint_cursor_image(&painter, &classic, -1, 632);
    }
    if (!emergency) {
        /* Separate samples retain the original grids above while exercising
         * the optional shadow with real pack silhouettes and hardware uploads. */
        g_effects = SXN_EFFECTS_CURSOR;
        scene_set_effects(g_effects);
        for (theme = 0; theme < 3; ++theme) {
            const struct cursor_image *asset;
            struct scene_cursor cursor;
            if (cursor_catalog_select(themes[theme]) < 0) failures++;
            asset = cursor_catalog_get(SAVANXP_CURSOR_ARROW);
            cursor = (struct scene_cursor){35 + (int)theme * 128 + asset->hotspot_x,
                683 + asset->hotspot_y, SAVANXP_CURSOR_ARROW, 1};
            sx_painter_fill_rect(&painter, sx_rect_make(32 + theme * 128, 680, 96, 64), 0xffffffffu);
            scene_paint_cursor(&painter, &cursor);
            for (shape = 0; shape < SAVANXP_CURSOR_SHAPE_COUNT; ++shape)
                if (state->hw_cursor && upload_cursor_shape(state, shape) < 0) failures++;
        }
        g_effects = 0;
        scene_set_effects(0);
    }
    if (state->hw_cursor) {
        position.x = 900;
        position.y = 680;
        position.visible = 1;
        if (gpu_move_cursor(state->gpu_fd, &position) < 0) failures++;
        /* Simulate a refused upload after successful uploads of all shapes.
         * This exercises the normal fallback, including hiding the plane. */
        state->gpu_info.flags &= ~SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE;
        state->cursor_applied.shape = -1;
        apply_cursor(state);
        if (state->hw_cursor) failures++;
        (void)sxn_log("compositord: cursor selftest hardware uploads and software fallback checked");
    }
    sx_region_clear(&region);
    sx_region_set_rect(&region, sx_rect_make(0, 0, state->display_info.width, state->display_info.height));
    if (present_region(state, &region) < 0 || finish_present(state, 1) < 0) failures++;
    (void)sxn_log(failures ? "CURSOR SMOKE FAIL" : "CURSOR SMOKE PASS");
    /* Keep the displayed frame stable for the QMP screenshot. */
    for (;;) (void)sxn_wait_one(state->frame_event, -1);
}

int main(int argc, char **argv)
{
    struct compositor_state state;
    const char *port_name = SAVANXP_COMPOSITOR_PORT_NAME;
    int request_fd = -1;
    int reply_fd = -1;
    long port;
    long cursor_port = -1;
    long effects_port = -1;
    unsigned control_index;

    cursor_catalog_load();
    effects_load();
    compositor_state_init(&state);
    if (argc >= 2 && argv != 0 && argv[1] != 0 && argv[1][0] != '\0')
    {
        port_name = argv[1];
    }
    if (compositor_startup(&state) < 0)
    {
        (void)sxn_log("compositord: cannot drive the display");
        compositor_state_close(&state);
        sxn_exit(1);
    }
    if (argc >= 2 && strcmp(argv[1], "--cursor-selftest") == 0) cursor_selftest(&state, 0);
    if (argc >= 2 && strcmp(argv[1], "--cursor-fallback-selftest") == 0) cursor_selftest(&state, 1);
    for (control_index = 0; control_index < 4; ++control_index) cursor_clients[control_index].input = cursor_clients[control_index].output = -1;
    for (control_index = 0; control_index < 2; ++control_index) effects_clients[control_index].input = effects_clients[control_index].output = -1;
    if (!strcmp(port_name, SAVANXP_COMPOSITOR_PORT_NAME)) {
        effects_port = sxn_port_create(SXN_EFFECTS_PORT);
        if (effects_port < 0) (void)sxn_log("compositord: effects settings port unavailable");
        cursor_port = sxn_port_create(SXN_CURSOR_PORT);
        if (cursor_port < 0) (void)sxn_log("compositord: cursor settings port unavailable");
    }
    /* Last: the port existing is what tells sessiond the display is ready. */
    port = sxn_port_create(port_name);
    if (port < 0)
    {
        (void)sxn_log("compositord: cannot create its port");
        compositor_state_close(&state);
        sxn_exit(1);
    }

    stats_open();
    if (accept_wm(&state, port, &request_fd, &reply_fd) == 0)
    {
        for (;;)
        {
            int32_t handles[13];
            unsigned long count = 0;
            long timeout = -1;
            long ready;

            handles[count++] = request_fd;
            handles[count++] = (int32_t)port;
            handles[count++] = (int32_t)state.frame_event;
            if (effects_port >= 0) handles[count++] = (int32_t)effects_port;
            for (control_index = 0; control_index < 2; ++control_index) if (effects_clients[control_index].input >= 0) {
                handles[count++] = (int32_t)effects_clients[control_index].input;
                timeout = 50;
            }
            if (cursor_port >= 0) handles[count++] = (int32_t)cursor_port;
            for (control_index = 0; control_index < 4; ++control_index) if (cursor_clients[control_index].input >= 0) {
                handles[count++] = (int32_t)cursor_clients[control_index].input;
                timeout = 50;
            }
            /* Damage left over while a present was in flight is composed once
             * it is on the screen: wait for that too, on the GPU's present
             * event, or by polling where there is none. */
            if (scene_has_damage() && state.in_flight != 0)
            {
                if (state.present_event_fd >= 0)
                {
                    handles[count++] = state.present_event_fd;
                }
                else
                {
                    timeout = 2;
                }
            }
            ready = sxn_wait_many(handles, count, timeout);
            if (ready < 0 && ready != -SAVANXP_ETIMEDOUT)
            {
                break;
            }
            cursor_control_service(&state, cursor_port);
            effects_service(&state, effects_port);
            /* A client published a frame: reset BEFORE looking, so one
             * published after this point wakes the next wait. */
            if (sxn_wait_one(state.frame_event, 0) == 0)
            {
                (void)sxn_event_reset(state.frame_event);
                scene_consume_frames();
            }
            if (produce_frame(&state, 0) < 0)
            {
                break;
            }
            /* One WM per compositord: anyone else is turned away. */
            if (sxn_wait_one(port, 0) == 0)
            {
                struct sxn_port_connection other;

                memset(&other, 0, sizeof(other));
                if (sxn_port_accept(port, &other) == 0)
                {
                    (void)sxn_close(other.read_handle);
                    (void)sxn_close(other.write_handle);
                }
            }
            if (sxn_wait_one(request_fd, 0) == 0 && serve_request(&state, request_fd, reply_fd) <= 0)
            {
                break;
            }
            /* A frame a present in flight held back, now that it may go. */
            if (produce_frame(&state, 0) < 0)
            {
                break;
            }
            stats_report();
        }
    }

    /* The WM hung up or shut the session down: there is no other to serve
     * (no reconnection), so the display goes back to the kernel. */
    compositor_state_close(&state);
    if (effects_port >= 0) (void)sxn_close(effects_port);
    for (control_index = 0; control_index < 2; ++control_index) effects_close(&effects_clients[control_index]);
    if (cursor_port >= 0) (void)sxn_close(cursor_port);
    for (control_index = 0; control_index < 4; ++control_index) cursor_control_close(&cursor_clients[control_index]);
    (void)sxn_close(port);
    return 0;
}
