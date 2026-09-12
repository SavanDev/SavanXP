/*
 * poll(), incluyendo el camino que BLOQUEA.
 *
 * Los tres primeros casos no paran nunca el proceso: preguntan por algo que ya
 * esta resuelto. Eran todo lo que habia, y por eso no dijeron nada el dia que
 * se descubrio que poll() giraba en el contexto del que llamaba en vez de
 * estacionarlo (docs/SYSTEM_MONITORING.md). Los cuatro que siguen son los que
 * obligan al proceso a dormirse y a que alguien lo despierte: por vencimiento,
 * por un pipe que otro proceso escribe, sin vencimiento, y por un objeto del
 * kernel que se senaliza solo.
 */
#include "libc.h"

/* Cuanto tarda el escritor en escribir, y cuanto se le da de plazo al poll que
 * lo espera. La diferencia es grande a proposito: lo que se afirma es que el
 * poll vuelve por el DATO y no por el vencimiento. */
#define POLLTEST_WRITE_DELAY_MS 150ul
#define POLLTEST_LONG_TIMEOUT_MS 5000l
#define POLLTEST_EXPIRY_MS 200l

static int fail(const char *message)
{
    puts_out("polltest: ");
    puts_out(message);
    puts_out("\n");
    return 1;
}

/* Hijo que espera y escribe un byte, para despertar al padre que esta parado en
 * poll. Devuelve el pid, o -1. */
static long spawn_writer(int write_fd)
{
    long child = savanxp_fork();

    if (child < 0)
    {
        return -1;
    }
    if (child == 0)
    {
        char value = 'x';

        sleep_ms(POLLTEST_WRITE_DELAY_MS);
        (void)savanxp_write(write_fd, &value, 1);
        exit(0);
    }
    return child;
}

/* Un poll que se despierta por el pipe que escribe el hijo. `timeout_ms` entra
 * como parametro porque el caso interesante es el par: con plazo largo y sin
 * plazo, que en el kernel son dos caminos distintos (wake_tick puesto o en
 * cero). */
static int check_wakeup_by_pipe(long timeout_ms, const char *label)
{
    int fds[2] = {-1, -1};
    struct savanxp_pollfd ready = {0};
    char value = '\0';
    unsigned long start_ms;
    unsigned long elapsed_ms;
    long result;
    long child;
    int status = -1;

    if (savanxp_pipe(fds) < 0)
    {
        return fail("pipe failed");
    }

    child = spawn_writer(fds[1]);
    if (child < 0)
    {
        return fail("fork failed");
    }

    ready.fd = fds[0];
    ready.events = SAVANXP_POLLIN;
    ready.revents = 0;

    start_ms = uptime_ms();
    result = savanxp_poll(&ready, 1, timeout_ms);
    elapsed_ms = uptime_ms() - start_ms;

    if (result != 1 || (ready.revents & SAVANXP_POLLIN) == 0)
    {
        printf("polltest: %s: poll devolvio %ld revents=%d\n", label, result, (int)ready.revents);
        return 1;
    }
    /* Que haya vuelto por el dato y no por el plazo: el hijo tarda 150 ms y el
     * plazo, cuando lo hay, son 5 s. */
    if (elapsed_ms >= 3000ul)
    {
        printf("polltest: %s: tardo %lu ms, parece el vencimiento y no el dato\n", label, elapsed_ms);
        return 1;
    }
    if (savanxp_read(fds[0], &value, 1) != 1 || value != 'x')
    {
        return fail("readback failed");
    }

    (void)savanxp_waitpid((int)child, &status);
    savanxp_close(fds[0]);
    savanxp_close(fds[1]);
    printf("polltest: %s ok en %lu ms\n", label, elapsed_ms);
    return 0;
}

int main(void)
{
    int fds[2] = {-1, -1};
    char value = '\0';
    struct savanxp_pollfd ready = {
        .fd = -1,
        .events = SAVANXP_POLLIN,
        .revents = 0,
    };

    if (savanxp_pipe(fds) < 0)
    {
        return fail("pipe failed");
    }

    ready.fd = fds[0];
    if (savanxp_poll(&ready, 1, 0) != 0 || ready.revents != 0)
    {
        return fail("unexpected readiness before write");
    }

    if (savanxp_fcntl(fds[0], SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
    {
        return fail("fcntl set nonblock failed");
    }

    if (savanxp_read(fds[0], &value, 1) != -SAVANXP_EAGAIN)
    {
        return fail("expected EAGAIN on empty nonblocking read");
    }

    value = 'x';
    if (savanxp_write(fds[1], &value, 1) != 1)
    {
        return fail("write failed");
    }

    ready.revents = 0;
    if (savanxp_poll(&ready, 1, 1000) != 1 || (ready.revents & SAVANXP_POLLIN) == 0)
    {
        return fail("poll did not observe readable pipe");
    }

    value = '\0';
    if (savanxp_read(fds[0], &value, 1) != 1 || value != 'x')
    {
        return fail("readback failed");
    }

    /* Vencimiento: el pipe quedo vacio otra vez, asi que este poll no tiene mas
     * salida que el plazo. El piso es holgado porque el plazo se redondea a
     * ticks; el techo es lo que atrapa un poll que se cuelga. */
    {
        unsigned long start_ms = uptime_ms();
        unsigned long elapsed_ms;

        ready.revents = 0;
        if (savanxp_poll(&ready, 1, POLLTEST_EXPIRY_MS) != 0 || ready.revents != 0)
        {
            return fail("poll con pipe vacio devolvio algo listo");
        }
        elapsed_ms = uptime_ms() - start_ms;
        if (elapsed_ms + 50ul < (unsigned long)POLLTEST_EXPIRY_MS)
        {
            printf("polltest: el vencimiento de %ld ms volvio a los %lu ms\n",
                   POLLTEST_EXPIRY_MS, elapsed_ms);
            return 1;
        }
        if (elapsed_ms > 3000ul)
        {
            printf("polltest: el vencimiento de %ld ms tardo %lu ms\n", POLLTEST_EXPIRY_MS, elapsed_ms);
            return 1;
        }
        printf("polltest: vencimiento ok en %lu ms\n", elapsed_ms);
    }

    savanxp_close(fds[0]);
    savanxp_close(fds[1]);

    if (check_wakeup_by_pipe(POLLTEST_LONG_TIMEOUT_MS, "despertar con plazo") != 0)
    {
        return 1;
    }
    /* Sin plazo es el camino de shell_core, y el unico donde wake_tick queda en
     * cero: si el despertar por dato fallara ahi, el proceso no vuelve nunca. */
    if (check_wakeup_by_pipe(-1, "despertar sin plazo") != 0)
    {
        return 1;
    }

    /* Un objeto del kernel no es un archivo: en poll cae por la otra rama, la
     * que pregunta si el objeto esta senalizado. Un timer se senaliza solo, asi
     * que prueba esa rama mientras el proceso esta parado, sin otro proceso de
     * por medio. */
    {
        long timer = timer_create(SAVANXP_TIMER_MANUAL_RESET);
        struct savanxp_pollfd timer_ready = {0};
        unsigned long start_ms;
        unsigned long elapsed_ms;
        long result;

        if (timer < 0)
        {
            return fail("timer_create failed");
        }
        if (timer_set((int)timer, POLLTEST_WRITE_DELAY_MS, 0) < 0)
        {
            return fail("timer_set failed");
        }

        timer_ready.fd = (int32_t)timer;
        timer_ready.events = SAVANXP_POLLIN;
        timer_ready.revents = 0;

        start_ms = uptime_ms();
        result = savanxp_poll(&timer_ready, 1, POLLTEST_LONG_TIMEOUT_MS);
        elapsed_ms = uptime_ms() - start_ms;

        if (result != 1 || (timer_ready.revents & SAVANXP_POLLIN) == 0)
        {
            printf("polltest: el timer devolvio %ld revents=%d\n", result, (int)timer_ready.revents);
            return 1;
        }
        if (elapsed_ms >= 3000ul)
        {
            printf("polltest: el timer tardo %lu ms, parece el vencimiento\n", elapsed_ms);
            return 1;
        }
        savanxp_close((int)timer);
        printf("polltest: timer ok en %lu ms\n", elapsed_ms);
    }

    puts_out("polltest: ok\n");
    return 0;
}
