#include <stdint.h>
#include <string.h>

#include "savanxp/compositor_protocol.h"
#include "sxn/sxn.h"
/* After the SavanXP headers: it uses the cursor shape enum. */
#include "cursor_asset.h"

/*
 * compositord is a native process (EI_OSABI 0x53, docs/desktop/
 * DESKTOP_ARCHITECTURE.md phase 3): it speaks only SXN. Its channels are the
 * handles windowd hands it (request and reply pipes, the display section), the
 * GPU is a device it opens by name, and every GPU request is the same control
 * code the POSIX side sends through ioctl. Nothing here may call into the
 * POSIX runtime: from a native process those calls are ENOSYS.
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

struct compositor_state
{
    int gpu_fd;
    int present_event_fd;
    uint32_t display_surface_id;
    struct savanxp_fb_info display_info;
    struct savanxp_gpu_info gpu_info;
    int initialized;
    int cursor_enabled;
    /* The pipe requests arrive on, and the display section the client queued
     * on it before INIT (compositor_protocol.h). -1 until taken. */
    long request_fd;
    long display_section;
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
    state->request_fd = -1;
    state->display_section = -1;
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
    state->initialized = 0;
    state->cursor_enabled = 0;
}

/* (Re)imports the shared display section as the scanout surface with the
 * geometry in state->display_info. It is always the same section -- the one
 * the client handed over before INIT -- sized for the largest mode, so a
 * smaller mode fits without reallocating anything. */
static int compositor_import_display(struct compositor_state *state)
{
    struct savanxp_gpu_surface_import import_request;
    long result;

    if (state->display_section < 0)
    {
        state->display_section = sxn_pipe_receive_handle(state->request_fd);
        if (state->display_section < 0)
        {
            state->display_section = -1;
            return -SAVANXP_EINVAL;
        }
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

/* Programa un modo y deja la seccion de display importada contra el. Todo o
 * nada: si falla no queda superficie importada, y el que llama decide si
 * repone el modo anterior. Exige que no haya una superficie viva (SET_MODE
 * rehace el scanout). */
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

    return compositor_import_display(state);
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

static int compositor_init(
    struct compositor_state *state,
    const struct savanxp_fb_info *requested_info,
    struct savanxp_compositor_reply *reply)
{
    struct savanxp_gpu_mode mode;
    long result;

    if (state == 0 || requested_info == 0 || requested_info->width == 0 || requested_info->height == 0)
    {
        return -SAVANXP_EINVAL;
    }
    if (state->initialized)
    {
        return 0;
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

    memset(&mode, 0, sizeof(mode));
    mode.width = requested_info->width;
    mode.height = requested_info->height;
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

    result = gpu_get_info(state->gpu_fd, &state->gpu_info);
    if (result == 0)
    {
        state->gpu_info.width = state->display_info.width;
        state->gpu_info.height = state->display_info.height;
        state->gpu_info.pitch = state->display_info.pitch;
        state->gpu_info.bpp = state->display_info.bpp;
        state->gpu_info.buffer_size = state->display_info.buffer_size;
    }

    state->present_event_fd = (int)gpu_create_present_event(state->gpu_fd);
    if (state->present_event_fd < 0)
    {
        state->present_event_fd = -1;
    }

    result = compositor_import_display(state);
    if (result < 0)
    {
        return (int)result;
    }

    state->initialized = 1;
    reply->fb_info = state->display_info;
    reply->gpu_info = state->gpu_info;
    return 0;
}

/* Delta contra un instante anterior, robusto al reloj sin calibrar: si
 * monotonic_ns devuelve 0 (o el reloj retrocede) el intervalo vale 0, que el
 * consumidor ya trata como 'sin muestra'. */
static uint64_t elapsed_since(unsigned long long mark)
{
    unsigned long long now = monotonic_ns();
    if (mark == 0ull || now <= mark)
    {
        return 0ull;
    }
    return (uint64_t)(now - mark);
}

static int compositor_present(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request,
    struct savanxp_compositor_reply *reply)
{
    struct savanxp_gpu_present_timeline timeline;
    struct savanxp_gpu_surface_present_batch batch;
    long result;
    uint32_t index;
    unsigned long long mark;

    if (state == 0 || request == 0 || reply == 0 || !state->initialized)
    {
        return -SAVANXP_ENODEV;
    }
    if (request->surface_id != SAVANXP_COMPOSITOR_SURFACE_DISPLAY ||
        request->rect_count == 0 ||
        request->rect_count > SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS)
    {
        return -SAVANXP_EINVAL;
    }

    memset(&timeline, 0, sizeof(timeline));
    mark = monotonic_ns();
    result = gpu_get_present_timeline(state->gpu_fd, &timeline);
    reply->service_timing.timeline_ns = elapsed_since(mark);
    if (result < 0)
    {
        return (int)result;
    }

    memset(&batch, 0, sizeof(batch));
    batch.surface_id = state->display_surface_id;
    batch.rect_count = request->rect_count;
    batch.flags = request->flags & SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE;
    batch.present_cookie = timeline.submitted_sequence + 1u;
    for (index = 0; index < request->rect_count; ++index)
    {
        batch.rects[index] = request->rects[index];
    }

    mark = monotonic_ns();
    result = gpu_present_surface_batch(state->gpu_fd, &batch);
    reply->service_timing.gpu_ns = elapsed_since(mark);
    if (result < 0)
    {
        return (int)result;
    }

    reply->present_sequence = batch.present_cookie;
    return 0;
}

static int compositor_sync_present(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request,
    struct savanxp_compositor_reply *reply)
{
    struct savanxp_gpu_present_timeline timeline;
    struct savanxp_gpu_present_wait wait_request;
    long result;

    if (state == 0 || request == 0 || reply == 0 || !state->initialized)
    {
        return -SAVANXP_ENODEV;
    }

    reply->ready = 1u;
    if (request->target_sequence == 0)
    {
        return 0;
    }

    if (!request->wait_for_target)
    {
        if (state->present_event_fd >= 0)
        {
            result = sxn_wait_one(state->present_event_fd, 0);
            if (result == -SAVANXP_ETIMEDOUT)
            {
                reply->ready = 0;
                return 0;
            }
        }

        memset(&timeline, 0, sizeof(timeline));
        result = gpu_get_present_timeline(state->gpu_fd, &timeline);
        if (result < 0)
        {
            return (int)result;
        }
        if (timeline.retired_sequence < request->target_sequence)
        {
            reply->ready = 0;
            return 0;
        }
    }

    memset(&wait_request, 0, sizeof(wait_request));
    wait_request.target_sequence = request->target_sequence;
    result = gpu_wait_present(state->gpu_fd, &wait_request);
    if (result < 0)
    {
        return (int)result;
    }
    if ((wait_request.flags & SAVANXP_GPU_PRESENT_TIMELINE_FLAG_TARGET_FAILED) != 0)
    {
        return -SAVANXP_EIO;
    }
    if (state->present_event_fd >= 0)
    {
        (void)sxn_event_reset(state->present_event_fd);
    }
    reply->timeline.retired_sequence = wait_request.retired_sequence;
    reply->timeline.pending_count = wait_request.pending_count;
    reply->timeline.flags = wait_request.flags;
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

static int upload_cursor_shape(struct compositor_state *state, uint32_t shape)
{
    static uint32_t cursor_pixels[64 * 64];
    struct savanxp_gpu_cursor_image image;
    const struct desktop_cursor_asset *asset;
    int row;
    int column;

    if (state == 0 || (state->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) == 0)
    {
        return -SAVANXP_ENOSYS;
    }
    if (shape >= (uint32_t)SAVANXP_CURSOR_SHAPE_COUNT)
    {
        shape = SAVANXP_CURSOR_ARROW;
    }
    asset = &k_desktop_cursor_assets[shape];

    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    for (row = 0; row < asset->height && row < 64; ++row)
    {
        for (column = 0; column < asset->width && column < 64; ++column)
        {
            cursor_pixels[(row * 64) + column] = asset->pixels[(row * asset->width) + column];
        }
    }

    image.pixels = (uint64_t)(unsigned long)cursor_pixels;
    image.width = 64;
    image.height = 64;
    image.pitch = 64u * sizeof(uint32_t);
    image.hotspot_x = (uint32_t)asset->hotspot_x;
    image.hotspot_y = (uint32_t)asset->hotspot_y;
    return gpu_set_cursor(state->gpu_fd, &image) < 0 ? -SAVANXP_EIO : 0;
}

static int compositor_enable_cursor(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request)
{
    int result;

    if (state == 0 || request == 0 || !state->initialized)
    {
        return -SAVANXP_ENODEV;
    }

    result = upload_cursor_shape(state, request->cursor_position.shape);
    if (result < 0)
    {
        return result;
    }

    state->cursor_enabled = 1;
    return gpu_move_cursor(state->gpu_fd, &request->cursor_position) < 0 ? -SAVANXP_EIO : 0;
}

static int compositor_move_cursor(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request)
{
    if (state == 0 || request == 0 || !state->initialized || !state->cursor_enabled)
    {
        return -SAVANXP_ENODEV;
    }
    return gpu_move_cursor(state->gpu_fd, &request->cursor_position) < 0 ? -SAVANXP_EIO : 0;
}

static int compositor_set_cursor_shape(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request)
{
    if (state == 0 || request == 0 || !state->initialized || !state->cursor_enabled)
    {
        return -SAVANXP_ENODEV;
    }
    /* Shape swap only -- must not touch position/visibility, or every shape
     * change would snap the hardware cursor to (0,0) and hide it. */
    return upload_cursor_shape(state, request->cursor_position.shape);
}

static int handle_request(
    struct compositor_state *state,
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
        return compositor_init(state, &request->fb_info, reply);
    case SAVANXP_COMPOSITOR_MSG_PRESENT:
        return compositor_present(state, request, reply);
    case SAVANXP_COMPOSITOR_MSG_SYNC_PRESENT:
        return compositor_sync_present(state, request, reply);
    case SAVANXP_COMPOSITOR_MSG_GET_TIMELINE:
        return compositor_get_timeline(state, reply);
    case SAVANXP_COMPOSITOR_MSG_ENABLE_CURSOR:
        return compositor_enable_cursor(state, request);
    case SAVANXP_COMPOSITOR_MSG_MOVE_CURSOR:
        return compositor_move_cursor(state, request);
    case SAVANXP_COMPOSITOR_MSG_SET_CURSOR_SHAPE:
        return compositor_set_cursor_shape(state, request);
    case SAVANXP_COMPOSITOR_MSG_SET_MODE:
        return compositor_set_mode(state, &request->fb_info, reply);
    case SAVANXP_COMPOSITOR_MSG_SHUTDOWN:
        return 0;
    default:
        return -SAVANXP_EINVAL;
    }
}

int main(int argc, char **argv)
{
    struct compositor_state state;
    int32_t channel[2] = {-1, -1};
    int running = 1;
    unsigned long long service_begin = 0ull;

    compositor_state_init(&state);
    /* The client's port (compositor_protocol.h): [0] carries replies to it,
     * [1] brings its requests. */
    if (argc < 2 || argv == 0 || argv[1] == 0 || sxn_port_connect(argv[1], channel) != 0)
    {
        (void)sxn_log("compositord: no client port to connect to");
        sxn_exit(1);
    }
    state.request_fd = channel[1];

    while (running)
    {
        struct savanxp_compositor_request request;
        struct savanxp_compositor_reply reply;
        int result = read_exact((int)state.request_fd, &request, sizeof(request));
        if (result < 0)
        {
            break;
        }

        /* El cronometro arranca apenas volvio read_exact: lo que se mide es el
           servicio, no la espera por una request que todavia no llego. */
        service_begin = monotonic_ns();
        memset(&reply, 0, sizeof(reply));
        reply.magic = SAVANXP_COMPOSITOR_PROTOCOL_MAGIC;
        reply.version = SAVANXP_COMPOSITOR_PROTOCOL_VERSION;
        reply.type = request.type;
        reply.serial = request.serial;
        reply.status = handle_request(&state, &request, &reply);
        reply.service_timing.service_ns = elapsed_since(service_begin);
        if (request.type == SAVANXP_COMPOSITOR_MSG_SHUTDOWN)
        {
            running = 0;
        }

        if (write_exact(channel[0], &reply, sizeof(reply)) < 0)
        {
            break;
        }
    }

    compositor_state_close(&state);
    /* Never return: crt0 would end the process through POSIX EXIT, which a
     * native process does not have. */
    sxn_exit(0);
}
