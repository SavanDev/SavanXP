/*
 * SavanXP - sxguihost: harness headless para sxguiapp INTERACTIVA (Fase 3).
 *
 * Interpreta el rol del compositor (como test/guihost.c) y ademas MANEJA la app:
 * compone el frame inicial y valida el render sxgui (boton levantado + lampara
 * apagada), luego envia un press del puntero sobre "Aceptar" (verifica que el
 * boton se hunde), un release (verifica que se enciende la lampara verde y el
 * boton vuelve a levantarse), y por ultimo senala shutdown y comprueba que el
 * cliente salga limpio. Imprime SXGUI HOST PASS/FAIL.
 *
 * Las coordenadas replican el layout FIJO de haxe-sxgui/Main.hx.
 */
#include "savanxp/libc.h"

#define SXGUI_WIDTH 260u
#define SXGUI_HEIGHT 180u
#define SXGUI_PAGE 4096u
#define SXGUI_CLIENT_PATH "/disk/bin/sxguiapp"
#define SXGUI_DEADLINE_MS 8000ul

/* Pixeles firma y coordenadas de interaccion (layout de haxe-sxgui/Main.hx). */
#define SXGUI_FACE 0x00C0C0C0u
#define SXGUI_LIGHT 0x00FFFFFFu
#define SXGUI_SHADOW 0x00808080u
#define SXGUI_GREEN 0x00008000u
#define SXGUI_BTN_TOP_X 20u  /* fila superior del boton "Aceptar" (16,96,96,26) */
#define SXGUI_BTN_TOP_Y 96u
#define SXGUI_LAMP_X 238u    /* centro de la lampara (228,36,20,20) */
#define SXGUI_LAMP_Y 46u
#define SXGUI_ACEPTAR_CX 64  /* centro del boton "Aceptar" */
#define SXGUI_ACEPTAR_CY 109

static int fail(const char *reason) {
    printf("sxguihost: %s\n", reason);
    puts("SXGUI HOST FAIL\n");
    return 1;
}

/* Estado del harness compartido por los helpers. */
static struct savanxp_gpu_client_surface_header *g_header;
static struct savanxp_gpu_dirty_rect_batch *g_batches;
static uint32_t *g_pixels;
static int g_submit_event;
static int g_retire_event;

/* Espera y compone el proximo frame sometido (avanza composed y senala retire).
 * Devuelve 0 al componer al menos uno, -1 en batch inconsistente, -2 timeout. */
static int compose_next_frame(void) {
    unsigned long deadline = uptime_ms() + SXGUI_DEADLINE_MS;
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
    return g_pixels[y * SXGUI_WIDTH + x];
}

static int send_pointer(int fd, int x, int y, uint32_t buttons) {
    struct savanxp_gui_pointer_event event;
    event.x = x;
    event.y = y;
    event.buttons = buttons;
    return savanxp_write(fd, &event, sizeof(event)) == (long)sizeof(event) ? 0 : -1;
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
        guard_fds[guard_index] = savanxp_dup(0);
        if (guard_fds[guard_index] < 0) {
            return fail("no se pudieron reservar los fds guardia");
        }
    }

    command_bytes = SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch);
    pixels_offset = (sizeof(*g_header) + command_bytes + (SXGUI_PAGE - 1u)) & ~(unsigned long)(SXGUI_PAGE - 1u);
    buffer_size = (unsigned long)SXGUI_WIDTH * 4u * SXGUI_HEIGHT;

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
    g_header->info.width = SXGUI_WIDTH;
    g_header->info.height = SXGUI_HEIGHT;
    g_header->info.pitch = SXGUI_WIDTH * 4u;
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
        savanxp_pipe(input_pipe) < 0 || savanxp_pipe(mouse_pipe) < 0 || savanxp_pipe(launch_pipe) < 0) {
        return fail("no se pudieron crear eventos/pipes");
    }
    g_submit_event = submit_event;
    g_retire_event = retire_event;

    pid = savanxp_fork();
    if (pid < 0) {
        return fail("fork fallo");
    }
    if (pid == 0) {
        const char *argv[2] = {SXGUI_CLIENT_PATH, 0};
        if (savanxp_dup2((int)section_fd, 3) < 0 ||
            savanxp_dup2(input_pipe[0], 4) < 0 ||
            savanxp_dup2(mouse_pipe[0], 5) < 0 ||
            savanxp_dup2(submit_event, 6) < 0 ||
            savanxp_dup2(retire_event, 7) < 0 ||
            savanxp_dup2(shutdown_event, 8) < 0 ||
            savanxp_dup2(launch_pipe[1], 9) < 0) {
            exit(1);
        }
        (void)exec(SXGUI_CLIENT_PATH, argv, 1);
        printf("sxguihost: exec de %s fallo\n", SXGUI_CLIENT_PATH);
        exit(1);
    }

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        (void)savanxp_close(guard_fds[guard_index]);
    }

    /* 1) Frame inicial: boton levantado (borde superior LIGHT) y lampara apagada
     *    (FACE). */
    if (compose_next_frame() != 0) {
        return fail("no llego el frame inicial");
    }
    if (pixel_at(SXGUI_BTN_TOP_X, SXGUI_BTN_TOP_Y) != SXGUI_LIGHT) {
        return fail("boton no levantado en el frame inicial");
    }
    if (pixel_at(SXGUI_LAMP_X, SXGUI_LAMP_Y) != SXGUI_FACE) {
        return fail("lampara no apagada en el frame inicial");
    }

    /* 2) Press sobre "Aceptar": el boton debe hundirse (borde superior SHADOW). */
    if (send_pointer(mouse_pipe[1], SXGUI_ACEPTAR_CX, SXGUI_ACEPTAR_CY, SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
        return fail("no se pudo enviar el press");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame del press");
    }
    if (pixel_at(SXGUI_BTN_TOP_X, SXGUI_BTN_TOP_Y) != SXGUI_SHADOW) {
        return fail("el boton no se hundio con el press (hit-test/estado)");
    }

    /* 3) Release sobre "Aceptar": click -> lampara verde + boton levantado. */
    if (send_pointer(mouse_pipe[1], SXGUI_ACEPTAR_CX, SXGUI_ACEPTAR_CY, 0u) != 0) {
        return fail("no se pudo enviar el release");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame del release");
    }
    if (pixel_at(SXGUI_LAMP_X, SXGUI_LAMP_Y) != SXGUI_GREEN) {
        return fail("la lampara no se encendio con el click");
    }
    if (pixel_at(SXGUI_BTN_TOP_X, SXGUI_BTN_TOP_Y) != SXGUI_LIGHT) {
        return fail("el boton no volvio a levantarse tras el release");
    }

    /* 4) Shutdown y salida limpia. */
    (void)event_set(shutdown_event);
    if (savanxp_waitpid((int)pid, &status) != pid) {
        return fail("waitpid fallo");
    }
    if (status != 0) {
        printf("sxguihost: cliente salio con %d\n", status);
        puts("SXGUI HOST FAIL\n");
        return 1;
    }

    printf("sxguihost: interaccion OK (press=hundido, click=lampara verde), frames=%d\n",
           (int)g_header->composed_sequence);
    puts("SXGUI HOST PASS\n");
    return 0;
}
