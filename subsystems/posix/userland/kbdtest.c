#include "libc.h"

/* Smoke test automatizado del driver de teclado: a diferencia de keytest.c
 * (manual, requiere un humano) y de windowd_selftest (que inyecta un
 * savanxp_input_event a mano sin tocar PS/2), este binario adquiere la sesion
 * grafica igual que gputest.c en modo interactivo y lee /dev/input0 de
 * verdad, mientras el harness host (build.ps1 kbd-smoke) mueve el teclado
 * emulado por QMP. Verifica make/break, la tabla shifted, el bit de
 * modificador de Ctrl y el camino de tecla extendida (0xE0) de una sola vez. */

#define KBDTEST_TIMEOUT_MS 20000u

struct kbd_checkpoint {
    const char* label;
    int use_key;
    unsigned int key;
    int ascii;
    unsigned int required_modifiers;
    unsigned int forbidden_modifiers;
};

static const struct kbd_checkpoint g_checkpoints[] = {
    { "letra simple 'a'", 0, 0, 'a', 0, SAVANXP_KEY_MOD_SHIFT | SAVANXP_KEY_MOD_CTRL },
    { "shift+a", 0, 0, 'A', SAVANXP_KEY_MOD_SHIFT, SAVANXP_KEY_MOD_CTRL },
    { "ctrl+c", 0, 0, 'c', SAVANXP_KEY_MOD_CTRL, 0 },
    { "flecha derecha", 1, SAVANXP_KEY_RIGHT, 0, 0, 0 },
    { "enter", 1, SAVANXP_KEY_ENTER, 0, 0, 0 },
};

static int checkpoint_matches(const struct kbd_checkpoint* cp, const struct savanxp_input_event* event) {
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
        return 0;
    }
    if (cp->use_key) {
        if (event->key != cp->key) {
            return 0;
        }
    } else {
        if (event->ascii != cp->ascii) {
            return 0;
        }
    }
    if ((event->modifiers & cp->required_modifiers) != cp->required_modifiers) {
        return 0;
    }
    if ((event->modifiers & cp->forbidden_modifiers) != 0) {
        return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    struct savanxp_input_event event = {0};
    long gpu_fd;
    long input_fd;
    size_t checkpoint_index = 0;
    unsigned long deadline_ms;
    const size_t checkpoint_count = sizeof(g_checkpoints) / sizeof(g_checkpoints[0]);

    if (argc <= 1 || strcmp(argv[1], "--selftest") != 0) {
        puts_fd(2, "kbdtest: uso: kbdtest --selftest\n");
        return 1;
    }

    gpu_fd = gpu_open();
    if (gpu_fd < 0) {
        puts_fd(2, "KBD SMOKE FAIL /dev/gpu0 no disponible\n");
        return 1;
    }
    if (gpu_acquire((int)gpu_fd) < 0) {
        puts_fd(2, "KBD SMOKE FAIL GPU_IOC_ACQUIRE fallo\n");
        close((int)gpu_fd);
        return 1;
    }

    input_fd = open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (input_fd < 0) {
        puts_fd(2, "KBD SMOKE FAIL /dev/input0 no disponible\n");
        gpu_release((int)gpu_fd);
        close((int)gpu_fd);
        return 1;
    }

    /* El harness host espera esta linea en el log serial antes de empezar a
     * inyectar teclas por QMP: sin ella podria mandarlas antes de que este
     * proceso sea el dueno de la sesion grafica y se perderian. */
    puts("KBD SMOKE READY\n");

    deadline_ms = uptime_ms() + KBDTEST_TIMEOUT_MS;
    while (checkpoint_index < checkpoint_count) {
        while (read((int)input_fd, &event, sizeof(event)) == (long)sizeof(event)) {
            if (checkpoint_matches(&g_checkpoints[checkpoint_index], &event)) {
                printf("kbdtest: checkpoint '%s' OK\n", g_checkpoints[checkpoint_index].label);
                checkpoint_index += 1;
                if (checkpoint_index >= checkpoint_count) {
                    break;
                }
            }
        }

        if (checkpoint_index >= checkpoint_count) {
            break;
        }
        if (uptime_ms() >= deadline_ms) {
            printf("KBD SMOKE FAIL timeout esperando '%s'\n", g_checkpoints[checkpoint_index].label);
            close((int)input_fd);
            gpu_release((int)gpu_fd);
            close((int)gpu_fd);
            return 1;
        }
        sleep_ms(20);
    }

    close((int)input_fd);
    gpu_release((int)gpu_fd);
    close((int)gpu_fd);
    puts("KBD SMOKE PASS\n");
    return 0;
}
