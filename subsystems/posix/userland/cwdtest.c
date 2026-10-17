#include "libc.h"

/*
 * Directorio actual: la propiedad que la rebanada B de aislamiento movio de
 * sitio. Antes vivia en un campo cwd de Process, en el kernel; ahora vive en el
 * estado privado que declara el subsistema POSIX y el kernel solo lo reserva.
 *
 * El comportamiento no cambio, asi que esto no es un test nuevo: es la
 * asercion que faltaba. chdir/getcwd no los probaba nadie, y el cambio movia
 * justo la herencia por fork y la conservacion por exec, que son las dos cosas
 * que un movimiento de estado suele romper sin que se note.
 *
 *     cwdtest            el padre: comprueba todo y despacha a los hijos
 *     cwdtest child      un fork: el hijo tiene que heredar el cwd
 *     cwdtest exec       un exec: el cwd se conserva (exec no lo cambia)
 */

#define CWDTEST_EXPECTED_PARENT "/disk"
#define CWDTEST_EXPECTED_CHILD "/disk"

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
    const char* const exec_argv[] = {"/disk/bin/cwdtest", "exec", 0};
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

    /* 3. La resolucion relativa usa el cwd: esto es exactamente lo que el
     * kernel resolvia contra su campo cwd y ahora resuelve contra el estado
     * de POSIX. Si el estado no llegara, "bin/progman" seria relativo a "/". */
    fd = savanxp_open("bin/progman");
    check(fd >= 0, "una ruta relativa se resuelve contra el cwd");
    if (fd >= 0) {
        savanxp_close((int)fd);
    }

    /* 4. Errores que deben seguir siendo errores. */
    check(savanxp_chdir("/disk/no_existe") < 0, "chdir a un directorio ausente falla");
    check(savanxp_chdir("/disk/bin/progman") < 0, "chdir a un fichero falla");

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

    /* 6. exec conserva el cwd: se reejecuta este mismo binario, que al volver
     * a entrar comprueba que sigue en /disk. */
    check(exec("/disk/bin/cwdtest", exec_argv, 2) == 0, "exec de si mismo");

    puts_out(g_failures == 0 ? "CWDTEST PASS\n" : "CWDTEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}