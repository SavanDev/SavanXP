#include "libc.h"
#include "cursor_asset.h"
#include "desktop_session.h"
#include "desktop_menu.h"
#include "desktop_layout.h"
#include "desktop_render.h"

static const char *k_shellapp_path = "/bin/shellapp";

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
            cursor_pixels[(row * 64) + column] = k_desktop_cursor_pixels[(row * DESKTOP_CURSOR_WIDTH) + column];
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

static void reset_client(struct desktop_session *session)
{
    memset(&session->client, 0, sizeof(session->client));
    session->client.section_fd = -1;
    session->client.input_write_fd = -1;
    session->client.mouse_write_fd = -1;
    session->client.present_read_fd = -1;
    session->client.present_nonblocking = 0;
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
    return 0;
}

static int present_frame(struct desktop_session *session, const struct desktop_dirty_rect *dirty)
{
    struct savanxp_gpu_surface_present present = {
        .surface_id = session->display_surface_id,
        .x = dirty != 0 ? (uint32_t)dirty->x : 0,
        .y = dirty != 0 ? (uint32_t)dirty->y : 0,
        .width = dirty != 0 ? (uint32_t)dirty->width : session->gfx.info.width,
        .height = dirty != 0 ? (uint32_t)dirty->height : session->gfx.info.height,
    };
    struct savanxp_gpu_present_timeline timeline = {0};
    long result;

    result = gpu_present_surface_region(session->gpu_fd, &present);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_PRESENT_SURFACE_REGION", result);
    }
    result = gpu_get_present_timeline(session->gpu_fd, &timeline);
    if (result < 0)
    {
        return desktop_stage_failed("GPU_IOC_GET_PRESENT_TIMELINE", result);
    }
    session->pending_present_sequence = timeline.submitted_sequence;
    return 0;
}

static void destroy_client(struct desktop_session *session, int terminate_client)
{
    int status = 0;

    if (session->client.pid > 0)
    {
        if (terminate_client)
        {
            (void)kill((int)session->client.pid, SAVANXP_SIGKILL);
        }
        waitpid((int)session->client.pid, &status);
    }
    close_fd_if_needed(&session->client.input_write_fd);
    close_fd_if_needed(&session->client.mouse_write_fd);
    close_fd_if_needed(&session->client.present_read_fd);
    if (session->client.mapped_view != 0 && !result_is_error((long)session->client.mapped_view))
    {
        (void)unmap_view(session->client.mapped_view);
    }
    close_fd_if_needed(&session->client.section_fd);
    reset_client(session);
}

static int launch_client(struct desktop_session *session, const char *path)
{
    struct savanxp_gpu_client_surface_header *header;
    struct savanxp_fb_info client_info = {0};
    unsigned long section_size = 0;
    int input_pipe[2] = {-1, -1};
    int mouse_pipe[2] = {-1, -1};
    int present_pipe[2] = {-1, -1};
    const char *argv[2] = {path, 0};
    long pid;
    long result;

    reset_client(session);
    desktop_fill_client_surface_info(&session->gfx.info, &client_info);
    if (client_info.width == 0 || client_info.height == 0 || client_info.buffer_size == 0)
    {
        return -1;
    }
    section_size = (unsigned long)sizeof(*header) + client_info.buffer_size;
    session->client.section_fd = (int)section_create(section_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (session->client.section_fd < 0)
    {
        return -1;
    }
    session->client.mapped_view = map_view(session->client.section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)session->client.mapped_view))
    {
        close_fd_if_needed(&session->client.section_fd);
        return -1;
    }

    header = (struct savanxp_gpu_client_surface_header *)session->client.mapped_view;
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->pixels_offset = sizeof(*header);
    header->info = client_info;
    session->client.header = header;
    session->client.pixels = (uint32_t *)((unsigned char *)session->client.mapped_view + header->pixels_offset);
    memset(session->client.pixels, 0, client_info.buffer_size);

    if (pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || pipe(present_pipe) < 0)
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
        if (dup2(session->client.section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(present_pipe[1], 6) < 0)
        {
            exit(1);
        }

        close_fd_unless_target(&input_pipe[0], 4);
        close_fd_if_needed(&input_pipe[1]);
        close_fd_unless_target(&mouse_pipe[0], 5);
        close_fd_if_needed(&mouse_pipe[1]);
        close_fd_if_needed(&present_pipe[0]);
        close_fd_unless_target(&present_pipe[1], 6);
        close_fd_unless_target(&session->client.section_fd, 3);
        if (exec(path, argv, 1) < 0)
        {
            eprintf("desktop: exec failed for %s\n", path);
        }
        exit(1);
    }

    session->client.path = path;
    session->client.pid = pid;
    session->client.input_write_fd = input_pipe[1];
    session->client.mouse_write_fd = mouse_pipe[1];
    session->client.present_read_fd = present_pipe[0];
    result = fcntl(session->client.present_read_fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK);
    if (result >= 0)
    {
        session->client.present_nonblocking = 1;
    }
    else
    {
        eprintf("desktop: present pipe remained blocking (%s)\n", result_error_string(result));
    }
    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&present_pipe[1]);
    return 0;

fail:
    close_fd_if_needed(&input_pipe[0]);
    close_fd_if_needed(&input_pipe[1]);
    close_fd_if_needed(&mouse_pipe[0]);
    close_fd_if_needed(&mouse_pipe[1]);
    close_fd_if_needed(&present_pipe[0]);
    close_fd_if_needed(&present_pipe[1]);
    if (session->client.mapped_view != 0 && !result_is_error((long)session->client.mapped_view))
    {
        (void)unmap_view(session->client.mapped_view);
    }
    close_fd_if_needed(&session->client.section_fd);
    reset_client(session);
    return -1;
}

static int switch_client(struct desktop_session *session, const char *path)
{
    destroy_client(session, 1);
    return launch_client(session, path);
}

static int open_compositor_session(struct desktop_session *session)
{
    struct savanxp_gpu_mode mode = {0};
    struct savanxp_gpu_surface_import import_request = {0};
    long result = 0;

    memset(session, 0, sizeof(*session));
    session->gpu_fd = -1;
    session->input_fd = -1;
    session->mouse_fd = -1;
    session->display_section_fd = -1;
    session->hw_cursor_enabled = 0;
    session->pending_present_sequence = 0;
    reset_client(session);

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

    session->display_section_fd = (int)section_create(session->gfx.info.buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (session->display_section_fd < 0)
    {
        return desktop_stage_failed("section_create compositor surface", session->display_section_fd);
    }
    session->display_view = map_view(session->display_section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
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
    return 0;
}

static void close_compositor_session(struct desktop_session *session)
{
    destroy_client(session, 1);
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
    return switch_client(session, item->path) < 0 ? -1 : 0;
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

    if (open_compositor_session(&session) < 0 || launch_client(&session, k_shellapp_path) < 0)
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
        struct savanxp_pollfd poll_fds[3];
        int input_poll_index = -1;
        int mouse_poll_index = -1;
        int client_poll_index = -1;
        int poll_count = 0;
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
        if (session.client.present_read_fd >= 0)
        {
            client_poll_index = poll_count;
            poll_fds[poll_count].fd = session.client.present_read_fd;
            poll_fds[poll_count].events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
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
                    desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                    desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                    menu_open = !menu_open;
                    if (menu_open)
                    {
                        selected_index = 0;
                    }
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
                        int launch_result = launch_selected_item(&session, selected_index);
                        launch_requested = 1;
                        if (launch_result < 0)
                        {
                            puts_fd(2, "desktop: failed to switch client\n");
                        }
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
                else if (session.client.input_write_fd >= 0)
                {
                    (void)route_packet(session.client.input_write_fd, &key_event, sizeof(key_event));
                }
            }
        }

        if (mouse_poll_index >= 0 && (poll_fds[mouse_poll_index].revents & SAVANXP_POLLIN) != 0)
        {
            while ((count = read(session.mouse_fd, &mouse_event, sizeof(mouse_event))) == (long)sizeof(mouse_event))
            {
                uint32_t pressed_buttons = mouse_event.buttons;
                uint32_t left_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
                uint32_t right_pressed = pressed_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
                uint32_t left_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
                uint32_t right_was_pressed = last_buttons & SAVANXP_MOUSE_BUTTON_RIGHT;
                int taskbar_y = (int)session.gfx.info.height - DESKTOP_TASKBAR_HEIGHT;
                int previous_cursor_x = cursor_x;
                int previous_cursor_y = cursor_y;
                int previous_menu_open = menu_open;
                int previous_selected_index = selected_index;
                int previous_in_client = 0;
                int current_in_client = 0;
                int launch_requested = 0;

                cursor_x = desktop_clamp_int(cursor_x + mouse_event.delta_x, 0, (int)session.gfx.info.width - 1);
                cursor_y = desktop_clamp_int(cursor_y + mouse_event.delta_y, 0, (int)session.gfx.info.height - 1);
                previous_in_client = desktop_point_in_client_area(&session, previous_cursor_x, previous_cursor_y);
                current_in_client = desktop_point_in_client_area(&session, cursor_x, cursor_y);
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
                        int launch_result = launch_selected_item(&session, hovered);
                        launch_requested = 1;
                        if (launch_result < 0)
                        {
                            puts_fd(2, "desktop: failed to switch client\n");
                        }
                        menu_open = 0;
                        pressed_buttons = 0;
                    }
                    else if (menu_open)
                    {
                        menu_open = 0;
                    }
                    else if (session.client.mouse_write_fd >= 0 && current_in_client)
                    {
                        (void)route_packet(session.client.mouse_write_fd, &mouse_event, sizeof(mouse_event));
                    }
                }
                else if (!menu_open && session.client.mouse_write_fd >= 0 && (current_in_client || previous_in_client))
                {
                    (void)route_packet(session.client.mouse_write_fd, &mouse_event, sizeof(mouse_event));
                }

                if (right_pressed != 0 && right_was_pressed == 0 && menu_open)
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
                }
                if (previous_menu_open != menu_open || previous_selected_index != selected_index)
                {
                    desktop_dirty_rect_add_menu(&dirty, &session.gfx.info);
                    desktop_dirty_rect_add_taskbar(&dirty, &session.gfx.info);
                }
                if (launch_requested)
                {
                    desktop_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                }
                last_buttons = pressed_buttons;
            }
        }

        if (client_poll_index >= 0 && session.client.present_read_fd >= 0)
        {
            if ((poll_fds[client_poll_index].revents & SAVANXP_POLLHUP) != 0)
            {
                destroy_client(&session, 0);
                desktop_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
            }
            else if ((poll_fds[client_poll_index].revents & SAVANXP_POLLIN) != 0)
            {
                struct savanxp_gpu_client_present_packet packet;
                int client_failed = 0;

                do
                {
                    count = read(session.client.present_read_fd, &packet, sizeof(packet));
                    if (count == (long)sizeof(packet))
                    {
                        desktop_dirty_rect_add(
                            &dirty,
                            &session.gfx.info,
                            (int)packet.x,
                            (int)packet.y,
                            (int)packet.width,
                            (int)packet.height);
                    }
                    else if (count < 0)
                    {
                        if (result_error_code(count) == SAVANXP_EAGAIN)
                        {
                            break;
                        }
                        client_failed = 1;
                        break;
                    }
                    else if (count == 0)
                    {
                        client_failed = 1;
                        break;
                    }
                    else
                    {
                        client_failed = 1;
                        break;
                    }
                }
                while (session.client.present_nonblocking);

                if (client_failed)
                {
                    destroy_client(&session, 0);
                    desktop_dirty_rect_add_fullscreen(&dirty, &session.gfx.info);
                }
            }
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
            if (!dirty.valid || !frame_ready)
            {
                continue;
            }
        }
        desktop_draw_desktop_region(&session, cursor_x, cursor_y, menu_open, selected_index, &dirty);
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
