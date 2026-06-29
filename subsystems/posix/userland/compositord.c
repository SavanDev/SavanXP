#include "libc.h"
#include "cursor_asset.h"
#include "savanxp/compositor_protocol.h"

struct compositor_state
{
    int gpu_fd;
    int present_event_fd;
    uint32_t display_surface_id;
    struct savanxp_fb_info display_info;
    struct savanxp_gpu_info gpu_info;
    int initialized;
    int cursor_enabled;
};

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

static int read_exact(int fd, void *buffer, size_t size)
{
    unsigned char *cursor = (unsigned char *)buffer;
    size_t offset = 0;

    while (offset < size)
    {
        long result = read(fd, cursor + offset, size - offset);
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
        long result = write(fd, cursor + offset, size - offset);
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

static int compositor_init(
    struct compositor_state *state,
    const struct savanxp_fb_info *requested_info,
    struct savanxp_compositor_reply *reply)
{
    struct savanxp_gpu_mode mode;
    struct savanxp_gpu_surface_import import_request;
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

    memset(&import_request, 0, sizeof(import_request));
    import_request.section_handle = SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD;
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
    state->initialized = 1;
    reply->fb_info = state->display_info;
    reply->gpu_info = state->gpu_info;
    return 0;
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
    result = gpu_get_present_timeline(state->gpu_fd, &timeline);
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

    result = gpu_present_surface_batch(state->gpu_fd, &batch);
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
            result = wait_one(state->present_event_fd, 0);
            if (result_is_error(result) && result_error_code(result) == SAVANXP_ETIMEDOUT)
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
        (void)event_reset(state->present_event_fd);
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

static int compositor_enable_cursor(
    struct compositor_state *state,
    const struct savanxp_compositor_request *request)
{
    static uint32_t cursor_pixels[64 * 64];
    struct savanxp_gpu_cursor_image image;
    int row;
    int column;

    if (state == 0 || request == 0 || !state->initialized)
    {
        return -SAVANXP_ENODEV;
    }
    if ((state->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) == 0)
    {
        return -SAVANXP_ENOSYS;
    }

    memset(cursor_pixels, 0, sizeof(cursor_pixels));
    for (row = 0; row < DESKTOP_CURSOR_HEIGHT; ++row)
    {
        for (column = 0; column < DESKTOP_CURSOR_WIDTH; ++column)
        {
            cursor_pixels[(row * 64) + column] =
                k_desktop_cursor_pixels[(row * DESKTOP_CURSOR_WIDTH) + column];
        }
    }

    image.pixels = (uint64_t)(unsigned long)cursor_pixels;
    image.width = 64;
    image.height = 64;
    image.pitch = 64u * sizeof(uint32_t);
    image.hotspot_x = DESKTOP_CURSOR_HOTSPOT_X;
    image.hotspot_y = DESKTOP_CURSOR_HOTSPOT_Y;
    if (gpu_set_cursor(state->gpu_fd, &image) < 0)
    {
        return -SAVANXP_EIO;
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
    case SAVANXP_COMPOSITOR_MSG_SHUTDOWN:
        return 0;
    default:
        return -SAVANXP_EINVAL;
    }
}

int main(void)
{
    struct compositor_state state;
    int running = 1;

    compositor_state_init(&state);

    while (running)
    {
        struct savanxp_compositor_request request;
        struct savanxp_compositor_reply reply;
        int result = read_exact(SAVANXP_COMPOSITOR_REQUEST_FD, &request, sizeof(request));
        if (result < 0)
        {
            break;
        }

        memset(&reply, 0, sizeof(reply));
        reply.magic = SAVANXP_COMPOSITOR_PROTOCOL_MAGIC;
        reply.version = SAVANXP_COMPOSITOR_PROTOCOL_VERSION;
        reply.type = request.type;
        reply.serial = request.serial;
        reply.status = handle_request(&state, &request, &reply);
        if (request.type == SAVANXP_COMPOSITOR_MSG_SHUTDOWN)
        {
            running = 0;
        }

        if (write_exact(SAVANXP_COMPOSITOR_REPLY_FD, &reply, sizeof(reply)) < 0)
        {
            break;
        }
    }

    compositor_state_close(&state);
    return 0;
}
