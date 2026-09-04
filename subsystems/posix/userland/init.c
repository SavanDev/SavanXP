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
    if (spec != 0 && text_contains(spec, "progman")) {
        return "PROGMAN SMOKE";
    }
    if (spec != 0 && text_contains(spec, "sxe")) {
        return "SXE SMOKE";
    }
    if (spec != 0 && text_contains(spec, "filesapp")) {
        return "FILESAPP SMOKE";
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
    if (spec != 0 && text_contains(spec, "audiostream")) {
        return "AUDIO STREAM";
    }
    if (spec != 0 && text_contains(spec, "netsmoke")) {
        return "NET SMOKE";
    }
    if (spec != 0 && text_contains(spec, "wavinfo")) {
        return "FFMPEG SMOKE";
    }
    if (spec != 0 && text_contains(spec, "guihost")) {
        return "NATIVEGUI HOST";
    }
    if (spec != 0 && text_contains(spec, "hello")) {
        return "NATIVE HELLO";
    }
    if (spec != 0 && text_contains(spec, "sxgui")) {
        return "SXGUI HOST";
    }
    return "SMOKE";
}

static int run_automation_spec(const char* spec) {
    const char* smoke_argv[] = {"/disk/bin/smoke", 0};
    const char* soak_argv[] = {"/disk/bin/gputest", "--soak", 0, 0};
    const char* windowd_argv[] = {"/bin/windowd", "--selftest", 0};
    const char* cursor_repro_argv[] = {"/bin/windowd", "--cursor-repro", 0};
    const char* progman_selftest_argv[] = {"/bin/progman", "--selftest", 0};
    const char* sxetest_argv[] = {"/disk/bin/sxetest", 0};
    const char* filesapp_selftest_argv[] = {"/bin/filesapp", "--selftest", 0};
    const char* audiostream_argv[] = {"/disk/bin/audiotest", "--stream", 0};
    const char* nettest_argv[] = {"/disk/bin/nettest", 0};
    const char* floatsmoke_argv[] = {"/disk/bin/floatsmoke", 0};
    const char* guihost_argv[] = {"/disk/bin/nativeguihost", 0};
    const char* nativehello_argv[] = {"/disk/bin/nativehello", 0};
    const char* sxguihost_argv[] = {"/disk/bin/sxguihost", 0};
    const char* kbdtest_argv[] = {"/disk/bin/kbdtest", "--selftest", 0};
    const char* wavinfo_argv[] = {"/disk/bin/wavinfo", "/disk/media/tono.wav", 0};
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
        } else if (strcmp(spec, "sxe-selftest") == 0 || strcmp(spec, "sxe") == 0) {
            path = "/disk/bin/sxetest";
            argv = sxetest_argv;
            argc = 1;
        } else if (strcmp(spec, "filesapp-selftest") == 0 || strcmp(spec, "filesapp") == 0) {
            path = "/bin/filesapp";
            argv = filesapp_selftest_argv;
            argc = 2;
        } else if (strcmp(spec, "netsmoke") == 0) {
            path = "/disk/bin/nettest";
            argv = nettest_argv;
            argc = 1;
        } else if (strcmp(spec, "wavinfo") == 0) {
            path = "/disk/bin/wavinfo";
            argv = wavinfo_argv;
            argc = 2;
        } else if (strcmp(spec, "floatsmoke") == 0 || strcmp(spec, "float-smoke") == 0) {
            path = "/disk/bin/floatsmoke";
            argv = floatsmoke_argv;
            argc = 1;
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

    input_fd = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ);
    if (input_fd < 0) {
        return;
    }
    (void)input_set_layout((int)input_fd, SAVANXP_KEYBOARD_LAYOUT_EN);
    savanxp_close((int)input_fd);
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

    for (;;) {
        int status = 0;
        unsigned long runtime_ms = 0;
        long pid = spawn("/bin/windowd", windowd_argv, 1);
        if (pid < 0) {
            printf("init: failed to spawn windowd (%s)\n", result_error_string(pid));
            sleep_ms(1000);
            continue;
        }

        last_windowd_start_ms = uptime_ms();
        savanxp_waitpid((int)pid, &status);
        runtime_ms = uptime_ms() - last_windowd_start_ms;
        printf("init: windowd exited with %d, restarting\n", status);

        if (status != 0 && runtime_ms < 2000UL) {
            rapid_failures += 1;
        } else {
            rapid_failures = 0;
        }

        if (rapid_failures >= 3) {
            printf("init: windowd unstable, falling back to /bin/sh\n");
            pid = spawn("/bin/sh", shell_argv, 1);
            if (pid < 0) {
                printf("init: failed to spawn fallback shell (%s)\n", result_error_string(pid));
                sleep_ms(1000);
            } else {
                savanxp_waitpid((int)pid, &status);
                printf("init: fallback shell exited with %d, retrying windowd\n", status);
            }
            rapid_failures = 0;
        }

        sleep_ms(250);
    }
}
