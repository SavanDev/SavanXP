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
    if (spec != 0 && text_contains(spec, "windowd-hw-native")) {
        return "NATIVE HW";
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
    if (spec != 0 && text_contains(spec, "sessiontest")) {
        return "SESSION SMOKE";
    }
    if (spec != 0 && text_contains(spec, "elevtest")) {
        return "ELEVATION SMOKE";
    }
    if (spec != 0 && text_contains(spec, "nativetest")) {
        return "NATIVE TEST";
    }
    return "SMOKE";
}

static int run_automation_spec(const char* spec) {
    const char* smoke_argv[] = {"/bin/smoke", 0};
    const char* soak_argv[] = {"/bin/gputest", "--soak", 0, 0};
    const char* windowd_argv[] = {"/bin/windowd.sxe", "--selftest", 0};
    const char* windowd_hw_native_argv[] = {"/bin/windowd.sxe", "--hw-native-test", 0};
    const char* cursor_repro_argv[] = {"/bin/windowd.sxe", "--cursor-repro", 0};
    const char* progman_selftest_argv[] = {"/bin/progman", "--selftest", 0};
    const char* appwiz_selftest_argv[] = {"/bin/appwiz", "--selftest", 0};
    const char* sxetest_argv[] = {"/bin/sxetest", 0};
    const char* filesapp_selftest_argv[] = {"/bin/filesapp", "--selftest", 0};
    const char* taskmgr_selftest_argv[] = {"/bin/taskmgr", "--selftest", 0};
    const char* calc_selftest_argv[] = {"/bin/calc", "--selftest", 0};
    const char* mines_selftest_argv[] = {"/bin/mines", "--selftest", 0};
    const char* paint_selftest_argv[] = {"/bin/paint", "--selftest", 0};
    const char* clocktest_argv[] = {"/bin/clocktest", 0};
    const char* datetest_argv[] = {"/bin/datetest", 0};
    const char* needstest_argv[] = {"/bin/needstest", 0};
    /* Los cuatro diagnósticos del cargador. Cada uno afirmaba en error y nada más,
     * así que ninguno podía tener escenario: ahora cada uno imprime su token de
     * éxito y hay uno que lo ejecuta. */
    const char* brokentest_argv[] = {"/bin/brokentest", 0};
    const char* missingtest_argv[] = {"/bin/missingtest", 0};
    const char* slottest_argv[] = {"/bin/slottest", 0};
    const char* diamondtest_argv[] = {"/bin/diamondtest", 0};
    const char* dltest_argv[] = {"/bin/dltest", 0};
    const char* ffmpegload_argv[] = {"/bin/ffmpegload", 0};
    const char* audiostream_argv[] = {"/bin/audiotest", "--stream", 0};
    const char* audiostream_quiet_argv[] = {"/bin/audiotest", "--stream-quiet", 0};
    const char* audiod_selftest_argv[] = {"/bin/audiod", "--selftest", 0};
    const char* volume_argv[] = {"/bin/volume", "75", 0};
    const char* audiorecord_argv[] = {"/bin/audiotest", "--record", 0};
    const char* nettest_argv[] = {"/bin/nettest", 0};
    const char* tcptest_argv[] = {"/bin/tcptest", 0, 0};
    const char* ntptest_argv[] = {"/bin/ntptest", 0, 0};
    const char* floatsmoke_argv[] = {"/bin/floatsmoke", 0};
    const char* nativetest_argv[] = {"/bin/nativetest.sxe", 0};
    const char* pthreadtest_argv[] = {"/bin/pthreadtest", 0};
    const char* sessiontest_argv[] = {"/bin/sessiontest", 0};
    const char* elevtest_argv[] = {"/bin/elevtest", 0};
    const char* ccleste_argv[] = {"/bin/ccleste", "--selftest", 0};
    const char* kbdtest_argv[] = {"/bin/kbdtest", "--selftest", 0};
    const char* mousetest_argv[] = {"/bin/mousetest", "--selftest", 0};
    /* Tres caminos: audio solo (WAV), video solo sin contenedor (MJPEG crudo) y
     * los dos intercalados en AVI, que ademas mide la sincronia. */
    const char* mediaplayer_argv[] = {"/bin/mediaplayer", "--selftest", "/media/tono.wav",
                                      "/media/clip.mjpeg", "--sync", "/media/avsync.avi", 0};
    const char* mediaplayer_show_argv[] = {"/bin/mediaplayer", "--gpu-hold", "8000", "/media/avsync.avi", 0};
    const char* path = "/bin/smoke";
    const char* const* argv = smoke_argv;
    const char* label = automation_label_for_spec(spec);
    int argc = 1;
    int status = 0;

    if (spec != 0 && strcmp(spec, "smoke") != 0 && spec[0] != '\0') {
        if (strcmp(spec, "windowd-selftest") == 0 || strcmp(spec, "windowd") == 0) {
            path = "/bin/windowd.sxe";
            argv = windowd_argv;
            argc = 2;
        } else if (strcmp(spec, "windowd-hw-native") == 0) {
            path = "/bin/windowd.sxe";
            argv = windowd_hw_native_argv;
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
            path = "/bin/sxetest";
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
        } else if (strcmp(spec, "paint-selftest") == 0 || strcmp(spec, "paint") == 0) {
            path = "/bin/paint";
            argv = paint_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "calc-selftest") == 0 || strcmp(spec, "calc") == 0) {
            path = "/bin/calc";
            argv = calc_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "ffmpegload") == 0) {
            /* Carga libffmpeg.so.0.4 y mide. El escenario instala el port antes,
             * asi que la libreria esta en el volumen. */
            path = "/bin/ffmpegload";
            argv = ffmpegload_argv;
            argc = 1;
        } else if (strcmp(spec, "brokentest") == 0) {
            path = "/bin/brokentest";
            argv = brokentest_argv;
            argc = 1;
        } else if (strcmp(spec, "missingtest") == 0) {
            path = "/bin/missingtest";
            argv = missingtest_argv;
            argc = 1;
        } else if (strcmp(spec, "slottest") == 0) {
            path = "/bin/slottest";
            argv = slottest_argv;
            argc = 1;
        } else if (strcmp(spec, "diamondtest") == 0) {
            path = "/bin/diamondtest";
            argv = diamondtest_argv;
            argc = 1;
        } else if (strcmp(spec, "dltest") == 0) {
            path = "/bin/dltest";
            argv = dltest_argv;
            argc = 1;
        } else if (strcmp(spec, "needstest") == 0) {
            /* El programa con una dependencia declarada. El escenario
             * needstest-missing borra la libreria del volumen antes de arrancar;
             * con ella en su sitio el mismo programa resuelve y se va. */
            path = "/bin/needstest";
            argv = needstest_argv;
            argc = 1;
        } else if (strcmp(spec, "clocktest") == 0) {
            path = "/bin/clocktest";
            argv = clocktest_argv;
            argc = 1;
        } else if (strcmp(spec, "datetest") == 0) {
            path = "/bin/datetest";
            argv = datetest_argv;
            argc = 1;
        } else if (strcmp(spec, "netsmoke") == 0) {
            path = "/bin/nettest";
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
            path = "/bin/floatsmoke";
            argv = floatsmoke_argv;
            argc = 1;
        } else if (strcmp(spec, "elevtest") == 0) {
            path = "/bin/elevtest";
            argv = elevtest_argv;
            argc = 1;
        } else if (strcmp(spec, "sessiontest") == 0) {
            path = "/bin/sessiontest";
            argv = sessiontest_argv;
            argc = 1;
        } else if (strcmp(spec, "pthreadtest") == 0 || strcmp(spec, "pthread-test") == 0) {
            path = "/bin/pthreadtest";
            argv = pthreadtest_argv;
            argc = 1;
        } else if (strcmp(spec, "nativetest") == 0 || strcmp(spec, "native-test") == 0) {
            path = "/bin/nativetest.sxe";
            argv = nativetest_argv;
            argc = 1;
        } else if (strcmp(spec, "ccleste-selftest") == 0 || strcmp(spec, "ccleste") == 0) {
            path = "/bin/ccleste";
            argv = ccleste_argv;
            argc = 2;
        } else if (strcmp(spec, "windowd-cursor-repro") == 0) {
            path = "/bin/windowd.sxe";
            argv = cursor_repro_argv;
            argc = 2;
        } else if (strcmp(spec, "soak") == 0 || strcmp(spec, "gputest --soak") == 0) {
            path = "/bin/gputest";
            argv = soak_argv;
            argc = 2;
        } else if (strcmp(spec, "audiostream") == 0) {
            path = "/bin/audiotest";
            argv = audiostream_argv;
            argc = 2;
        } else if (strcmp(spec, "audiostream-quiet") == 0) {
            path = "/bin/audiotest";
            argv = audiostream_quiet_argv;
            argc = 2;
        } else if (strcmp(spec, "audiodselftest") == 0) {
            path = "/bin/audiod";
            argv = audiod_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "volumesmoke") == 0) {
            path = "/bin/volume";
            argv = volume_argv;
            argc = 2;
        } else if (strcmp(spec, "audiorecord") == 0) {
            path = "/bin/audiotest";
            argv = audiorecord_argv;
            argc = 2;
        } else if (strcmp(spec, "kbdtest") == 0 || strcmp(spec, "kbd-selftest") == 0) {
            path = "/bin/kbdtest";
            argv = kbdtest_argv;
            argc = 2;
        } else if (strcmp(spec, "mousetest") == 0 || strcmp(spec, "mousetest --selftest") == 0) {
            /* El gemelo del kbdtest para el puntero: mismo patron (sesion
             * grafica, READY, el host mueve el device por QMP, checkpoints),
             * distinto device. */
            path = "/bin/mousetest";
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
            path = "/bin/tcptest";
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
            path = "/bin/ntptest";
            ntptest_argv[1] = port;
            argv = ntptest_argv;
            argc = 2;
        } else if (text_starts_with(spec, "gputest --soak ")) {
            const char* iterations = skip_spaces(spec + strlen("gputest --soak"));
            if (iterations[0] == '\0') {
                printf("%s FAIL missing soak iteration count\n", label);
                return 1;
            }
            path = "/bin/gputest";
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

#define KEYBOARD_LAYOUT_CONFIG_PATH "/keyboard.cfg"
#define AUDIO_CONFIG_PATH "/audio.cfg"

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

/* --- the rescue system ------------------------------------------------------
 *
 * The kernel starts /rescue/bin/init when no disk mounted at "/"
 * (docs/kernel/FILESYSTEMS.md#the-rescue-system). There is no desktop to bring
 * up, no audiod and no /bin: init only keeps a shell from the initramfs alive
 * on the text console. */

#define RESCUE_ROOT "/rescue"
#define RESCUE_SHELL RESCUE_ROOT "/bin/sh"

static int started_from_rescue(int argc, char** argv) {
    return argc > 0 && argv != 0 && argv[0] != 0 && text_starts_with(argv[0], RESCUE_ROOT "/");
}

/* Runs a program and returns its exit status, or -1 if it did not start. A
 * failed check prints "rescue: FAIL", not the failure token: the runner stops
 * at the token, and the remaining checks still have something to say. */
static int rescue_run(const char* path, const char* const* argv, int argc) {
    int status = 0;
    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("rescue: FAIL spawn %s (%s)\n", path, result_error_string(pid));
        return -1;
    }
    savanxp_waitpid((int)pid, &status);
    return status;
}

static int rescue_expect(const char* path, const char* const* argv, int argc, int expected, const char* what) {
    const int status = rescue_run(path, argv, argc);
    if (status != expected) {
        printf("rescue: FAIL %s: %s exited with %d, expected %d\n", what, path, status, expected);
        return 0;
    }
    printf("rescue: ok %s\n", what);
    return 1;
}

/* What the rescue system has to be able to do with no disk at all. */
static int rescue_selftest(void) {
    const char* ls_argv[] = {RESCUE_ROOT "/bin/ls", RESCUE_ROOT "/bin", 0};
    const char* cat_argv[] = {RESCUE_ROOT "/bin/cat", RESCUE_ROOT "/README", 0};
    // Neither command is a path: sh has to find both in /rescue/bin, with no /bin.
    const char* sh_argv[] = {RESCUE_SHELL, "-c", "echo rescue-shell | cat", 0};
    const char* df_argv[] = {RESCUE_ROOT "/bin/df", 0};
    const char* imagetest_argv[] = {RESCUE_ROOT "/bin/imagetest", 0};
    int ok = 1;

    long disk_init = savanxp_open("/bin/init");
    if (disk_init >= 0) {
        savanxp_close((int)disk_init);
        puts_out("RESCUE SMOKE FAIL /bin/init exists: this boot has a disk\n");
        return 1;
    }
    ok = rescue_expect(ls_argv[0], ls_argv, 2, 0, "ls lists /rescue/bin") && ok;
    ok = rescue_expect(cat_argv[0], cat_argv, 2, 0, "cat reads /rescue/README") && ok;
    ok = rescue_expect(sh_argv[0], sh_argv, 3, 0, "sh finds commands in /rescue/bin") && ok;
    ok = rescue_expect(df_argv[0], df_argv, 1, 1, "df reports no root volume") && ok;
    ok = rescue_expect(imagetest_argv[0], imagetest_argv, 1, 0, "the in-memory executable reader") && ok;
    puts_out(ok ? "RESCUE SMOKE PASS\n" : "RESCUE SMOKE FAIL\n");
    return ok ? 0 : 1;
}

__attribute__((noreturn)) static void run_rescue_system(void) {
    const char* shell_argv[] = {RESCUE_SHELL, 0};

    long smoke_trigger = savanxp_open(RESCUE_ROOT "/SMOKE");
    if (smoke_trigger >= 0) {
        char automation_spec[64] = {};
        long bytes_read = savanxp_read((int)smoke_trigger, automation_spec, sizeof(automation_spec) - 1);
        savanxp_close((int)smoke_trigger);
        if (bytes_read < 0) {
            automation_spec[0] = '\0';
        }
        trim_automation_spec(automation_spec);
        if (strcmp(automation_spec, "rescue") == 0) {
            (void)rescue_selftest();
        } else {
            printf("RESCUE SMOKE FAIL '%s' needs the disk; this boot has none\n", automation_spec);
        }
        for (;;) {
            sleep_ms(1000);
        }
    }

    puts_out("init: no system disk mounted; starting the rescue shell\n");
    for (;;) {
        int status = 0;
        long pid = spawn(RESCUE_SHELL, shell_argv, 1);
        if (pid < 0) {
            printf("init: failed to spawn %s (%s)\n", RESCUE_SHELL, result_error_string(pid));
            sleep_ms(1000);
            continue;
        }
        savanxp_waitpid((int)pid, &status);
        printf("init: the rescue shell exited with %d, starting it again\n", status);
        sleep_ms(250);
    }
}

int main(int argc, char** argv) {
    if (started_from_rescue(argc, argv)) {
        run_rescue_system();
    }

    /* The graphical session belongs to sessiond (docs/desktop/
     * DESKTOP_ARCHITECTURE.md#the-session-manager-sessiond); init keeps audiod
     * and the fallback to the text shell. */
    const char* sessiond_argv[] = {"/bin/sessiond.sxe", 0};
    const char* shell_argv[] = {"/bin/sh", 0};
    unsigned long last_sessiond_start_ms = 0;
    int rapid_failures = 0;

    /* The smoke trigger is staged into the initramfs, never onto the disk, so a
     * smoke build leaves the persistent image alone. */
    long smoke_trigger = savanxp_open("/rescue/SMOKE");
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

    /* The elevation broker before the session (docs/kernel/
     * USERS_AND_PERMISSIONS.md#elevation): its port name is taken before any
     * program could take it. It is restarted if it dies, unless it dies at
     * once, which is a broken binary or a name already taken. */
    const char* elevationd_argv[] = {"/bin/elevationd.sxe", 0};
    long elevationd_pid = spawn("/bin/elevationd.sxe", elevationd_argv, 1);
    unsigned long elevationd_started_ms = uptime_ms();
    if (elevationd_pid < 0) {
        printf("init: failed to spawn elevationd (%s)\n", result_error_string(elevationd_pid));
        elevationd_pid = 0;
    }

    long sessiond_pid = 0;
    for (;;) {
        int status = 0;
        unsigned long runtime_ms = 0;

        if (sessiond_pid <= 0) {
            sessiond_pid = spawn("/bin/sessiond.sxe", sessiond_argv, 1);
            if (sessiond_pid < 0) {
                printf("init: failed to spawn sessiond (%s)\n", result_error_string(sessiond_pid));
                sessiond_pid = 0;
                sleep_ms(1000);
            } else {
                last_sessiond_start_ms = uptime_ms();
            }
        }

        if (audiod_pid <= 0 && sessiond_pid <= 0) {
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

        if (pid == elevationd_pid && elevationd_pid != 0) {
            elevationd_pid = 0;
            if (uptime_ms() - elevationd_started_ms < 2000UL) {
                printf("init: elevationd exited at once with %d; elevation is unavailable\n", status);
            } else {
                printf("init: elevationd exited with %d, restarting\n", status);
                elevationd_pid = spawn("/bin/elevationd.sxe", elevationd_argv, 1);
                elevationd_started_ms = uptime_ms();
                if (elevationd_pid < 0) {
                    printf("init: failed to spawn elevationd (%s)\n", result_error_string(elevationd_pid));
                    elevationd_pid = 0;
                }
            }
            continue;
        }

        if (pid == sessiond_pid && sessiond_pid != 0) {
            runtime_ms = uptime_ms() - last_sessiond_start_ms;
            printf("init: sessiond exited with %d, restarting\n", status);
            sessiond_pid = 0;

            if (status != 0 && runtime_ms < 2000UL) {
                rapid_failures += 1;
            } else {
                rapid_failures = 0;
            }

            /* SESSIOND_EXIT_GAVE_UP (3): it already retried windowd itself and
             * the session will not come up. */
            if (status == 3 || rapid_failures >= 3) {
                printf("init: the graphical session is unstable, falling back to /bin/sh\n");
                long shell_pid = spawn("/bin/sh", shell_argv, 1);
                if (shell_pid < 0) {
                    printf("init: failed to spawn fallback shell (%s)\n", result_error_string(shell_pid));
                    sleep_ms(1000);
                } else {
                    int shell_status = 0;
                    savanxp_waitpid((int)shell_pid, &shell_status);
                    printf("init: fallback shell exited with %d, retrying the session\n", shell_status);
                }
                rapid_failures = 0;
            }

            sleep_ms(250);
            continue;
        }
        /* A child that is none of the three (an app reparented when its
         * launcher died, for instance): reaped and ignored. */
    }
}
