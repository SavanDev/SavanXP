/* First: renames the POSIX calls to their native versions (windowd_sys.h). */
#include "windowd_sys.h"

#include "windowd_compositor_client.h"

#include <stdio.h>

#include "sxn/sxn.h"

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

    ready = savanxp_poll(&poll_fd, 1, timeout_ms);
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
        savanxp_close(*fd);
        *fd = -1;
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
        result = savanxp_read(fd, cursor + offset, size - offset);
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
        result = savanxp_write(fd, cursor + offset, size - offset);
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

static int compositor_rpc(
    struct windowd_compositor_connection *connection,
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

void windowd_compositor_connection_init(struct windowd_compositor_connection *connection)
{
    if (connection == 0)
    {
        return;
    }

    memset(connection, 0, sizeof(*connection));
    connection->pid = -1;
    connection->request_fd = -1;
    connection->reply_fd = -1;
}

/* Starts a compositord of windowd's own, for the test modes that run without
 * a session, on a private port it names on the command line. */
static int spawn_own_compositor(struct windowd_compositor_connection *connection)
{
    const char *argv[3] = {k_compositord_path, 0, 0};
    long pid;

    snprintf(connection->port_name, sizeof(connection->port_name), "%s%ld",
        SAVANXP_COMPOSITOR_PORT_PREFIX, savanxp_getpid());
    argv[1] = connection->port_name;
    pid = spawn(k_compositord_path, argv, 2);
    if (pid < 0)
    {
        eprintf("windowd: cannot start %s (%s)\n", k_compositord_path, result_error_string(pid));
        return (int)pid;
    }
    connection->pid = pid;
    return 0;
}

/* Connects to `name`, waiting up to the RPC watchdog for it to exist when
 * windowd started that compositord itself. */
static int connect_compositor(struct windowd_compositor_connection *connection, const char *name, int wait)
{
    unsigned long deadline = uptime_ms() + SAVANXP_COMPOSITOR_RPC_TIMEOUT_MS;

    for (;;)
    {
        int32_t channel[2] = {-1, -1};
        long result = sxn_port_connect(name, channel);

        if (result == 0)
        {
            connection->request_fd = channel[0];
            connection->reply_fd = channel[1];
            return 0;
        }
        if (!wait || uptime_ms() >= deadline)
        {
            return (int)result;
        }
        sleep_ms(10);
    }
}

/*
 * Connects to compositord and runs INIT (compositor_protocol.h v4). In the
 * session compositord is already there, started by sessiond; `own_daemon`
 * starts one first, for windowd's test modes. The reply brings the mode and
 * the display section, which windowd maps and composes into.
 */
int windowd_compositor_open(struct windowd_compositor_connection *connection, int own_daemon)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    long section;
    int result;

    if (connection == 0)
    {
        return -SAVANXP_EINVAL;
    }
    windowd_compositor_connection_init(connection);

    if (own_daemon)
    {
        result = spawn_own_compositor(connection);
        if (result < 0)
        {
            return result;
        }
    }
    result = connect_compositor(
        connection, own_daemon ? connection->port_name : SAVANXP_COMPOSITOR_PORT_NAME, own_daemon);
    if (result < 0)
    {
        windowd_compositor_close(connection);
        return result;
    }
    connection->connected = 1;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_INIT;
    result = compositor_rpc(connection, &request, &reply);
    if (result < 0)
    {
        windowd_compositor_close(connection);
        return result;
    }
    connection->display_info = reply.fb_info;
    connection->gpu_info = reply.gpu_info;
    /* The native mode: the one to go back to after a fullscreen mode. */
    connection->requested_info = reply.fb_info;

    section = sxn_pipe_receive_handle(connection->reply_fd);
    if (section < 0)
    {
        windowd_compositor_close(connection);
        return (int)section;
    }
    connection->display_view = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    /* The mapping keeps the section alive; the handle is one less in the
     * 64-entry table. */
    (void)savanxp_close((int)section);
    if (result_is_error((long)connection->display_view))
    {
        result = (int)(long)connection->display_view;
        connection->display_view = 0;
        windowd_compositor_close(connection);
        return result;
    }
    connection->framebuffer = (uint32_t *)connection->display_view;
    return 0;
}

int windowd_compositor_connected(const struct windowd_compositor_connection *connection)
{
    return connection != 0 && connection->connected;
}

void windowd_compositor_close(struct windowd_compositor_connection *connection)
{
    int shut_down = 0;

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
        shut_down = compositor_rpc(connection, &request, &reply) == 0;
    }

    close_fd_if_needed(&connection->request_fd);
    close_fd_if_needed(&connection->reply_fd);

    /* compositord ends when its WM hangs up; one windowd started itself is
     * its child and is waited for. One that did not confirm the shutdown may
     * be wedged: kill it so the wait cannot block forever. */
    if (connection->pid > 0)
    {
        int status = 0;

        if (!shut_down)
        {
            (void)savanxp_kill((int)connection->pid, SAVANXP_SIGKILL);
        }
        (void)savanxp_waitpid((int)connection->pid, &status);
    }

    if (connection->display_view != 0 && !result_is_error((long)connection->display_view))
    {
        (void)unmap_view(connection->display_view);
    }
    windowd_compositor_connection_init(connection);
}

int windowd_compositor_present(
    struct windowd_compositor_connection *connection,
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
        connection->last_present_timing.service_ns += reply.service_timing.service_ns;
        connection->last_present_timing.gpu_ns += reply.service_timing.gpu_ns;
        connection->last_present_timing.timeline_ns += reply.service_timing.timeline_ns;
    }

    return 0;
}

int windowd_compositor_sync_present(
    struct windowd_compositor_connection *connection,
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

int windowd_compositor_get_timeline(
    struct windowd_compositor_connection *connection,
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

static int compositor_request_mode(
    struct windowd_compositor_connection *connection,
    uint32_t width,
    uint32_t height,
    struct savanxp_compositor_reply *reply)
{
    struct savanxp_compositor_request request;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_SET_MODE;
    request.fb_info.width = width;
    request.fb_info.height = height;
    request.fb_info.bpp = 32u;
    return compositor_rpc(connection, &request, reply);
}

int windowd_compositor_set_mode(
    struct windowd_compositor_connection *connection,
    uint32_t width,
    uint32_t height)
{
    struct savanxp_compositor_reply reply;
    int result;

    if (connection == 0 || width == 0 || height == 0)
    {
        return -1;
    }

    result = compositor_request_mode(connection, width, height, &reply);
    if (result != 0)
    {
        return result;
    }

    /* The geometry that counts is the reply's, not the one asked for: the
     * device decides the pitch. compositord refuses a mode that would not fit
     * the display section. */
    connection->display_info = reply.fb_info;
    connection->gpu_info = reply.gpu_info;
    return 0;
}

int windowd_compositor_enable_cursor(
    struct windowd_compositor_connection *connection,
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
        connection->cursor_shape = SAVANXP_CURSOR_ARROW;
    }
    return result;
}

int windowd_compositor_move_cursor(
    struct windowd_compositor_connection *connection,
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

int windowd_compositor_set_cursor_shape(
    struct windowd_compositor_connection *connection,
    int shape)
{
    struct savanxp_compositor_request request;
    struct savanxp_compositor_reply reply;
    int result;

    memset(&request, 0, sizeof(request));
    request.type = SAVANXP_COMPOSITOR_MSG_SET_CURSOR_SHAPE;
    request.cursor_position.shape = (uint32_t)shape;
    result = compositor_rpc(connection, &request, &reply);
    if (result == 0 && connection != 0)
    {
        connection->cursor_shape = shape;
    }
    return result;
}
