#include "libc.h"

#include "savanxp/syscall.h"

/* keyecho: diagnostico manual de la entrega de eventos de teclado. El kernel lo
 * lanzo automaticamente en ed6dfd6; con sh32 en la cadena ya no hace falta y
 * queda en el volumen para correrlo a mano (no hay exec todavia, asi que el
 * shell no lo puede invocar: ahi queda). Lee eventos savanxp_input_event de
 * stdin y los escribe por stdout hasta que el operador cierre la maquina. */

static void print_u32(uint32_t value) {
    char buffer[10];
    int index = 0;

    if (value == 0) {
        putchar_fd(1, '0');
        return;
    }
    while (value != 0 && index < (int)sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (index > 0) {
        putchar_fd(1, buffer[--index]);
    }
}

static void print_line(const struct savanxp_input_event* event) {
    puts_out(event->type == SAVANXP_INPUT_EVENT_KEY_DOWN ? "keyecho: down key=0x"
                                                         : "keyecho: up   key=0x");
    print_u32(event->key);
    puts_out(" ascii=");
    print_u32((uint32_t)event->ascii);
    if (event->ascii >= 32 && event->ascii < 127) {
        puts_out(" '");
        putchar_fd(1, (char)event->ascii);
        puts_out("'");
    }
    putchar_fd(1, '\n');
}

int main(void) {
    // These diagnostics consume raw events; the reference shell uses TTY lines.
    long input = savanxp_open("/dev/input0");
    if (input < 0 || savanxp_dup2((int)input, 0) < 0) return 1;
    savanxp_close((int)input);
    struct savanxp_input_event event;

    puts_out("keyecho: leyendo stdin\n");
    for (;;) {
        struct savanxp_pollfd input_poll = {0, SAVANXP_POLLIN, 0};
        if (savanxp_poll(&input_poll, 1, -1) < 0) return 1;
        const long received = savanxp_read(0, &event, sizeof(event));
        if (received != (long)sizeof(event)) {
            /* The reference raw device can drain between poll and read. */
            if (received < 0) {
                puts_out("keyecho: read fallo ");
                print_u32((uint32_t)(-received));
                putchar_fd(1, '\n');
            }
            continue;
        }
        print_line(&event);
    }
    return 0;
}
