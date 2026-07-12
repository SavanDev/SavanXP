/*
 * SavanXP - sxguihost: harness headless para sxguiapp (Fase 3).
 *
 * Interpreta el rol del compositor (como test/guihost.c) para la primera app
 * sxgui-style nativa: crea la seccion compartida, instala los canales en los
 * fds 3..9, lanza /disk/bin/sxguiapp, compone los frames que somete y verifica
 * pixeles firma del render sxgui (fondo FACE + bisel levantado de un boton),
 * ademas de que el cliente salga limpio. Imprime SXGUI HOST PASS/FAIL.
 *
 * Es una version enfocada de guihost.c: sin canal de puntero. Las coordenadas
 * de los pixeles firma replican el layout FIJO de haxe-sxgui/Main.hx.
 */
#include "savanxp/libc.h"

#define SXGUI_WIDTH 260u
#define SXGUI_HEIGHT 180u
#define SXGUI_PAGE 4096u
#define SXGUI_CLIENT_PATH "/disk/bin/sxguiapp"
#define SXGUI_DEADLINE_MS 8000ul
#define SXGUI_EXPECTED_FRAMES 3ul

/* Pixeles firma (deben coincidir con el layout de haxe-sxgui/Main.hx). */
#define SXGUI_FACE 0x00C0C0C0u
#define SXGUI_LIGHT 0x00FFFFFFu
#define SXGUI_FACE_X 4u
#define SXGUI_FACE_Y 4u
#define SXGUI_BEVEL_X 20u /* fila superior del boton "Aceptar" en (16,96,96,26) */
#define SXGUI_BEVEL_Y 96u

static int fail(const char *reason) {
    printf("sxguihost: %s\n", reason);
    puts("SXGUI HOST FAIL\n");
    return 1;
}

int main(void) {
    struct savanxp_gpu_client_surface_header *header;
    struct savanxp_gpu_dirty_rect_batch *batches;
    uint32_t *pixels;
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
    unsigned long deadline;
    int checked = 0;
    int status = -1;
    int guard_fds[7];
    int guard_index;

    /* Reservar los fds 3..9 antes de crear recursos (guihost/sxguihost arrancan
     * con la tabla casi vacia; si no, los dup2 del hijo se pisarian). */
    for (guard_index = 0; guard_index < 7; ++guard_index) {
        guard_fds[guard_index] = dup(0);
        if (guard_fds[guard_index] < 0) {
            return fail("no se pudieron reservar los fds guardia");
        }
    }

    command_bytes = SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch);
    pixels_offset = (sizeof(*header) + command_bytes + (SXGUI_PAGE - 1u)) & ~(unsigned long)(SXGUI_PAGE - 1u);
    buffer_size = (unsigned long)SXGUI_WIDTH * 4u * SXGUI_HEIGHT;

    section_fd = section_create(pixels_offset + buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section_fd < 0) {
        return fail("section_create fallo");
    }
    view = map_view((int)section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)view)) {
        return fail("map_view fallo");
    }

    header = (struct savanxp_gpu_client_surface_header *)view;
    memset(view, 0, sizeof(*header) + command_bytes);
    header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    header->command_offset = (uint32_t)sizeof(*header);
    header->pixels_offset = (uint32_t)pixels_offset;
    header->info.width = SXGUI_WIDTH;
    header->info.height = SXGUI_HEIGHT;
    header->info.pitch = SXGUI_WIDTH * 4u;
    header->info.bpp = 32;
    header->info.buffer_size = (uint32_t)buffer_size;
    header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_3;
    header->pixel_format = SAVANXP_GPU_SURFACE_FORMAT_BGRX8888;
    header->batch_capacity = SAVANXP_GPU_CLIENT_BATCH_CAPACITY;
    header->rect_capacity = SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    batches = (struct savanxp_gpu_dirty_rect_batch *)((unsigned char *)view + header->command_offset);
    pixels = (uint32_t *)((unsigned char *)view + pixels_offset);
    memset(pixels, 0, buffer_size);

    submit_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    retire_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    shutdown_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (submit_event < 0 || retire_event < 0 || shutdown_event < 0 ||
        pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || pipe(launch_pipe) < 0) {
        return fail("no se pudieron crear eventos/pipes");
    }

    pid = fork();
    if (pid < 0) {
        return fail("fork fallo");
    }
    if (pid == 0) {
        const char *argv[2] = {SXGUI_CLIENT_PATH, 0};
        if (dup2((int)section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(submit_event, 6) < 0 ||
            dup2(retire_event, 7) < 0 ||
            dup2(shutdown_event, 8) < 0 ||
            dup2(launch_pipe[1], 9) < 0) {
            exit(1);
        }
        (void)exec(SXGUI_CLIENT_PATH, argv, 1);
        printf("sxguihost: exec de %s fallo\n", SXGUI_CLIENT_PATH);
        exit(1);
    }

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        (void)close(guard_fds[guard_index]);
    }

    deadline = uptime_ms() + SXGUI_DEADLINE_MS;
    while (uptime_ms() < deadline && header->composed_sequence < SXGUI_EXPECTED_FRAMES) {
        uint64_t submitted;

        (void)wait_one(submit_event, 100);
        (void)event_reset(submit_event);

        submitted = header->submit_sequence;
        while (header->composed_sequence < submitted) {
            uint64_t sequence = header->composed_sequence + 1u;
            const struct savanxp_gpu_dirty_rect_batch *batch =
                &batches[(sequence - 1u) % header->batch_capacity];

            if (batch->submit_sequence != sequence) {
                return fail("secuencia de batch inconsistente");
            }
            if (batch->rect_count == 0 || batch->rect_count > header->rect_capacity) {
                return fail("rect_count invalido en el batch");
            }

            header->retired_sequence = sequence;
            header->composed_sequence = sequence;
            (void)event_set(retire_event);
        }

        /* Con el primer frame compuesto: verificar los pixeles firma sxgui y
         * mandar una tecla. */
        if (!checked && header->composed_sequence >= 1) {
            struct savanxp_input_event event;
            uint32_t face = pixels[SXGUI_FACE_Y * SXGUI_WIDTH + SXGUI_FACE_X];
            uint32_t bevel = pixels[SXGUI_BEVEL_Y * SXGUI_WIDTH + SXGUI_BEVEL_X];
            if (face != SXGUI_FACE) {
                return fail("fondo FACE ausente tras el primer frame");
            }
            if (bevel != SXGUI_LIGHT) {
                return fail("bisel levantado del boton ausente (raised/render sxgui)");
            }
            event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
            event.key = SAVANXP_KEY_ENTER;
            event.ascii = 13;
            (void)write(input_pipe[1], &event, sizeof(event));
            checked = 1;
        }
    }

    if (header->composed_sequence < SXGUI_EXPECTED_FRAMES) {
        return fail("timeout: el cliente no llego a los frames esperados");
    }

    if (waitpid((int)pid, &status) != pid) {
        return fail("waitpid fallo");
    }
    if (status != 0) {
        printf("sxguihost: cliente salio con %d\n", status);
        puts("SXGUI HOST FAIL\n");
        return 1;
    }

    printf("sxguihost: frames compuestos=%d, fondo FACE + bisel OK\n", (int)header->composed_sequence);
    puts("SXGUI HOST PASS\n");
    return 0;
}
