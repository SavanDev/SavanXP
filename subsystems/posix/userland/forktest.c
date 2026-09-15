#include "libc.h"

int main(void) {
    puts_out("forktest: start\n");

    /* El hijo hereda la FPU, no solo la memoria. El valor se deja en un registro
     * xmm -- que es lo unico que el fork tiene que copiar aparte del espacio de
     * direcciones -- y se lee del otro lado; leerlo de una variable no probaria
     * nada, porque esa ya viaja en la copia de la memoria. Si el hijo arrancara
     * con el estado limpio que siembra el kernel, aca saldria 0. */
    const double seed = 3.5;
    double inherited = 0.0;
    __asm__ volatile("movsd %0, %%xmm5" ::"m"(seed) : "xmm5", "memory");

    long child = savanxp_fork();

    __asm__ volatile("movsd %%xmm5, %0" : "=m"(inherited)::"memory");
    if (child < 0) {
        printf("forktest: fork failed (%s)\n", result_error_string(child));
        return 1;
    }

    if (child == 0) {
        if (inherited != 3.5) {
            printf("forktest: el hijo perdio el estado FPU (%f)\n", inherited);
            exit(43);
        }
        puts_out("forktest: child\n");
        exit(42);
    }

    int status = -1;
    if (savanxp_waitpid((int)child, &status) < 0) {
        puts_out("forktest: waitpid failed\n");
        return 1;
    }
    if (status != 42) {
        printf("forktest: expected 42 got %d\n", status);
        return 1;
    }

    printf("forktest: ok child=%d status=%d\n", (int)child, status);
    return 0;
}
