/*
 * Harness del lector de recursos SXE (fase 1 de docs/SXE_FORMAT.md).
 *
 * Todo el peso esta en sxe_selftest(), dentro del runtime del SDK: este
 * binario solo lo corre y traduce el resultado a los tokens que busca
 * build.ps1 sxe-smoke.
 */

#include "savanxp/libc.h"
#include "savanxp/sxe.h"

int main(int argc, char **argv)
{
    int failures = 0;

    (void)argc;
    (void)argv;

    failures = sxe_selftest();
    if (failures != 0)
    {
        printf("SXE SMOKE FAIL %d checks\n", failures);
        return 1;
    }

    printf("SXE SMOKE PASS\n");
    return 0;
}
