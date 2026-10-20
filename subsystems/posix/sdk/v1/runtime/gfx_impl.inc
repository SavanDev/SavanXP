/* Text rendering uses offline-baked fonts (tools/font/genfont.py):
 *   - Noto Sans, proportional and antialiased, for desktop/UI text.
 *   - RetroSans, proportional pixel face at 12px, for window captions.
 *   - UniFont, fixed 8x16 1-bit, for the monospace terminal path.
 * PixelSans and ImpactfulBits are baked as committed tables
 * (gfx_font_pixelsans.inc, gfx_font_impactfulbits.inc) but are not linked
 * into the graphics library yet.
 */
#include "gfx_font_noto.inc"
#include "gfx_font_title.inc"
#include "console_font_unifont.inc"
#include "gfx_utf8.inc"

uint32_t gfx_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
}

uint32_t gfx_stride_pixels(const struct savanxp_fb_info* info) {
    if (info == 0 || info->pitch == 0) {
        return 0;
    }
    return info->pitch / 4u;
}

size_t gfx_buffer_pixels(const struct savanxp_fb_info* info) {
    if (info == 0 || info->pitch == 0 || info->height == 0) {
        return 0;
    }
    return (size_t)gfx_stride_pixels(info) * (size_t)info->height;
}

size_t gfx_buffer_bytes(const struct savanxp_fb_info* info) {
    return gfx_buffer_pixels(info) * sizeof(uint32_t);
}

static int gfx_clip_rect(const struct savanxp_fb_info* info, int* x, int* y, int* width, int* height) {
    int clipped_x;
    int clipped_y;
    int clipped_width;
    int clipped_height;

    if (info == 0 || x == 0 || y == 0 || width == 0 || height == 0 || info->pitch == 0) {
        return 0;
    }

    clipped_x = *x;
    clipped_y = *y;
    clipped_width = *width;
    clipped_height = *height;

    if (clipped_width <= 0 || clipped_height <= 0) {
        return 0;
    }

    if (clipped_x < 0) {
        clipped_width += clipped_x;
        clipped_x = 0;
    }
    if (clipped_y < 0) {
        clipped_height += clipped_y;
        clipped_y = 0;
    }
    if (clipped_width <= 0 || clipped_height <= 0) {
        return 0;
    }
    if (clipped_x >= (int)info->width || clipped_y >= (int)info->height) {
        return 0;
    }
    if (clipped_x + clipped_width > (int)info->width) {
        clipped_width = (int)info->width - clipped_x;
    }
    if (clipped_y + clipped_height > (int)info->height) {
        clipped_height = (int)info->height - clipped_y;
    }
    if (clipped_width <= 0 || clipped_height <= 0) {
        return 0;
    }

    *x = clipped_x;
    *y = clipped_y;
    *width = clipped_width;
    *height = clipped_height;
    return 1;
}

static void gfx_fill_span(uint32_t* destination, int count, uint32_t colour) {
    int index;
    for (index = 0; index < count; ++index) {
        destination[index] = colour;
    }
}

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

static int gfx_shutdown_requested(const struct savanxp_gfx_context* context) {
    return context != 0 && context->header != 0 &&
        (context->header->flags & SAVANXP_GPU_CLIENT_SURFACE_FLAG_SHUTDOWN) != 0;
}

/* Espera a que `ready` se cumpla, con corte por pedido de cierre. El evento de
 * wake es uno para varias condiciones, asi que el orden importa: resetear,
 * volver a mirar el header y recien ahi dormir. Al reves, un wake que llega
 * entre la mirada y el reset se pierde y la espera no vuelve nunca. */
static long gfx_wait_for_header(
    const struct savanxp_gfx_context* context,
    int (*ready)(const struct savanxp_gfx_context* context)) {
    while (!ready(context)) {
        long wait_result = 0;

        if (gfx_shutdown_requested(context)) {
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
            return -SAVANXP_EPIPE;
        }

        wait_result = wait_one(context->wake_event_fd, -1);
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
    uint32_t welcome_flags;
};

static struct gfx_wm_connection g_gfx_wm = {0, -1, -1, -1, -1, -1, -1, 0};

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

static long gfx_wm_connect(void) {
    int32_t handles[2] = {-1, -1};
    int received[4] = {-1, -1, -1, -1};
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
    /* section, wake, submit, and the window list for the shell. A POSIX
     * client reads its input from the pipe, so it never asks for the ring. */
    if (welcome.handle_count < 3u || welcome.handle_count > 4u ||
        (welcome.handle_count == 4u) != ((welcome.flags & SAVANXP_WM_WELCOME_SHELL) != 0u)) {
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
    g_gfx_wm.window_list_fd = received[3];
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
    for (index = 0; index < 4u; ++index) {
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

static long gfx_open_client(struct savanxp_gfx_context* context) {
    long result = gfx_wm_connect();

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
    gfx_publish_presentation(context);
    /* Before the first frame the WM sees it on the next pass anyway; wake it
     * so the title is right from the start. */
    (void)event_set(context->submit_event_fd);
    return 0;
}

long gfx_open(struct savanxp_gfx_context* context) {
    if (context == 0) {
        return -SAVANXP_EINVAL;
    }

    memset(context, 0, sizeof(*context));
    context->fb_fd = -1;
    context->input_fd = -1;
    context->submit_event_fd = -1;
    context->wake_event_fd = -1;

    return gfx_open_client(context);
}

long gfx_close(struct savanxp_gfx_context* context) {
    long result = 0;
    if (context == 0) {
        return -SAVANXP_EINVAL;
    }

    if (context->mode == kGfxModeClient && context->mapped_view != 0) {
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
    if (savanxp_poll(&pollfd, 1, 0) <= 0 || (pollfd.revents & SAVANXP_POLLIN) == 0) {
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
    long result = gfx_wm_connect();

    return result < 0 ? result : savanxp_dup(g_gfx_wm.events_fd);
}

/* El fd suelto no dice de que ventana es: es la principal, como siempre. */
int gfx_poll_pointer(int fd, struct savanxp_gui_pointer_event* event) {
    return gfx_poll_pointer_for(fd, 0, event);
}

int gfx_poll_window_pointer(const struct savanxp_gfx_context* context, struct savanxp_gui_pointer_event* event) {
    if (context == 0) {
        return -SAVANXP_EINVAL;
    }
    return gfx_poll_pointer_for(context->input_fd, context->window_id, event);
}

int gfx_should_close(const struct savanxp_gfx_context* context) {
    return gfx_shutdown_requested(context);
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

/* Los pedidos al WM se escriben en el header y se avisan con el mismo evento
 * de submit que los frames (savanxp_wm_client_requests). */
static long gfx_notify_wm(const struct savanxp_gfx_context* context) {
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

void gfx_clear(uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t colour) {
    uint32_t pitch_pixels;
    uint32_t y;

    if (pixels == 0 || info == 0 || info->pitch == 0) {
        return;
    }

    pitch_pixels = info->pitch / 4;
    for (y = 0; y < info->height; ++y) {
        gfx_fill_span(pixels + ((size_t)y * pitch_pixels), (int)pitch_pixels, colour);
    }
}

void gfx_pixel(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, uint32_t colour) {
    uint32_t pitch_pixels;
    if (pixels == 0 || info == 0 || x < 0 || y < 0 || x >= (int)info->width || y >= (int)info->height || info->pitch == 0) {
        return;
    }

    pitch_pixels = info->pitch / 4;
    pixels[(uint32_t)y * pitch_pixels + (uint32_t)x] = colour;
}

/* Alpha-composite a colour over the destination pixel. colour is 0x00RRGGBB
 * (as produced by gfx_rgb); alpha 0 = transparent, 255 = opaque. */
static void gfx_pixel_blend(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, uint32_t colour, unsigned int alpha) {
    uint32_t pitch_pixels;
    uint32_t dst;
    unsigned int sr, sg, sb, dr, dg, db, inv;
    if (pixels == 0 || info == 0 || x < 0 || y < 0 || x >= (int)info->width || y >= (int)info->height || info->pitch == 0) {
        return;
    }
    if (alpha == 0) {
        return;
    }
    pitch_pixels = info->pitch / 4;
    if (alpha >= 255) {
        pixels[(uint32_t)y * pitch_pixels + (uint32_t)x] = colour;
        return;
    }
    dst = pixels[(uint32_t)y * pitch_pixels + (uint32_t)x];
    sr = (colour >> 16) & 0xFF; sg = (colour >> 8) & 0xFF; sb = colour & 0xFF;
    dr = (dst >> 16) & 0xFF; dg = (dst >> 8) & 0xFF; db = dst & 0xFF;
    inv = 255 - alpha;
    pixels[(uint32_t)y * pitch_pixels + (uint32_t)x] =
        (((sr * alpha + dr * inv) / 255) << 16) |
        (((sg * alpha + dg * inv) / 255) << 8) |
        ((sb * alpha + db * inv) / 255);
}

void gfx_hline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, uint32_t colour) {
    uint32_t pitch_pixels;
    uint32_t* row;
    int height = 1;
    if (pixels == 0 || !gfx_clip_rect(info, &x, &y, &width, &height)) {
        return;
    }

    pitch_pixels = info->pitch / 4u;
    row = pixels + ((size_t)y * pitch_pixels) + (uint32_t)x;
    gfx_fill_span(row, width, colour);
}

void gfx_vline(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int height, uint32_t colour) {
    uint32_t pitch_pixels;
    int row;
    int width = 1;
    if (pixels == 0 || !gfx_clip_rect(info, &x, &y, &width, &height)) {
        return;
    }

    pitch_pixels = info->pitch / 4u;
    for (row = 0; row < height; ++row) {
        pixels[(size_t)(y + row) * pitch_pixels + (uint32_t)x] = colour;
    }
}

void gfx_rect(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour) {
    uint32_t pitch_pixels;
    int row;
    if (pixels == 0 || !gfx_clip_rect(info, &x, &y, &width, &height)) {
        return;
    }

    pitch_pixels = info->pitch / 4u;
    for (row = 0; row < height; ++row) {
        uint32_t* destination = pixels + ((size_t)(y + row) * pitch_pixels) + (uint32_t)x;
        gfx_fill_span(destination, width, colour);
    }
}

void gfx_frame(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, int width, int height, uint32_t colour) {
    if (width <= 0 || height <= 0) {
        return;
    }
    gfx_hline(pixels, info, x, y, width, colour);
    if (height > 1) {
        gfx_hline(pixels, info, x, y + height - 1, width, colour);
    }
    if (height > 2) {
        gfx_vline(pixels, info, x, y + 1, height - 2, colour);
        if (width > 1) {
            gfx_vline(pixels, info, x + width - 1, y + 1, height - 2, colour);
        }
    }
}

/* --- Proportional Noto Sans (default desktop/UI text) ------------------- */

int gfx_text_width(const char* text) {
    int width = 0;
    if (text == 0) {
        return 0;
    }
    while (*text != '\0') {
        width += sx_noto_glyph(sx_utf8_next(&text))->advance;
    }
    return width;
}

int gfx_text_height(void) {
    return SX_NOTO_LINE_HEIGHT;
}

/* y is the top of the text box; glyphs sit on a baseline SX_NOTO_ASCENT below.
 * has_clip restricts blended pixels to [clip_x0, clip_x1) x [clip_y0, clip_y1)
 * so a glyph that only partially overlaps the caller's clip rect does not get
 * fully re-blended outside it (matters when a layer is repainted once per
 * occlusion fragment, which would otherwise double-blend the same glyph). */
static void gfx_blit_text_impl(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                                int has_clip, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int pen_x = x;
    int baseline = y + SX_NOTO_ASCENT;
    while (text != 0 && *text != '\0') {
        const struct sx_noto_glyph* g = sx_noto_glyph(sx_utf8_next(&text));
        int row;
        int column;
        for (row = 0; row < g->rows; ++row) {
            int py = baseline - g->top + row;
            if (has_clip && (py < clip_y0 || py >= clip_y1)) {
                continue;
            }
            for (column = 0; column < g->width; ++column) {
                int px = pen_x + g->left + column;
                if (has_clip && (px < clip_x0 || px >= clip_x1)) {
                    continue;
                }
                unsigned int alpha = kNotoCoverage[g->offset + (unsigned int)row * g->width + column];
                if (alpha != 0) {
                    gfx_pixel_blend(pixels, info, px, py, colour, alpha);
                }
            }
        }
        pen_x += g->advance;
    }
}

void gfx_blit_text(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    gfx_blit_text_impl(pixels, info, x, y, text, colour, 0, 0, 0, 0, 0);
}

void gfx_blit_text_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                         int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    gfx_blit_text_impl(pixels, info, x, y, text, colour, 1, clip_x0, clip_y0, clip_x1, clip_y1);
}

/* --- 18px RetroSans caption variant ------------------------------------- */

int gfx_text_width_title(const char* text) {
    int pen_x = 0;
    int right = 0;
    if (text == 0) {
        return 0;
    }
    while (*text != '\0') {
        const struct sx_title_glyph* g = sx_title_glyph(sx_utf8_next(&text));
        int glyph_right = pen_x + g->left + g->width;
        if (glyph_right > right) {
            right = glyph_right;
        }
        pen_x += g->advance;
    }
    /* The pixel atlas can extend a final stem past its advance. Keep that
     * pixel in the painter's clip box instead of shaving it off at the edge. */
    return right > pen_x ? right : pen_x;
}

int gfx_text_height_title(void) {
    return SX_TITLE_LINE_HEIGHT;
}

/* The title face has the same baseline contract as the regular UI face, just
 * with a smaller pixel coverage atlas. Keeping the clipped path here
 * means the painter's region/clip behavior is identical for every font. */
static void gfx_blit_text_title_impl(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                                     int has_clip, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int pen_x = x;
    int baseline = y + SX_TITLE_ASCENT;
    while (text != 0 && *text != '\0') {
        const struct sx_title_glyph* g = sx_title_glyph(sx_utf8_next(&text));
        int row;
        int column;
        for (row = 0; row < g->rows; ++row) {
            int py = baseline - g->top + row;
            if (has_clip && (py < clip_y0 || py >= clip_y1)) {
                continue;
            }
            for (column = 0; column < g->width; ++column) {
                int px = pen_x + g->left + column;
                if (has_clip && (px < clip_x0 || px >= clip_x1)) {
                    continue;
                }
                unsigned int alpha = kTitleCoverage[g->offset + (unsigned int)row * g->width + column];
                if (alpha != 0) {
                    gfx_pixel_blend(pixels, info, px, py, colour, alpha);
                }
            }
        }
        pen_x += g->advance;
    }
}

void gfx_blit_text_title(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    gfx_blit_text_title_impl(pixels, info, x, y, text, colour, 0, 0, 0, 0, 0);
}

void gfx_blit_text_title_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                              int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    gfx_blit_text_title_impl(pixels, info, x, y, text, colour, 1, clip_x0, clip_y0, clip_x1, clip_y1);
}

/* --- Fixed 8x16 UniFont (monospace terminal text) ----------------------- */

int gfx_cell_width(void) {
    return SX_CONSOLE_GLYPH_W;
}

int gfx_cell_height(void) {
    return SX_CONSOLE_GLYPH_H;
}

int gfx_text_width_mono(const char* text) {
    int count = 0;
    if (text == 0) {
        return 0;
    }
    /* Celdas, no bytes: en UTF-8 un caracter acentuado ocupa dos bytes pero una
     * sola celda de la grilla monospace. */
    while (*text != '\0') {
        (void)sx_utf8_next(&text);
        ++count;
    }
    return count * SX_CONSOLE_GLYPH_W;
}

/* El clip efectivo es la interseccion del clip del llamador con la superficie:
 * calculado una sola vez, el loop interno no vuelve a validar nada. La version
 * anterior llamaba gfx_pixel() por pixel encendido, y gfx_pixel revalida los
 * cuatro bordes y recalcula info->pitch / 4 -- una division -- en cada pixel.
 * Una grilla de 80x24 son ~245k iteraciones por repintado, asi que esa division
 * por pixel era el costo dominante de dibujar texto monospace. */
static void gfx_blit_text_mono_impl(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                                     int has_clip, int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int clip_left = 0;
    int clip_top = 0;
    int clip_right;
    int clip_bottom;
    uint32_t pitch_pixels;
    int pen_x = x;

    if (pixels == 0 || info == 0 || info->pitch == 0) {
        return;
    }

    clip_right = (int)info->width;
    clip_bottom = (int)info->height;
    if (has_clip) {
        if (clip_x0 > clip_left) {
            clip_left = clip_x0;
        }
        if (clip_y0 > clip_top) {
            clip_top = clip_y0;
        }
        if (clip_x1 < clip_right) {
            clip_right = clip_x1;
        }
        if (clip_y1 < clip_bottom) {
            clip_bottom = clip_y1;
        }
    }
    if (clip_left >= clip_right || clip_top >= clip_bottom) {
        return;
    }

    pitch_pixels = info->pitch / 4;

    while (text != 0 && *text != '\0') {
        const unsigned char* glyph = sx_console_glyph(sx_utf8_next(&text));
        int column_begin = clip_left - pen_x;
        int column_end = clip_right - pen_x;
        int row;

        if (column_begin < 0) {
            column_begin = 0;
        }
        if (column_end > SX_CONSOLE_GLYPH_W) {
            column_end = SX_CONSOLE_GLYPH_W;
        }
        /* La celda entera cayo fuera del clip: el pen igual avanza, porque la
         * grilla tiene que seguir alineada con el texto que si se dibuja. */
        if (column_begin >= column_end) {
            pen_x += SX_CONSOLE_GLYPH_W;
            continue;
        }

        for (row = 0; row < SX_CONSOLE_GLYPH_H; ++row) {
            const int py = y + row;
            const unsigned int bits = glyph[row];
            uint32_t* row_pixels;
            int column;

            if (bits == 0 || py < clip_top || py >= clip_bottom) {
                continue;
            }

            row_pixels = pixels + ((size_t)py * pitch_pixels);
            for (column = column_begin; column < column_end; ++column) {
                if ((bits & (0x80u >> column)) != 0) {
                    row_pixels[pen_x + column] = colour;
                }
            }
        }
        pen_x += SX_CONSOLE_GLYPH_W;
    }
}

void gfx_blit_text_mono(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour) {
    gfx_blit_text_mono_impl(pixels, info, x, y, text, colour, 0, 0, 0, 0, 0);
}

void gfx_blit_text_mono_clip(uint32_t* pixels, const struct savanxp_fb_info* info, int x, int y, const char* text, uint32_t colour,
                              int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    gfx_blit_text_mono_impl(pixels, info, x, y, text, colour, 1, clip_x0, clip_y0, clip_x1, clip_y1);
}
