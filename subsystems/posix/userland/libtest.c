/* Un programa que linkea contra libmath.so.0.4 de verdad, y no contra una
 * copia de math.c.
 *
 * Es la diferencia entre "el cargador funciona" y "el cargador sirve para algo".
 * Todo lo anterior se probaba llamando al cargador a mano: here main() lo llama
 * una vez y despues usa sqrt como si fuera una funcion normal, sin buscar su
 * direccion en ningun lado. Si el GOT no quedo bien rellenado, la llamada sale
 * por el PLT a una entrada vacia y no vuelve.
 *
 * La unica diferencia de build con el resto del arbol es que el runtime de este
 * objetivo no trae math.c. Con el runtime completo, sqrt estaria definida aca y
 * el enlazador nunca emitiria un DT_NEEDED. */

#include "libc.h"
#include <savanxp/ldso.h>
#include "math.h"

/* A pointer into the executable exercises R_386_RELATIVE with its implicit
 * addend. The sqrt call exercises R_386_JUMP_SLOT by symbol name. */
extern void* stdout;

static int check_relocated_pointer(void) {
    const unsigned long here = (unsigned long)(void*)&check_relocated_pointer;
    const unsigned long there = (unsigned long)stdout;
    if (there < here || (there - here) > 0x400000UL) {
        eprintf("libtest: stdout=%lu no cae en la imagen (esta en %lu)\n", there, here);
        return 0;
    }
    return 1;
}

int main(void) {
    /* Nada de llamar al cargador aca. crt0 ya lo corrio antes de llegar a
     * main, porque este binario enlaza el cargador y crt0 lo encuentra por el
     * hook debil. Si el GOT no estuviera relleno, la llamada de abajo sale por
     * el PLT a una entrada vacia. Que esta prueba no llame a ldso_start es lo
     * que la convierte en prueba. */

    /* sqrt(144) tiene que dar 12. */
    if (!check_relocated_pointer()) {
        return 1;
    }
    const double got = sqrt(144.0);
    if (got < 11.999 || got > 12.001) {
        eprintf("libtest: sqrt(144) dio %f\n", got);
        return 1;
    }

    /* La direccion que el resolvedor reporta y la que el GOT tiene que ser la
     * misma. Si difieren, la llamada de arriba no fue a donde el cargador dice,
     * y que haya dado 12 seria casualidad. */
    typedef double (*math_fn)(double);
    const math_fn reported = (math_fn)ldso_lookup("sqrt");
    if (reported == 0) {
        eprintf("libtest: sqrt no aparece en ninguna libreria cargada\n");
        return 1;
    }
    if ((const void*)reported != (const void*)sqrt) {
        eprintf("libtest: el resolvedor y el GOT apuntan a distinto lado\n");
        return 1;
    }
    return 0;
}