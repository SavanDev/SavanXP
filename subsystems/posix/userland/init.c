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
    if (spec != 0 && text_contains(spec, "soak")) {
        return "SOAK";
    }
    if (spec != 0 && text_contains(spec, "desktop")) {
        return "DESKTOP SMOKE";
    }
    return "SMOKE";
}

static int run_automation_spec(const char* spec) {
    const char* smoke_argv[] = {"/disk/bin/smoke", 0};
    const char* soak_argv[] = {"/disk/bin/gputest", "--soak", 0, 0};
    const char* desktop_argv[] = {"/bin/desktop", "--selftest", 0};
    const char* path = "/disk/bin/smoke";
    const char* const* argv = smoke_argv;
    const char* label = automation_label_for_spec(spec);
    int argc = 1;
    int status = 0;

    if (spec != 0 && strcmp(spec, "smoke") != 0 && spec[0] != '\0') {
        if (strcmp(spec, "desktop-selftest") == 0 || strcmp(spec, "desktop") == 0) {
            path = "/bin/desktop";
            argv = desktop_argv;
            argc = 2;
        } else if (strcmp(spec, "soak") == 0 || strcmp(spec, "gputest --soak") == 0) {
            path = "/disk/bin/gputest";
            argv = soak_argv;
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

    long runner_fd = open(path);
    if (runner_fd < 0) {
        printf("%s FAIL missing runner %s (%s)\n", label, path, result_error_string(runner_fd));
        return 1;
    }
    close((int)runner_fd);

    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("%s FAIL spawn %s (%s)\n", label, path, result_error_string(pid));
        return 1;
    }

    waitpid((int)pid, &status);
    printf("init: %s runner exited with %d\n", label, status);
    if (status == 0) {
        printf("%s PASS\n", label);
    } else {
        printf("%s FAIL status=%d\n", label, status);
    }
    return status;
}

int main(void) {
    const char* desktop_argv[] = {"/bin/desktop", 0};
    const char* shell_argv[] = {"/bin/sh", 0};
    unsigned long last_desktop_start_ms = 0;
    int rapid_failures = 0;

    long smoke_trigger = open("/SMOKE");
    if (smoke_trigger >= 0) {
        char automation_spec[64] = {};
        long bytes_read = read((int)smoke_trigger, automation_spec, sizeof(automation_spec) - 1);
        close((int)smoke_trigger);
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

    for (;;) {
        int status = 0;
        unsigned long runtime_ms = 0;
        long pid = spawn("/bin/desktop", desktop_argv, 1);
        if (pid < 0) {
            printf("init: failed to spawn desktop (%s)\n", result_error_string(pid));
            sleep_ms(1000);
            continue;
        }

        last_desktop_start_ms = uptime_ms();
        waitpid((int)pid, &status);
        runtime_ms = uptime_ms() - last_desktop_start_ms;
        printf("init: desktop exited with %d, restarting\n", status);

        if (status != 0 && runtime_ms < 2000UL) {
            rapid_failures += 1;
        } else {
            rapid_failures = 0;
        }

        if (rapid_failures >= 3) {
            printf("init: desktop unstable, falling back to /bin/sh\n");
            pid = spawn("/bin/sh", shell_argv, 1);
            if (pid < 0) {
                printf("init: failed to spawn fallback shell (%s)\n", result_error_string(pid));
                sleep_ms(1000);
            } else {
                waitpid((int)pid, &status);
                printf("init: fallback shell exited with %d, retrying desktop\n", status);
            }
            rapid_failures = 0;
        }

        sleep_ms(250);
    }
}
