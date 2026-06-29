#include "desktop_compositor_client.h"

static const char *k_compositord_path = "/bin/compositord";

/* Watchdog for a single request/reply hop. A healthy daemon answers a present in
   well under a frame; a blocking drain (gpu_wait_present) still retires quickly.
   This bound only fires when compositord is alive but wedged, in which case we
   tear it down and reconnect instead of hanging the shell forever. Peer death is
   detected immediately via poll without waiting this out. */
#define SAVANXP_COMPOSITOR_RPC_TIMEOUT_MS 5000

/* Block until `fd` is ready for `events`, the peer hangs up, or the watchdog
   expires. Returns 1 when ready, 0 on timeout, -1 on hangup/error. */
static int wait_fd(int fd, short events, long timeout_ms)
{
    struct savanxp_pollfd poll_fd;
    long ready;

    poll_fd.fd = fd;
    poll_fd.events = events;
    poll_fd.revents = 0;

    ready = poll(&poll_fd, 1, timeout_ms);
    if (ready < 0)
    {
        return -1;
    }
    if (ready == 0)
    {
        return 0;
    }
    if ((poll_fd.revents & (SAVANXP_POLLERR | SAVANXP_POLLNVAL)) != 0)
    {
        return -1;
    }
    if ((poll_fd.revents & events) != 0)
    {
        return 1;
    }
    /* POLLHUP with the wanted event still set is handled above; a bare hangup
       means the daemon is gone. */
    return -1;
}

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

/*
 * The compositor child remaps its inherited handles onto descriptors 3..5 (the
 * request pipe, reply pipe, and display section). A source handle may already
 * sit inside that 3..5 window, so dup-ing it straight onto a target could clobber
 * another source we still need. Both helpers below run only in the forked child:
 * first park every source above the window, then dup2 down cleanly. This makes the
 * handoff independent of the order fds were allocated in, which matters because a
 * reconnect reuses the long-lived section fd while the pipes are freshly created.
 */
static int child_relocate_above_window(int fd)
{
    /* dup() returns the lowest free descriptor, so retry until the copy clears
       the handoff window. */
    while (fd >= 0 && fd <= SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD)
    {
        long moved = dup(fd);
        if (moved < 0)
        {
            break;
        }
        fd = (int)moved;
    }
    return fd;
}

static void child_close_above_window(void)
{
    /* Leave only stdio plus the three handoff fds; closing an unopened fd is a
       harmless no-op, so a fixed sweep is enough for this tiny fd table. */
    int fd;
    for (fd = SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD + 1; fd < 64; ++fd)
    {
        close(fd);
    }
}

static int read_exact(int fd, void *buffer, size_t size)
{
    unsigned char *cursor = (unsigned char *)buffer;
    size_t offset = 0;

    while (offset < size)
    {
        long result;
        int ready = wait_fd(fd, SAVANXP_POLLIN, SAVANXP_COMPOSITOR_RPC_TIMEOUT_MS);
        if (ready == 0)
        {
            return -SAVANXP_ETIMEDOUT;
        }
        if (ready < 0)
        {
            return -SAVANXP_EPIPE;
        }
        result = read(fd, cursor + offset, size - offset);
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
        long result;
        int ready = wait_fd(fd, SAVANXP_POLLOUT, SAVANXP_COMPOSITOR_RPC_TIMEOUT_MS);
        if (ready == 0)
        {
            return -SAVANXP_ETIMEDOUT;
        }
        if (ready < 0)
        {
            return -SAVANXP_EPIPE;
        }
        result = write(fd, cursor + offset, size - offset);
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

static void fill_default_display_info(struct savanxp_fb_info *info)
{
    struct savanxp_system_info system = {0};

    memset(info, 0, sizeof(*info));
    if (system_info(&system) == 0 &&
        system.framebuffer_width != 0 &&
        system.framebuffer_height != 0)
    {
        info->width = system.framebuffer_width;
        info->height = system.framebuffer_height;
        info->bpp = system.framebuffer_bpp != 0 ? system.framebuffer_bpp : 32u;
    }
    else
    {
        info->width = 1024u;
        info->height = 768u;
        info->bpp = 32u;
    }

    if (info->bpp != 32u)
    {
        info->bpp = 32u;
    }
    info->pitch = info->width * (uint32_t)sizeof(uint32_t);
    info->buffer_size = (info->pitch * info->height) + (4u * 1024u * 1024u);
}

static int compositor_rpc(
    struct desktop_compositor_connection *connection,
    struct savanxp_compositor_request *request,
    struct savanxp_compositor_reply *reply)
{
    int result;

    if (connection == 0 || !connection->connected || connection->request_fd < 0 || connection->reply_fd < 0)
    {
        return -SAVANXP_ENODEV;
    }

    request->magic = SAVANXP_COMPOSITOR_PROTOCOL_MAGIC;
    request->version = SAVANXP_COMPOSITOR_PROTOCOL_VERSION;
    request->serial = ++connection->next_serial;

    result = write_exact(connection->request_fd, request, sizeof(*request));
    if (result < 0)
    {
        connection->connected = 0;
        return result;
    }

    memset(reply, 0, sizeof(*reply));
    result = read_exact(connection->reply_fd, reply, sizeof(*reply));
    if (result < 0)
    {
        connection->connected = 0;
        return result;
    }

    if (reply->magic != SAVANXP_COMPOSITOR_PROTOCOL_MAGIC ||
        reply->version != SAVANXP_COMPOSITOR_PROTOCOL_VERSION ||
        reply->type != request->type ||
        reply->serial != request->serial)
    {
        connection->connected = 0;
        return -SAVANXP_EIO;
    }

    return reply->status < 0 ? reply->status : 0;
}

void desktop_compositor_connection_init(struct desktop_compositor_connection *connection)
{
    if (connection == 0)
    {
        return;
    }

    memset(connection, 0, sizeof(*connection));
    connection->pid = -1;
    connection->request_fd = -1;
    connection->reply_fd = -1;
    connection->display_section_fd = -1;
}

/*
 * Fork + exec compositord and run the INIT handshake. The caller owns the
 * display section (connection->display_section_fd) and the requested geometry
 * (connection->requested_info); both are reused as-is, which lets a reconnect
 * respawn the daemon over the same framebuffer the shell is already rendering
 * into. On failure the daemon endpoints are reset but the section is left intact
 * for the caller to keep or tear down.
 */
static int spawn_compositor_daemon(struct desktop_compositor_connection *connection)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int request_pipe[2] = {-1, -1};
    int reply_pipe[2] = {-1, -1};
    const char *argv[2] = {k_compositord_path, 0};
    long pid;
    int result;

    if (connection == 0 || connection->display_section_fd < 0)
    {
        return -SAVANXP_EINVAL;
    }

    if (pipe(request_pipe) < 0 || pipe(reply_pipe) < 0)
    {
        result = -SAVANXP_EIO;
        goto fail;
    }

    pid = fork();
    if (pid < 0)
    {
        result = (int)pid;
        goto fail;
    }
    if (pid == 0)
    {
        int child_request = child_relocate_above_window(request_pipe[0]);
        int child_reply = child_relocate_above_window(reply_pipe[1]);
        int child_section = child_relocate_above_window(connection->display_section_fd);

        if (dup2(child_request, SAVANXP_COMPOSITOR_REQUEST_FD) < 0 ||
            dup2(child_reply, SAVANXP_COMPOSITOR_REPLY_FD) < 0 ||
            dup2(child_section, SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD) < 0)
        {
            exit(1);
        }

        child_close_above_window();

        if (exec(k_compositord_path, argv, 1) < 0)
        {
            eprintf("desktop: exec failed for %s\n", k_compositord_path);
        }
        exit(1);
    }

    connection->pid = pid;
    connection->request_fd = request_pipe[1];
    connection->reply_fd = reply_pipe[0];
    connection->connected = 1;
    connection->next_serial = 0;
    connection->pending_present_sequence = 0;
    close_fd_if_needed(&request_pipe[0]);
    close_fd_if_needed(&reply_pipe[1]);

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_INIT;
    request.fb_info = connection->requested_info;
    result = compositor_rpc(connection, &request, &reply);
    if (result < 0)
    {
        goto fail;
    }

    connection->display_info = reply.fb_info;
    connection->gpu_info = reply.gpu_info;
    if (connection->display_info.buffer_size > connection->requested_info.buffer_size)
    {
        result = -SAVANXP_EIO;
        goto fail;
    }
    return 0;

fail:
    close_fd_if_needed(&request_pipe[0]);
    close_fd_if_needed(&request_pipe[1]);
    close_fd_if_needed(&reply_pipe[0]);
    close_fd_if_needed(&reply_pipe[1]);
    close_fd_if_needed(&connection->request_fd);
    close_fd_if_needed(&connection->reply_fd);
    if (connection->pid > 0)
    {
        int status = 0;
        (void)kill((int)connection->pid, SAVANXP_SIGKILL);
        (void)waitpid((int)connection->pid, &status);
        connection->pid = -1;
    }
    connection->connected = 0;
    return result;
}

/* Push the entire current framebuffer to the daemon's scanout. Used after a
   reconnect to re-display the content the shell already has in memory. */
static int present_full_surface(struct desktop_compositor_connection *connection)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int result;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_PRESENT;
    request.surface_id = SAVANXP_COMPOSITOR_SURFACE_DISPLAY;
    request.flags = SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE;
    request.rect_count = 1;
    request.rects[0].x = 0;
    request.rects[0].y = 0;
    request.rects[0].width = connection->display_info.width;
    request.rects[0].height = connection->display_info.height;

    result = compositor_rpc(connection, &request, &reply);
    if (result < 0)
    {
        return result;
    }
    if (reply.present_sequence != 0)
    {
        connection->pending_present_sequence = reply.present_sequence;
    }
    return 0;
}

/* Tear down a dead or wedged daemon while keeping the display section mapped.
   A wedged-but-alive daemon is killed so waitpid does not block. */
static void reap_daemon(struct desktop_compositor_connection *connection)
{
    if (connection == 0)
    {
        return;
    }

    close_fd_if_needed(&connection->request_fd);
    close_fd_if_needed(&connection->reply_fd);

    if (connection->pid > 0)
    {
        int status = 0;
        (void)kill((int)connection->pid, SAVANXP_SIGKILL);
        (void)waitpid((int)connection->pid, &status);
    }
    connection->pid = -1;
    connection->connected = 0;
    connection->pending_present_sequence = 0;
}

int desktop_compositor_open(struct desktop_compositor_connection *connection)
{
    int result;

    if (connection == 0)
    {
        return -SAVANXP_EINVAL;
    }

    desktop_compositor_connection_init(connection);
    fill_default_display_info(&connection->requested_info);

    connection->display_section_fd = (int)section_create(
        connection->requested_info.buffer_size,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (connection->display_section_fd < 0)
    {
        result = connection->display_section_fd;
        desktop_compositor_close(connection);
        return result;
    }

    connection->display_view = map_view(
        connection->display_section_fd,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)connection->display_view))
    {
        result = (int)(long)connection->display_view;
        connection->display_view = 0;
        desktop_compositor_close(connection);
        return result;
    }
    connection->framebuffer = (uint32_t *)connection->display_view;
    memset(connection->framebuffer, 0, connection->requested_info.buffer_size);

    result = spawn_compositor_daemon(connection);
    if (result < 0)
    {
        desktop_compositor_close(connection);
        return result;
    }
    return 0;
}

int desktop_compositor_connected(const struct desktop_compositor_connection *connection)
{
    return connection != 0 && connection->connected;
}

int desktop_compositor_reconnect(struct desktop_compositor_connection *connection)
{
    int result;

    if (connection == 0 || connection->display_section_fd < 0 || connection->framebuffer == 0)
    {
        return -SAVANXP_ENODEV;
    }

    reap_daemon(connection);

    result = spawn_compositor_daemon(connection);
    if (result < 0)
    {
        return result;
    }

    /* Restore the hardware cursor the shell had enabled before the crash. Best
       effort: a missing cursor plane must not fail the reconnect. */
    if (connection->cursor_enabled)
    {
        connection->cursor_enabled = 0;
        (void)desktop_compositor_enable_cursor(connection, connection->cursor_x, connection->cursor_y);
        (void)desktop_compositor_move_cursor(
            connection, connection->cursor_x, connection->cursor_y, connection->cursor_visible);
    }

    return present_full_surface(connection);
}

void desktop_compositor_close(struct desktop_compositor_connection *connection)
{
    if (connection == 0)
    {
        return;
    }

    if (connection->connected && connection->request_fd >= 0 && connection->reply_fd >= 0)
    {
        struct savanxp_compositor_request request;
        struct savanxp_compositor_reply reply;
        memset(&request, 0, sizeof(request));
        request.type = SAVANXP_COMPOSITOR_MSG_SHUTDOWN;
        (void)compositor_rpc(connection, &request, &reply);
    }

    close_fd_if_needed(&connection->request_fd);
    close_fd_if_needed(&connection->reply_fd);

    if (connection->pid > 0)
    {
        int status = 0;
        (void)waitpid((int)connection->pid, &status);
    }

    if (connection->display_view != 0 && !result_is_error((long)connection->display_view))
    {
        (void)unmap_view(connection->display_view);
    }
    close_fd_if_needed(&connection->display_section_fd);
    desktop_compositor_connection_init(connection);
}

int desktop_compositor_present(
    struct desktop_compositor_connection *connection,
    const struct sx_rect *rects,
    size_t rect_count)
{
    size_t index = 0;

    if (connection == 0 || rects == 0 || rect_count == 0)
    {
        return 0;
    }

    while (index < rect_count)
    {
        struct savanxp_compositor_request request;
        struct savanxp_compositor_reply reply;
        uint32_t request_rect_count = 0;
        int result;

        memset(&request, 0, sizeof(request));
        request.type = SAVANXP_COMPOSITOR_MSG_PRESENT;
        request.surface_id = SAVANXP_COMPOSITOR_SURFACE_DISPLAY;

        while (index < rect_count && request_rect_count < SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS)
        {
            const struct sx_rect *rect = &rects[index++];
            if (rect->width <= 0 || rect->height <= 0)
            {
                continue;
            }
            request.rects[request_rect_count].x = (uint32_t)rect->x;
            request.rects[request_rect_count].y = (uint32_t)rect->y;
            request.rects[request_rect_count].width = (uint32_t)rect->width;
            request.rects[request_rect_count].height = (uint32_t)rect->height;
            request_rect_count += 1u;
        }

        if (request_rect_count == 0)
        {
            continue;
        }

        request.rect_count = request_rect_count;
        result = compositor_rpc(connection, &request, &reply);
        if (result < 0)
        {
            return result;
        }
        if (reply.present_sequence != 0)
        {
            connection->pending_present_sequence = reply.present_sequence;
        }
    }

    return 0;
}

int desktop_compositor_sync_present(
    struct desktop_compositor_connection *connection,
    int wait_for_target,
    int *ready)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int result;

    if (ready != 0)
    {
        *ready = 1;
    }
    if (connection == 0 || connection->pending_present_sequence == 0)
    {
        return 0;
    }

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_SYNC_PRESENT;
    request.target_sequence = connection->pending_present_sequence;
    request.wait_for_target = wait_for_target ? 1u : 0u;
    result = compositor_rpc(connection, &request, &reply);
    if (result < 0)
    {
        return result;
    }
    if (!reply.ready)
    {
        if (ready != 0)
        {
            *ready = 0;
        }
        return 0;
    }

    connection->pending_present_sequence = 0;
    return 0;
}

int desktop_compositor_get_timeline(
    struct desktop_compositor_connection *connection,
    struct savanxp_gpu_present_timeline *timeline)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int result;

    if (timeline == 0)
    {
        return -SAVANXP_EINVAL;
    }

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_GET_TIMELINE;
    result = compositor_rpc(connection, &request, &reply);
    if (result < 0)
    {
        return result;
    }

    *timeline = reply.timeline;
    return 0;
}

int desktop_compositor_enable_cursor(
    struct desktop_compositor_connection *connection,
    int cursor_x,
    int cursor_y)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int result;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_ENABLE_CURSOR;
    request.cursor_position.x = (uint32_t)cursor_x;
    request.cursor_position.y = (uint32_t)cursor_y;
    request.cursor_position.visible = 1u;
    result = compositor_rpc(connection, &request, &reply);
    if (result == 0 && connection != 0)
    {
        connection->cursor_enabled = 1;
        connection->cursor_x = cursor_x;
        connection->cursor_y = cursor_y;
        connection->cursor_visible = 1;
    }
    return result;
}

int desktop_compositor_move_cursor(
    struct desktop_compositor_connection *connection,
    int cursor_x,
    int cursor_y,
    int visible)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int result;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_MOVE_CURSOR;
    request.cursor_position.x = (uint32_t)cursor_x;
    request.cursor_position.y = (uint32_t)cursor_y;
    request.cursor_position.visible = visible ? 1u : 0u;
    result = compositor_rpc(connection, &request, &reply);
    if (result == 0 && connection != 0)
    {
        connection->cursor_x = cursor_x;
        connection->cursor_y = cursor_y;
        connection->cursor_visible = visible ? 1 : 0;
    }
    return result;
}
