#include "libc.h"
#include "cursor_asset.h"
#include "desktop_session.h"
#include "desktop_menu.h"
#include "desktop_layout.h"
#include "desktop_render.h"

static const char *k_shellapp_path = "/bin/shellapp";

static void close_fd_if_needed(int *fd)
{
    if (fd != 0 && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

static void close_fd_unless_target(int *fd, int target_fd)
{
    if (fd != 0 && *fd >= 0 && *fd != target_fd)
    {
        close(*fd);
        *fd = -1;
    }
}

static void reset_client(struct desktop_client *client)
{
    if (client == 0)
    {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->section_fd = -1;
    client->input_write_fd = -1;
    client->mouse_write_fd = -1;
    client->submit_event_fd = -1;
    client->retire_event_fd = -1;
    client->shutdown_event_fd = -1;
}

static int overlay_slot_valid(int slot)
{
    return slot >= 0 && slot < DESKTOP_MAX_OVERLAY_CLIENTS;
}

static struct desktop_client *overlay_client_at(struct desktop_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot))
    {
        return 0;
    }
    return &session->overlay_clients[slot];
}

static const struct desktop_client *overlay_client_at_const(const struct desktop_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot))
    {
        return 0;
    }
    return &session->overlay_clients[slot];
}

static int overlay_slot_for_client_ptr(const struct desktop_session *session, const struct desktop_client *client)
{
    int slot;

    if (session == 0 || client == 0)
    {
        return -1;
    }

    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        if (client == &session->overlay_clients[slot])
        {
            return slot;
        }
    }
    return -1;
}

static int find_free_overlay_slot(const struct desktop_session *session)
{
    int slot;

    if (session == 0)
    {
        return -1;
    }

    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        if (session->overlay_clients[slot].pid <= 0)
        {
            return slot;
        }
    }
    return -1;
}

static void remove_overlay_from_order(struct desktop_session *session, int slot)
{
    int index;

    if (session == 0 || !overlay_slot_valid(slot))
    {
        return;
    }

    for (index = 0; index < session->overlay_count; ++index)
    {
        if (session->overlay_order[index] == slot)
        {
            for (; index + 1 < session->overlay_count; ++index)
            {
                session->overlay_order[index] = session->overlay_order[index + 1];
            }
            session->overlay_order[session->overlay_count - 1] = -1;
            session->overlay_count -= 1;
            break;
        }
    }
}

static void append_overlay_to_order(struct desktop_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot))
    {
        return;
    }

    remove_overlay_from_order(session, slot);
    if (session->overlay_count >= DESKTOP_MAX_OVERLAY_CLIENTS)
    {
        return;
    }
    session->overlay_order[session->overlay_count++] = slot;
}

static void refresh_active_state(struct desktop_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    if (!overlay_slot_valid(session->active_overlay_slot) ||
        session->overlay_clients[session->active_overlay_slot].pid <= 0)
    {
        session->active_overlay_slot = -1;
    }

    if (session->active_client_kind == DESKTOP_CLIENT_APP && session->active_overlay_slot < 0)
    {
        session->active_client_kind = DESKTOP_CLIENT_SHELL;
    }

    if (session->active_client_kind == DESKTOP_CLIENT_SHELL && session->overlay_count > 0 && session->shell_client.pid <= 0)
    {
        session->active_client_kind = DESKTOP_CLIENT_APP;
        session->active_overlay_slot = session->overlay_order[session->overlay_count - 1];
    }

    if (session->active_client_kind == DESKTOP_CLIENT_APP && session->active_overlay_slot < 0 && session->overlay_count > 0)
    {
        session->active_overlay_slot = session->overlay_order[session->overlay_count - 1];
    }

    if (session->active_client_kind == DESKTOP_CLIENT_SHELL)
    {
        session->active_overlay_slot = -1;
    }

    session->shell_client.active = session->shell_client.pid > 0 && session->active_client_kind == DESKTOP_CLIENT_SHELL;
    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        session->overlay_clients[slot].active =
            session->overlay_clients[slot].pid > 0 &&
            session->active_client_kind == DESKTOP_CLIENT_APP &&
            slot == session->active_overlay_slot;
    }
}

static void activate_shell(struct desktop_session *session)
{
    if (session == 0)
    {
        return;
    }

    session->active_client_kind = DESKTOP_CLIENT_SHELL;
    session->active_overlay_slot = -1;
    refresh_active_state(session);
}

static void raise_overlay(struct desktop_session *session, int slot)
{
    if (session == 0 || !overlay_slot_valid(slot) || session->overlay_clients[slot].pid <= 0)
    {
        refresh_active_state(session);
        return;
    }

    append_overlay_to_order(session, slot);
    session->active_client_kind = DESKTOP_CLIENT_APP;
    session->active_overlay_slot = slot;
    refresh_active_state(session);
}

static struct desktop_client *active_client(struct desktop_session *session)
{
    if (session == 0)
    {
        return 0;
    }
    if (session->active_client_kind == DESKTOP_CLIENT_APP && overlay_slot_valid(session->active_overlay_slot))
    {
        return &session->overlay_clients[session->active_overlay_slot];
    }
    return session->shell_client.pid > 0 ? &session->shell_client : 0;
}

static int drag_overlay_slot_active(const struct desktop_session *session, int slot)
{
    return session != 0 && overlay_slot_valid(slot) && session->overlay_clients[slot].pid > 0;
}

static void move_overlay_client_window(
    struct desktop_session *session,
    struct desktop_dirty_rect *dirty,
    int slot,
    int window_x,
    int window_y)
{
    struct desktop_client *client = overlay_client_at(session, slot);
    struct sx_rect previous_frame;
    struct sx_rect current_frame;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }

    previous_frame = desktop_client_frame_rect(client);
    desktop_clamp_overlay_frame_position(
        &session->gfx.info,
        previous_frame.width,
        previous_frame.height,
        &window_x,
        &window_y);
    if (client->window_x == window_x && client->window_y == window_y)
    {
        return;
    }

    client->window_x = window_x;
    client->window_y = window_y;
    current_frame = desktop_client_frame_rect(client);
    desktop_dirty_rect_add(
        dirty,
        &session->gfx.info,
        previous_frame.x,
        previous_frame.y,
        previous_frame.width,
        previous_frame.height);
    desktop_dirty_rect_add(
        dirty,
        &session->gfx.info,
        current_frame.x,
        current_frame.y,
        current_frame.width,
        current_frame.height);
}

static const struct desktop_client *top_overlay_client_at_point(const struct desktop_session *session, int x, int y)
{
    int order_index;

    if (session == 0)
    {
        return 0;
    }

    for (order_index = session->overlay_count - 1; order_index >= 0; --order_index)
    {
        int slot = session->overlay_order[order_index];
        const struct desktop_client *client = overlay_client_at_const(session, slot);
        if (client != 0 && client->pid > 0 && desktop_point_in_frame(client, x, y))
        {
            return client;
        }
    }
    return 0;
}

static const struct desktop_client *top_client_at_point(const struct desktop_session *session, int x, int y)
{
    const struct desktop_client *overlay = top_overlay_client_at_point(session, x, y);

    if (overlay != 0)
    {
        return overlay;
    }
    if (session != 0 && session->shell_client.pid > 0 && desktop_point_in_client(&session->shell_client, x, y))
    {
        return &session->shell_client;
    }
    return 0;
}

static int set_hw_cursor_position(struct desktop_session *session, int cursor_x, int cursor_y, int visible)
{
    struct savanxp_gpu_cursor_position position;

    if (session == 0 || !session->hw_cursor_enabled || session->gpu_fd < 0)
    {
        return -1;
    }

    position.x = (uint32_t)cursor_x;
    position.y = (uint32_t)cursor_y;
    position.visible = visible ? 1u : 0u;
    position.reserved0 = 0;
    return gpu_move_cursor(session->gpu_fd, &position) < 0 ? -1 : 0;
}

static int try_enable_hw_cursor(struct desktop_session *session, int cursor_x, int cursor_y)
{
    static uint32_t cursor_pixels[64 * 64];
    struct savanxp_gpu_cursor_image image;
    int row;
    int column;

    if (session == 0 || session->gpu_fd < 0)
    {
        return 0;
    }
    if ((session->gpu_info.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) == 0)
    {
        return 0;
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
    if (gpu_set_cursor(session->gpu_fd, &image) < 0)
    {
        return 0;
    }

    session->hw_cursor_enabled = 1;
    if (set_hw_cursor_position(session, cursor_x, cursor_y, 1) < 0)
    {
        session->hw_cursor_enabled = 0;
        return 0;
    }
    return 1;
}

static int desktop_stage_failed(const char *stage, long result)
{
    if (result < 0)
    {
        eprintf("desktop: %s failed (%s)\n", stage, result_error_string(result));
    }
    else
    {
        eprintf("desktop: %s failed\n", stage);
    }
    return -1;
}

static int route_packet(int fd, const void *packet, size_t size)
{
    return fd >= 0 && write(fd, packet, size) == (long)size;
}

static int desktop_process_alive(long pid)
{
    struct savanxp_process_info info;
    unsigned long index = 0;

    if (pid <= 0)
    {
        return 0;
    }

    for (;;)
    {
        long result = proc_info(index, &info);
        if (result <= 0)
        {
            return 0;
        }
        if ((long)info.pid == pid && info.state != SAVANXP_PROC_ZOMBIE)
        {
            return 1;
        }
        ++index;
    }
}

static void add_client_present_damage(
    struct desktop_session *session,
    struct desktop_dirty_rect *dirty,
    const struct desktop_client *client,
    const struct savanxp_gpu_dirty_rect *rect)
{
    struct sx_rect surface_rect;

    if (session == 0 || dirty == 0 || client == 0 || rect == 0 || client->pid <= 0)
    {
        return;
    }

    surface_rect = desktop_client_surface_rect(client);
    desktop_dirty_rect_add(
        dirty,
        &session->gfx.info,
        surface_rect.x + (int)rect->x,
        surface_rect.y + (int)rect->y,
        (int)rect->width,
        (int)rect->height);
}

static void signal_client_retire(struct desktop_client *client, uint64_t retired_sequence)
{
    int advanced = 0;

    if (client == 0 || client->header == 0 || retired_sequence == 0)
    {
        return;
    }

    if (client->header->retired_sequence < retired_sequence)
    {
        client->header->retired_sequence = retired_sequence;
        advanced = 1;
    }
    if (advanced && client->retire_event_fd >= 0)
    {
        (void)event_set(client->retire_event_fd);
    }
}

static void retire_presented_batches(struct desktop_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    if (session->shell_client.pending_retire_sequence != 0)
    {
        signal_client_retire(&session->shell_client, session->shell_client.pending_retire_sequence);
        session->shell_client.pending_retire_sequence = 0;
    }

    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        struct desktop_client *client = &session->overlay_clients[slot];
        if (client->pending_retire_sequence != 0)
        {
            signal_client_retire(client, client->pending_retire_sequence);
            client->pending_retire_sequence = 0;
        }
    }
}

static int consume_client_present_batches(
    struct desktop_session *session,
    struct desktop_dirty_rect *dirty,
    struct desktop_client *client)
{
    uint64_t next_sequence = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->header == 0 || client->command_batches == 0)
    {
        return 0;
    }

    next_sequence = client->consumed_submit_sequence + 1u;
    while (next_sequence <= client->header->submit_sequence)
    {
        struct savanxp_gpu_dirty_rect_batch *batch = 0;
        uint32_t rect_index = 0;

        if (client->header->batch_capacity == 0)
        {
            return -1;
        }

        batch = &client->command_batches[(next_sequence - 1u) % client->header->batch_capacity];
        if (batch->submit_sequence != next_sequence ||
            batch->rect_count > client->header->rect_capacity ||
            batch->rect_count > SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS)
        {
            return -1;
        }

        if ((batch->flags & SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE) != 0)
        {
            struct sx_rect surface_rect = desktop_client_surface_rect(client);
            desktop_dirty_rect_add(
                dirty,
                &session->gfx.info,
                surface_rect.x,
                surface_rect.y,
                surface_rect.width,
                surface_rect.height);
        }
        else
        {
            for (rect_index = 0; rect_index < batch->rect_count; ++rect_index)
            {
                const struct savanxp_gpu_dirty_rect *rect = &batch->rects[rect_index];

                if (rect->width == 0 || rect->height == 0 ||
                    rect->x >= client->surface_info.width ||
                    rect->y >= client->surface_info.height ||
                    rect->width > (client->surface_info.width - rect->x) ||
                    rect->height > (client->surface_info.height - rect->y))
                {
                    return -1;
                }
                add_client_present_damage(session, dirty, client, rect);
            }
        }

        client->consumed_submit_sequence = next_sequence;
        next_sequence += 1u;
    }

    if (client->submit_event_fd >= 0 &&
        client->consumed_submit_sequence >= client->header->submit_sequence)
    {
        (void)event_reset(client->submit_event_fd);
    }

    return 0;
}

static void snapshot_pending_retire_sequences(struct desktop_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    session->shell_client.pending_retire_sequence = session->shell_client.consumed_submit_sequence;
    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        session->overlay_clients[slot].pending_retire_sequence = session->overlay_clients[slot].consumed_submit_sequence;
    }
}

static int sync_pending_present(struct desktop_session *session, int wait_for_target, int *ready)
{
    struct savanxp_gpu_present_wait wait_request = {0};
    struct savanxp_gpu_present_timeline timeline = {0};
    long result;

    if (ready != 0)
    {
        *ready = 1;
    }
    if (session == 0 || session->gpu_fd < 0 || session->pending_present_sequence == 0)
    {
        return 0;
    }

    if (!wait_for_target)
    {
        result = gpu_get_present_timeline(session->gpu_fd, &timeline);
        if (result < 0)
        {
            return desktop_stage_failed("GPU_IOC_GET_PRESENT_TIMELINE", result);
        }
        if (timeline.retired_sequence < session->pending_present_sequence)
        {
            if (ready != 0)
            {
                *ready = 0;
            }
            return 0;
        }
    }

    wait_request.target_sequence = session->pending_present_sequence;
    result = gpu_wait_present(session->gpu_fd, &wait_request);
    session->pending_present_sequence = 0;
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_WAIT_PRESENT", result);
    }
    if ((wait_request.flags & SAVANXP_GPU_PRESENT_TIMELINE_FLAG_TARGET_FAILED) != 0)
    {
        return desktop_stage_failed("GPU wait target failed", -SAVANXP_EIO);
    }
    retire_presented_batches(session);
    return 0;
}

static int present_frame(struct desktop_session *session, const struct desktop_dirty_rect *dirty)
{
    struct savanxp_gpu_surface_present_batch batch;
    struct savanxp_gpu_present_timeline timeline = {0};
    size_t index = 0;
    long result;

    if (session == 0 || dirty == 0 || !desktop_dirty_rect_valid(dirty))
    {
        return 0;
    }

    snapshot_pending_retire_sequences(session);
    memset(&batch, 0, sizeof(batch));
    batch.surface_id = session->display_surface_id;
    batch.present_cookie = session->pending_present_sequence + 1u;

    for (index = 0; index < desktop_dirty_rect_count(dirty); ++index)
    {
        const struct sx_rect *rect = desktop_dirty_rect_at(dirty, index);

        if (rect == 0 || rect->width <= 0 || rect->height <= 0)
        {
            continue;
        }

        batch.rects[batch.rect_count].x = (uint32_t)rect->x;
        batch.rects[batch.rect_count].y = (uint32_t)rect->y;
        batch.rects[batch.rect_count].width = (uint32_t)rect->width;
        batch.rects[batch.rect_count].height = (uint32_t)rect->height;
        batch.rect_count += 1u;

        if (batch.rect_count >= SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS)
        {
            result = gpu_present_surface_batch(session->gpu_fd, &batch);
            if (result < 0)
            {
                return desktop_stage_failed("GPU_IOC_PRESENT_SURFACE_BATCH", result);
            }
            batch.rect_count = 0;
            batch.present_cookie += 1u;
        }
    }

    if (batch.rect_count != 0)
    {
        result = gpu_present_surface_batch(session->gpu_fd, &batch);
        if (result < 0)
        {
            return desktop_stage_failed("GPU_IOC_PRESENT_SURFACE_BATCH", result);
        }
    }
    else
    {
        return 0;
    }

    result = gpu_get_present_timeline(session->gpu_fd, &timeline);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_GET_PRESENT_TIMELINE", result);
    }
    session->pending_present_sequence = timeline.submitted_sequence;
    return 0;
}

static void fill_client_surface_info(
    const struct desktop_session *session,
    enum desktop_client_kind kind,
    struct savanxp_fb_info *client_info)
{
    if (session == 0 || client_info == 0)
    {
        return;
    }

    if (kind == DESKTOP_CLIENT_APP)
    {
        desktop_fill_overlay_surface_info(&session->gfx.info, client_info);
    }
    else
    {
        desktop_fill_shell_surface_info(&session->gfx.info, client_info);
    }
}

static void position_client_window(
    const struct desktop_session *session,
    struct desktop_client *client,
    enum desktop_client_kind kind,
    int cascade_index)
{
    if (session == 0 || client == 0)
    {
        return;
    }

    if (kind == DESKTOP_CLIENT_APP)
    {
        desktop_place_overlay_window(
            &session->gfx.info,
            &client->surface_info,
            cascade_index,
            &client->window_x,
            &client->window_y,
            &client->window_width,
            &client->window_height);
        client->frame_visible = 1;
    }
    else
    {
        client->window_x = 0;
        client->window_y = 0;
        client->window_width = (int)client->surface_info.width;
        client->window_height = (int)client->surface_info.height;
        client->frame_visible = 0;
    }
}

static void destroy_client_instance(struct desktop_client *client, int terminate_client)
{
    int status = 0;

    if (client == 0)
    {
        return;
    }

    if (client->shutdown_event_fd >= 0)
    {
        (void)event_set(client->shutdown_event_fd);
    }
    if (client->pid > 0)
    {
        if (terminate_client)
        {
            (void)kill((int)client->pid, SAVANXP_SIGKILL);
        }
        (void)waitpid((int)client->pid, &status);
    }
    close_fd_if_needed(&client->input_write_fd);
    close_fd_if_needed(&client->mouse_write_fd);
    close_fd_if_needed(&client->submit_event_fd);
    close_fd_if_needed(&client->retire_event_fd);
    close_fd_if_needed(&client->shutdown_event_fd);
    if (client->mapped_view != 0 && !result_is_error((long)client->mapped_view))
    {
        (void)unmap_view(client->mapped_view);
    }
    close_fd_if_needed(&client->section_fd);
    reset_client(client);
}

static int start_client_process(struct desktop_client *client, const char *path)
{
    struct savanxp_gpu_client_surface_header *header;
    unsigned long command_bytes = 0;
    unsigned long section_size = 0;
    int input_pipe[2] = {-1, -1};
    int mouse_pipe[2] = {-1, -1};
    int submit_event = -1;
    int retire_event = -1;
    int shutdown_event = -1;
    const char *argv[2] = {path, 0};
    long pid;

    if (client == 0 || path == 0 ||
        client->surface_info.width == 0 || client->surface_info.height == 0 || client->surface_info.buffer_size == 0)
    {
        return -1;
    }

    command_bytes = (unsigned long)(SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch));
    section_size = (unsigned long)sizeof(*header) + command_bytes + client->surface_info.buffer_size;
    client->section_fd = (int)section_create(section_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (client->section_fd < 0)
    {
        return -1;
    }
    client->mapped_view = map_view(client->section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)client->mapped_view))
    {
        close_fd_if_needed(&client->section_fd);
        return -1;
    }

    header = (struct savanxp_gpu_client_surface_header *)client->mapped_view;
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->command_offset = (uint32_t)sizeof(*header);
    header->pixels_offset = (uint32_t)(sizeof(*header) + command_bytes);
    header->info = client->surface_info;
    header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_3;
    header->flags = 0;
    header->pixel_format = SAVANXP_GPU_SURFACE_FORMAT_BGRX8888;
    header->reserved0 = 0;
    header->batch_capacity = SAVANXP_GPU_CLIENT_BATCH_CAPACITY;
    header->rect_capacity = SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    header->reserved1 = 0;
    header->submit_sequence = 0;
    header->retired_sequence = 0;
    client->header = header;
    client->command_batches = (struct savanxp_gpu_dirty_rect_batch *)((unsigned char *)client->mapped_view + header->command_offset);
    client->pixels = (uint32_t *)((unsigned char *)client->mapped_view + header->pixels_offset);
    memset(client->command_batches, 0, command_bytes);
    memset(client->pixels, 0, client->surface_info.buffer_size);

    submit_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    retire_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    shutdown_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || submit_event < 0 || retire_event < 0 || shutdown_event < 0)
    {
        goto fail;
    }

    pid = fork();
    if (pid < 0)
    {
        goto fail;
    }
    if (pid == 0)
    {
        if (dup2(client->section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(submit_event, 6) < 0 ||
            dup2(retire_event, 7) < 0 ||
            dup2(shutdown_event, 8) < 0)
        {
            exit(1);
        }

        close_fd_unless_target(&input_pipe[0], 4);
        close_fd_if_needed(&input_pipe[1]);
        close_fd_unless_target(&mouse_pipe[0], 5);
        close_fd_if_needed(&mouse_pipe[1]);
        close_fd_unless_target(&submit_event, 6);
        close_fd_unless_target(&retire_event, 7);
        close_fd_unless_target(&shutdown_event, 8);
        close_fd_unless_target(&client->section_fd, 3);
        if (exec(path, argv, 1) < 0)
        {
            eprintf("desktop: exec failed for %s\n", path);
        }
        exit(1);
    }

    client->path = path;
    client->pid = pid;
    client->input_write_fd = input_pipe[1];
    client->mouse_write_fd = mouse_pipe[1];
    client->submit_event_fd = submit_event;
    client->retire_event_fd = retire_event;
    client->shutdown_event_fd = shutdown_event;

    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&mouse_pipe[0]);
    return 0;

fail:
    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&input_pipe[1]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&mouse_pipe[1]);
    close_fd_if_needed(&submit_event);
    close_fd_if_needed(&retire_event);
    close_fd_if_needed(&shutdown_event);
    destroy_client_instance(client, 0);
    return -1;
}

static void destroy_shell_client(struct desktop_session *session, int terminate_client)
{
    if (session == 0)
    {
        return;
    }

    destroy_client_instance(&session->shell_client, terminate_client);
    activate_shell(session);
}

static void destroy_overlay_client(struct desktop_session *session, int slot, int terminate_client)
{
    struct desktop_client *client = overlay_client_at(session, slot);

    if (client == 0)
    {
        return;
    }

    destroy_client_instance(client, terminate_client);
    remove_overlay_from_order(session, slot);
    refresh_active_state(session);
}

static int launch_shell_client(struct desktop_session *session, const char *path)
{
    struct desktop_client *client = 0;

    if (session == 0 || path == 0)
    {
        return -1;
    }

    destroy_shell_client(session, 1);
    client = &session->shell_client;
    reset_client(client);
    fill_client_surface_info(session, DESKTOP_CLIENT_SHELL, &client->surface_info);
    position_client_window(session, client, DESKTOP_CLIENT_SHELL, 0);
    if (start_client_process(client, path) < 0)
    {
        return -1;
    }
    activate_shell(session);
    return 0;
}

static int launch_overlay_client(struct desktop_session *session, const char *path)
{
    struct desktop_client *client = 0;
    int slot = -1;

    if (session == 0 || path == 0)
    {
        return -1;
    }

    slot = find_free_overlay_slot(session);
    if (!overlay_slot_valid(slot))
    {
        eprintf("desktop: no free overlay slots for %s\n", path);
        return -1;
    }

    client = &session->overlay_clients[slot];
    reset_client(client);
    fill_client_surface_info(session, DESKTOP_CLIENT_APP, &client->surface_info);
    position_client_window(session, client, DESKTOP_CLIENT_APP, session->overlay_count);
    if (start_client_process(client, path) < 0)
    {
        return -1;
    }
    append_overlay_to_order(session, slot);
    raise_overlay(session, slot);
    return 0;
}

static int relaunch_shell_client(struct desktop_session *session)
{
    return launch_shell_client(session, k_shellapp_path);
}

static int open_compositor_session(struct desktop_session *session)
{
    struct savanxp_gpu_mode mode = {0};
    struct savanxp_gpu_surface_import import_request = {0};
    long result = 0;
    int slot;

    memset(session, 0, sizeof(*session));
    session->gpu_fd = -1;
    session->input_fd = -1;
    session->mouse_fd = -1;
    session->display_section_fd = -1;
    session->hw_cursor_enabled = 0;
    session->pending_present_sequence = 0;
    session->active_client_kind = DESKTOP_CLIENT_SHELL;
    session->active_overlay_slot = -1;
    session->overlay_count = 0;
    reset_client(&session->shell_client);
    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        reset_client(&session->overlay_clients[slot]);
        session->overlay_order[slot] = -1;
    }

    session->gpu_fd = (int)gpu_open();
    if (session->gpu_fd < 0)
    {
        return desktop_stage_failed("open /dev/gpu0", session->gpu_fd);
    }
    result = gpu_get_info(session->gpu_fd, &session->gpu_info);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_GET_INFO", result);
    }
    result = gpu_acquire(session->gpu_fd);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_ACQUIRE", result);
    }
    result = gpu_set_mode(session->gpu_fd, &mode);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_SET_MODE", result);
    }

    session->gfx.info.width = mode.width;
    session->gfx.info.height = mode.height;
    session->gfx.info.pitch = mode.pitch;
    session->gfx.info.bpp = mode.bpp;
    session->gfx.info.buffer_size = mode.buffer_size;
    session->input_fd = (int)open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (session->input_fd < 0)
    {
        return desktop_stage_failed("open /dev/input0", session->input_fd);
    }

    session->mouse_fd = (int)open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
    if (session->mouse_fd < 0)
    {
        eprintf("desktop: /dev/mouse0 unavailable (%s), continuing keyboard-only\n", result_error_string(session->mouse_fd));
        session->mouse_fd = -1;
    }

    session->display_section_fd = (int)section_create(
        session->gfx.info.buffer_size,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (session->display_section_fd < 0)
    {
        return desktop_stage_failed("section_create compositor surface", session->display_section_fd);
    }
    session->display_view = map_view(
        session->display_section_fd,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)session->display_view))
    {
        return desktop_stage_failed("map_view compositor surface", (long)session->display_view);
    }
    session->display_pixels = (uint32_t *)session->display_view;
    memset(session->display_pixels, 0, session->gfx.info.buffer_size);
    desktop_set_backbuffer(session->display_pixels);

    import_request.section_handle = session->display_section_fd;
    import_request.width = session->gfx.info.width;
    import_request.height = session->gfx.info.height;
    import_request.pitch = session->gfx.info.pitch;
    import_request.bpp = session->gfx.info.bpp;
    import_request.buffer_size = session->gfx.info.buffer_size;
    import_request.flags = SAVANXP_GPU_SURFACE_FLAG_SCANOUT;
    result = gpu_import_section(session->gpu_fd, &import_request);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_IMPORT_SECTION", result);
    }
    session->display_surface_id = (uint32_t)import_request.surface_id;
    refresh_active_state(session);
    return 0;
}

static void close_compositor_session(struct desktop_session *session)
{
    int slot;

    if (session == 0)
    {
        return;
    }

    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        destroy_overlay_client(session, slot, 1);
    }
    destroy_shell_client(session, 1);
    if (session->display_surface_id != 0 && session->gpu_fd >= 0)
    {
        (void)gpu_release_surface(session->gpu_fd, session->display_surface_id);
    }
    if (session->display_view != 0 && !result_is_error((long)session->display_view))
    {
        (void)unmap_view(session->display_view);
    }
    close_fd_if_needed(&session->display_section_fd);
    close_fd_if_needed(&session->input_fd);
    close_fd_if_needed(&session->mouse_fd);
    if (session->gpu_fd >= 0)
    {
        (void)sync_pending_present(session, 1, 0);
        (void)gpu_release(session->gpu_fd);
        close_fd_if_needed(&session->gpu_fd);
    }
    desktop_set_backbuffer(0);
}

static int launch_selected_item(struct desktop_session *session, int index)
{
    const struct desktop_menu_item *item = desktop_menu_item_at(index);

    if (item == 0)
    {
        return 0;
    }
    return launch_overlay_client(session, item->path) < 0 ? -1 : 0;
}

static int service_client_batches(
    struct desktop_session *session,
    struct desktop_dirty_rect *dirty,
    struct desktop_client *client)
{
    int slot = -1;
    int is_shell = 0;

    if (session == 0 || dirty == 0 || client == 0 || client->pid <= 0)
    {
        return 0;
    }

    slot = overlay_slot_for_client_ptr(session, client);
    is_shell = client == &session->shell_client;
    if (consume_client_present_batches(session, dirty, client) < 0)
    {
        if (is_shell)
        {
            destroy_shell_client(session, 1);
            if (relaunch_shell_client(session) < 0)
            {
                puts_fd(2, "desktop: failed to relaunch shellapp\n");
                return -1;
            }
        }
        else if (overlay_slot_valid(slot))
        {
            destroy_overlay_client(session, slot, 1);
        }
        desktop_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    return 0;
}

static int reap_dead_clients(struct desktop_session *session, struct desktop_dirty_rect *dirty)
{
    int slot;

    if (session == 0 || dirty == 0)
    {
        return 0;
    }

    if (session->shell_client.pid > 0 && !desktop_process_alive(session->shell_client.pid))
    {
        destroy_shell_client(session, 0);
        if (relaunch_shell_client(session) < 0)
        {
            puts_fd(2, "desktop: failed to relaunch shellapp\n");
            return -1;
        }
        desktop_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
    }

    for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
    {
        struct desktop_client *client = &session->overlay_clients[slot];
        if (client->pid > 0 && !desktop_process_alive(client->pid))
        {
            destroy_overlay_client(session, slot, 0);
            desktop_dirty_rect_add_fullscreen(dirty, &session->gfx.info);
        }
    }

    return 0;
}

int main(void)
{
    struct desktop_session session;
    struct savanxp_input_event key_event = {0};
    struct savanxp_mouse_event mouse_event = {0};
    struct desktop_dirty_rect dirty = {0};
    int cursor_x = 24;
    int cursor_y = 24;
    int menu_open = 0;
    int selected_index = 0;
    uint32_t last_buttons = 0;
    unsigned long last_clock_stamp = 0;
    int drag_overlay_slot = -1;
    int drag_offset_x = 0;
    int drag_offset_y = 0;

    if (open_compositor_session(&session) < 0)
    {
        puts_fd(2, "desktop: compositor startup failed\n");
        close_compositor_session(&session);
        return 1;
    }
    (void)try_enable_hw_cursor(&session, cursor_x, cursor_y);

    {
        char clock_text[6];
        last_clock_stamp = desktop_current_clock_stamp(clock_text);
    }
    desktop_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);

    for (;;)
    {
        struct savanxp_pollfd poll_fds[2];
        int input_poll_index = -1;
        int mouse_poll_index = -1;
        int poll_count = 0;
        int slot;
        long count = 0;

        input_poll_index = poll_count;
        poll_fds[poll_count].fd = session.input_fd;
        poll_fds[poll_count].events = SAVANXP_POLLIN;
        poll_fds[poll_count].revents = 0;
        ++poll_count;

        if (session.mouse_fd >= 0)
        {
            mouse_poll_index = poll_count;
            poll_fds[poll_count].fd = session.mouse_fd;
            poll_fds[poll_count].events = SAVANXP_POLLIN;
            poll_fds[poll_count].revents = 0;
            ++poll_count;
        }

        if (poll(poll_fds, (unsigned long)poll_count, 16) < 0)
        {
            break;
        }

        if (input_poll_index >= 0 && (poll_fds[input_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = read(session.input_fd, &key_event, sizeof(key_event))) == (long)sizeof(key_event))
            {
                if (key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && key_event.key == SAVANXP_KEY_SUPER)
                {
                    menu_open = !menu_open;
                    if (menu_open)
                    {
                        selected_index = 0;
                    }
                    desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                    desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                }
                else if (menu_open && key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN)
                {
                    int launch_requested = 0;

                    desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                    if (key_event.key == SAVANXP_KEY_ESC)
                    {
                        menu_open = 0;
                    }
                    else if (key_event.key == SAVANXP_KEY_UP)
                    {
                        selected_index = (selected_index + desktop_menu_item_count() - 1) % desktop_menu_item_count();
                    }
                    else if (key_event.key == SAVANXP_KEY_DOWN)
                    {
                        selected_index = (selected_index + 1) % desktop_menu_item_count();
                    }
                    else if (key_event.key == SAVANXP_KEY_ENTER)
                    {
                        if (launch_selected_item(&session, selected_index) < 0)
                        {
                            puts_fd(2, "desktop: failed to launch selected client\n");
                        }
                        launch_requested = 1;
                        menu_open = 0;
                        last_buttons = 0;
                    }
                    desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                    if (menu_open)
                    {
                        desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                    }
                    if (launch_requested)
                    {
                        desktop_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                    }
                }
                else
                {
                    struct desktop_client *client = active_client(&session);
                    if (client != 0 && client->input_write_fd >= 0)
                    {
                        (void)route_packet(client->input_write_fd, &key_event, sizeof(key_event));
                    }
                }
            }
        }

        if (mouse_poll_index >= 0 && (poll_fds[mouse_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = read(session.mouse_fd, &mouse_event, sizeof(mouse_event))) == (long)sizeof(mouse_event))
            {
                const struct desktop_client *previous_hover_client = 0;
                const struct desktop_client *current_hover_client = 0;
                uint32_t pressed_buttons = mouse_event.buttons;
                uint32_t left_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
                uint32_t right_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
                uint32_t left_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
                int previous_cursor_x = cursor_x;
                int previous_cursor_y = cursor_y;
                int previous_menu_open = menu_open;
                int previous_selected_index = selected_index;
                int previous_active_kind = session.active_client_kind;
                int previous_active_overlay_slot = session.active_overlay_slot;
                int drag_was_active = 0;
                int drag_active_now = 0;
                int launch_requested = 0;
                int mouse_routed = 0;
                int taskbar_y = (int)session.gfx.info.height - DESKTOP_TASKBAR_HEIGHT;

                if (!drag_overlay_slot_active(&session, drag_overlay_slot))
                {
                    drag_overlay_slot = -1;
                }
                drag_was_active = drag_overlay_slot_active(&session, drag_overlay_slot);
                previous_hover_client = top_client_at_point(&session, cursor_x, cursor_y);
                cursor_x = desktop_clamp_int(cursor_x + mouse_event.delta_x, 0, (int)session.gfx.info.width - 1);
                cursor_y = desktop_clamp_int(cursor_y + mouse_event.delta_y, 0, (int)session.gfx.info.height - 1);
                current_hover_client = top_client_at_point(&session, cursor_x, cursor_y);

                if (menu_open)
                {
                    int hovered = desktop_selected_item_from_cursor(&session.gfx, cursor_x, cursor_y);
                    if (hovered >= 0)
                    {
                        selected_index = hovered;
                    }
                }

                if (left_pressed != 0 && left_was_pressed == 0)
                {
                    int hovered = menu_open ? desktop_selected_item_from_cursor(&session.gfx, cursor_x, cursor_y) : -1;

                    if (desktop_point_in_rect(cursor_x, cursor_y, 6, taskbar_y + 6, DESKTOP_START_BUTTON_WIDTH, DESKTOP_TASKBAR_HEIGHT - 12))
                    {
                        menu_open = !menu_open;
                        if (menu_open)
                        {
                            selected_index = 0;
                        }
                    }
                    else if (menu_open && hovered >= 0)
                    {
                        if (launch_selected_item(&session, hovered) < 0)
                        {
                            puts_fd(2, "desktop: failed to launch selected client\n");
                        }
                        launch_requested = 1;
                        menu_open = 0;
                        pressed_buttons = 0;
                    }
                    else if (menu_open)
                    {
                        menu_open = 0;
                    }
                    else if (current_hover_client != 0)
                    {
                        if (current_hover_client == &session.shell_client)
                        {
                            activate_shell(&session);
                        }
                        else
                        {
                            int target_slot = overlay_slot_for_client_ptr(&session, current_hover_client);
                            raise_overlay(&session, target_slot);
                            current_hover_client = overlay_client_at_const(&session, target_slot);
                        }
                        if (current_hover_client != 0 &&
                            current_hover_client != &session.shell_client &&
                            desktop_point_in_close_button(current_hover_client, cursor_x, cursor_y))
                        {
                            int target_slot = overlay_slot_for_client_ptr(&session, current_hover_client);
                            struct sx_rect closed_frame = desktop_client_frame_rect(current_hover_client);

                            if (overlay_slot_valid(target_slot))
                            {
                                destroy_overlay_client(&session, target_slot, 1);
                                desktop_dirty_rect_add(
                                    &dirty,
                                    &session.gfx.info,
                                    closed_frame.x,
                                    closed_frame.y,
                                    closed_frame.width,
                                    closed_frame.height);
                                current_hover_client = 0;
                                drag_overlay_slot = -1;
                            }
                        }
                        else if (current_hover_client != 0 &&
                                 current_hover_client != &session.shell_client &&
                                 desktop_point_in_titlebar(current_hover_client, cursor_x, cursor_y))
                        {
                            int target_slot = overlay_slot_for_client_ptr(&session, current_hover_client);
                            if (drag_overlay_slot_active(&session, target_slot))
                            {
                                drag_overlay_slot = target_slot;
                                drag_offset_x = cursor_x - current_hover_client->window_x;
                                drag_offset_y = cursor_y - current_hover_client->window_y;
                            }
                        }
                        else if (current_hover_client != 0 && current_hover_client->mouse_write_fd >= 0)
                        {
                            (void)route_packet(current_hover_client->mouse_write_fd, &mouse_event, sizeof(mouse_event));
                            mouse_routed = 1;
                        }
                    }
                }
                drag_active_now = drag_overlay_slot_active(&session, drag_overlay_slot);
                if (drag_active_now && left_pressed != 0)
                {
                    move_overlay_client_window(
                        &session,
                        &dirty,
                        drag_overlay_slot,
                        cursor_x - drag_offset_x,
                        cursor_y - drag_offset_y);
                }
                if (drag_was_active && left_pressed == 0)
                {
                    drag_overlay_slot = -1;
                    drag_active_now = 0;
                }

                if (!mouse_routed &&
                    !menu_open &&
                    !drag_was_active &&
                    !drag_active_now &&
                    current_hover_client != 0 &&
                    current_hover_client->mouse_write_fd >= 0 &&
                    !(left_pressed != 0 && left_was_pressed == 0))
                {
                    (void)route_packet(current_hover_client->mouse_write_fd, &mouse_event, sizeof(mouse_event));
                }
                else if (!mouse_routed &&
                         !menu_open &&
                         !drag_was_active &&
                         !drag_active_now &&
                         current_hover_client == 0 &&
                         previous_hover_client != 0 &&
                         previous_hover_client->mouse_write_fd >= 0 &&
                         !(left_pressed != 0 && left_was_pressed == 0))
                {
                    (void)route_packet(previous_hover_client->mouse_write_fd, &mouse_event, sizeof(mouse_event));
                }

                if (right_pressed != 0 && menu_open)
                {
                    menu_open = 0;
                }

                if (previous_cursor_x != cursor_x || previous_cursor_y != cursor_y)
                {
                    if (session.hw_cursor_enabled)
                    {
                        (void)set_hw_cursor_position(&session, cursor_x, cursor_y, 1);
                    }
                    else
                    {
                        desktop_dirty_rect_add_cursor(&dirty, &session.gfx.info, previous_cursor_x, previous_cursor_y);
                        desktop_dirty_rect_add_cursor(&dirty, &session.gfx.info, cursor_x, cursor_y);
                    }
                    if (menu_open)
                    {
                        int menu_x = 0;
                        int menu_y = 0;
                        int menu_width = 0;
                        int menu_height = 0;

                        desktop_start_menu_bounds(&session.gfx.info, &menu_x, &menu_y, &menu_width, &menu_height);
                        if (desktop_point_in_rect(previous_cursor_x, previous_cursor_y, menu_x, menu_y, menu_width, menu_height) ||
                            desktop_point_in_rect(cursor_x, cursor_y, menu_x, menu_y, menu_width, menu_height))
                        {
                            desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                        }
                    }
                }
                if (previous_menu_open != menu_open || previous_selected_index != selected_index)
                {
                    desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                    desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                }
                if (previous_active_kind != session.active_client_kind || previous_active_overlay_slot != session.active_overlay_slot)
                {
                    desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                    if (overlay_slot_valid(previous_active_overlay_slot))
                    {
                        desktop_dirty_rect_add_client(&dirty, overlay_client_at_const(&session, previous_active_overlay_slot));
                    }
                    if (overlay_slot_valid(session.active_overlay_slot))
                    {
                        desktop_dirty_rect_add_client(&dirty, overlay_client_at_const(&session, session.active_overlay_slot));
                    }
                }
                if (launch_requested)
                {
                    desktop_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                }
                last_buttons = pressed_buttons;
            }
        }

        if (reap_dead_clients(&session, &dirty) < 0)
        {
            break;
        }
        if (service_client_batches(&session, &dirty, &session.shell_client) < 0)
        {
            break;
        }
        for (slot = 0; slot < DESKTOP_MAX_OVERLAY_CLIENTS; ++slot)
        {
            if (service_client_batches(&session, &dirty, &session.overlay_clients[slot]) < 0)
            {
                break;
            }
        }
        if (slot < DESKTOP_MAX_OVERLAY_CLIENTS)
        {
            break;
        }

        {
            char clock_text[6];
            unsigned long clock_stamp = desktop_current_clock_stamp(clock_text);
            if (clock_stamp != last_clock_stamp)
            {
                last_clock_stamp = clock_stamp;
                desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
            }
        }

        {
            int frame_ready = 1;

            if (sync_pending_present(&session, 0, &frame_ready) < 0)
            {
                break;
            }
            if (!desktop_dirty_rect_valid(&dirty) || !frame_ready)
            {
                continue;
            }
        }

        desktop_draw_desktop(&session, cursor_x, cursor_y, menu_open, selected_index, &dirty);
        if (present_frame(&session, &dirty) < 0)
        {
            puts_fd(2, "desktop: present failed\n");
            break;
        }
        desktop_dirty_rect_reset(&dirty);
    }

    close_compositor_session(&session);
    return 1;
}
