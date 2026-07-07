/*
 * SavanXP - implementacion del cliente del compositor para el subsistema
 * nativo. Espejo funcional de gfx_open_client y el flujo de submit del SDK
 * posix (subsystems/posix/sdk/v1/runtime/gfx_impl.inc), sobre los fds 3..9
 * heredados del shell y syscalls del baseline. Diferencias deliberadas:
 *  - una sesion global por proceso (el modelo del compositor actual);
 *  - sin backbuffer privado en seccion: el frame del cliente vive donde el
 *    llamador quiera (en Haxe, un Array<Int> contiguo) y se copia a la
 *    superficie compartida al presentar;
 *  - sin dup de los fds: se usan los numeros fijos 3..9.
 */
#include "savanxp_native.h"

/* --- Envolturas del baseline usadas por el cliente ---------------------------- */

static long sxn_gui_map_view(int handle, unsigned long flags) {
    return sxn_syscall3(SXN_SYS_MAP_VIEW, handle, (long)flags, 0);
}

static long sxn_gui_unmap_view(void *base) {
    return sxn_syscall1(SXN_SYS_UNMAP_VIEW, (long)base);
}

static long sxn_gui_event_set(int handle) {
    return sxn_syscall1(SXN_SYS_EVENT_SET, handle);
}

static long sxn_gui_event_reset(int handle) {
    return sxn_syscall1(SXN_SYS_EVENT_RESET, handle);
}

static long sxn_gui_wait_one(int handle, long timeout_ms) {
    return sxn_syscall3(SXN_SYS_WAIT_ONE, handle, timeout_ms, 0);
}

static long sxn_gui_wait_many(const int *handles, unsigned long count,
                              unsigned long flags, long timeout_ms) {
    return sxn_syscall5(SXN_SYS_WAIT_MANY, (long)handles, (long)count, (long)flags,
                        timeout_ms, 0);
}

struct sxn_gui_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

static long sxn_gui_poll(struct sxn_gui_pollfd *fds, unsigned long count, long timeout_ms) {
    return sxn_syscall3(SXN_SYS_POLL, (long)fds, (long)count, timeout_ms);
}

static long sxn_gui_read(int fd, void *buffer, unsigned long count) {
    return sxn_syscall3(SXN_SYS_READ, fd, (long)buffer, (long)count);
}

static void sxn_gui_sleep_ms(long milliseconds) {
    (void)sxn_syscall1(SXN_SYS_SLEEP_MS, milliseconds);
}

static int sxn_gui_is_error(long value) {
    return value < 0 && value > -4096;
}

/* --- Estado de la sesion ------------------------------------------------------ */

#define SXN_GUI_EINVAL 22
#define SXN_GUI_ENODEV 19
#define SXN_GUI_EPIPE 32

static struct {
    void *mapped_view;
    struct sxn_gui_surface_header *header;
    struct sxn_gui_batch *batches;
    unsigned char *shared_pixels;
    unsigned int notified_width;
    unsigned int notified_height;
    int open;
} g_gui;

long sxn_gui_open(void) {
    long mapped = sxn_gui_map_view(SXN_GUI_FD_SECTION, SXN_SECTION_READ | SXN_SECTION_WRITE);
    struct sxn_gui_surface_header *header;

    if (sxn_gui_is_error(mapped) || mapped == 0) {
        return -SXN_GUI_ENODEV;
    }

    header = (struct sxn_gui_surface_header *)mapped;
    if (header->magic != SXN_GUI_SURFACE_MAGIC ||
        header->version != SXN_GUI_SURFACE_VERSION_3 ||
        header->pixels_offset < sizeof(*header) ||
        header->command_offset < sizeof(*header) ||
        header->batch_capacity == 0 ||
        header->batch_capacity > SXN_GUI_BATCH_CAPACITY ||
        header->rect_capacity == 0 ||
        header->rect_capacity > SXN_GUI_BATCH_MAX_RECTS ||
        header->command_offset >= header->pixels_offset ||
        header->pixels_offset - header->command_offset <
            header->batch_capacity * sizeof(struct sxn_gui_batch) ||
        header->info.width == 0 ||
        header->info.height == 0 ||
        header->info.pitch < header->info.width * 4u ||
        header->info.buffer_size < header->info.pitch * header->info.height) {
        (void)sxn_gui_unmap_view((void *)mapped);
        return -SXN_GUI_EINVAL;
    }

    g_gui.mapped_view = (void *)mapped;
    g_gui.header = header;
    g_gui.batches = (struct sxn_gui_batch *)((unsigned char *)mapped + header->command_offset);
    g_gui.shared_pixels = (unsigned char *)mapped + header->pixels_offset;
    g_gui.notified_width = header->info.width;
    g_gui.notified_height = header->info.height;
    g_gui.open = 1;
    return 0;
}

void sxn_gui_close(void) {
    if (g_gui.mapped_view != 0) {
        (void)sxn_gui_unmap_view(g_gui.mapped_view);
    }
    g_gui.mapped_view = 0;
    g_gui.header = 0;
    g_gui.batches = 0;
    g_gui.shared_pixels = 0;
    g_gui.open = 0;
}

unsigned int sxn_gui_width(void) {
    return g_gui.open ? g_gui.header->info.width : 0;
}

unsigned int sxn_gui_height(void) {
    return g_gui.open ? g_gui.header->info.height : 0;
}

unsigned int sxn_gui_stride_pixels(void) {
    return g_gui.open ? g_gui.header->info.pitch / 4u : 0;
}

unsigned long sxn_gui_composed_sequence(void) {
    return g_gui.open ? g_gui.header->composed_sequence : 0;
}

int sxn_gui_should_close(void) {
    if (!g_gui.open) {
        return 1;
    }
    return sxn_gui_wait_one(SXN_GUI_FD_SHUTDOWN_EVENT, 0) >= 0 ? 1 : 0;
}

/* Espera hasta que (submit - composed) < limite, con corte por shutdown. */
static long sxn_gui_wait_below(unsigned long limit) {
    while ((g_gui.header->submit_sequence - g_gui.header->composed_sequence) >= limit) {
        int handles[2];
        long wait_result;

        if (sxn_gui_wait_one(SXN_GUI_FD_SHUTDOWN_EVENT, 0) >= 0) {
            return -SXN_GUI_EPIPE;
        }

        (void)sxn_gui_event_reset(SXN_GUI_FD_RETIRE_EVENT);
        if ((g_gui.header->submit_sequence - g_gui.header->composed_sequence) < limit) {
            break;
        }

        handles[0] = SXN_GUI_FD_SHUTDOWN_EVENT;
        handles[1] = SXN_GUI_FD_RETIRE_EVENT;
        wait_result = sxn_gui_wait_many(handles, 2, SXN_WAIT_FLAG_ANY, -1);
        if (wait_result < 0) {
            return wait_result;
        }
        if (wait_result == 0) {
            return -SXN_GUI_EPIPE;
        }
    }
    return 0;
}

static long sxn_gui_submit(const struct sxn_gui_dirty_rect *rect, uint32_t flags) {
    struct sxn_gui_batch *batch;
    uint64_t sequence;
    uint32_t index;
    long result;

    result = sxn_gui_wait_below(g_gui.header->batch_capacity);
    if (result < 0) {
        return result;
    }

    sequence = g_gui.header->submit_sequence + 1u;
    batch = &g_gui.batches[(sequence - 1u) % g_gui.header->batch_capacity];
    batch->rect_count = 1;
    batch->flags = flags;
    batch->rects[0] = *rect;
    for (index = 1; index < g_gui.header->rect_capacity && index < SXN_GUI_BATCH_MAX_RECTS;
         ++index) {
        batch->rects[index].x = 0;
        batch->rects[index].y = 0;
        batch->rects[index].width = 0;
        batch->rects[index].height = 0;
    }

    batch->submit_sequence = sequence;
    __asm__ volatile("" ::: "memory");
    g_gui.header->submit_sequence = sequence;
    return sxn_gui_event_set(SXN_GUI_FD_SUBMIT_EVENT);
}

/* Copia el rectangulo del frame del cliente a la superficie compartida. Ambos
 * usan el layout de la superficie (filas de pitch bytes). */
static void sxn_gui_copy_rect(const void *frame, unsigned int x, unsigned int y,
                              unsigned int width, unsigned int height) {
    const unsigned char *source = (const unsigned char *)frame;
    unsigned long pitch = g_gui.header->info.pitch;
    unsigned long row_bytes = (unsigned long)width * 4u;
    unsigned int row;

    for (row = 0; row < height; ++row) {
        memcpy(g_gui.shared_pixels + (unsigned long)(y + row) * pitch + (unsigned long)x * 4u,
               source + (unsigned long)(y + row) * pitch + (unsigned long)x * 4u,
               row_bytes);
    }
}

long sxn_gui_present_region(const void *frame, unsigned int x, unsigned int y,
                            unsigned int width, unsigned int height) {
    struct sxn_gui_dirty_rect rect;
    long result;

    if (!g_gui.open || frame == 0 || width == 0 || height == 0 ||
        x >= g_gui.header->info.width || y >= g_gui.header->info.height ||
        width > g_gui.header->info.width - x || height > g_gui.header->info.height - y) {
        return -SXN_GUI_EINVAL;
    }

    /* Idle: no pisar la superficie mientras el compositor tiene frames sin
     * componer (mismo criterio que el cliente posix). */
    result = sxn_gui_wait_below(1);
    if (result < 0) {
        return result;
    }

    sxn_gui_copy_rect(frame, x, y, width, height);
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    return sxn_gui_submit(&rect, 0);
}

long sxn_gui_present(const void *frame) {
    if (!g_gui.open) {
        return -SXN_GUI_EINVAL;
    }
    return sxn_gui_present_region(frame, 0, 0, g_gui.header->info.width,
                                  g_gui.header->info.height);
}

int sxn_gui_poll_event(struct sxn_gui_input_event *event) {
    struct sxn_gui_pollfd pollfd;
    long result;

    if (!g_gui.open || event == 0) {
        return -SXN_GUI_EINVAL;
    }

    /* Resize: el compositor muta width/height del header al redimensionar la
     * ventana; se sintetiza un evento como hace el cliente posix. */
    if (g_gui.header->info.width != g_gui.notified_width ||
        g_gui.header->info.height != g_gui.notified_height) {
        g_gui.notified_width = g_gui.header->info.width;
        g_gui.notified_height = g_gui.header->info.height;
        event->type = SXN_GUI_EVENT_RESIZED;
        event->key = g_gui.notified_width;
        event->ascii = (int32_t)g_gui.notified_height;
        return 1;
    }

    pollfd.fd = SXN_GUI_FD_INPUT;
    pollfd.events = SXN_POLLIN | SXN_POLLHUP;
    pollfd.revents = 0;
    if (sxn_gui_poll(&pollfd, 1, 0) <= 0 || (pollfd.revents & SXN_POLLIN) == 0) {
        return 0;
    }

    result = sxn_gui_read(SXN_GUI_FD_INPUT, event, sizeof(*event));
    if (result < 0) {
        return (int)result;
    }
    return result == (long)sizeof(*event) ? 1 : 0;
}

/* Utilidad para demos: dormir sin quemar CPU. */
void sxn_sleep_ms(long milliseconds) {
    sxn_gui_sleep_ms(milliseconds);
}
