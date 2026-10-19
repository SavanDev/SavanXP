#include "libc.h"

#include "savanxp/syscall.h"

/* keyecho: el segundo programa del objetivo i386. El kernel lo lanza cuando
 * sysinfo sale (arch/x86/syscall.cpp, kSysExit) y su unico trabajo es leer
 * eventos savanxp_input_event de stdin (syscall READ sobre la cola del
 * teclado) y escribirlos por stdout. Es lo que hace verificable la entrega
 * "evento decodificado -> syscall -> userland" en
 * tools/legacy_boot_test.py --expect-key. Se queda en el bucle para que el
 * teclado siga vivo hasta que el test cierre la maquina. */

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
    struct savanxp_input_event event;

    puts_out("keyecho: leyendo stdin\n");
    for (;;) {
        const long received = savanxp_read(0, &event, sizeof(event));
        if (received != (long)sizeof(event)) {
            /* Sin eventos la lectura se bloquea dentro del kernel; un error
             * real (-errno) se reporta y el bucle sigue: keyecho no sale. */
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
