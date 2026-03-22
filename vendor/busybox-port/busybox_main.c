#include "libbb.h"

int cat_main(int argc, char** argv);
int echo_main(int argc, char** argv);
int mkdir_main(int argc, char** argv);
int rm_main(int argc, char** argv);
int mv_main(int argc, char** argv);
int cp_main(int argc, char** argv);
int ash_main(int argc, char** argv);
int bb_ls_main(int argc, char** argv);
int bb_ps_main(int argc, char** argv);
int bb_sh_main(int argc, char** argv);
int bb_true_main(int argc, char** argv);
int bb_false_main(int argc, char** argv);
int bb_sleep_main(int argc, char** argv);

struct AppletEntry {
    const char* name;
    int (*main_fn)(int argc, char** argv);
};

static const struct AppletEntry kApplets[] = {
    {"ash", ash_main},
    {"cat", cat_main},
    {"cp", cp_main},
    {"echo", echo_main},
    {"false", bb_false_main},
    {"ls", bb_ls_main},
    {"mkdir", mkdir_main},
    {"mv", mv_main},
    {"ps", bb_ps_main},
    {"rm", rm_main},
    {"sh", bb_sh_main},
    {"sleep", bb_sleep_main},
    {"true", bb_true_main},
};

static int run_applet(const char* name, int argc, char** argv) {
    for (size_t index = 0; index < ARRAY_SIZE(kApplets); ++index) {
        if (strcmp(kApplets[index].name, name) == 0) {
            applet_name = kApplets[index].name;
            return kApplets[index].main_fn(argc, argv);
        }
    }

    fprintf(stderr, "busybox: unknown applet '%s'\n", name != NULL ? name : "(null)");
    fprintf(stderr, "busybox: available:");
    for (size_t index = 0; index < ARRAY_SIZE(kApplets); ++index) {
        fprintf(stderr, " %s", kApplets[index].name);
    }
    fprintf(stderr, "\n");
    return 1;
}

const char applet_names[] ALIGN1 =
    "ash\0"
    "cat\0"
    "cp\0"
    "echo\0"
    "false\0"
    "ls\0"
    "mkdir\0"
    "mv\0"
    "ps\0"
    "rm\0"
    "sh\0"
    "sleep\0"
    "true\0"
    "\0";

int find_applet_by_name(const char* name) {
    for (size_t index = 0; index < ARRAY_SIZE(kApplets); ++index) {
        if (strcmp(kApplets[index].name, name) == 0) {
            return (int)index;
        }
    }
    return -1;
}

void run_applet_no_and_exit(int applet_no, const char* name, char** argv) {
    int argc = 0;
    if (applet_no < 0 || (size_t)applet_no >= ARRAY_SIZE(kApplets)) {
        fprintf(stderr, "busybox: unknown applet '%s'\n", name != NULL ? name : "(null)");
        exit(127);
    }
    while (argv != NULL && argv[argc] != NULL) {
        argc += 1;
    }
    applet_name = kApplets[applet_no].name;
    exit(kApplets[applet_no].main_fn(argc, argv));
}

int main(int argc, char** argv) {
    const char* invoked = bb_basename((argc > 0 && argv[0] != NULL) ? argv[0] : "busybox");

    if (strcmp(invoked, "busybox") == 0) {
        if (argc < 2) {
            applet_name = "busybox";
            fprintf(stdout, "BusyBox 1.37.0 subset for SavanXP\n");
            fprintf(stdout, "Applets:");
            for (size_t index = 0; index < ARRAY_SIZE(kApplets); ++index) {
                fprintf(stdout, " %s", kApplets[index].name);
            }
            fprintf(stdout, "\n");
            return 0;
        }
        argv += 1;
        argc -= 1;
        invoked = argv[0];
    }

    return run_applet(invoked, argc, argv);
}
