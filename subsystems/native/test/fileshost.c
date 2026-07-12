/*
 * SavanXP - fileshost: harness headless para la filesapp portada a Haxe.
 *
 * Maneja la app (navegador de directorios): valida que el Listbox renderice con
 * el item 0 seleccionado (navy), mueve la seleccion con las flechas (verifica
 * que el highlight se mueve de fila), ejercita Enter (entrar a un directorio) +
 * Backspace (subir) y por ultimo ESC para cerrar (exit 0). La navegacion real
 * se confirma tambien por el serial (la app loguea cada ruta). Imprime FILES
 * HOST PASS/FAIL.
 *
 * Coordenadas segun el layout de haxe-files/Main.hx (listbox 12,30,W-24,H-70).
 */
#include "savanxp/libc.h"

#define FILES_WIDTH 460u
#define FILES_HEIGHT 360u
#define FILES_PAGE 4096u
#define FILES_CLIENT_PATH "/disk/bin/filesapp"
#define FILES_DEADLINE_MS 8000ul

#define FILES_NAVY 0x00000080u  /* item seleccionado */
#define FILES_FIELD 0x00FFFFFFu /* fondo del listbox */
/* Puntos a la derecha (mas alla del texto) de la fila 0 y la fila 1 del
 * listbox: y+2 + fila*altoTexto(18). Fila 0 -> y=40, fila 1 -> y=58. */
#define FILES_ROW0_X 440u
#define FILES_ROW0_Y 40u
#define FILES_ROW1_X 440u
#define FILES_ROW1_Y 58u

static int fail(const char *reason) {
    printf("fileshost: %s\n", reason);
    puts("FILES HOST FAIL\n");
    return 1;
}

static struct savanxp_gpu_client_surface_header *g_header;
static struct savanxp_gpu_dirty_rect_batch *g_batches;
static uint32_t *g_pixels;
static int g_submit_event;
static int g_retire_event;

static int compose_next_frame(void) {
    unsigned long deadline = uptime_ms() + FILES_DEADLINE_MS;
    while (uptime_ms() < deadline) {
        if (g_header->submit_sequence > g_header->composed_sequence) {
            while (g_header->composed_sequence < g_header->submit_sequence) {
                uint64_t sequence = g_header->composed_sequence + 1u;
                const struct savanxp_gpu_dirty_rect_batch *batch =
                    &g_batches[(sequence - 1u) % g_header->batch_capacity];
                if (batch->submit_sequence != sequence) {
                    return -1;
                }
                g_header->retired_sequence = sequence;
                g_header->composed_sequence = sequence;
                (void)event_set(g_retire_event);
            }
            return 0;
        }
        (void)wait_one(g_submit_event, 100);
        (void)event_reset(g_submit_event);
    }
    return -2;
}

static uint32_t pixel_at(unsigned int x, unsigned int y) {
    return g_pixels[y * FILES_WIDTH + x];
}

static int send_key(int fd, uint32_t key) {
    struct savanxp_input_event event;
    event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
    event.key = key;
    event.ascii = 0;
    return write(fd, &event, sizeof(event)) == (long)sizeof(event) ? 0 : -1;
}

int main(void) {
    void *view;
    unsigned long command_bytes;
    unsigned long pixels_offset;
    unsigned long buffer_size;
    long section_fd;
    int input_pipe[2] = {-1, -1};
    int mouse_pipe[2] = {-1, -1};
    int launch_pipe[2] = {-1, -1};
    int submit_event;
    int retire_event;
    int shutdown_event;
    long pid;
    int status = -1;
    int guard_fds[7];
    int guard_index;

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        guard_fds[guard_index] = dup(0);
        if (guard_fds[guard_index] < 0) {
            return fail("no se pudieron reservar los fds guardia");
        }
    }

    command_bytes = SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch);
    pixels_offset = (sizeof(*g_header) + command_bytes + (FILES_PAGE - 1u)) & ~(unsigned long)(FILES_PAGE - 1u);
    buffer_size = (unsigned long)FILES_WIDTH * 4u * FILES_HEIGHT;

    section_fd = section_create(pixels_offset + buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section_fd < 0) {
        return fail("section_create fallo");
    }
    view = map_view((int)section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)view)) {
        return fail("map_view fallo");
    }

    g_header = (struct savanxp_gpu_client_surface_header *)view;
    memset(view, 0, sizeof(*g_header) + command_bytes);
    g_header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    g_header->command_offset = (uint32_t)sizeof(*g_header);
    g_header->pixels_offset = (uint32_t)pixels_offset;
    g_header->info.width = FILES_WIDTH;
    g_header->info.height = FILES_HEIGHT;
    g_header->info.pitch = FILES_WIDTH * 4u;
    g_header->info.bpp = 32;
    g_header->info.buffer_size = (uint32_t)buffer_size;
    g_header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_3;
    g_header->pixel_format = SAVANXP_GPU_SURFACE_FORMAT_BGRX8888;
    g_header->batch_capacity = SAVANXP_GPU_CLIENT_BATCH_CAPACITY;
    g_header->rect_capacity = SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    g_batches = (struct savanxp_gpu_dirty_rect_batch *)((unsigned char *)view + g_header->command_offset);
    g_pixels = (uint32_t *)((unsigned char *)view + pixels_offset);
    memset(g_pixels, 0, buffer_size);

    submit_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    retire_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    shutdown_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (submit_event < 0 || retire_event < 0 || shutdown_event < 0 ||
        pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || pipe(launch_pipe) < 0) {
        return fail("no se pudieron crear eventos/pipes");
    }
    g_submit_event = submit_event;
    g_retire_event = retire_event;

    pid = fork();
    if (pid < 0) {
        return fail("fork fallo");
    }
    if (pid == 0) {
        const char *argv[2] = {FILES_CLIENT_PATH, 0};
        if (dup2((int)section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(submit_event, 6) < 0 ||
            dup2(retire_event, 7) < 0 ||
            dup2(shutdown_event, 8) < 0 ||
            dup2(launch_pipe[1], 9) < 0) {
            exit(1);
        }
        (void)exec(FILES_CLIENT_PATH, argv, 1);
        printf("fileshost: exec de %s fallo\n", FILES_CLIENT_PATH);
        exit(1);
    }

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        (void)close(guard_fds[guard_index]);
    }

    /* 1) Frame inicial: el listbox rinde con el item 0 seleccionado (navy) y la
     *    fila 1 con fondo FIELD. */
    if (compose_next_frame() != 0) {
        return fail("no llego el frame inicial");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_NAVY) {
        return fail("item 0 no resaltado en el frame inicial");
    }
    if (pixel_at(FILES_ROW1_X, FILES_ROW1_Y) != FILES_FIELD) {
        return fail("fila 1 no tiene fondo FIELD (listbox no rinde)");
    }

    /* 2) Flecha abajo: el resaltado baja a la fila 1. */
    if (send_key(input_pipe[1], SAVANXP_KEY_DOWN) != 0) {
        return fail("no se pudo enviar flecha abajo");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras flecha abajo");
    }
    if (pixel_at(FILES_ROW1_X, FILES_ROW1_Y) != FILES_NAVY) {
        return fail("la seleccion no bajo a la fila 1");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_FIELD) {
        return fail("la fila 0 sigue resaltada tras bajar");
    }

    /* 3) Flecha arriba: vuelve a la fila 0. */
    if (send_key(input_pipe[1], SAVANXP_KEY_UP) != 0) {
        return fail("no se pudo enviar flecha arriba");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras flecha arriba");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_NAVY) {
        return fail("la seleccion no volvio a la fila 0");
    }

    /* 4) Enter: entra al directorio seleccionado (item 0 de la raiz = un dir). */
    if (send_key(input_pipe[1], SAVANXP_KEY_ENTER) != 0) {
        return fail("no se pudo enviar Enter");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras entrar al directorio");
    }

    /* 5) Backspace: sube al directorio padre. */
    if (send_key(input_pipe[1], SAVANXP_KEY_BACKSPACE) != 0) {
        return fail("no se pudo enviar Backspace");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras subir");
    }

    /* 6) ESC: la app se cierra sola (exit 0). */
    if (send_key(input_pipe[1], SAVANXP_KEY_ESC) != 0) {
        return fail("no se pudo enviar ESC");
    }
    if (waitpid((int)pid, &status) != pid) {
        return fail("waitpid fallo");
    }
    if (status != 0) {
        printf("fileshost: cliente salio con %d\n", status);
        puts("FILES HOST FAIL\n");
        return 1;
    }

    printf("fileshost: render + seleccion + navegacion OK, frames=%d\n", (int)g_header->composed_sequence);
    puts("FILES HOST PASS\n");
    return 0;
}
