/* Que el enlazador falle CON NOMBRE cuando una dependencia pide un simbolo que nada
 * define.
 *
 * Esto es una prueba negativa del enlazador, no de una aplicacion. La libreria
 * libbroken.so.0.4 llama a sx_missing_symbol_nobody_defines, que no existe en
 * libmath, ni en libgfx2d, ni en el ejecutable. Cargarla tiene que fallar.
 *
 * Lo que se comprueba NO es solo que falle -- eso ya lo hacia, con un -9 --, sino
 * que el cargador sepa el nombre del simbolo. Un numero de paso dice DONDE se
 * rompio; el nombre dice QUE, y con el nombre el arreglo son diez segundos.
 *
 * El nombre no se afirma sobre el mensaje impreso porque una prueba no lee la
 * salida estandar. El cargador lo guarda en un global justamente para esto. */
#include "libc.h"
#include <savanxp/ldso.h>

int main(void)
{
    const int why = ldso_load("/disk/lib/libbroken.so.0.4");
    if (why == 0) {
        eprintf("brokentest: libbroken cargo, y deberia haber fallado por el simbolo\n");
        return 1;
    }
    /* -9 es lo que ldso_load devuelve cuando apply_relocs_in falla, que es donde
     * cae un simbolo que no se puede resolver. */
    if (why != -9) {
        eprintf("brokentest: fallo por el paso equivocado (%d), no por la reubicacion\n", why);
        return 1;
    }
    if (g_lib_fail_symbol == 0) {
        eprintf("brokentest: la carga fallo pero el cargador no guardo ningun simbolo\n");
        return 1;
    }
    if (strcmp(g_lib_fail_symbol, "sx_missing_symbol_nobody_defines") != 0) {
        eprintf("brokentest: el simbolo reportado es \"%s\", no el que falta\n",
                g_lib_fail_symbol);
        return 1;
    }
    if (g_lib_fail_library == 0 ||
        strcmp(g_lib_fail_library, "libbroken.so.0.4") != 0) {
        eprintf("brokentest: la libreria reportada es \"%s\", no libbroken.so.0.4\n",
                g_lib_fail_library == 0 ? "(ninguna)" : g_lib_fail_library);
        return 1;
    }
    return 0;
}