#include "desktop_compositor_client.h"

static const char *k_compositord_path = "/bin/compositord";

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

static void close_daemon_setup_fd(int *fd)
{
    if (fd == 0)
    {
        return;
    }
    if (*fd > SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD)
    {
        close(*fd);
    }
    *fd = -1;
}

/*
 * The compositor child remaps its inherited handles onto descriptors 3..5.
 * Keep the display section allocated after both pipes so it cannot initially
 * occupy fd 3 and be clobbered by dup2(request_pipe[0], 3).
 */
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

int desktop_compositor_open(struct desktop_compositor_connection *connection)
{
    struct savanxp_fb_info requested_info;
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int request_pipe[2] = {-1, -1};
    int reply_pipe[2] = {-1, -1};
    const char *argv[2] = {k_compositord_path, 0};
    long pid;
    int result;

    if (connection == 0)
    {
        return -SAVANXP_EINVAL;
    }

    desktop_compositor_connection_init(connection);
    fill_default_display_info(&requested_info);

    if (pipe(request_pipe) < 0 || pipe(reply_pipe) < 0)
    {
        result = -SAVANXP_EIO;
        goto fail;
    }

    connection->display_section_fd = (int)section_create(
        requested_info.buffer_size,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (connection->display_section_fd < 0)
    {
        result = connection->display_section_fd;
        goto fail;
    }

    connection->display_view = map_view(
        connection->display_section_fd,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)connection->display_view))
    {
        result = (int)(long)connection->display_view;
        connection->display_view = 0;
        goto fail;
    }
    connection->framebuffer = (uint32_t *)connection->display_view;
    memset(connection->framebuffer, 0, requested_info.buffer_size);

    pid = fork();
    if (pid < 0)
    {
        result = (int)pid;
        goto fail;
    }
    if (pid == 0)
    {
        if (dup2(request_pipe[0], SAVANXP_COMPOSITOR_REQUEST_FD) < 0 ||
            dup2(reply_pipe[1], SAVANXP_COMPOSITOR_REPLY_FD) < 0 ||
            dup2(connection->display_section_fd, SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD) < 0)
        {
            exit(1);
        }

        close_daemon_setup_fd(&request_pipe[0]);
        close_daemon_setup_fd(&request_pipe[1]);
        close_daemon_setup_fd(&reply_pipe[0]);
        close_daemon_setup_fd(&reply_pipe[1]);
        close_daemon_setup_fd(&connection->display_section_fd);

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
    close_fd_if_needed(&request_pipe[0]);
    close_fd_if_needed(&reply_pipe[1]);

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_INIT;
    request.fb_info = requested_info;
    result = compositor_rpc(connection, &request, &reply);
    if (result < 0)
    {
        goto fail;
    }

    connection->display_info = reply.fb_info;
    connection->gpu_info = reply.gpu_info;
    if (connection->display_info.buffer_size > requested_info.buffer_size)
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
    desktop_compositor_close(connection);
    return result;
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

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_ENABLE_CURSOR;
    request.cursor_position.x = (uint32_t)cursor_x;
    request.cursor_position.y = (uint32_t)cursor_y;
    request.cursor_position.visible = 1u;
    return compositor_rpc(connection, &request, &reply);
}

int desktop_compositor_move_cursor(
    struct desktop_compositor_connection *connection,
    int cursor_x,
    int cursor_y,
    int visible)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_MOVE_CURSOR;
    request.cursor_position.x = (uint32_t)cursor_x;
    request.cursor_position.y = (uint32_t)cursor_y;
    request.cursor_position.visible = visible ? 1u : 0u;
    return compositor_rpc(connection, &request, &reply);
}
