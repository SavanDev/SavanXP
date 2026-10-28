#include "libc.h"

static int run_and_expect(const char* path, const char* const* argv, int argc, int expected_status) {
    int status = -1;
    long pid = spawn(path, argv, argc);
    if (pid < 0) {
        printf("SMOKE FAIL spawn %s (%s)\n", path, result_error_string(pid));
        return 0;
    }
    if (savanxp_waitpid((int)pid, &status) < 0) {
        printf("SMOKE FAIL waitpid %s\n", path);
        return 0;
    }
    if (status != expected_status) {
        printf("SMOKE FAIL status %s expected=%d got=%d\n", path, expected_status, status);
        return 0;
    }
    printf("smoke: ok %s\n", path);
    return 1;
}

/* Runs a program with its standard output in a file, for the checks that
 * have to read what it printed. */
static int run_to_file(const char* path, const char* const* argv, int argc, const char* output_path) {
    int status = -1;
    long fd = savanxp_open_mode(output_path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    long pid;
    if (fd < 0) {
        printf("SMOKE FAIL open %s\n", output_path);
        return 0;
    }
    pid = spawn_fds(path, argv, argc, 0, (int)fd, 2);
    savanxp_close((int)fd);
    if (pid < 0 || savanxp_waitpid((int)pid, &status) < 0 || status != 0) {
        printf("SMOKE FAIL run %s > %s\n", path, output_path);
        return 0;
    }
    printf("smoke: ok %s > %s\n", path, output_path);
    return 1;
}

static int file_exists(const char* path) {
    long fd = savanxp_open(path);
    if (fd < 0) {
        return 0;
    }
    savanxp_close((int)fd);
    return 1;
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

static int validate_file_contains(const char* path, const char* needle) {
    char buffer[256] = {};
    savanxp_sync();
    long fd = savanxp_open(path);
    if (fd < 0) {
        printf("SMOKE FAIL missing %s\n", path);
        return 0;
    }
    if (savanxp_read((int)fd, buffer, sizeof(buffer) - 1) <= 0) {
        savanxp_close((int)fd);
        printf("SMOKE FAIL unable to read %s\n", path);
        return 0;
    }
    savanxp_close((int)fd);
    if (!text_contains(buffer, needle)) {
        printf("SMOKE FAIL %s missing content '%s'\n", path, needle);
        return 0;
    }
    printf("smoke: validated %s\n", path);
    return 1;
}

static int file_missing(const char* path) {
    return !file_exists(path);
}

static int prepare_smoke_directory(void) {
    long existing = savanxp_open("/smoke");
    const char* mkdir_argv[] = {"/bin/mkdir", "/smoke", 0};
    if (existing >= 0) {
        savanxp_close((int)existing);
        (void)savanxp_unlink("/smoke/sh.txt");
        (void)savanxp_unlink("/smoke/readme.copy");
        (void)savanxp_unlink("/smoke/readme.moved");
        printf("smoke: reusing /smoke\n");
        return 1;
    }
    if (!run_and_expect("/bin/mkdir", mkdir_argv, 2, 0)) {
        puts_out("SMOKE FAIL mkdir /smoke\n");
        return 0;
    }
    printf("smoke: prepared /smoke\n");
    return 1;
}

static void cleanup_smoke_directory(void) {
    (void)savanxp_unlink("/smoke/sh.txt");
    (void)savanxp_unlink("/smoke/which.txt");
    (void)savanxp_unlink("/smoke/readme.copy");
    (void)savanxp_unlink("/smoke/readme.moved");
    (void)savanxp_rmdir("/smoke");
}

int main(void) {
    const char* forktest_argv[] = {"/bin/forktest", 0};
    const char* cwdtest_argv[] = {"/bin/cwdtest", 0};
    const char* idtest_argv[] = {"/bin/idtest", 0};
    const char* smptest_argv[] = {"/bin/smptest", 0};
    const char* polltest_argv[] = {"/bin/polltest", 0};
    const char* sigtest_argv[] = {"/bin/sigtest", 0};
    const char* eventtest_argv[] = {"/bin/eventtest", 0};
    const char* timertest_argv[] = {"/bin/timertest", 0};
    const char* sectiontest_argv[] = {"/bin/sectiontest", 0};
    const char* ldtest_argv[] = {"/bin/ldtest", 0};
    const char* libtest_argv[] = {"/bin/libtest", 0};
    const char* interptest_argv[] = {"/bin/interptest", 0};
    const char* brokentest_argv[] = {"/bin/brokentest", 0};
    const char* missingtest_argv[] = {"/bin/missingtest", 0};
    const char* diamondtest_argv[] = {"/bin/diamondtest", 0};
    const char* dltest_argv[] = {"/bin/dltest", 0};
    const char* slottest_argv[] = {"/bin/slottest", 0};
    const char* needstest_argv[] = {"/bin/needstest", 0};
    const char* pietest_argv[] = {"/bin/pietest", 0};
    const char* handletest_argv[] = {"/bin/handletest", 0};
    const char* semaphoretest_argv[] = {"/bin/semaphoretest", 0};
    const char* cliptest_argv[] = {"/bin/cliptest", 0};
    const char* seltest_argv[] = {"/bin/seltest", 0};
    const char* mmaptest_argv[] = {"/bin/mmaptest", 0};
    const char* heaptest_argv[] = {"/bin/heaptest", 0};
    const char* sh_argv[] = {"/bin/sh", "-c", "echo shell-redirect > /smoke/sh.txt", 0};
    /* A SUN program is console.sxe on disk; the shell finds it as `console`. */
    const char* which_argv[] = {"/bin/sh", "-c", "which console", 0};
    const char* echo_argv[] = {"/bin/echo", "smoke-echo", 0};
    const char* cat_argv[] = {"/bin/cat", "/smoke/sh.txt", 0};
    const char* ls_argv[] = {"/bin/ls", "/bin", 0};
    const char* cp_argv[] = {"/bin/cp", "/README", "/smoke/readme.copy", 0};
    const char* mv_argv[] = {"/bin/mv", "/smoke/readme.copy", "/smoke/readme.moved", 0};
    const char* rm_argv[] = {"/bin/rm", "/smoke/readme.moved", 0};
    const char* ps_argv[] = {"/bin/ps", 0};
    /* The recursive forms: a tree made with mkdir -p, copied with cp -r and
     * removed with rm -r, both copies. */
    const char* mkdir_tree_argv[] = {"/bin/mkdir", "-p", "/smoke/tree/sub", 0};
    const char* cp_into_tree_argv[] = {"/bin/cp", "/README", "/smoke/tree/sub", 0};
    const char* cp_tree_argv[] = {"/bin/cp", "-r", "/smoke/tree", "/smoke/tree2", 0};
    const char* rm_tree_argv[] = {"/bin/rm", "-r", "/smoke/tree", "/smoke/tree2", 0};
    const char* libctest_argv[] = {"/bin/libctest", 0};
    const char* stacktest_argv[] = {"/bin/stacktest", 0};
    /* The two image backends have different readers in the kernel:
     * /rescue/bin comes from the initramfs (in memory) and /bin from the SxFS
     * vnode, which is the path that stopped copying the whole image. */
    const char* imagetest_argv[] = {"/rescue/bin/imagetest", 0};
    const char* imagetest_disk_argv[] = {"/bin/imagetest", 0};
    const char* gputest_argv[] = {"/bin/gputest", "--smoke", 0};
    const char* audiotest_argv[] = {"/bin/audiotest", "--smoke", 0};
    /* El informe de consistencia tiene que dar "clean" sobre el volumen que el
     * propio smoke acaba de usar para escribir y borrar. Es la unica forma de
     * comprobar que la reconciliacion no inventa fugas donde no las hay: si
     * create/write/rm/update dejan el bitmap y los extents fuera de sincronia,
     * aqui se ve. */
    const char* fscheck_argv[] = {"/bin/fscheck", 0};

    puts_out("SMOKE START\n");

    if (!file_exists("/bin/forktest") ||
        !file_exists("/bin/smptest") ||
        !file_exists("/bin/polltest") ||
        !file_exists("/bin/sigtest") ||
        !file_exists("/bin/eventtest") ||
        !file_exists("/bin/timertest") ||
        !file_exists("/bin/sectiontest") ||
        !file_exists("/bin/ldtest") ||
        !file_exists("/bin/brokentest") ||
        !file_exists("/bin/missingtest") ||
        !file_exists("/bin/diamondtest") ||
        !file_exists("/bin/dltest") ||
        !file_exists("/lib/libdlmod_two.so.0.4") ||
        !file_exists("/bin/slottest") ||
        !file_exists("/bin/needstest") ||
        !file_exists("/lib/libdia_top.so.0.4") ||
        !file_exists("/lib/libbroken.so.0.4") ||
        !file_exists("/bin/libtest") ||
        !file_exists("/bin/interptest") ||
        !file_exists("/bin/pietest") ||
        !file_exists("/bin/handletest") ||
        !file_exists("/bin/semaphoretest") ||
        !file_exists("/bin/cliptest") ||
        !file_exists("/bin/seltest") ||
        !file_exists("/bin/mmaptest") ||
        !file_exists("/bin/heaptest") ||
        !file_exists("/bin/idtest") ||
        !file_exists("/bin/sh") ||
        !file_exists("/bin/ls") ||
        !file_exists("/bin/cat") ||
        !file_exists("/bin/echo") ||
        !file_exists("/bin/mkdir") ||
        !file_exists("/bin/rm") ||
        !file_exists("/bin/mv") ||
        !file_exists("/bin/cp") ||
        !file_exists("/bin/ps") ||
        !file_exists("/bin/gputest") ||
        !file_exists("/bin/audiotest")) {
        puts_out("SMOKE FAIL missing binaries in /bin\n");
        return 1;
    }

    if (!run_and_expect("/bin/forktest", forktest_argv, 1, 0) ||
        /* cwd, herencia por fork y conservacion por exec (isolation slice B). */
        !run_and_expect("/bin/cwdtest", cwdtest_argv, 1, 0) ||
        /* Identities and the VFS permission check (USERS_AND_PERMISSIONS.md, phase 2). */
        !run_and_expect("/bin/idtest", idtest_argv, 1, 0) ||
        !run_and_expect("/bin/smptest", smptest_argv, 1, 0) ||
        !run_and_expect("/bin/polltest", polltest_argv, 1, 0) ||
        !run_and_expect("/bin/sigtest", sigtest_argv, 1, 0) ||
        !run_and_expect("/bin/eventtest", eventtest_argv, 1, 0) ||
        !run_and_expect("/bin/timertest", timertest_argv, 1, 0) ||
        !run_and_expect("/bin/sectiontest", sectiontest_argv, 1, 0) ||
        !run_and_expect("/bin/ldtest", ldtest_argv, 1, 0) ||
        !run_and_expect("/bin/libtest", libtest_argv, 1, 0) ||
        !run_and_expect("/bin/interptest", interptest_argv, 1, 0) ||
        /* El diagnostico del enlazador. Que la carga falle ya lo hacia; que el
         * mensaje diga el simbolo es lo que se comprueba. */
        !run_and_expect("/bin/brokentest", brokentest_argv, 1, 0) ||
        /* Los otros tres fallos que el enlazador tiene que saber explicar:
         * un archivo que no esta, una dependencia compartida que se trae dos
         * veces, y el limite de librerias. */
        !run_and_expect("/bin/missingtest", missingtest_argv, 1, 0) ||
        !run_and_expect("/bin/diamondtest", diamondtest_argv, 1, 0) ||
        /* dlopen: per-handle scopes, dladdr, and the rollback of a failed load. */
        !run_and_expect("/bin/dltest", dltest_argv, 1, 0) ||
        !run_and_expect("/bin/slottest", slottest_argv, 1, 0) ||
        /* Con la libreria en su sitio: resuelve y finds todo. */
        !run_and_expect("/bin/needstest", needstest_argv, 1, 0) ||
        !run_and_expect("/bin/pietest", pietest_argv, 1, 0) ||
        !run_and_expect("/bin/handletest", handletest_argv, 1, 0) ||
        !run_and_expect("/bin/semaphoretest", semaphoretest_argv, 1, 0) ||
        !run_and_expect("/bin/cliptest", cliptest_argv, 1, 0) ||
        !run_and_expect("/bin/seltest", seltest_argv, 1, 0) ||
        !run_and_expect("/bin/mmaptest", mmaptest_argv, 1, 0) ||
        !run_and_expect("/bin/heaptest", heaptest_argv, 1, 0) ||
        !prepare_smoke_directory() ||
        !run_and_expect("/bin/sh", sh_argv, 3, 0) ||
        !validate_file_contains("/smoke/sh.txt", "shell-redirect") ||
        !run_to_file("/bin/sh", which_argv, 3, "/smoke/which.txt") ||
        !validate_file_contains("/smoke/which.txt", "/bin/console.sxe") ||
        !run_and_expect("/bin/echo", echo_argv, 2, 0) ||
        !run_and_expect("/bin/cat", cat_argv, 2, 0) ||
        !run_and_expect("/bin/ls", ls_argv, 2, 0) ||
        !run_and_expect("/bin/cp", cp_argv, 3, 0) ||
        !validate_file_contains("/smoke/readme.copy", "SavanXP") ||
        !run_and_expect("/bin/mv", mv_argv, 3, 0) ||
        !file_exists("/smoke/readme.moved") ||
        !run_and_expect("/bin/rm", rm_argv, 2, 0) ||
        !file_missing("/smoke/readme.moved") ||
        !run_and_expect("/bin/mkdir", mkdir_tree_argv, 3, 0) ||
        !run_and_expect("/bin/cp", cp_into_tree_argv, 3, 0) ||
        !run_and_expect("/bin/cp", cp_tree_argv, 4, 0) ||
        !validate_file_contains("/smoke/tree2/sub/README", "SavanXP") ||
        !run_and_expect("/bin/rm", rm_tree_argv, 4, 0) ||
        !file_missing("/smoke/tree") ||
        !file_missing("/smoke/tree2") ||
        !run_and_expect("/bin/ps", ps_argv, 1, 0) ||
        !run_and_expect("/bin/libctest", libctest_argv, 1, 0) ||
        !run_and_expect("/bin/stacktest", stacktest_argv, 1, 0) ||
        !run_and_expect("/rescue/bin/imagetest", imagetest_argv, 1, 0) ||
        !run_and_expect("/bin/imagetest", imagetest_disk_argv, 1, 0) ||
        !run_and_expect("/bin/gputest", gputest_argv, 2, 0) ||
        !run_and_expect("/bin/audiotest", audiotest_argv, 2, 0) ||
        !run_and_expect("/bin/fscheck", fscheck_argv, 0, 0)) {
        puts_out("SMOKE FAIL\n");
        return 1;
    }

    cleanup_smoke_directory();
    savanxp_sync();
    sleep_ms(250);
    puts_out("SMOKE PASS\n");
    return 0;
}
