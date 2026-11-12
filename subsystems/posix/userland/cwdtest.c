#include "libc.h"

/*
 * The current directory: what isolation slice B moved out of the kernel's
 * Process into the state the POSIX subsystem declares, and VFS-B made a held
 * vnode instead of a path string.
 *
 * It checks the initial directory, chdir/getcwd, relative resolution, fork
 * inheritance and exec preservation -- the two things moving state tends to
 * break quietly -- and, since the directory is held, that it follows a rename
 * of itself and cannot be removed while the process stands in it.
 *
 *     cwdtest            the parent: checks everything and runs the children
 *     cwdtest child      a fork: the child must inherit the directory
 *     cwdtest exec       an exec: the directory is kept (exec does not change it)
 */

#define CWDTEST_EXPECTED_PARENT "/bin"
#define CWDTEST_EXPECTED_CHILD "/bin"

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (ok) {
        return;
    }
    printf("CWDTEST FAIL %s\n", what);
    g_failures += 1;
}

static void expect_cwd(const char* expected, const char* what) {
    char buffer[512] = {};
    long result = savanxp_getcwd(buffer, sizeof(buffer));
    check(result >= 0, what);
    check(strcmp(buffer, expected) == 0, what);
    if (strcmp(buffer, expected) != 0) {
        printf("CWDTEST FAIL %s: esperaba '%s', dio '%s'\n", what, expected, buffer);
    }
}

static int run_child(void) {
    expect_cwd(CWDTEST_EXPECTED_CHILD, "el fork hereda el cwd del padre");
    return g_failures == 0 ? 0 : 1;
}

static int run_exec_child(void) {
    /* Llega por exec: el cwd tiene que seguir siendo el del proceso que
     * ejecuto, porque exec no cambia el directorio actual. */
    expect_cwd(CWDTEST_EXPECTED_CHILD, "exec conserva el cwd");
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* const exec_argv[] = {"/bin/cwdtest", "exec", 0};
    long fd = 0;
    int status = -1;

    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        return run_child();
    }
    if (argc > 1 && strcmp(argv[1], "exec") == 0) {
        return run_exec_child();
    }

    /* 1. Arranca en la raiz. */
    expect_cwd("/", "el cwd inicial es /");

    /* 2. chdir mueve, getcwd lee. */
    check(savanxp_chdir(CWDTEST_EXPECTED_PARENT) == 0, "chdir a un directorio existente");
    expect_cwd(CWDTEST_EXPECTED_PARENT, "getcwd tras chdir");

    /* 3. Relative resolution uses the cwd: this is exactly what the kernel used
     * to resolve against its cwd field and now resolves against POSIX's state.
     * If the state did not arrive, "progman" would be relative to "/" and
     * would not open. */
    fd = savanxp_open("progman");
    check(fd >= 0, "una ruta relativa se resuelve contra el cwd");
    if (fd >= 0) {
        savanxp_close((int)fd);
    }

    /* 4. Errores que deben seguir siendo errores. */
    check(savanxp_chdir("/no_existe") < 0, "chdir to a missing directory fails");
    check(savanxp_chdir("/bin/progman") < 0, "chdir to a file fails");

    /* 4b. The current directory is held (VFS-B): it follows a rename of
     * itself, cannot be removed while a process stands in it, and can once
     * the process leaves. */
    (void)savanxp_rmdir("/tmp/cwdtest-a");
    (void)savanxp_rmdir("/tmp/cwdtest-b");
    check(savanxp_mkdir("/tmp/cwdtest-a") == 0, "mkdir a directory to stand in");
    check(savanxp_chdir("/tmp/cwdtest-a") == 0, "chdir into it");
    check(savanxp_rmdir("/tmp/cwdtest-a") < 0, "rmdir of the current directory refuses");
    check(savanxp_rename("/tmp/cwdtest-a", "/tmp/cwdtest-b") == 0, "rename of the current directory");
    expect_cwd("/tmp/cwdtest-b", "getcwd follows the rename");
    fd = savanxp_open_mode("here.txt", SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE);
    check(fd >= 0, "a relative path resolves in the renamed directory");
    if (fd >= 0) {
        savanxp_close((int)fd);
    }
    check(savanxp_unlink("/tmp/cwdtest-b/here.txt") == 0, "the file is where the directory now is");
    check(savanxp_chdir(CWDTEST_EXPECTED_PARENT) == 0, "chdir back");
    check(savanxp_rmdir("/tmp/cwdtest-b") == 0, "rmdir once nobody stands in it");

    /* 5. fork hereda el cwd. */
    {
        long pid = savanxp_fork();
        check(pid >= 0, "fork");
        if (pid > 0) {
            check(savanxp_waitpid((int)pid, &status) >= 0, "waitpid del fork");
            check(status == 0, "el hijo por fork vio el cwd heredado");
        }
        else if (pid == 0) {
            /* En el hijo no se vuelve: el padre sigue con el exec. */
            exit(run_child());
        }
    }

    /* 6. exec keeps the cwd: this same binary runs again, and on coming back
     * in it checks it is still in /bin. */
    check(exec("/bin/cwdtest", exec_argv, 2) == 0, "exec of itself");

    puts_out(g_failures == 0 ? "CWDTEST PASS\n" : "CWDTEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}