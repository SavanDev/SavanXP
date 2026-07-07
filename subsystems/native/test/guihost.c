/*
 * SavanXP - guihost: harness headless del protocolo cliente del compositor.
 *
 * Programa POSIX que interpreta el lado HOST del contrato de superficie v3
 * (el rol del escritorio/compositor, espejando start_client_process de
 * subsystems/posix/userland/desktop.c): crea la seccion compartida con header
 * + batches + pixeles, instala los canales en los fds 3..9 y ejecuta el
 * cliente nativo /disk/bin/nativegui. Despues compone (avanza
 * composed_sequence y senala retire) cada frame sometido, le manda una tecla
 * y verifica que el cliente salga limpio con los frames esperados.
 *
 * Esto valida headless, de punta a punta: el marcado nativo via exec (fork
 * posix -> exec ELF nativo), el mapeo de la seccion heredada, el handshake del
 * header, las secuencias submit/composed con eventos, y el input por pipe.
 * Imprime NATIVEGUI HOST PASS/FAIL (formato apto para el arnes de smoke).
 */
#include "savanxp/libc.h"

#define GUIHOST_WIDTH 320u
#define GUIHOST_HEIGHT 200u
#define GUIHOST_PAGE 4096u
#define GUIHOST_CLIENT_PATH "/disk/bin/nativegui"
#define GUIHOST_DEADLINE_MS 8000ul
#define GUIHOST_EXPECTED_FRAMES 4ul /* 1 present completo + 3 regiones */

static int fail(const char *reason) {
    printf("guihost: %s\n", reason);
    puts("NATIVEGUI HOST FAIL\n");
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
    int sent_key = 0;
    int status = -1;
    int guard_fds[7];
    int guard_index;

    /* Reservar los fds 3..9: guihost arranca con la tabla casi vacia, asi que
     * sin esto los recursos reales caerian dentro del rango destino de los
     * dup2 del hijo y se pisarian entre si (el escritorio real no lo sufre
     * porque ya tiene muchos fds abiertos). */
    for (guard_index = 0; guard_index < 7; ++guard_index) {
        guard_fds[guard_index] = dup(0);
        if (guard_fds[guard_index] < 0) {
            return fail("no se pudieron reservar los fds guardia");
        }
    }

    command_bytes = SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch);
    pixels_offset = (sizeof(*header) + command_bytes + (GUIHOST_PAGE - 1u)) & ~(unsigned long)(GUIHOST_PAGE - 1u);
    buffer_size = (unsigned long)GUIHOST_WIDTH * 4u * GUIHOST_HEIGHT;

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
    header->info.width = GUIHOST_WIDTH;
    header->info.height = GUIHOST_HEIGHT;
    header->info.pitch = GUIHOST_WIDTH * 4u;
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
        const char *argv[2] = {GUIHOST_CLIENT_PATH, 0};
        if (dup2((int)section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(submit_event, 6) < 0 ||
            dup2(retire_event, 7) < 0 ||
            dup2(shutdown_event, 8) < 0 ||
            dup2(launch_pipe[1], 9) < 0) {
            exit(1);
        }
        (void)exec(GUIHOST_CLIENT_PATH, argv, 1);
        /* exec del cliente nativo fallo: seguimos siendo posix, avisar. */
        printf("guihost: exec de %s fallo\n", GUIHOST_CLIENT_PATH);
        exit(1);
    }

    /* Cerrar los guardias del padre: solo existian para correr los fds reales
     * fuera del rango 3..9. En el hijo los dup2 los pisan solos. */
    for (guard_index = 0; guard_index < 7; ++guard_index) {
        (void)close(guard_fds[guard_index]);
    }

    /* Bucle de composicion: consumir submits, avanzar composed y retirar.
     * waitpid es bloqueante en SavanXP, asi que recien se llama cuando el
     * cliente ya compuso todos sus frames (esta saliendo o salio). */
    deadline = uptime_ms() + GUIHOST_DEADLINE_MS;
    while (uptime_ms() < deadline && header->composed_sequence < GUIHOST_EXPECTED_FRAMES) {
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
            if (batch->rects[0].x + batch->rects[0].width > GUIHOST_WIDTH ||
                batch->rects[0].y + batch->rects[0].height > GUIHOST_HEIGHT) {
                return fail("rect fuera de la superficie");
            }

            header->retired_sequence = sequence;
            header->composed_sequence = sequence;
            (void)event_set(retire_event);
        }

        /* Con el primer frame compuesto, verificar pixeles y mandar la tecla. */
        if (!sent_key && header->composed_sequence >= 1) {
            struct savanxp_input_event event;
            if (pixels[25 * GUIHOST_WIDTH + 30] == 0) {
                return fail("la superficie quedo vacia tras el primer frame");
            }
            event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
            event.key = SAVANXP_KEY_ENTER;
            event.ascii = 13;
            if (write(input_pipe[1], &event, sizeof(event)) != (long)sizeof(event)) {
                return fail("no se pudo mandar el evento de tecla");
            }
            sent_key = 1;
        }
    }

    if (header->composed_sequence < GUIHOST_EXPECTED_FRAMES) {
        return fail("timeout: el cliente no llego a los frames esperados");
    }

    if (waitpid((int)pid, &status) != pid) {
        return fail("waitpid fallo");
    }
    if (status != 0) {
        printf("guihost: cliente salio con %d\n", status);
        puts("NATIVEGUI HOST FAIL\n");
        return 1;
    }

    printf("guihost: frames compuestos=%d\n", (int)header->composed_sequence);
    puts("NATIVEGUI HOST PASS\n");
    return 0;
}
