/* The POSIX client of windowd: connection, surfaces, present, events,
 * launching and owned windows. The pixel and text primitives it draws with
 * are the platform's (system/gfx/pixels.c). */

enum {
    kGfxModeClient = 1,
};

static struct savanxp_gpu_dirty_rect_batch* gfx_batch_slot(const struct savanxp_gfx_context* context, uint64_t sequence) {
    if (context == 0 || context->command_batches == 0 || context->batch_capacity == 0 || sequence == 0) {
        return 0;
    }
    return &context->command_batches[(sequence - 1u) % context->batch_capacity];
}

static int gfx_header_dimensions_changed(const struct savanxp_gfx_context* context) {
    return context != 0 &&
        context->header != 0 &&
        (context->header->info.width != context->notified_width || context->header->info.height != context->notified_height);
}

static void gfx_refresh_notified_dimensions(struct savanxp_gfx_context* context) {
    if (context == 0 || context->header == 0) {
        return;
    }
    context->notified_width = context->header->info.width;
    context->notified_height = context->header->info.height;
}

static void gfx_copy_present_region(
    const struct savanxp_gfx_context* context,
    const uint32_t* pixels,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height) {
    const unsigned char* source = 0;
    unsigned char* destination = 0;
    size_t row_bytes = 0;
    uint32_t row = 0;

    if (context == 0 || context->shared_pixels == 0 || pixels == 0 || pixels == context->shared_pixels || width == 0 || height == 0) {
        return;
    }

    source = (const unsigned char*)pixels;
    destination = (unsigned char*)context->shared_pixels;
    row_bytes = (size_t)width * sizeof(uint32_t);
    for (row = 0; row < height; ++row) {
        memcpy(
            destination + ((size_t)(y + row) * context->info.pitch) + ((size_t)x * sizeof(uint32_t)),
            source + ((size_t)(y + row) * context->info.pitch) + ((size_t)x * sizeof(uint32_t)),
            row_bytes);
    }
}

/* 1 once the WM is gone: its end of the event pipe hung up. Nothing composes
 * the surface any more, so the window is as good as closed. */
static int g_gfx_wm_gone = 0;

static int gfx_shutdown_requested(const struct savanxp_gfx_context* context) {
    return context != 0 && context->header != 0 &&
        ((context->header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN) != 0 || g_gfx_wm_gone);
}

/* Looks at the event pipe for a hang-up. A poll, so only where waiting or
 * asking "should I close" already costs a call. */
static int gfx_check_wm_gone(const struct savanxp_gfx_context* context) {
    struct savanxp_pollfd pollfd;

    if (g_gfx_wm_gone || context == 0 || context->input_fd < 0) {
        return g_gfx_wm_gone;
    }
    pollfd.fd = context->input_fd;
    pollfd.events = 0;
    pollfd.revents = 0;
    if (savanxp_poll(&pollfd, 1, 0) > 0 && (pollfd.revents & SAVANXP_POLLHUP) != 0) {
        g_gfx_wm_gone = 1;
    }
    return g_gfx_wm_gone;
}

/*
 * The default answer to closing a window (DefWindowProc's WM_CLOSE): when the
 * WM asks the MAIN window to close, or goes away, the process ends at its next
 * poll or present. The WM no longer kills on close
 * (docs/desktop/DESKTOP_ARCHITECTURE.md), so this is what makes a program that
 * never looks at gfx_should_close still close. A program that wants to answer
 * itself -- ask to save, stop its audio first -- calls gfx_handle_close(1) and
 * then checks gfx_should_close. Owned windows (dialogs) never end the process.
 */
static int g_gfx_close_handled = 0;
static struct savanxp_gpu_client_surface_header* g_gfx_main_header = 0;

void gfx_handle_close(int handled) {
    g_gfx_close_handled = handled != 0;
}

static void gfx_default_close(void) {
    if (g_gfx_close_handled) {
        return;
    }
    if (g_gfx_wm_gone ||
        (g_gfx_main_header != 0 && (g_gfx_main_header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN) != 0)) {
        exit(0);
    }
}

/* How long a wait for the WM sleeps before looking whether it is still there. */
#define GFX_WM_ALIVE_CHECK_MS 250

/* Waits until `ready` holds, cut short by a close request or by the WM going
 * away. The wake event serves several conditions, so the order matters: reset,
 * look at the header again, and only then sleep. The other way round, a wake
 * that lands between the look and the reset is lost and the wait never
 * returns. */
static long gfx_wait_for_header(
    const struct savanxp_gfx_context* context,
    int (*ready)(const struct savanxp_gfx_context* context)) {
    while (!ready(context)) {
        long wait_result = 0;

        if (gfx_shutdown_requested(context)) {
            gfx_default_close();
            return -SAVANXP_EPIPE;
        }
        if (context->wake_event_fd < 0) {
            sleep_ms(1);
            continue;
        }

        (void)event_reset(context->wake_event_fd);
        if (ready(context)) {
            break;
        }
        if (gfx_shutdown_requested(context)) {
            gfx_default_close();
            return -SAVANXP_EPIPE;
        }

        wait_result = wait_one(context->wake_event_fd, GFX_WM_ALIVE_CHECK_MS);
        if (wait_result == -SAVANXP_ETIMEDOUT) {
            if (gfx_check_wm_gone(context)) {
                gfx_default_close();
                return -SAVANXP_EPIPE;
            }
            continue;
        }
        if (wait_result < 0) {
            return wait_result;
        }
    }

    return 0;
}

static int gfx_client_idle(const struct savanxp_gfx_context* context) {
    return context->header->composed_sequence >= context->header->submit_sequence;
}

static int gfx_client_slot_free(const struct savanxp_gfx_context* context) {
    return (context->header->submit_sequence - context->header->composed_sequence) < context->batch_capacity;
}

static long gfx_wait_for_client_idle(const struct savanxp_gfx_context* context) {
    if (context == 0 || context->header == 0) {
        return -SAVANXP_EINVAL;
    }
    return gfx_wait_for_header(context, gfx_client_idle);
}

static long gfx_wait_for_client_slot(const struct savanxp_gfx_context* context) {
    if (context == 0 || context->header == 0 || context->batch_capacity == 0) {
        return -SAVANXP_EINVAL;
    }
    return gfx_wait_for_header(context, gfx_client_slot_free);
}

static long gfx_submit_client_batch(
    const struct savanxp_gfx_context* context,
    const struct savanxp_gpu_dirty_rect* rects,
    uint32_t rect_count,
    uint32_t flags) {
    struct savanxp_gpu_dirty_rect_batch* batch = 0;
    uint64_t sequence = 0;
    uint32_t rect_index = 0;
    long result = 0;

    if (context == 0 || context->header == 0 || context->command_batches == 0 || context->submit_event_fd < 0 || rect_count == 0) {
        return -SAVANXP_EINVAL;
    }
    if (rects == 0 || rect_count > context->rect_capacity || rect_count > SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS) {
        return -SAVANXP_EINVAL;
    }

    result = gfx_wait_for_client_slot(context);
    if (result < 0) {
        return result;
    }

    sequence = context->header->submit_sequence + 1u;
    batch = gfx_batch_slot(context, sequence);
    if (batch == 0) {
        return -SAVANXP_EINVAL;
    }

    batch->rect_count = rect_count;
    batch->flags = flags;
    for (rect_index = 0; rect_index < rect_count; ++rect_index) {
        batch->rects[rect_index] = rects[rect_index];
    }
    for (; rect_index < context->rect_capacity && rect_index < SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS; ++rect_index) {
        batch->rects[rect_index].x = 0;
        batch->rects[rect_index].y = 0;
        batch->rects[rect_index].width = 0;
        batch->rects[rect_index].height = 0;
    }

    batch->submit_sequence = sequence;
    asm volatile("" ::: "memory");
    context->header->submit_sequence = sequence;
    return event_set(context->submit_event_fd);
}

/* Maps and validates the section of a WM surface -- the main one, which comes
 * with the welcome, or an owned window's, which comes later through the handle
 * queue -- and fills the context's mapping fields. The caller sets the
 * channels. */
static long gfx_map_client_surface(struct savanxp_gfx_context* context, int surface_fd) {
    struct savanxp_gpu_client_surface_header* header;
    void* mapped = map_view(surface_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    long private_section_fd = -1;
    void* private_pixels = 0;
    uint32_t* shared_pixels = 0;

    if (result_is_error((long)mapped)) {
        return -SAVANXP_ENODEV;
    }

    header = (struct savanxp_gpu_client_surface_header*)mapped;
    if (header->magic != SAVANXP_GPU_CLIENT_SURFACE_MAGIC ||
        header->version != SAVANXP_GPU_CLIENT_SURFACE_VERSION_5 ||
        header->pixels_offset < sizeof(*header) ||
        header->command_offset < sizeof(*header) ||
        header->batch_capacity == 0 ||
        header->batch_capacity > SAVANXP_GPU_CLIENT_BATCH_CAPACITY ||
        header->rect_capacity == 0 ||
        header->rect_capacity > SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS ||
        header->command_offset >= header->pixels_offset ||
        header->pixels_offset - header->command_offset < (header->batch_capacity * sizeof(struct savanxp_gpu_dirty_rect_batch)) ||
        header->info.width == 0 ||
        header->info.height == 0 ||
        header->info.pitch < (header->info.width * sizeof(uint32_t)) ||
        header->info.buffer_size < (header->info.pitch * header->info.height)) {
        (void)unmap_view(mapped);
        return -SAVANXP_EINVAL;
    }

    shared_pixels = (uint32_t*)((unsigned char*)mapped + header->pixels_offset);
    private_section_fd = section_create(header->info.buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (private_section_fd < 0) {
        (void)unmap_view(mapped);
        return private_section_fd;
    }

    private_pixels = map_view((int)private_section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    (void)savanxp_close((int)private_section_fd);
    if (result_is_error((long)private_pixels)) {
        (void)unmap_view(mapped);
        return (long)private_pixels;
    }
    memcpy(private_pixels, shared_pixels, header->info.buffer_size);

    context->mapped_view = mapped;
    context->header = header;
    context->command_batches = (struct savanxp_gpu_dirty_rect_batch*)((unsigned char*)mapped + header->command_offset);
    context->pixels_offset = header->pixels_offset;
    context->pixels = (uint32_t*)private_pixels;
    context->shared_pixels = shared_pixels;
    context->info = header->info;
    context->batch_capacity = header->batch_capacity;
    context->rect_capacity = header->rect_capacity;
    context->notified_width = header->info.width;
    context->notified_height = header->info.height;
    return 0;
}

static void gfx_unmap_client_surface(struct savanxp_gfx_context* context) {
    if (context->pixels != 0 && context->pixels != context->shared_pixels) {
        (void)unmap_view(context->pixels);
    }
    if (context->mapped_view != 0) {
        (void)unmap_view(context->mapped_view);
    }
    context->mapped_view = 0;
    context->pixels = 0;
    context->shared_pixels = 0;
    context->pixels_offset = 0;
    context->header = 0;
    context->command_batches = 0;
    context->batch_capacity = 0;
    context->rect_capacity = 0;
}

/* Duplica los canales del proceso sobre el contexto. `section_fd` queda como
 * fb_fd y pasa a ser del contexto aunque falle. */
static long gfx_adopt_channels(
    struct savanxp_gfx_context* context,
    long section_fd,
    int input_source,
    int submit_source,
    int wake_source) {
    long input_fd = savanxp_dup(input_source);
    long submit_event_fd = savanxp_dup(submit_source);
    long wake_event_fd = savanxp_dup(wake_source);

    if (section_fd < 0 || input_fd < 0 || submit_event_fd < 0 || wake_event_fd < 0) {
        if (section_fd >= 0) {
            (void)savanxp_close((int)section_fd);
        }
        if (input_fd >= 0) {
            (void)savanxp_close((int)input_fd);
        }
        if (submit_event_fd >= 0) {
            (void)savanxp_close((int)submit_event_fd);
        }
        if (wake_event_fd >= 0) {
            (void)savanxp_close((int)wake_event_fd);
        }
        return -SAVANXP_EBADF;
    }

    context->fb_fd = (int)section_fd;
    context->input_fd = (int)input_fd;
    context->submit_event_fd = (int)submit_event_fd;
    context->wake_event_fd = (int)wake_event_fd;
    context->mode = kGfxModeClient;
    return 0;
}

/*
 * Presentation (docs/desktop/DESKTOP_ARCHITECTURE.md#presentation-comes-from-
 * the-client): the window says who it is -- name, accent, window style, small
 * icon -- read from the program's own .sxe through argv[0], published in the
 * header under a seqlock before the first frame. The WM never opens the
 * binary. A program without resources publishes nothing and gets its file
 * name; so does one whose path is unknown.
 */
static void gfx_publish_presentation(struct savanxp_gfx_context* context) {
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    _Alignas(4) static uint8_t icon_buffer[SXE_ICON_MAX_BYTES];
    struct savanxp_wm_presentation* out = 0;
    struct savanxp_wm_presentation staging;
    struct sxe_meta meta;
    struct sxe_icons icons;
    const char* path = sx_program_path();
    uint32_t value = 0;
    int have_any = 0;

    if (context == 0 || context->header == 0 || path == 0 || path[0] == '\0') {
        return;
    }
    out = &context->header->requests.presentation;
    memset(&staging, 0, sizeof(staging));

    if (sxe_load_meta(path, meta_buffer, sizeof(meta_buffer), &meta) == SXE_OK) {
        if (sxe_meta_string(&meta, SXE_TAG_NAME, staging.title, sizeof(staging.title)) > 0u) {
            have_any = 1;
        }
        if (sxe_meta_u32(&meta, SXE_TAG_ACCENT, &value)) {
            staging.accent = value;
            staging.flags |= SAVANXP_WM_PRESENTATION_HAS_ACCENT;
            have_any = 1;
        }
        if (sxe_meta_u32(&meta, SXE_TAG_WINDOW_FLAGS, &value)) {
            staging.window_style = value;
            have_any = 1;
        }
    }
    if (sxe_load_icons(path, icon_buffer, sizeof(icon_buffer), &icons) == SXE_OK) {
        const struct sxe_icon_entry* entry = sxe_icons_best(&icons, SAVANXP_WM_PRESENTATION_ICON_EXTENT);
        const uint32_t* pixels = entry != 0 ? sxe_icons_pixels(&icons, entry) : 0;

        /* Only squares that fit the slot: the frame draws at 16. */
        if (pixels != 0 && entry->width == entry->height &&
            entry->width <= SAVANXP_WM_PRESENTATION_ICON_EXTENT) {
            memcpy(staging.icon_pixels, pixels, (size_t)entry->width * entry->height * sizeof(uint32_t));
            staging.icon_extent = entry->width;
            have_any = 1;
        }
    }
    if (!have_any) {
        return;
    }

    /* Seqlock: odd while the fields are being written. */
    staging.sequence = out->sequence | 1u;
    out->sequence = staging.sequence;
    __asm__ volatile("" ::: "memory");
    out->flags = staging.flags;
    out->accent = staging.accent;
    out->window_style = staging.window_style;
    out->icon_extent = staging.icon_extent;
    memcpy(out->title, staging.title, sizeof(out->title));
    memcpy(out->icon_pixels, staging.icon_pixels, sizeof(out->icon_pixels));
    __asm__ volatile("" ::: "memory");
    out->sequence = staging.sequence + 1u;
}

/* --- Connection to the WM (protocol v5, savanxp/wm_protocol.h) -----------------
 *
 * One per process, made the first time something needs it (gfx_open,
 * gfx_pointer_open) and kept until the process exits: the WM ties the window
 * to the process, not to a context. Every context duplicates what it uses, so
 * gfx_close on one never cuts the others off. */
struct gfx_wm_connection {
    int connected;
    int events_fd;      /* read end of the event pipe */
    int request_fd;     /* write end of the request pipe; the shell's only */
    int section_fd;     /* the main window's surface */
    int wake_fd;
    int submit_fd;
    int window_list_fd; /* the shell's only */
    /* Set after a request in the header (SAVANXP_WM_HELLO_REQUEST_EVENT); the
     * submit event is for frames only, which go to compositord. -1 if the WM
     * did not give one: then requests go with the submit event, and the WM
     * sees them on its next tick. */
    int request_event_fd;
    uint32_t welcome_flags;
};

static struct gfx_wm_connection g_gfx_wm = {0, -1, -1, -1, -1, -1, -1, -1, 0};

static long gfx_notify_wm(const struct savanxp_gfx_context* context);

static void gfx_close_fd(int* fd) {
    if (*fd >= 0) {
        (void)savanxp_close(*fd);
        *fd = -1;
    }
}

/* What the program declares about itself in its .sxe: the WM sizes the surface
 * of a low-resolution fullscreen program differently. Without resources,
 * nothing. */
static uint32_t gfx_own_launch_flags(void) {
    _Alignas(4) static uint8_t meta_buffer[SXE_META_MAX_BYTES];
    struct sxe_meta meta;
    const char* path = sx_program_path();
    uint32_t value = 0;

    if (path == 0 || path[0] == '\0' ||
        sxe_load_meta(path, meta_buffer, sizeof(meta_buffer), &meta) != SXE_OK ||
        !sxe_meta_u32(&meta, SXE_TAG_LAUNCH_FLAGS, &value)) {
        return SAVANXP_DESKTOP_LAUNCH_FLAG_NONE;
    }
    return value & SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN;
}

/* Waits for the welcome, bounded: a WM that never answers must not hang the
 * program. Records before it are not expected and are dropped. */
static long gfx_wait_welcome(int events_fd, struct savanxp_wm_welcome* welcome) {
    unsigned long deadline = uptime_ms() + SAVANXP_WM_CONNECT_TIMEOUT_MS;

    for (;;) {
        struct savanxp_pollfd pollfd;
        struct savanxp_wm_event record;
        unsigned long now = uptime_ms();
        long result = 0;

        if (now >= deadline) {
            return -SAVANXP_ETIMEDOUT;
        }
        pollfd.fd = events_fd;
        pollfd.events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
        pollfd.revents = 0;
        result = savanxp_poll(&pollfd, 1, (int)(deadline - now));
        if (result < 0) {
            return result;
        }
        if (result == 0) {
            continue;
        }
        if ((pollfd.revents & SAVANXP_POLLIN) == 0) {
            /* The WM closed the connection without a word. */
            return -SAVANXP_EPIPE;
        }
        result = savanxp_read(events_fd, &record, sizeof(record));
        if (result <= 0) {
            return result < 0 ? result : -SAVANXP_EPIPE;
        }
        if (result == (long)sizeof(record) && record.kind == SAVANXP_WM_EVENT_WELCOME) {
            *welcome = record.payload.welcome;
            return 0;
        }
    }
}

static long gfx_wm_connect(uint32_t hello_flags) {
    int32_t handles[2] = {-1, -1};
    int received[5] = {-1, -1, -1, -1, -1};
    uint32_t expected = 3u;
    uint32_t next = 3u;
    struct savanxp_wm_hello hello;
    struct savanxp_wm_welcome welcome;
    unsigned long deadline = 0;
    uint32_t index = 0;
    long result = 0;

    if (g_gfx_wm.connected) {
        return 0;
    }

    /* A full backlog is momentary: the WM accepts every pass of its loop. */
    deadline = uptime_ms() + SAVANXP_WM_CONNECT_TIMEOUT_MS;
    for (;;) {
        result = sxn_port_connect(SAVANXP_WM_PORT_NAME, handles);
        if (result != -SAVANXP_EAGAIN || uptime_ms() >= deadline) {
            break;
        }
        sleep_ms(10);
    }
    if (result < 0) {
        /* No port: there is no WM to connect to. */
        return result == -SAVANXP_ENOENT ? -SAVANXP_ENODEV : result;
    }

    memset(&hello, 0, sizeof(hello));
    hello.magic = SAVANXP_WM_HELLO_MAGIC;
    hello.version = SAVANXP_WM_PROTOCOL_VERSION;
    hello.flags = hello_flags | SAVANXP_WM_HELLO_REQUEST_EVENT;
    hello.launch_flags = gfx_own_launch_flags();
    if (savanxp_write(handles[0], &hello, sizeof(hello)) != (long)sizeof(hello)) {
        result = -SAVANXP_EPIPE;
        goto fail;
    }
    result = gfx_wait_welcome(handles[1], &welcome);
    if (result < 0) {
        goto fail;
    }
    if (welcome.status != 0) {
        result = welcome.status < 0 ? welcome.status : -SAVANXP_EIO;
        goto fail;
    }
    /* section, wake, submit, the window list for the shell, then the request
     * event. A POSIX client reads its input from the pipe, so it never asks
     * for the ring. */
    if ((welcome.flags & SAVANXP_WM_WELCOME_SHELL) != 0u) {
        expected += 1u;
    }
    if ((welcome.flags & SAVANXP_WM_WELCOME_REQUEST_EVENT) != 0u) {
        expected += 1u;
    }
    if (welcome.handle_count != expected) {
        result = -SAVANXP_EIO;
        goto fail;
    }
    for (index = 0; index < welcome.handle_count; ++index) {
        result = pipe_receive_handle(handles[1]);
        if (result < 0) {
            goto fail;
        }
        received[index] = (int)result;
    }

    g_gfx_wm.events_fd = handles[1];
    g_gfx_wm.section_fd = received[0];
    g_gfx_wm.wake_fd = received[1];
    g_gfx_wm.submit_fd = received[2];
    if ((welcome.flags & SAVANXP_WM_WELCOME_SHELL) != 0u) {
        g_gfx_wm.window_list_fd = received[next++];
    }
    if ((welcome.flags & SAVANXP_WM_WELCOME_REQUEST_EVENT) != 0u) {
        g_gfx_wm.request_event_fd = received[next++];
    }
    g_gfx_wm.welcome_flags = welcome.flags;
    if ((welcome.flags & SAVANXP_WM_WELCOME_SHELL) != 0u) {
        g_gfx_wm.request_fd = handles[0];
    } else {
        /* Only the shell has anything more to say. */
        (void)savanxp_close(handles[0]);
    }
    g_gfx_wm.connected = 1;
    return 0;

fail:
    for (index = 0; index < 5u; ++index) {
        gfx_close_fd(&received[index]);
    }
    gfx_close_fd(&handles[0]);
    gfx_close_fd(&handles[1]);
    return result;
}

int gfx_shell_request_fd(void) {
    return (g_gfx_wm.connected && (g_gfx_wm.welcome_flags & SAVANXP_WM_WELCOME_SHELL) != 0u) ? g_gfx_wm.request_fd : -1;
}

int gfx_shell_window_list_fd(void) {
    return (g_gfx_wm.connected && (g_gfx_wm.welcome_flags & SAVANXP_WM_WELCOME_SHELL) != 0u) ? g_gfx_wm.window_list_fd : -1;
}

static long gfx_open_client(struct savanxp_gfx_context* context, uint32_t hello_flags) {
    long result = gfx_wm_connect(hello_flags);

    if (result < 0) {
        return result;
    }
    result = gfx_map_client_surface(context, g_gfx_wm.section_fd);
    if (result < 0) {
        return result;
    }
    result = gfx_adopt_channels(
        context,
        savanxp_dup(g_gfx_wm.section_fd),
        g_gfx_wm.events_fd,
        g_gfx_wm.submit_fd,
        g_gfx_wm.wake_fd);
    if (result < 0) {
        gfx_unmap_client_surface(context);
        return result;
    }
    context->window_id = 0;
    g_gfx_main_header = context->header;
    gfx_publish_presentation(context);
    /* Before the first frame the WM sees it on the next pass anyway; wake it
     * so the title is right from the start. */
    (void)gfx_notify_wm(context);
    return 0;
}

static long gfx_open_with(struct savanxp_gfx_context* context, uint32_t hello_flags) {
    if (context == 0) {
        return -SAVANXP_EINVAL;
    }

    memset(context, 0, sizeof(*context));
    context->fb_fd = -1;
    context->input_fd = -1;
    context->submit_event_fd = -1;
    context->wake_event_fd = -1;

    return gfx_open_client(context, hello_flags);
}

long gfx_open(struct savanxp_gfx_context* context) {
    return gfx_open_with(context, 0);
}

long gfx_open_shell(struct savanxp_gfx_context* context) {
    return gfx_open_with(context, SAVANXP_WM_HELLO_SHELL);
}

long gfx_close(struct savanxp_gfx_context* context) {
    long result = 0;
    if (context == 0) {
        return -SAVANXP_EINVAL;
    }

    if (context->mode == kGfxModeClient && context->mapped_view != 0) {
        if (context->header == g_gfx_main_header) {
            g_gfx_main_header = 0;
        }
        gfx_unmap_client_surface(context);
    }

    if (context->fb_fd >= 0) {
        result = savanxp_close(context->fb_fd);
        context->fb_fd = -1;
    }
    if (context->input_fd >= 0) {
        (void)savanxp_close(context->input_fd);
        context->input_fd = -1;
    }
    if (context->submit_event_fd >= 0) {
        (void)savanxp_close(context->submit_event_fd);
        context->submit_event_fd = -1;
    }
    if (context->wake_event_fd >= 0) {
        (void)savanxp_close(context->wake_event_fd);
        context->wake_event_fd = -1;
    }
    context->mode = 0;
    return result;
}

long gfx_acquire(struct savanxp_gfx_context* context) {
    if (context == 0 || context->fb_fd < 0) {
        return -SAVANXP_EINVAL;
    }
    return context->mode == kGfxModeClient ? 0 : -SAVANXP_EINVAL;
}

long gfx_release(struct savanxp_gfx_context* context) {
    if (context == 0 || context->fb_fd < 0) {
        return -SAVANXP_EINVAL;
    }
    return context->mode == kGfxModeClient ? 0 : -SAVANXP_EINVAL;
}

long gfx_present(const struct savanxp_gfx_context* context, const uint32_t* pixels) {
    struct savanxp_gpu_dirty_rect rect;
    long result = 0;
    gfx_default_close();
    if (context == 0 || context->fb_fd < 0 || pixels == 0) {
        return -SAVANXP_EINVAL;
    }
    if (context->mode != kGfxModeClient || context->shared_pixels == 0 || context->submit_event_fd < 0) {
        return -SAVANXP_EINVAL;
    }
    result = gfx_wait_for_client_idle(context);
    if (result < 0) {
        return result;
    }
    if (pixels != context->shared_pixels) {
        memcpy(context->shared_pixels, pixels, context->info.buffer_size);
    }
    rect.x = 0;
    rect.y = 0;
    rect.width = context->info.width;
    rect.height = context->info.height;
    return gfx_submit_client_batch(context, &rect, 1, SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE);
}

long gfx_present_region(const struct savanxp_gfx_context* context, const uint32_t* pixels, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    struct savanxp_gpu_dirty_rect rect;
    gfx_default_close();
    if (context == 0 || context->fb_fd < 0 || pixels == 0 || width == 0 || height == 0) {
        return -SAVANXP_EINVAL;
    }

    if (context->mode != kGfxModeClient) {
        return -SAVANXP_EINVAL;
    }
    if (context->shared_pixels == 0 || context->submit_event_fd < 0) {
        return -SAVANXP_EINVAL;
    }
    if (x >= context->info.width || y >= context->info.height ||
        width > (context->info.width - x) || height > (context->info.height - y)) {
        return -SAVANXP_EINVAL;
    }

    {
        long result = gfx_wait_for_client_idle(context);
        if (result < 0) {
            return result;
        }
    }
    gfx_copy_present_region(context, pixels, x, y, width, height);
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    return gfx_submit_client_batch(context, &rect, 1, SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_NONE);
}

long gfx_present_rects(const struct savanxp_gfx_context* context, const uint32_t* pixels, const struct sx_rect* rects, size_t rect_count) {
    struct savanxp_gpu_dirty_rect batch_rects[SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS];
    size_t index;
    uint32_t batch_count = 0;
    long result = 0;

    gfx_default_close();

    if (context == 0 || pixels == 0 || rects == 0 || rect_count == 0) {
        return -SAVANXP_EINVAL;
    }
    if (context->mode != kGfxModeClient || context->shared_pixels == 0 || context->submit_event_fd < 0) {
        return -SAVANXP_EINVAL;
    }

    result = gfx_wait_for_client_idle(context);
    if (result < 0) {
        return result;
    }

    for (index = 0; index < rect_count; ++index) {
        struct sx_rect rect = rects[index];
        int x = rect.x;
        int y = rect.y;
        int width = rect.width;
        int height = rect.height;

        if (!gfx_clip_rect(&context->info, &x, &y, &width, &height)) {
            continue;
        }

        gfx_copy_present_region(context, pixels, (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height);
        batch_rects[batch_count].x = (uint32_t)x;
        batch_rects[batch_count].y = (uint32_t)y;
        batch_rects[batch_count].width = (uint32_t)width;
        batch_rects[batch_count].height = (uint32_t)height;
        batch_count += 1u;

        if (batch_count >= context->rect_capacity || batch_count >= SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS) {
            result = gfx_submit_client_batch(context, batch_rects, batch_count, SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_NONE);
            if (result < 0) {
                return result;
            }
            batch_count = 0;
        }
    }

    if (batch_count != 0) {
        result = gfx_submit_client_batch(context, batch_rects, batch_count, SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_NONE);
    }
    return result;
}

/* --- Canal de eventos del WM (savanxp/wm_protocol.h) -----------------------
 *
 * Teclado y puntero llegan mezclados por el mismo pipe, pero la API los pide
 * por separado. Lo que una funcion de poll lee y no es suyo lo guarda aca para
 * la otra, y lo tiene que leer igual: si una app que solo mira el teclado
 * dejara el puntero en el pipe, el pipe se llenaria y el WM descartaria las
 * teclas.
 *
 * El pipe es uno por PROCESO y las ventanas pueden ser varias (la principal y
 * las con dueno, docs/desktop/OWNED_WINDOWS.md), asi que lo guardado se separa tambien
 * por window_id: un evento del dialogo leido mientras se drenaba la principal
 * espera en la bandeja del dialogo.
 */
#define GFX_EVENT_STASH_CAPACITY 64u
#define GFX_EVENT_WINDOW_SLOTS (SAVANXP_WM_MAX_OWNED_WINDOWS + 1u)
/* Tope de registros por llamada: un pipe lleno entero. Evita quedarse drenando
 * para siempre si el WM escribe tan rapido como se lee. */
#define GFX_EVENT_DRAIN_LIMIT 256u
/* Programs started by gfx_desktop_launch that may still need reaping, and
 * how often gfx_poll_event looks for them. */
#define GFX_LAUNCHED_CAPACITY 16u
#define GFX_REAP_INTERVAL_MS 1000ul

struct gfx_key_stash {
    struct savanxp_input_event items[GFX_EVENT_STASH_CAPACITY];
    uint32_t head;
    uint32_t count;
};

struct gfx_pointer_stash {
    struct savanxp_gui_pointer_event items[GFX_EVENT_STASH_CAPACITY];
    uint32_t head;
    uint32_t count;
};

static struct gfx_key_stash g_gfx_key_stash[GFX_EVENT_WINDOW_SLOTS];
static struct gfx_pointer_stash g_gfx_pointer_stash[GFX_EVENT_WINDOW_SLOTS];

static void gfx_stash_key(uint32_t window_id, const struct savanxp_input_event* event) {
    struct gfx_key_stash* stash = 0;

    /* Una ventana que el runtime no conoce no tiene quien la lea. */
    if (window_id >= GFX_EVENT_WINDOW_SLOTS) {
        return;
    }
    stash = &g_gfx_key_stash[window_id];
    /* Llena: se descarta, que es lo mismo que haria el WM con el pipe lleno. */
    if (stash->count == GFX_EVENT_STASH_CAPACITY) {
        return;
    }
    stash->items[(stash->head + stash->count) % GFX_EVENT_STASH_CAPACITY] = *event;
    stash->count += 1u;
}

static void gfx_stash_pointer(uint32_t window_id, const struct savanxp_gui_pointer_event* event) {
    struct gfx_pointer_stash* stash = 0;

    if (window_id >= GFX_EVENT_WINDOW_SLOTS) {
        return;
    }
    stash = &g_gfx_pointer_stash[window_id];
    if (stash->count == GFX_EVENT_STASH_CAPACITY) {
        /* Llena: se funde con el ultimo. Posicion y botones son absolutos, pero
         * la rueda se SUMA -- un tick perdido es scroll que no ocurre nunca. */
        struct savanxp_gui_pointer_event* last = &stash->items[
            (stash->head + stash->count - 1u) % GFX_EVENT_STASH_CAPACITY];
        int32_t wheel = last->wheel + event->wheel;
        *last = *event;
        last->wheel = wheel;
        return;
    }
    stash->items[(stash->head + stash->count) % GFX_EVENT_STASH_CAPACITY] = *event;
    stash->count += 1u;
}

/* Lo que quedo guardado de una ventana que se cierra no es de nadie: si el id
 * se reusa, la ventana nueva no tiene que recibir clicks de la vieja. */
static void gfx_discard_stashes(uint32_t window_id) {
    if (window_id >= GFX_EVENT_WINDOW_SLOTS) {
        return;
    }
    g_gfx_key_stash[window_id].head = 0;
    g_gfx_key_stash[window_id].count = 0;
    g_gfx_pointer_stash[window_id].head = 0;
    g_gfx_pointer_stash[window_id].count = 0;
}

/* Lee un registro entero sin bloquear: 1 leido, 0 nada, negativo error. */
static int gfx_read_wm_event(int fd, struct savanxp_wm_event* record) {
    struct savanxp_pollfd pollfd;
    long result = 0;

    pollfd.fd = fd;
    pollfd.events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
    pollfd.revents = 0;
    if (savanxp_poll(&pollfd, 1, 0) <= 0) {
        return 0;
    }
    /* Every fd read here is a duplicate of the one event pipe. */
    if ((pollfd.revents & SAVANXP_POLLHUP) != 0) {
        g_gfx_wm_gone = 1;
    }
    if ((pollfd.revents & SAVANXP_POLLIN) == 0) {
        return 0;
    }
    result = savanxp_read(fd, record, sizeof(*record));
    if (result < 0) {
        return (int)result;
    }
    return result == (long)sizeof(*record) ? 1 : 0;
}

/* Drena el pipe hasta encontrar un registro de `kind` para `window_id`; todo lo
 * demas va a su bandeja. 1 = `record` trae el buscado, 0 = no habia. */
static int gfx_drain_for(int fd, uint32_t window_id, uint32_t kind, struct savanxp_wm_event* record) {
    uint32_t drained = 0;

    for (drained = 0; drained < GFX_EVENT_DRAIN_LIMIT; ++drained) {
        int result = gfx_read_wm_event(fd, record);

        if (result <= 0) {
            return result;
        }
        if (record->window_id == window_id && record->kind == kind) {
            return 1;
        }
        if (record->kind == SAVANXP_WM_EVENT_KEY) {
            gfx_stash_key(record->window_id, &record->payload.key);
        } else if (record->kind == SAVANXP_WM_EVENT_POINTER) {
            gfx_stash_pointer(record->window_id, &record->payload.pointer);
        }
    }
    return 0;
}

static void gfx_reap_launched_sometimes(void);

int gfx_poll_event(const struct savanxp_gfx_context* context, struct savanxp_input_event* event) {
    struct savanxp_wm_event record;
    struct gfx_key_stash* stash = 0;
    int result = 0;

    if (context == 0 || context->input_fd < 0 || event == 0 || context->window_id >= GFX_EVENT_WINDOW_SLOTS) {
        return -SAVANXP_EINVAL;
    }
    gfx_reap_launched_sometimes();
    gfx_default_close();

    if (gfx_header_dimensions_changed(context)) {
        struct savanxp_gfx_context* mutable_context = (struct savanxp_gfx_context*)context;
        gfx_refresh_notified_dimensions(mutable_context);
        event->type = SAVANXP_INPUT_EVENT_RESIZED;
        event->key = mutable_context->notified_width;
        event->ascii = (int32_t)mutable_context->notified_height;
        return 1;
    }

    stash = &g_gfx_key_stash[context->window_id];
    if (stash->count != 0) {
        *event = stash->items[stash->head];
        stash->head = (stash->head + 1u) % GFX_EVENT_STASH_CAPACITY;
        stash->count -= 1u;
        return 1;
    }

    result = gfx_drain_for(context->input_fd, context->window_id, SAVANXP_WM_EVENT_KEY, &record);
    if (result <= 0) {
        return result;
    }
    *event = record.payload.key;
    return 1;
}

static int gfx_poll_pointer_for(int fd, uint32_t window_id, struct savanxp_gui_pointer_event* event) {
    struct savanxp_wm_event record;
    struct gfx_pointer_stash* stash = 0;
    int result = 0;

    if (fd < 0 || event == 0 || window_id >= GFX_EVENT_WINDOW_SLOTS) {
        return -SAVANXP_EINVAL;
    }

    stash = &g_gfx_pointer_stash[window_id];
    if (stash->count != 0) {
        *event = stash->items[stash->head];
        stash->head = (stash->head + 1u) % GFX_EVENT_STASH_CAPACITY;
        stash->count -= 1u;
        return 1;
    }

    result = gfx_drain_for(fd, window_id, SAVANXP_WM_EVENT_POINTER, &record);
    if (result <= 0) {
        return result;
    }
    *event = record.payload.pointer;
    return 1;
}

long gfx_pointer_open(void) {
    long result = gfx_wm_connect(0);

    return result < 0 ? result : savanxp_dup(g_gfx_wm.events_fd);
}

/* El fd suelto no dice de que ventana es: es la principal, como siempre. */
int gfx_poll_pointer(int fd, struct savanxp_gui_pointer_event* event) {
    gfx_default_close();
    return gfx_poll_pointer_for(fd, 0, event);
}

int gfx_poll_window_pointer(const struct savanxp_gfx_context* context, struct savanxp_gui_pointer_event* event) {
    if (context == 0) {
        return -SAVANXP_EINVAL;
    }
    return gfx_poll_pointer_for(context->input_fd, context->window_id, event);
}

int gfx_should_close(const struct savanxp_gfx_context* context) {
    return gfx_shutdown_requested(context) || (context != 0 && context->header != 0 && gfx_check_wm_gone(context));
}

long gfx_apply_resize_event(struct savanxp_gfx_context* context, const struct savanxp_input_event* event) {
    uint32_t max_width = 0;
    uint32_t max_height = 0;

    if (context == 0 || event == 0 || event->type != SAVANXP_INPUT_EVENT_RESIZED) {
        return -SAVANXP_EINVAL;
    }
    if (context->info.pitch == 0) {
        return -SAVANXP_EINVAL;
    }

    max_width = context->info.pitch / (uint32_t)sizeof(uint32_t);
    max_height = context->info.buffer_size / context->info.pitch;
    if (event->key == 0 || event->ascii <= 0 || event->key > max_width || (uint32_t)event->ascii > max_height) {
        return -SAVANXP_EINVAL;
    }

    context->info.width = event->key;
    context->info.height = (uint32_t)event->ascii;
    return 0;
}

/* Requests to the WM are written into the header (savanxp_wm_client_requests)
 * and announced with the WM's request event; a WM that gave none gets the
 * submit event, as before. */
static long gfx_notify_wm(const struct savanxp_gfx_context* context) {
    if (g_gfx_wm.request_event_fd >= 0) {
        return event_set(g_gfx_wm.request_event_fd);
    }
    return context->submit_event_fd >= 0 ? event_set(context->submit_event_fd) : 0;
}

/* --- Launching (protocol v5) ---------------------------------------------------
 *
 * The caller starts the program itself and the program connects to the WM on
 * its own; the WM never forks. spawn gives the child only stdio, so none of
 * the caller's channels leak into it. The child stays the caller's until it
 * exits, and a zombie holds a process slot, so the runtime reaps it: at the
 * next launch and every GFX_REAP_INTERVAL_MS from gfx_poll_event. */
static long g_gfx_launched[GFX_LAUNCHED_CAPACITY];
static uint32_t g_gfx_launched_count = 0;
static unsigned long g_gfx_last_reap_ms = 0;

static void gfx_reap_launched_now(void) {
    struct savanxp_process_info info;
    uint8_t state[GFX_LAUNCHED_CAPACITY]; /* 0 gone, 1 running, 2 zombie */
    unsigned long index = 0;
    uint32_t slot = 0;

    if (g_gfx_launched_count == 0) {
        return;
    }
    memset(state, 0, sizeof(state));
    while (proc_info(index, &info) > 0) {
        for (slot = 0; slot < g_gfx_launched_count; ++slot) {
            if ((long)info.pid == g_gfx_launched[slot]) {
                state[slot] = info.state == SAVANXP_PROC_ZOMBIE ? 2u : 1u;
            }
        }
        ++index;
    }
    slot = 0;
    while (slot < g_gfx_launched_count) {
        if (state[slot] == 1u) {
            ++slot;
            continue;
        }
        if (state[slot] == 2u) {
            int status = 0;
            /* A zombie: waitpid returns at once. */
            (void)savanxp_waitpid((int)g_gfx_launched[slot], &status);
        }
        g_gfx_launched_count -= 1u;
        g_gfx_launched[slot] = g_gfx_launched[g_gfx_launched_count];
        state[slot] = state[g_gfx_launched_count];
    }
}

static void gfx_reap_launched_sometimes(void) {
    unsigned long now = 0;

    if (g_gfx_launched_count == 0) {
        return;
    }
    now = uptime_ms();
    if (now - g_gfx_last_reap_ms < GFX_REAP_INTERVAL_MS) {
        return;
    }
    g_gfx_last_reap_ms = now;
    gfx_reap_launched_now();
}

long gfx_desktop_launch_arg(
        const struct savanxp_gfx_context* context,
        const char* path,
        const char* argument,
        uint32_t flags) {
    const char* argv[3];
    int argc = 1;
    long pid = 0;

    (void)context;
    (void)flags;
    if (path == 0 || path[0] != '/' || strlen(path) >= SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY) {
        return -SAVANXP_EINVAL;
    }
    if (argument != 0 && strlen(argument) >= SAVANXP_DESKTOP_LAUNCH_ARG_CAPACITY) {
        return -SAVANXP_EINVAL;
    }
    argv[0] = path;
    argv[1] = 0;
    argv[2] = 0;
    if (argument != 0 && argument[0] != '\0') {
        argv[1] = argument;
        argc = 2;
    }

    gfx_reap_launched_now();
    pid = spawn(path, argv, argc);
    if (pid < 0) {
        return pid;
    }
    /* With the table full of live children this one goes untracked: it is
     * reaped when the caller exits, like any orphan. */
    if (g_gfx_launched_count < GFX_LAUNCHED_CAPACITY) {
        g_gfx_launched[g_gfx_launched_count++] = pid;
    }
    return pid;
}

long gfx_desktop_launch_ex(const struct savanxp_gfx_context* context, const char* path, uint32_t flags) {
    return gfx_desktop_launch_arg(context, path, 0, flags);
}

long gfx_desktop_launch(const struct savanxp_gfx_context* context, const char* path) {
    return gfx_desktop_launch_ex(context, path, SAVANXP_DESKTOP_LAUNCH_FLAG_NONE);
}

long gfx_desktop_set_cursor_shape(const struct savanxp_gfx_context* context, uint32_t shape) {
    if (context == 0 || context->header == 0 || shape >= SAVANXP_CURSOR_SHAPE_COUNT) {
        return -SAVANXP_EINVAL;
    }

    context->header->requests.cursor_shape = shape;
    return gfx_notify_wm(context) < 0 ? -SAVANXP_EIO : 0;
}

long gfx_request_content_size(const struct savanxp_gfx_context* context, uint32_t width, uint32_t height) {
    struct savanxp_wm_client_requests* requests = 0;

    if (context == 0 || context->header == 0 || width == 0 || height == 0) {
        return -SAVANXP_EINVAL;
    }

    /* Seqlock: impar mientras el par esta a medio escribir. */
    requests = &context->header->requests;
    requests->size_hint_sequence |= 1u;
    requests->size_hint_width = width;
    requests->size_hint_height = height;
    requests->size_hint_sequence += 1u;
    return gfx_notify_wm(context) < 0 ? -SAVANXP_EIO : 0;
}

long gfx_wait_content_size(struct savanxp_gfx_context* context, unsigned long timeout_ms) {
    unsigned long deadline = 0;

    if (context == 0 || context->header == 0) {
        return -SAVANXP_EINVAL;
    }

    deadline = uptime_ms() + timeout_ms;
    for (;;) {
        struct savanxp_input_event event;

        if (gfx_header_dimensions_changed(context)) {
            /* El WM escribe ancho y alto por separado: una pausa corta evita
             * quedarse con la mitad del par y aplicar una geometria que la
             * superficie no tiene. */
            sleep_ms(2);
            /* Consumimos el RESIZED nosotros: gfx_poll_event lo sintetiza del
             * header, asi que dejarlo pendiente solo duplicaria el relayout. */
            if (gfx_poll_event(context, &event) > 0 && event.type == SAVANXP_INPUT_EVENT_RESIZED) {
                (void)gfx_apply_resize_event(context, &event);
            }
            return 1;
        }
        if (uptime_ms() >= deadline) {
            return 0;
        }
        sleep_ms(2);
    }
}

/* --- Ventanas con dueno (docs/desktop/OWNED_WINDOWS.md) ------------------------------
 *
 * El pedido va por el header de la ventana principal y la superficie vuelve
 * como handle por la cola del pipe de eventos. La espera es acotada: un WM que
 * no atiende no puede colgar a la app, que cae a dibujar el dialogo adentro de
 * su propia superficie como antes. */
#define GFX_WINDOW_OPEN_WAIT_MS 1000ul
#define GFX_WINDOW_CLOSE_WAIT_MS 250ul

/* Where a role window goes (savanxp_wm_window_role). NORMAL ignores it. */
struct gfx_window_placement {
    uint32_t role;
    uint32_t role_flags;
    int32_t x;
    int32_t y;
};

static long gfx_window_request(
    const struct savanxp_gfx_context* owner,
    uint32_t action,
    uint32_t window_id,
    uint32_t width,
    uint32_t height,
    const char* title,
    const struct gfx_window_placement* placement,
    unsigned long timeout_ms) {
    struct savanxp_wm_client_requests* requests = &owner->header->requests;
    uint32_t sequence = 0;
    unsigned long deadline = 0;

    /* Seqlock: impar mientras el pedido esta a medio escribir. */
    requests->window_sequence |= 1u;
    memset(&requests->window, 0, sizeof(requests->window));
    requests->window.action = action;
    requests->window.window_id = window_id;
    requests->window.width = width;
    requests->window.height = height;
    if (title != 0) {
        size_t length = strlen(title);
        if (length >= SAVANXP_WM_WINDOW_TITLE_CAPACITY) {
            length = SAVANXP_WM_WINDOW_TITLE_CAPACITY - 1u;
        }
        memcpy(requests->window.title, title, length);
    }
    if (placement != 0) {
        requests->window.role = placement->role;
        requests->window.role_flags = placement->role_flags;
        requests->window.x = placement->x;
        requests->window.y = placement->y;
    }
    requests->window_sequence += 1u;
    sequence = requests->window_sequence;
    (void)gfx_notify_wm(owner);

    deadline = uptime_ms() + timeout_ms;
    for (;;) {
        if (requests->window_reply_sequence == sequence) {
            return requests->window_reply_status;
        }
        if (gfx_shutdown_requested(owner)) {
            return -SAVANXP_EPIPE;
        }
        if (owner->wake_event_fd < 0 || uptime_ms() >= deadline) {
            return -SAVANXP_EAGAIN;
        }
        /* El wake sirve a varias condiciones: resetear, volver a mirar y recien
         * ahi dormir, igual que gfx_wait_for_header. */
        (void)event_reset(owner->wake_event_fd);
        if (requests->window_reply_sequence == sequence) {
            return requests->window_reply_status;
        }
        (void)wait_one(owner->wake_event_fd, 20);
    }
}

static long gfx_window_open_with(
    const struct savanxp_gfx_context* owner,
    struct savanxp_gfx_context* window,
    uint32_t window_id,
    uint32_t width,
    uint32_t height,
    const char* title,
    const struct gfx_window_placement* placement) {
    long section_fd = -1;
    long result = 0;

    /* A panel's width is the WM's to decide (the whole edge): only its
     * thickness comes from the client. */
    int width_required = placement == 0 || placement->role != SAVANXP_WM_WINDOW_ROLE_PANEL;

    if (owner == 0 || owner->header == 0 || owner->window_id != 0 || owner->input_fd < 0 || window == 0 ||
        window_id == 0 || window_id > SAVANXP_WM_MAX_OWNED_WINDOWS || (width_required && width == 0) ||
        height == 0) {
        return -SAVANXP_EINVAL;
    }

    memset(window, 0, sizeof(*window));
    window->fb_fd = -1;
    window->input_fd = -1;
    window->submit_event_fd = -1;
    window->wake_event_fd = -1;

    /* Un handle que ya esta en la cola es de un pedido anterior que vencio del
     * lado del cliente: no es el de este pedido y no tiene quien lo use. */
    while ((section_fd = pipe_receive_handle(owner->input_fd)) >= 0) {
        (void)savanxp_close((int)section_fd);
    }

    result = gfx_window_request(
        owner, SAVANXP_WM_WINDOW_ACTION_OPEN, window_id, width, height, title, placement, GFX_WINDOW_OPEN_WAIT_MS);
    if (result < 0) {
        if (result == -SAVANXP_EAGAIN) {
            /* Vencio el plazo: si el WM la crea igual, que la destruya. */
            (void)gfx_window_request(owner, SAVANXP_WM_WINDOW_ACTION_CLOSE, window_id, 0, 0, 0, 0, 0);
        }
        return result;
    }

    section_fd = pipe_receive_handle(owner->input_fd);
    if (section_fd < 0) {
        result = section_fd;
        goto fail;
    }
    result = gfx_map_client_surface(window, (int)section_fd);
    if (result < 0) {
        (void)savanxp_close((int)section_fd);
        goto fail;
    }
    result = gfx_adopt_channels(window, section_fd, owner->input_fd, owner->submit_event_fd, owner->wake_event_fd);
    if (result < 0) {
        gfx_unmap_client_surface(window);
        goto fail;
    }
    window->window_id = window_id;
    gfx_discard_stashes(window_id);
    return 0;

fail:
    (void)gfx_window_request(
        owner, SAVANXP_WM_WINDOW_ACTION_CLOSE, window_id, 0, 0, 0, 0, GFX_WINDOW_CLOSE_WAIT_MS);
    return result;
}

long gfx_window_open(
    const struct savanxp_gfx_context* owner,
    struct savanxp_gfx_context* window,
    uint32_t window_id,
    uint32_t width,
    uint32_t height,
    const char* title) {
    return gfx_window_open_with(owner, window, window_id, width, height, title, 0);
}

long gfx_window_open_role(
    const struct savanxp_gfx_context* owner,
    struct savanxp_gfx_context* window,
    uint32_t window_id,
    uint32_t role,
    uint32_t role_flags,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height) {
    struct gfx_window_placement placement;

    placement.role = role;
    placement.role_flags = role_flags;
    placement.x = x;
    placement.y = y;
    return gfx_window_open_with(owner, window, window_id, width, height, 0, &placement);
}

long gfx_window_close(const struct savanxp_gfx_context* owner, struct savanxp_gfx_context* window) {
    uint32_t window_id = 0;

    if (window == 0 || window->window_id == 0) {
        return -SAVANXP_EINVAL;
    }
    window_id = window->window_id;
    (void)gfx_close(window);
    if (owner != 0 && owner->header != 0) {
        (void)gfx_window_request(
            owner, SAVANXP_WM_WINDOW_ACTION_CLOSE, window_id, 0, 0, 0, 0, GFX_WINDOW_CLOSE_WAIT_MS);
    }
    /* Despues del pedido y no antes: lo que el WM haya mandado hasta destruirla
     * cae en la bandeja y se tira aca. */
    gfx_discard_stashes(window_id);
    window->window_id = 0;
    return 0;
}
