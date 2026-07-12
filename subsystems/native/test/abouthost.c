/*
 * SavanXP - abouthost: harness headless para la aboutapp portada a Haxe.
 *
 * Interpreta el rol del compositor y maneja la app: valida el render (fondo
 * FACE, linea etched de un group box, boton levantado), ejercita el boton
 * "Refrescar" (press = hundido, release = levantado) y por ultimo hace click en
 * "Cerrar" y comprueba que la app se cierre sola con exit 0 (la accion de
 * cerrar, no un shutdown externo). Imprime ABOUT HOST PASS/FAIL.
 *
 * Coordenadas segun el layout de haxe-about/Main.hx (igual que aboutapp.c).
 */
#include "savanxp/libc.h"

#define ABOUT_WIDTH 456u
#define ABOUT_HEIGHT 430u
#define ABOUT_PAGE 4096u
#define ABOUT_CLIENT_PATH "/disk/bin/aboutapp"
#define ABOUT_DEADLINE_MS 8000ul

#define ABOUT_FACE 0x00C0C0C0u
#define ABOUT_LIGHT 0x00FFFFFFu
#define ABOUT_SHADOW 0x00808080u
/* Linea SHADOW del marco etched del group box "Sistema" (16,56,424,148): la
 * linea superior queda en y = 56 + altoTexto/2 = 65. */
#define ABOUT_GROUP_X 200u
#define ABOUT_GROUP_Y 65u
/* Borde superior de los botones (y=392) y sus centros. */
#define ABOUT_REFRESH_TOP_X 66u
#define ABOUT_CLOSE_TOP_X 178u
#define ABOUT_BTN_TOP_Y 392u
#define ABOUT_REFRESH_CX 66
#define ABOUT_CLOSE_CX 178
#define ABOUT_BTN_CY 405

static int fail(const char *reason) {
    printf("abouthost: %s\n", reason);
    puts("ABOUT HOST FAIL\n");
    return 1;
}

static struct savanxp_gpu_client_surface_header *g_header;
static struct savanxp_gpu_dirty_rect_batch *g_batches;
static uint32_t *g_pixels;
static int g_submit_event;
static int g_retire_event;

static int compose_next_frame(void) {
    unsigned long deadline = uptime_ms() + ABOUT_DEADLINE_MS;
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
    return g_pixels[y * ABOUT_WIDTH + x];
}

static int send_pointer(int fd, int x, int y, uint32_t buttons) {
    struct savanxp_gui_pointer_event event;
    event.x = x;
    event.y = y;
    event.buttons = buttons;
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
    pixels_offset = (sizeof(*g_header) + command_bytes + (ABOUT_PAGE - 1u)) & ~(unsigned long)(ABOUT_PAGE - 1u);
    buffer_size = (unsigned long)ABOUT_WIDTH * 4u * ABOUT_HEIGHT;

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
    g_header->info.width = ABOUT_WIDTH;
    g_header->info.height = ABOUT_HEIGHT;
    g_header->info.pitch = ABOUT_WIDTH * 4u;
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
        const char *argv[2] = {ABOUT_CLIENT_PATH, 0};
        if (dup2((int)section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(submit_event, 6) < 0 ||
            dup2(retire_event, 7) < 0 ||
            dup2(shutdown_event, 8) < 0 ||
            dup2(launch_pipe[1], 9) < 0) {
            exit(1);
        }
        (void)exec(ABOUT_CLIENT_PATH, argv, 1);
        printf("abouthost: exec de %s fallo\n", ABOUT_CLIENT_PATH);
        exit(1);
    }

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        (void)close(guard_fds[guard_index]);
    }

    /* 1) Frame inicial: fondo FACE, marco etched del group box (SHADOW) y boton
     *    "Cerrar" levantado (LIGHT). */
    if (compose_next_frame() != 0) {
        return fail("no llego el frame inicial");
    }
    if (pixel_at(4, 4) != ABOUT_FACE) {
        return fail("fondo FACE ausente");
    }
    if (pixel_at(ABOUT_GROUP_X, ABOUT_GROUP_Y) != ABOUT_SHADOW) {
        return fail("marco etched del group box ausente");
    }
    if (pixel_at(ABOUT_CLOSE_TOP_X, ABOUT_BTN_TOP_Y) != ABOUT_LIGHT) {
        return fail("boton Cerrar no levantado en el frame inicial");
    }

    /* 2) "Refrescar": press hunde el boton, release lo levanta. */
    if (send_pointer(mouse_pipe[1], ABOUT_REFRESH_CX, ABOUT_BTN_CY, SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
        return fail("no se pudo enviar el press de Refrescar");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame del press de Refrescar");
    }
    if (pixel_at(ABOUT_REFRESH_TOP_X, ABOUT_BTN_TOP_Y) != ABOUT_SHADOW) {
        return fail("Refrescar no se hundio con el press");
    }
    if (send_pointer(mouse_pipe[1], ABOUT_REFRESH_CX, ABOUT_BTN_CY, 0u) != 0) {
        return fail("no se pudo enviar el release de Refrescar");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame del release de Refrescar");
    }
    if (pixel_at(ABOUT_REFRESH_TOP_X, ABOUT_BTN_TOP_Y) != ABOUT_LIGHT) {
        return fail("Refrescar no volvio a levantarse tras el release");
    }

    /* 3) "Cerrar": click cierra la app (se cierra sola, exit 0). */
    if (send_pointer(mouse_pipe[1], ABOUT_CLOSE_CX, ABOUT_BTN_CY, SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
        return fail("no se pudo enviar el press de Cerrar");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame del press de Cerrar");
    }
    if (send_pointer(mouse_pipe[1], ABOUT_CLOSE_CX, ABOUT_BTN_CY, 0u) != 0) {
        return fail("no se pudo enviar el release de Cerrar");
    }

    /* La app se cierra sola al hacer click en "Cerrar". */
    if (waitpid((int)pid, &status) != pid) {
        return fail("waitpid fallo");
    }
    if (status != 0) {
        printf("abouthost: cliente salio con %d\n", status);
        puts("ABOUT HOST FAIL\n");
        return 1;
    }

    printf("abouthost: render + Refrescar + Cerrar OK, frames=%d\n", (int)g_header->composed_sequence);
    puts("ABOUT HOST PASS\n");
    return 0;
}
