#include "libc.h"

/*
 * Paso de handles por pipe (SAVANXP_SYS_PIPE_SEND_HANDLE / _RECEIVE_HANDLE).
 * Es el transporte con el que el WM le entrega a un proceso ya corriendo la
 * superficie de una ventana con dueno (docs/OWNED_WINDOWS.md), asi que lo que
 * se prueba es lo que ese camino necesita: que la memoria sea la misma de los
 * dos lados, que cruce un fork, que la cola tenga tope, que los extremos y los
 * tipos equivocados se rechacen, y que lo que nadie recibio no se pierda para
 * siempre en la tabla global de secciones.
 */

static int expect_error(long result, long expected, const char* label) {
    if (result != -expected) {
        eprintf("handletest: %s returned %ld, expected -%ld\n", label, result, expected);
        return 0;
    }
    return 1;
}

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("handletest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int test_section_round_trip(void) {
    int fds[2] = {-1, -1};
    long section = -1;
    long received = -1;
    unsigned char* first = 0;
    unsigned char* second = 0;

    if (!expect_success(savanxp_pipe(fds), "pipe") ||
        !expect_success(section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE), "section")) {
        return 0;
    }
    first = (unsigned char*)map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)first)) {
        eprintf("handletest: map first view failed\n");
        return 0;
    }
    first[0] = 0x5a;

    if (!expect_error(pipe_receive_handle(fds[0]), SAVANXP_EAGAIN, "receive from empty queue") ||
        !expect_success(pipe_send_handle(fds[1], (int)section), "send section")) {
        return 0;
    }
    /* El emisor puede cerrar su fd: la cola retiene el objeto mientras viaja. */
    (void)savanxp_close((int)section);

    received = pipe_receive_handle(fds[0]);
    if (!expect_success(received, "receive section")) {
        return 0;
    }
    second = (unsigned char*)map_view((int)received, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)second)) {
        eprintf("handletest: map received view failed\n");
        return 0;
    }
    if (second[0] != 0x5a) {
        eprintf("handletest: received section does not share memory (%u)\n", (unsigned)second[0]);
        return 0;
    }
    second[1] = 0xa5;
    if (first[1] != 0xa5) {
        eprintf("handletest: write through received view not visible\n");
        return 0;
    }

    (void)unmap_view(second);
    (void)unmap_view(first);
    (void)savanxp_close((int)received);
    (void)savanxp_close(fds[0]);
    (void)savanxp_close(fds[1]);
    return 1;
}

static int test_rejections(void) {
    int fds[2] = {-1, -1};
    long event = -1;
    int index;

    if (!expect_success(savanxp_pipe(fds), "pipe") ||
        !expect_success(event = event_create(SAVANXP_EVENT_MANUAL_RESET), "event")) {
        return 0;
    }
    /* Extremos al reves y objetos de E/S no viajan. */
    if (!expect_error(pipe_send_handle(fds[0], (int)event), SAVANXP_EBADF, "send on read end") ||
        !expect_error(pipe_receive_handle(fds[1]), SAVANXP_EBADF, "receive on write end") ||
        !expect_error(pipe_send_handle(fds[1], fds[0]), SAVANXP_EINVAL, "send a pipe end") ||
        !expect_error(pipe_send_handle(fds[1], 60), SAVANXP_EBADF, "send a closed fd")) {
        return 0;
    }
    /* La cola tiene tope y avisa en vez de crecer. */
    for (index = 0; index < 4; ++index) {
        if (!expect_success(pipe_send_handle(fds[1], (int)event), "fill queue")) {
            return 0;
        }
    }
    if (!expect_error(pipe_send_handle(fds[1], (int)event), SAVANXP_EAGAIN, "send on full queue")) {
        return 0;
    }
    for (index = 0; index < 4; ++index) {
        long received = pipe_receive_handle(fds[0]);
        if (!expect_success(received, "drain queue")) {
            return 0;
        }
        (void)savanxp_close((int)received);
    }
    /* Sin lectores nadie lo va a recibir. */
    (void)savanxp_close(fds[0]);
    if (!expect_error(pipe_send_handle(fds[1], (int)event), SAVANXP_EPIPE, "send without readers")) {
        return 0;
    }
    (void)savanxp_close(fds[1]);
    (void)savanxp_close((int)event);
    return 1;
}

/* Un handle que se manda despues del fork llega al otro proceso. */
static int test_across_fork(void) {
    int fds[2] = {-1, -1};
    long event = -1;
    long pid = -1;
    int status = -1;

    if (!expect_success(savanxp_pipe(fds), "pipe") ||
        !expect_success(event = event_create(SAVANXP_EVENT_MANUAL_RESET), "event")) {
        return 0;
    }
    pid = savanxp_fork();
    if (pid < 0) {
        eprintf("handletest: fork failed\n");
        return 0;
    }
    if (pid == 0) {
        char byte = 0;
        long received = -1;

        /* exit y no return: volver a main haria pasar al hijo por el resto de
         * las pruebas. */
        (void)savanxp_close(fds[1]);
        (void)savanxp_close((int)event);
        /* El byte anuncia el handle, como el registro del WM. */
        if (savanxp_read(fds[0], &byte, 1) != 1) {
            exit(2);
        }
        received = pipe_receive_handle(fds[0]);
        if (received < 0) {
            exit(3);
        }
        exit(event_set((int)received) < 0 ? 4 : 0);
    }

    (void)savanxp_close(fds[0]);
    if (!expect_success(pipe_send_handle(fds[1], (int)event), "send to child") ||
        savanxp_write(fds[1], "h", 1) != 1 ||
        !expect_success(wait_one((int)event, 2000), "wait event set by child")) {
        return 0;
    }
    if (savanxp_waitpid((int)pid, &status) < 0 || status != 0) {
        eprintf("handletest: child status %d\n", status);
        return 0;
    }
    (void)savanxp_close(fds[1]);
    (void)savanxp_close((int)event);
    return 1;
}

/* Lo que queda en la cola al morir el pipe se suelta. Hay 64 secciones en todo
 * el sistema: si se perdieran, esta vuelta se quedaria sin ninguna. */
static int test_queue_released_with_pipe(void) {
    int round;

    for (round = 0; round < 96; ++round) {
        int fds[2] = {-1, -1};
        long section = -1;

        if (savanxp_pipe(fds) < 0) {
            eprintf("handletest: pipe failed at round %d\n", round);
            return 0;
        }
        section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (section < 0) {
            eprintf("handletest: section_create failed at round %d: queued handles leak\n", round);
            return 0;
        }
        if (!expect_success(pipe_send_handle(fds[1], (int)section), "queue section")) {
            return 0;
        }
        (void)savanxp_close((int)section);
        /* Primero el lector: la cola se suelta sin esperar al escritor. */
        (void)savanxp_close(fds[0]);
        (void)savanxp_close(fds[1]);
    }
    return 1;
}

int main(void) {
    if (!test_section_round_trip() ||
        !test_rejections() ||
        !test_across_fork() ||
        !test_queue_released_with_pipe()) {
        return 1;
    }
    puts_fd(1, "handletest: ok\n");
    return 0;
}
