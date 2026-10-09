#include "libc.h"

static int is_space_char(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int text_contains(const char* text, const char* needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0) {
        return 1;
    }
    for (size_t index = 0; text[index] != '\0'; ++index) {
        if (strncmp(text + index, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int text_starts_with(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static const char* skip_spaces(const char* text) {
    while (text != 0 && is_space_char(*text)) {
        ++text;
    }
    return text;
}

static void trim_automation_spec(char* spec) {
    size_t length = strlen(spec);
    while (length != 0 && is_space_char(spec[length - 1])) {
        spec[length - 1] = '\0';
        length -= 1;
    }
}

static const char* automation_label_for_spec(const char* spec) {
    if (spec != 0 && text_contains(spec, "float")) {
        return "FLOAT SMOKE";
    }
    if (spec != 0 && text_contains(spec, "ccleste")) {
        return "CCLESTE SELFTEST";
    }
    if (spec != 0 && text_contains(spec, "progman")) {
        return "PROGMAN SMOKE";
    }
    if (spec != 0 && text_contains(spec, "appwiz")) {
        return "APPWIZ SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxe")) {
        return "SXE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "filesapp")) {
        return "FILESAPP SMOKE";
    }
    if (spec != 0 && text_contains(spec, "taskmgr")) {
        return "TASKMGR SMOKE";
    }
    if (spec != 0 && text_contains(spec, "mines")) {
        return "MINES SMOKE";
    }
    if (spec != 0 && text_contains(spec, "calc")) {
        return "CALC SMOKE";
    }
    if (spec != 0 && text_contains(spec, "clocktest")) {
        return "CLOCK SMOKE";
    }
    if (spec != 0 && text_contains(spec, "soak")) {
        return "SOAK";
    }
    if (spec != 0 && text_contains(spec, "windowd")) {
        return "WINDOWD SMOKE";
    }
    if (spec != 0 && text_contains(spec, "kbd")) {
        return "KBD SMOKE";
    }
    if (spec != 0 && text_contains(spec, "mousetest")) {
        return "POINTER SMOKE";
    }
    if (spec != 0 && text_contains(spec, "audiostream")) {
        return "AUDIO STREAM";
    }
    if (spec != 0 && text_contains(spec, "audiorecord")) {
        return "AUDIO RECORD";
    }
    if (spec != 0 && text_contains(spec, "netsmoke")) {
        return "NET SMOKE";
    }
    if (spec != 0 && text_contains(spec, "tcptest")) {
        return "TCP SMOKE";
    }
    if (spec != 0 && text_contains(spec, "mediaplayer-show")) {
        return "MEDIAPLAYER DISPLAY";
    }
    if (spec != 0 && text_contains(spec, "mediaplayer")) {
        return "MEDIAPLAYER SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxgui")) {
        return "SXGUI HOST";
    }
    if (spec != 0 && text_contains(spec, "guihost")) {
        return "NATIVEGUI HOST";
    }
    if (spec != 0 && text_contains(spec, "hello")) {
        return "NATIVE HELLO";
    }
    return "SMOKE";
}

static int run_automation_spec(const char* spec) {
    const char* smoke_argv[] = {"/disk/bin/smoke", 0};
    const char* soak_argv[] = {"/disk/bin/gputest", "--soak", 0, 0};
    const char* windowd_argv[] = {"/bin/windowd", "--selftest", 0};
    const char* cursor_repro_argv[] = {"/bin/windowd", "--cursor-repro", 0};
    const char* progman_selftest_argv[] = {"/bin/progman", "--selftest", 0};
    const char* appwiz_selftest_argv[] = {"/bin/appwiz", "--selftest", 0};
    const char* sxetest_argv[] = {"/disk/bin/sxetest", 0};
    const char* filesapp_selftest_argv[] = {"/bin/filesapp", "--selftest", 0};
    const char* taskmgr_selftest_argv[] = {"/bin/taskmgr", "--selftest", 0};
    const char* calc_selftest_argv[] = {"/bin/calc", "--selftest", 0};
    const char* mines_selftest_argv[] = {"/bin/mines", "--selftest", 0};
    const char* clocktest_argv[] = {"/disk/bin/clocktest", 0};
    const char* datetest_argv[] = {"/disk/bin/datetest", 0};
    const char* needstest_argv[] = {"/disk/bin/needstest", 0};
    /* Los cuatro diagnósticos del cargador. Cada uno afirmaba en error y nada más,
     * así que ninguno podía tener escenario: ahora cada uno imprime su token de
     * éxito y hay uno que lo ejecuta. */
    const char* brokentest_argv[] = {"/disk/bin/brokentest", 0};
    const char* missingtest_argv[] = {"/disk/bin/missingtest", 0};
    const char* slottest_argv[] = {"/disk/bin/slottest", 0};
    const char* diamondtest_argv[] = {"/disk/bin/diamondtest", 0};
    const char* ffmpegload_argv[] = {"/disk/bin/ffmpegload", 0};
    const char* audiostream_argv[] = {"/disk/bin/audiotest", "--stream", 0};
    const char* audiostream_quiet_argv[] = {"/disk/bin/audiotest", "--stream-quiet", 0};
    const char* audiod_selftest_argv[] = {"/bin/audiod", "--selftest", 0};
    const char* volume_argv[] = {"/disk/bin/volume", "75", 0};
    const char* audiorecord_argv[] = {"/disk/bin/audiotest", "--record", 0};
    const char* nettest_argv[] = {"/disk/bin/nettest", 0};
    const char* tcptest_argv[] = {"/disk/bin/tcptest", 0, 0};
    const char* ntptest_argv[] = {"/disk/bin/ntptest", 0, 0};
    const char* floatsmoke_argv[] = {"/disk/bin/floatsmoke", 0};
    const char* ccleste_argv[] = {"/disk/bin/ccleste", "--selftest", 0};
    const char* guihost_argv[] = {"/disk/bin/nativeguihost", 0};
    const char* nativehello_argv[] = {"/disk/bin/nativehello", 0};
    const char* sxguihost_argv[] = {"/disk/bin/sxguihost", 0};
    const char* kbdtest_argv[] = {"/disk/bin/kbdtest", "--selftest", 0};
    const char* mousetest_argv[] = {"/disk/bin/mousetest", "--selftest", 0};
    /* Tres caminos: audio solo (WAV), video solo sin contenedor (MJPEG crudo) y
     * los dos intercalados en AVI, que ademas mide la sincronia. */
    const char* mediaplayer_argv[] = {"/bin/mediaplayer", "--selftest", "/disk/media/tono.wav",
                                      "/disk/media/clip.mjpeg", "--sync", "/disk/media/avsync.avi", 0};
    const char* mediaplayer_show_argv[] = {"/bin/mediaplayer", "--gpu-hold", "8000", "/disk/media/avsync.avi", 0};
    const char* path = "/disk/bin/smoke";
    const char* const* argv = smoke_argv;
    const char* label = automation_label_for_spec(spec);
    int argc = 1;
    int status = 0;

    if (spec != 0 && strcmp(spec, "smoke") != 0 && spec[0] != '\0') {
        if (strcmp(spec, "windowd-selftest") == 0 || strcmp(spec, "windowd") == 0) {
            path = "/bin/windowd";
            argv = windowd_argv;
            argc = 2;
        } else if (strcmp(spec, "progman-selftest") == 0 || strcmp(spec, "progman") == 0) {
            path = "/bin/progman";
            argv = progman_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "appwiz-selftest") == 0 || strcmp(spec, "appwiz") == 0) {
            path = "/bin/appwiz";
            argv = appwiz_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "sxe-selftest") == 0 || strcmp(spec, "sxe") == 0) {
            path = "/disk/bin/sxetest";
            argv = sxetest_argv;
            argc = 1;
        } else if (strcmp(spec, "filesapp-selftest") == 0 || strcmp(spec, "filesapp") == 0) {
            path = "/bin/filesapp";
            argv = filesapp_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "taskmgr-selftest") == 0 || strcmp(spec, "taskmgr") == 0) {
            path = "/bin/taskmgr";
            argv = taskmgr_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "mines-selftest") == 0 || strcmp(spec, "mines") == 0) {
            path = "/bin/mines";
            argv = mines_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "calc-selftest") == 0 || strcmp(spec, "calc") == 0) {
            path = "/bin/calc";
            argv = calc_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "ffmpegload") == 0) {
            /* Carga libffmpeg.so.0.4 y mide. El escenario instala el port antes,
             * asi que la libreria esta en el volumen. */
            path = "/disk/bin/ffmpegload";
            argv = ffmpegload_argv;
            argc = 1;
        } else if (strcmp(spec, "brokentest") == 0) {
            path = "/disk/bin/brokentest";
            argv = brokentest_argv;
            argc = 1;
        } else if (strcmp(spec, "missingtest") == 0) {
            path = "/disk/bin/missingtest";
            argv = missingtest_argv;
            argc = 1;
        } else if (strcmp(spec, "slottest") == 0) {
            path = "/disk/bin/slottest";
            argv = slottest_argv;
            argc = 1;
        } else if (strcmp(spec, "diamondtest") == 0) {
            path = "/disk/bin/diamondtest";
            argv = diamondtest_argv;
            argc = 1;
        } else if (strcmp(spec, "needstest") == 0) {
            /* El programa con una dependencia declarada. El escenario
             * needstest-missing borra la libreria del volumen antes de arrancar;
             * con ella en su sitio el mismo programa resuelve y se va. */
            path = "/disk/bin/needstest";
            argv = needstest_argv;
            argc = 1;
        } else if (strcmp(spec, "clocktest") == 0) {
            path = "/disk/bin/clocktest";
            argv = clocktest_argv;
            argc = 1;
        } else if (strcmp(spec, "datetest") == 0) {
            path = "/disk/bin/datetest";
            argv = datetest_argv;
            argc = 1;
        } else if (strcmp(spec, "netsmoke") == 0) {
            path = "/disk/bin/nettest";
            argv = nettest_argv;
            argc = 1;
        } else if (strcmp(spec, "mediaplayer") == 0) {
            path = "/bin/mediaplayer";
            argv = mediaplayer_argv;
            argc = 6;
        } else if (strcmp(spec, "mediaplayer-show") == 0) {
            path = "/bin/mediaplayer";
            argv = mediaplayer_show_argv;
            argc = 4;
        } else if (strcmp(spec, "floatsmoke") == 0 || strcmp(spec, "float-smoke") == 0) {
            path = "/disk/bin/floatsmoke";
            argv = floatsmoke_argv;
            argc = 1;
        } else if (strcmp(spec, "ccleste-selftest") == 0 || strcmp(spec, "ccleste") == 0) {
            path = "/disk/bin/ccleste";
            argv = ccleste_argv;
            argc = 2;
        } else if (strcmp(spec, "windowd-cursor-repro") == 0) {
            path = "/bin/windowd";
            argv = cursor_repro_argv;
            argc = 2;
        } else if (strcmp(spec, "soak") == 0 || strcmp(spec, "gputest --soak") == 0) {
            path = "/disk/bin/gputest";
            argv = soak_argv;
            argc = 2;
        } else if (strcmp(spec, "audiostream") == 0) {
            path = "/disk/bin/audiotest";
            argv = audiostream_argv;
            argc = 2;
        } else if (strcmp(spec, "audiostream-quiet") == 0) {
            path = "/disk/bin/audiotest";
            argv = audiostream_quiet_argv;
            argc = 2;
        } else if (strcmp(spec, "audiodselftest") == 0) {
            path = "/bin/audiod";
            argv = audiod_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "volumesmoke") == 0) {
            path = "/disk/bin/volume";
            argv = volume_argv;
            argc = 2;
        } else if (strcmp(spec, "audiorecord") == 0) {
            path = "/disk/bin/audiotest";
            argv = audiorecord_argv;
            argc = 2;
        } else if (strcmp(spec, "guihost") == 0 || strcmp(spec, "native-guihost") == 0) {
            path = "/disk/bin/nativeguihost";
            argv = guihost_argv;
            argc = 1;
        } else if (strcmp(spec, "nativehello") == 0 || strcmp(spec, "native-hello") == 0) {
            path = "/disk/bin/nativehello";
            argv = nativehello_argv;
            argc = 1;
        } else if (strcmp(spec, "sxguihost") == 0 || strcmp(spec, "native-sxgui") == 0) {
            path = "/disk/bin/sxguihost";
            argv = sxguihost_argv;
            argc = 1;
        } else if (strcmp(spec, "kbdtest") == 0 || strcmp(spec, "kbd-selftest") == 0) {
            path = "/disk/bin/kbdtest";
            argv = kbdtest_argv;
            argc = 2;
        } else if (strcmp(spec, "mousetest") == 0 || strcmp(spec, "mousetest --selftest") == 0) {
            /* El gemelo del kbdtest para el puntero: mismo patron (sesion
             * grafica, READY, el host mueve el device por QMP, checkpoints),
             * distinto device. */
            path = "/disk/bin/mousetest";
            argv = mousetest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "tcptest ")) {
            /* El puerto no se puede hornear en el binario: lo elige el host al
             * levantar tools/tcp_echo_server.py. */
            const char* port = skip_spaces(spec + strlen("tcptest"));
            if (port[0] == '\0') {
                printf("%s FAIL missing port\n", label);
                return 1;
            }
            path = "/disk/bin/tcptest";
            tcptest_argv[1] = port;
            argv = tcptest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "ntptest ")) {
            /* Igual que tcptest: el puerto lo elige el host al levantar
             * tools/ntp_server.py y llega sustituido en el spec. */
            const char* port = skip_spaces(spec + strlen("ntptest"));
            if (port[0] == '\0') {
                printf("%s FAIL missing port\n", label);
                return 1;
            }
            path = "/disk/bin/ntptest";
            ntptest_argv[1] = port;
            argv = ntptest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "gputest --soak ")) {
            const char* iterations = skip_spaces(spec + strlen("gputest --soak"));
            if (iterations[0] == '\0') {
                printf("%s FAIL missing soak iteration count\n", label);
                return 1;
            }
            path = "/disk/bin/gputest";
            soak_argv[2] = iterations;
            argv = soak_argv;
            argc = 3;
        } else {
            printf("%s FAIL unknown runner '%s'\n", label, spec);
            return 1;
        }
    }

    long runner_fd = savanxp_open(path);
    if (runner_fd < 0) {
        printf("%s FAIL missing runner %s (%s)\n", label, path, result_error_string(runner_fd));
        return 1;
    }
    savanxp_close((int)runner_fd);

    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("%s FAIL spawn %s (%s)\n", label, path, result_error_string(pid));
        return 1;
    }

    savanxp_waitpid((int)pid, &status);
    printf("init: %s runner exited with %d\n", label, status);
    if (status == 0) {
        printf("%s PASS\n", label);
    } else {
        printf("%s FAIL status=%d\n", label, status);
    }
    return status;
}

#define KEYBOARD_LAYOUT_CONFIG_PATH "/disk/keyboard.cfg"
#define AUDIO_CONFIG_PATH "/disk/audio.cfg"

/* Layout preferido (mismo patron de 1 digito ASCII que desktop.cfg): se
 * aplica ANTES de arrancar windowd para que el layout ya este activo cuando
 * el primer evento de teclado llegue. Abre /dev/input0 solo para el ioctl --
 * nunca para leer, porque la cola de eventos es global y windowd todavia no
 * arranco para drenarla. */
static void apply_keyboard_layout_preference(void) {
    char digit = 0;
    long config_fd = savanxp_open(KEYBOARD_LAYOUT_CONFIG_PATH);
    long input_fd;

    if (config_fd < 0) {
        return;
    }
    if (savanxp_read((int)config_fd, &digit, 1) != 1) {
        savanxp_close((int)config_fd);
        return;
    }
    savanxp_close((int)config_fd);
    if (digit != '0' + SAVANXP_KEYBOARD_LAYOUT_EN) {
        return;
    }

    input_fd = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (input_fd < 0) {
        return;
    }
    (void)input_set_layout((int)input_fd, SAVANXP_KEYBOARD_LAYOUT_EN);
    savanxp_close((int)input_fd);
}

/* Volumen preferido (mismo patron que keyboard.cfg pero con dos numeros:
 * "75 0" = volumen 75 sin mutear). Se aplica ANTES de arrancar windowd para
 * que el primer sonido ya salga al nivel guardado. Sin fichero o con
 * contenido roto no se toca nada: el kernel arranca a 100 sin mutear. */
static void apply_audio_preference(void) {
    char text[16] = {0};
    long config_fd = savanxp_open(AUDIO_CONFIG_PATH);
    long audio_fd;
    long bytes_read;
    unsigned int volume = 0;
    unsigned int muted = 0;
    size_t index = 0;

    if (config_fd < 0) {
        return;
    }
    bytes_read = savanxp_read((int)config_fd, text, sizeof(text) - 1);
    savanxp_close((int)config_fd);
    if (bytes_read <= 0 || bytes_read >= (long)sizeof(text)) {
        return;
    }
    text[bytes_read] = '\0';

    while (is_space_char(text[index])) {
        ++index;
    }
    if (text[index] < '0' || text[index] > '9') {
        return;
    }
    while (text[index] >= '0' && text[index] <= '9') {
        volume = volume * 10u + (unsigned int)(text[index] - '0');
        if (volume > 100u) {
            return;
        }
        ++index;
    }
    if (!is_space_char(text[index])) {
        return;
    }
    while (is_space_char(text[index])) {
        ++index;
    }
    if (text[index] != '0' && text[index] != '1') {
        return;
    }
    muted = (unsigned int)(text[index] - '0');
    ++index;
    while (text[index] != '\0') {
        if (!is_space_char(text[index])) {
            return;
        }
        ++index;
    }

    /* Solo para los ioctl: el volumen vive en el kernel y se lee o fija sin
     * ser dueno del dispositivo, asi que abrir aca no le quita el audio a
     * nadie. */
    audio_fd = savanxp_open_mode("/dev/audio0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (audio_fd < 0) {
        return;
    }
    (void)audio_set_volume((int)audio_fd, (int)volume);
    (void)audio_set_muted((int)audio_fd, (int)muted);
    savanxp_close((int)audio_fd);
}

int main(void) {
    const char* windowd_argv[] = {"/bin/windowd", 0};
    const char* shell_argv[] = {"/bin/sh", 0};
    unsigned long last_windowd_start_ms = 0;
    int rapid_failures = 0;

    long smoke_trigger = savanxp_open("/SMOKE");
    if (smoke_trigger >= 0) {
        char automation_spec[64] = {};
        long bytes_read = savanxp_read((int)smoke_trigger, automation_spec, sizeof(automation_spec) - 1);
        savanxp_close((int)smoke_trigger);
        if (bytes_read < 0) {
            automation_spec[0] = '\0';
        }
        trim_automation_spec(automation_spec);
        if (automation_spec[0] == '\0') {
            memcpy(automation_spec, "smoke", sizeof("smoke"));
        }
        (void)run_automation_spec(automation_spec);
        for (;;) {
            sleep_ms(1000);
        }
    }

    apply_keyboard_layout_preference();
    apply_audio_preference();

    /* El demonio de audio antes que windowd: asi gana siempre la carrera por
     * /dev/audio0 y los clientes lo encuentran ya escuchando. Sin red o sin
     * audio sale solo y todo sigue directo; el supervisor de abajo solo lo
     * relanza si muere sirviendo, no si su salida es deliberada. En modo smoke
     * no se llega aca y el device queda libre como siempre. */
    const char* audiod_argv[] = {"/bin/audiod", 0};
    long audiod_pid = spawn("/bin/audiod", audiod_argv, 1);
    if (audiod_pid < 0) {
        printf("init: failed to spawn audiod (%s)\n", result_error_string(audiod_pid));
        audiod_pid = 0;
    }

    long windowd_pid = 0;
    for (;;) {
        int status = 0;
        unsigned long runtime_ms = 0;

        if (windowd_pid <= 0) {
            windowd_pid = spawn("/bin/windowd", windowd_argv, 1);
            if (windowd_pid < 0) {
                printf("init: failed to spawn windowd (%s)\n", result_error_string(windowd_pid));
                windowd_pid = 0;
                sleep_ms(1000);
            } else {
                last_windowd_start_ms = uptime_ms();
            }
        }

        if (audiod_pid <= 0 && windowd_pid <= 0) {
            sleep_ms(1000);
            continue;
        }

        long pid = savanxp_waitpid(-1, &status);
        if (pid < 0) {
            /* Sin hijo que reapear todavia (carrera): reintentar en un rato. */
            sleep_ms(250);
            continue;
        }

        if (pid == audiod_pid && audiod_pid != 0) {
            audiod_pid = 0;
            if (status == 0) {
                /* Salida deliberada (sin red, sin audio, otro demonio): el
                 * sistema sigue directo por turnos, no hay que insistir. */
                printf("init: audiod left without mixing (status 0)\n");
            } else {
                printf("init: audiod exited with %d, restarting\n", status);
                sleep_ms(250);
                audiod_pid = spawn("/bin/audiod", audiod_argv, 1);
                if (audiod_pid < 0) {
                    printf("init: failed to spawn audiod (%s)\n", result_error_string(audiod_pid));
                    audiod_pid = 0;
                }
            }
            continue;
        }

        if (pid == windowd_pid && windowd_pid != 0) {
            runtime_ms = uptime_ms() - last_windowd_start_ms;
            printf("init: windowd exited with %d, restarting\n", status);
            windowd_pid = 0;

            if (status != 0 && runtime_ms < 2000UL) {
                rapid_failures += 1;
            } else {
                rapid_failures = 0;
            }

            if (rapid_failures >= 3) {
                printf("init: windowd unstable, falling back to /bin/sh\n");
                long shell_pid = spawn("/bin/sh", shell_argv, 1);
                if (shell_pid < 0) {
                    printf("init: failed to spawn fallback shell (%s)\n", result_error_string(shell_pid));
                    sleep_ms(1000);
                } else {
                    int shell_status = 0;
                    savanxp_waitpid((int)shell_pid, &shell_status);
                    printf("init: fallback shell exited with %d, retrying windowd\n", shell_status);
                }
                rapid_failures = 0;
            }

            sleep_ms(250);
            continue;
        }
        /* Un hijo que no es audiod ni windowd (una app reparentada al morir
         * windowd, por ejemplo): se reapea y se sigue; a init no le toca. */
    }
}
