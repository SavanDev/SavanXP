#include "libc.h"

#include "savanxp/syscall.h"

/* sh32: la shell minima del objetivo i386. Es el segundo programa de la
 * cadena (el kernel la lanza cuando sysinfo sale) y el primero usable:
 * prompt, edicion de linea con eco por stdout, y un puñado de comandos sobre
 * las syscalls que este kernel entiende. No tiene pipes ni redirecciones; para
 * eso esta la agenda, no este slice.
 *
 * El camino de entrada traduce savanxp_input_event a caracteres: solo los
 * eventos KEY_DOWN con ascii imprimible o control basico; las solturas y los
 * modificadores puros se ignoran. El eco es del shell, no del kernel: lo que
 * llega tarde se refleja cuando se lee, no en cada IRQ. */

#define SH32_LINE_CAPACITY 128
#define SH32_ARGV_CAPACITY 8

static char g_line[SH32_LINE_CAPACITY];
static int g_length = 0;

static void print_u64(uint64_t value) {
    char buffer[24];
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

static void show_prompt(void) {
    puts_out("sh32> ");
}

/* Un evento KEY_DOWN con ascii es un caracter de linea: enter la cierra,
 * backspace borra con eco, lo imprimible se suma con eco, el resto se ignora
 * (teclas de funcion, flechas, modificadores puros, solturas). */
static int feed_event(const struct savanxp_input_event* event) {
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
        return 0;
    }
    if (event->key == SAVANXP_KEY_BACKSPACE) {
        if (g_length > 0) {
            --g_length;
            putchar_fd(1, '\b');
        }
        return 0;
    }
    if (event->ascii == '\n' || event->key == SAVANXP_KEY_ENTER) {
        putchar_fd(1, '\n');
        g_line[g_length] = '\0';
        return 1;
    }
    if (event->ascii >= 32 && event->ascii < 127 && g_length + 1 < (int)sizeof(g_line)) {
        g_line[g_length++] = (char)event->ascii;
        putchar_fd(1, (char)event->ascii);
    }
    return 0;
}

static int read_line(void) {
    struct savanxp_input_event event;

    g_length = 0;
    for (;;) {
        const long received = savanxp_read(0, &event, sizeof(event));
        if (received != (long)sizeof(event)) {
            /* La syscall READ bloquea en el kernel; un error real (-errno)
             * se reporta y se sigue leyendo. El shell no sale por un error. */
            if (received < 0) {
                puts_out("sh32: read fallo\n");
            }
            continue;
        }
        if (feed_event(&event)) {
            return 1;
        }
    }
}

static int split_line(char* argv[], int capacity) {
    int argc = 0;
    int index = 0;

    while (g_line[index] != '\0' && argc < capacity) {
        while (g_line[index] == ' ') {
            ++index;
        }
        if (g_line[index] == '\0') {
            break;
        }
        argv[argc++] = &g_line[index];
        while (g_line[index] != '\0' && g_line[index] != ' ') {
            ++index;
        }
        if (g_line[index] == ' ') {
            g_line[index++] = '\0';
        }
    }
    return argc;
}

static unsigned long parse_ulong(const char* text, int* ok) {
    unsigned long value = 0;

    *ok = 0;
    if (text == 0 || *text == '\0') {
        return 0;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return 0;
        }
        value = value * 10 + (unsigned long)(*text - '0');
        ++text;
    }
    *ok = 1;
    return value;
}

static void command_sysinfo(void) {
    struct savanxp_system_info info;
    const long status = system_info(&info);

    /* El test teclea 'sysinfo' y exige esta linea: es la prueba de que un
     * programa del shell puede llamar al sistema y volver. */
    if (status < 0) {
        puts_out("sh32: system_info fallo\n");
        return;
    }
    puts_out("sh32: system_info ok, archivos ");
    print_u64(info.sxfs_file_count);
    puts_out(", uptime ");
    print_u64(info.uptime_ms);
    puts_out(" ms\n");
}

static void command_uptime(void) {
    puts_out("uptime ");
    print_u64((uint64_t)uptime_ms());
    puts_out(" ms\n");
}

static void command_sleep(int argc, char* argv[]) {
    int ok = 0;
    const unsigned long milliseconds = parse_ulong(argc > 1 ? argv[1] : 0, &ok);

    if (!ok) {
        puts_out("uso: sleep <ms>\n");
        return;
    }
    sleep_ms(milliseconds);
    puts_out("slept ");
    print_u64((uint64_t)milliseconds);
    puts_out(" ms\n");
}

static void command_echo(int argc, char* argv[]) {
    int index = 1;

    while (index < argc) {
        puts_out(argv[index]);
        if (++index < argc) {
            putchar_fd(1, ' ');
        }
    }
    putchar_fd(1, '\n');
}

static void command_launch(const char* path, const char* const* argv, int argc, int background) {
    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        puts_out("sh32: spawn failed\n");
        return;
    }
    if (background) {
        puts_out("started pid ");
        print_u64((uint64_t)pid);
        puts_out("\n");
    } else {
        int status = 0;
        if (savanxp_waitpid((int)pid, &status) != pid) {
            puts_out("sh32: wait failed\n");
        } else {
            puts_out("child status ");
            print_u64((unsigned int)status);
            puts_out("\n");
        }
    }
}

static int run_command(int argc, char* argv[]) {
    if (argc == 0) {
        return 1;
    }
    if (strcmp(argv[0], "exit") == 0) {
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        puts_out("commands: help sysinfo uptime sleep echo clear run start wait desktop multitest exit\n");
        return 1;
    }
    if (strcmp(argv[0], "run") == 0 || strcmp(argv[0], "start") == 0) {
        if (argc < 2) {
            puts_out("usage: run|start PATH [ARGS]\n");
        } else {
            command_launch(argv[1], (const char* const*)&argv[1], argc - 1,
                           strcmp(argv[0], "start") == 0);
        }
        return 1;
    }
    if (strcmp(argv[0], "wait") == 0) {
        int ok = 0;
        unsigned long pid = parse_ulong(argc > 1 ? argv[1] : 0, &ok);
        int status = 0;
        if (!ok || pid == 0 || pid >= 0x80000000u ||
            savanxp_waitpid((int)pid, &status) < 0) {
            puts_out("sh32: wait failed\n");
        } else {
            puts_out("child status ");
            print_u64((unsigned int)status);
            puts_out("\n");
        }
        return 1;
    }
    if (strcmp(argv[0], "desktop") == 0 || strcmp(argv[0], "multitest") == 0) {
        const char* path = strcmp(argv[0], "desktop") == 0 ?
                           "/disk/bin/desktop32" : "/disk/bin/multitest32";
        const char* arguments[] = {path};
        command_launch(path, arguments, 1, 0);
        return 1;
    }
    if (strcmp(argv[0], "sysinfo") == 0) {
        command_sysinfo();
        return 1;
    }
    if (strcmp(argv[0], "uptime") == 0) {
        command_uptime();
        return 1;
    }
    if (strcmp(argv[0], "sleep") == 0) {
        command_sleep(argc, argv);
        return 1;
    }
    if (strcmp(argv[0], "echo") == 0) {
        command_echo(argc, argv);
        return 1;
    }
    if (strcmp(argv[0], "clear") == 0) {
        /* Clear by scrolling the actual console height: graphics has more
         * rows than the 25-row EGA fallback. Glyphs are 8x16 in this backend. */
        struct savanxp_system_info info;
        unsigned int rows = 25;
        memset(&info, 0, sizeof(info));
        if (system_info(&info) == 0 && info.framebuffer_bpp == 32 &&
            info.framebuffer_height >= 16 && info.framebuffer_height <= 768) {
            rows = info.framebuffer_height / 16;
        }
        for (unsigned int line = 0; line < rows; ++line) {
            putchar_fd(1, '\n');
        }
        return 1;
    }
    puts_out("sh32: comando desconocido: ");
    puts_out(argv[0]);
    putchar_fd(1, '\n');
    return 1;
}

int main(void) {
    char* argv[SH32_ARGV_CAPACITY];

    /* El token del arranque: el test lo exige para saber que el shell llego a
     * ring 3 y esta esperando stdin antes de teclear. */
    puts_out("sh32: listo\n");
    for (;;) {
        show_prompt();
        read_line();
        if (!run_command(split_line(argv, SH32_ARGV_CAPACITY), argv)) {
            break;
        }
    }
    return 0;
}
