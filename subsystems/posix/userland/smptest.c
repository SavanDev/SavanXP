/*
 * smptest: que los procesos corran de verdad en varios cores a la vez.
 *
 * Que el sistema arranque y pase el resto de los tests con -Smp 4 no prueba
 * nada: pasaria igual con los APs estacionados. Lo que si lo prueba es el
 * estado de los procesos. En un core, mientras este proceso esta adentro de una
 * syscall, el unico en State::running es el: todos los demas estan listos,
 * bloqueados o dormidos. Ver a OTRO proceso en running desde aca es verlo
 * corriendo en otro core en ese mismo instante.
 *
 * No se mide con ticks ni con trabajo hecho: bajo TCG los vCPUs pueden turnarse
 * un solo hilo del host, y ahi ni los ticks de cada core ni el trabajo total
 * dicen cuantos corrieron a la vez. El estado si.
 *
 * Y dos caminos que solo existen con varios cores:
 *  - matar a un proceso que esta corriendo en OTRO core, que el kernel no puede
 *    desarmar en el acto y le pide a ese core que lo termine;
 *  - el ida y vuelta por un pipe, donde el que despierta al otro le manda el
 *    IPI de replanificar a un core ocioso en vez de cederle el suyo.
 *
 * Con un solo core no hay paralelismo que medir: esa parte se saltea y el
 * resto corre igual.
 */
#include "libc.h"

#define SPIN_MS 800ul
#define PING_ROUNDS 2000

/* Un hijo que no es este proceso ni un ocioso, visto en running. */
static int child_seen_running(const long* children, unsigned int count) {
    struct savanxp_process_info info;
    unsigned long index;
    unsigned int child;

    for (index = 0; proc_info(index, &info) >= 0; ++index) {
        if (info.state != SAVANXP_PROC_RUNNING) {
            continue;
        }
        for (child = 0; child < count; ++child) {
            if (info.pid == (uint32_t)children[child]) {
                return 1;
            }
        }
    }
    return 0;
}

static int check_parallel_spin(unsigned int cores) {
    long children[32];
    unsigned int spinners = cores - 1u;
    unsigned int index;
    int seen = 0;
    const unsigned long deadline = uptime_ms() + SPIN_MS;

    for (index = 0; index < spinners; ++index) {
        children[index] = savanxp_fork();
        if (children[index] < 0) {
            printf("smptest: fork failed (%s)\n", result_error_string(children[index]));
            return 0;
        }
        if (children[index] == 0) {
            while (uptime_ms() < deadline) {
                /* Girar sin dormir: la unica forma de quedarse con un core. */
            }
            exit(0);
        }
    }

    /* El padre tambien gira, preguntando. Mientras pregunta esta en running en
     * su core, asi que un hijo en running esta en otro. */
    while (uptime_ms() < deadline) {
        if (child_seen_running(children, spinners)) {
            seen = 1;
            break;
        }
    }

    for (index = 0; index < spinners; ++index) {
        int status = -1;
        if (savanxp_waitpid((int)children[index], &status) < 0 || status != 0) {
            printf("smptest: FAIL waitpid del spinner %ld (%d)\n", children[index], status);
            return 0;
        }
    }

    if (!seen) {
        printf("smptest: FAIL ningun spinner corrio a la vez que este proceso\n");
        return 0;
    }
    printf("smptest: %u spinners corriendo en paralelo\n", spinners);
    return 1;
}

static int check_cross_core_kill(void) {
    long victim = savanxp_fork();
    int status = -1;

    if (victim < 0) {
        puts_out("smptest: fork failed\n");
        return 0;
    }
    if (victim == 0) {
        for (;;) {
            /* Siempre en ring 3 y siempre corriendo: con cores libres, en otro
             * core que el que lo mata. */
        }
    }

    sleep_ms(50);
    if (savanxp_kill((int)victim, SAVANXP_SIGTERM) < 0) {
        puts_out("smptest: kill failed\n");
        return 0;
    }
    if (savanxp_waitpid((int)victim, &status) < 0) {
        puts_out("smptest: waitpid after kill failed\n");
        return 0;
    }
    if (status != 128 + SAVANXP_SIGTERM) {
        printf("smptest: FAIL kill esperaba %d, llego %d\n", 128 + SAVANXP_SIGTERM, status);
        return 0;
    }
    return 1;
}

static int check_pipe_ping_pong(void) {
    int to_child[2];
    int to_parent[2];
    long child;
    int round;

    if (savanxp_pipe(to_child) < 0 || savanxp_pipe(to_parent) < 0) {
        puts_out("smptest: pipe failed\n");
        return 0;
    }

    child = savanxp_fork();
    if (child < 0) {
        puts_out("smptest: fork failed\n");
        return 0;
    }
    if (child == 0) {
        uint32_t value = 0;
        savanxp_close(to_child[1]);
        savanxp_close(to_parent[0]);
        while (savanxp_read(to_child[0], &value, sizeof(value)) == (long)sizeof(value)) {
            value += 1;
            if (savanxp_write(to_parent[1], &value, sizeof(value)) != (long)sizeof(value)) {
                exit(2);
            }
        }
        exit(0);
    }

    savanxp_close(to_child[0]);
    savanxp_close(to_parent[1]);
    {
        const unsigned long start = uptime_ms();
        for (round = 0; round < PING_ROUNDS; ++round) {
            uint32_t value = (uint32_t)round * 2u;
            if (savanxp_write(to_child[1], &value, sizeof(value)) != (long)sizeof(value) ||
                savanxp_read(to_parent[0], &value, sizeof(value)) != (long)sizeof(value)) {
                printf("smptest: FAIL pipe cortado en la vuelta %d\n", round);
                return 0;
            }
            if (value != (uint32_t)round * 2u + 1u) {
                printf("smptest: FAIL vuelta %d devolvio %u\n", round, (unsigned)value);
                return 0;
            }
        }
        printf("smptest: %d idas y vueltas por pipe en %lu ms\n", PING_ROUNDS, uptime_ms() - start);
    }

    savanxp_close(to_child[1]);
    {
        int status = -1;
        if (savanxp_waitpid((int)child, &status) < 0 || status != 0) {
            printf("smptest: FAIL el eco termino con %d\n", status);
            return 0;
        }
    }
    savanxp_close(to_parent[0]);
    return 1;
}

int main(void) {
    struct savanxp_system_info info;
    unsigned int cores;

    if (system_info(&info) < 0) {
        puts_out("smptest: system_info failed\n");
        return 1;
    }
    cores = info.cpu_online != 0u ? info.cpu_online : 1u;
    if (cores > 32u) {
        cores = 32u;
    }
    printf("smptest: %u core(s) planificando\n", cores);

    if (cores > 1u && !check_parallel_spin(cores)) {
        return 1;
    }
    if (!check_cross_core_kill() || !check_pipe_ping_pong()) {
        return 1;
    }

    puts_out("smptest: ok\n");
    return 0;
}
