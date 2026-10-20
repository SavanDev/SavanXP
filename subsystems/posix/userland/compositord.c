#include <stdint.h>
#include <string.h>

#include "savanxp/compositor_protocol.h"
#include "sxn/sxn.h"
/* After the SavanXP headers: it uses the cursor shape enum. */
#include "cursor_asset.h"

/*
 * compositord is a native process (EI_OSABI 0x53) and a session service
 * (docs/desktop/DESKTOP_ARCHITECTURE.md#composition-in-compositord): sessiond
 * starts it before windowd, it takes the GPU and allocates the display memory
 * at startup, and serves one window manager through its port
 * (compositor_protocol.h). It speaks only SXN: the GPU is a device it opens by
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
    /* The display memory, allocated at startup for the largest mode, and its
     * size: a mode whose buffer does not fit is refused. */
    long display_section;
    uint64_t display_capacity;
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
    if (state->display_section >= 0)
    {
        (void)sxn_close(state->display_section);
        state->display_section = -1;
    }
    state->initialized = 0;
    state->cursor_enabled = 0;
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

/*
 * Takes the GPU and the display at startup, before the port exists: a
 * compositord that cannot drive the display never offers itself to a WM. The
 * mode is the one the firmware left (the native one); the display section is
 * sized for it plus slack, as the largest mode this session will use.
 */
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

    state->display_capacity = ((uint64_t)width * 4u * height) + (4ull * 1024ull * 1024ull);
    state->display_section = sxn_section_create(state->display_capacity, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (state->display_section < 0)
    {
        return (int)state->display_section;
    }

    state->present_event_fd = (int)gpu_create_present_event(state->gpu_fd);
    if (state->present_event_fd < 0)
    {
        state->present_event_fd = -1;
    }

    result = compositor_apply_mode(state, width, height);
    if (result < 0)
    {
        return (int)result;
    }
    state->initialized = 1;
    return 0;
}

/* INIT: what compositord set up at startup, and the display section. */
static int compositor_init(
    struct compositor_state *state,
    int reply_fd,
    struct savanxp_compositor_reply *reply)
{
    if (!state->initialized)
    {
        return -SAVANXP_ENODEV;
    }
    if (sxn_pipe_send_handle(reply_fd, state->display_section) < 0)
    {
        return -SAVANXP_EIO;
    }
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
        return compositor_init(state, reply_fd, reply);
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
    reply.status = handle_request(state, reply_fd, &request, &reply);
    reply.service_timing.service_ns = elapsed_since(service_begin);
    if (write_exact(reply_fd, &reply, sizeof(reply)) < 0)
    {
        return -SAVANXP_EPIPE;
    }
    return request.type == SAVANXP_COMPOSITOR_MSG_SHUTDOWN ? 0 : 1;
}

/* Whether `pid` may be this compositord's window manager: the session's
 * windowd (a sibling, started by the same parent) or a windowd that started
 * this compositord for its tests (the parent itself). */
static int may_be_wm(long pid)
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
        struct sxn_port_connection connection;
        struct savanxp_compositor_request request;
        struct savanxp_compositor_reply reply;

        if (sxn_wait_one(port, -1) < 0)
        {
            return -1;
        }
        memset(&connection, 0, sizeof(connection));
        if (sxn_port_accept(port, &connection) < 0)
        {
            continue;
        }
        if (may_be_wm((long)connection.client_pid) &&
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
            reply.status = compositor_init(state, connection.write_handle, &reply);
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

int main(int argc, char **argv)
{
    struct compositor_state state;
    const char *port_name = SAVANXP_COMPOSITOR_PORT_NAME;
    int request_fd = -1;
    int reply_fd = -1;
    long port;

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
    /* Last: the port existing is what tells sessiond the display is ready. */
    port = sxn_port_create(port_name);
    if (port < 0)
    {
        (void)sxn_log("compositord: cannot create its port");
        compositor_state_close(&state);
        sxn_exit(1);
    }

    if (accept_wm(&state, port, &request_fd, &reply_fd) == 0)
    {
        for (;;)
        {
            int32_t handles[2];
            long ready;

            handles[0] = request_fd;
            handles[1] = (int32_t)port;
            ready = sxn_wait_many(handles, 2, -1);
            if (ready < 0)
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
        }
    }

    /* The WM hung up or shut the session down: there is no other to serve
     * (no reconnection), so the display goes back to the kernel. */
    compositor_state_close(&state);
    (void)sxn_close(port);
    /* Never return: crt0 would end the process through POSIX EXIT, which a
     * native process does not have. */
    sxn_exit(0);
}
