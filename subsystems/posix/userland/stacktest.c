/* Test del stack de usuario: crecimiento por demanda, pagina de guarda y argv.
 *
 * Las tres cosas se verifican desde el mismo binario, que se relanza a si mismo
 * con distintos modos porque dos de ellas necesitan un proceso que muera:
 *
 *   --deep      recursion profunda que pasa MUY por arriba de lo que se mapea
 *               al arrancar; si el stack no creciera, esto faultearia.
 *   --overflow  recursion sin fondo: tiene que morir con #PF al tocar la
 *               pagina de guarda, no seguir pisando memoria de abajo.
 *   --argv      informa cuantos argumentos recibio, para el caso de mas de 15.
 *   --longarg   verifica un argumento mas largo que los 63 caracteres que
 *               entraban en el buffer viejo del syscall.
 *
 * Sin argumentos corre los tres y reporta.
 */

#include "libc.h"

#include <stdio.h>
#include <string.h>

/* Cada nivel se lleva ~1 KiB. Con 400 niveles el programa toca ~400 KiB, muy
 * por encima de las 8 paginas (32 KiB) que el kernel mapea al arrancar y por
 * encima tambien de los 128 KiB que era el stack entero antes. */
#define STACK_FRAME_BYTES 1024
#define STACK_DEEP_LEVELS 400
#define STACK_EXPECTED_STATUS 142 /* 128 + 14 (#PF) */

/* 125 caracteres: el doble de lo que entraba por argumento antes, y del
 * orden de una ruta real. */
#define STACK_LONG_ARGUMENT "/disk/una/ruta/bastante/larga/para/el/buffer/viejo/de/64/bytes/que/ahora/tiene/que/llegar/entera/sin/recortarse/nunca/mas.txt"

static int g_failures = 0;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("stacktest: FALLO %s\n", what);
        g_failures += 1;
    }
}

/* noinline y volatile para que el compilador no pliegue la recursion ni se
 * saltee el buffer: el punto es consumir stack de verdad. */
__attribute__((noinline)) static unsigned long descend(int level) {
    volatile unsigned char frame[STACK_FRAME_BYTES];
    unsigned long total = 0;
    int index = 0;

    for (index = 0; index < STACK_FRAME_BYTES; index += 64) {
        frame[index] = (unsigned char)(level + index);
    }
    if (level > 0) {
        total = descend(level - 1);
    }
    for (index = 0; index < STACK_FRAME_BYTES; index += 64) {
        total += frame[index];
    }
    return total;
}

/* Suma de referencia, calculada sin tocar el stack. */
static unsigned long expected_total(void) {
    unsigned long total = 0;
    int level = 0;
    int index = 0;

    for (level = 0; level <= STACK_DEEP_LEVELS; ++level) {
        for (index = 0; index < STACK_FRAME_BYTES; index += 64) {
            total += (unsigned char)(level + index);
        }
    }
    return total;
}

__attribute__((noinline)) static unsigned long fall_forever(unsigned long depth) {
    volatile unsigned char frame[STACK_FRAME_BYTES];
    frame[0] = (unsigned char)depth;
    frame[STACK_FRAME_BYTES - 1] = (unsigned char)depth;
    return frame[0] + fall_forever(depth + 1);
}

static int run_self(const char* const* argv, int argc, int expected_status) {
    int status = -1;
    long pid = spawn("/bin/stacktest", argv, argc);

    if (pid < 0) {
        printf("stacktest: FALLO spawn (%s)\n", result_error_string(pid));
        return 0;
    }
    if (savanxp_waitpid((int)pid, &status) < 0) {
        printf("stacktest: FALLO waitpid\n");
        return 0;
    }
    if (status != expected_status) {
        printf("stacktest: FALLO status esperado=%d obtenido=%d\n", expected_status, status);
        return 0;
    }
    return 1;
}

int main(int argc, const char* const* argv) {
    if (argc >= 2 && strcmp(argv[1], "--deep") == 0) {
        return descend(STACK_DEEP_LEVELS) == expected_total() ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "--overflow") == 0) {
        printf("stacktest: el hijo va a desbordar el stack a proposito\n");
        return (int)(fall_forever(0) & 1u);
    }
    if (argc >= 2 && strcmp(argv[1], "--argv") == 0) {
        /* Se espera "stacktest --argv a0 a1 ... a37": 40 en total. */
        int index = 0;
        if (argc != 40) {
            printf("stacktest: argc=%d, esperaba 40\n", argc);
            return 1;
        }
        for (index = 2; index < argc; ++index) {
            char expected[16];
            snprintf(expected, sizeof(expected), "a%d", index - 2);
            if (strcmp(argv[index], expected) != 0) {
                printf("stacktest: argv[%d]='%s', esperaba '%s'\n", index, argv[index], expected);
                return 1;
            }
        }
        return 0;
    }

    printf("stacktest: arrancando\n");

    if (argc >= 3 && strcmp(argv[1], "--longarg") == 0) {
        return strcmp(argv[2], STACK_LONG_ARGUMENT) == 0 ? 0 : 1;
    }

    /* 1. El stack crece cuando el programa baja. */
    check(descend(STACK_DEEP_LEVELS) == expected_total(), "recursion profunda en el mismo proceso");

    /* 2. Y tambien en un proceso recien creado. */
    {
        const char* const deep_argv[] = {"/bin/stacktest", "--deep", 0};
        check(run_self(deep_argv, 2, 0), "recursion profunda en un hijo");
    }

    /* 3. La guarda corta el desborde: el hijo tiene que morir con #PF. */
    {
        const char* const overflow_argv[] = {"/bin/stacktest", "--overflow", 0};
        check(run_self(overflow_argv, 2, STACK_EXPECTED_STATUS),
              "el desborde mata al proceso con #PF");
    }

    /* 4. argv mas largo que el viejo tope de 15. */
    {
        char storage[38][16];
        const char* wide_argv[41];
        int index = 0;

        wide_argv[0] = "/bin/stacktest";
        wide_argv[1] = "--argv";
        for (index = 0; index < 38; ++index) {
            snprintf(storage[index], sizeof(storage[index]), "a%d", index);
            wide_argv[index + 2] = storage[index];
        }
        wide_argv[40] = 0;
        check(run_self(wide_argv, 40, 0), "argv de 40 argumentos");
    }

    /* 5. Un argumento mas largo que el viejo tope de 63 caracteres. */
    {
        const char* const long_argv[] = {"/bin/stacktest", "--longarg", STACK_LONG_ARGUMENT, 0};
        check(run_self(long_argv, 3, 0), "argumento de mas de 63 caracteres");
    }

    if (g_failures != 0) {
        printf("STACKTEST FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("STACKTEST PASS\n");
    return 0;
}
