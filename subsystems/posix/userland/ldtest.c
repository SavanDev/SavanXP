/* Prueba del cargador: mapea libmath.so.0.4, lo reubica y llama funciones de
 * verdad a traves de punteros resueltos por nombre.
 *
 * Lo que prueba de verdad es la ultima parte: que el codigo corra desde una
 * seccion respaldada por el archivo y que sus datos reubicados esten donde el
 * codigo los busca. Un .so con GOT no arranca si eso esta mal. */

#include "libc.h"
#include "ldso.h"

typedef double (*math_fn1)(double);
typedef double (*math_fn2)(double, double);

/* Raiz cuadrada de 144 tiene que dar 12. Si la reubicacion del GOT quedo mal,
 * esto no devuelve 12: llama a cualquier otra cosa o a nada. */
static int check(const char* name, math_fn1 fn, double input, double expected) {
    if (fn == 0) {
        eprintf("ldtest: %s no se encontro en la libreria\n", name);
        return 0;
    }
    const double got = fn(input);
    const double delta = got > expected ? got - expected : expected - got;
    if (delta > 0.0001) {
        eprintf("ldtest: %s(%f) dio %f, se esperaba %f\n", name, input, got, expected);
        return 0;
    }
    return 1;
}

int main(void) {
    if (!ldso_loaded()) {
        const int why = ldso_load("/disk/lib/libmath.so.0.4");
        if (why != 0) {
            eprintf("ldtest: no se cargo libmath.so.0.4 (paso %d, segmento %d)\n",
                    -why, g_lib_fail_index);
            return 1;
        }
    }

    math_fn1 sqrt_fn = (math_fn1)ldso_lookup("sqrt");
    math_fn1 fabs_fn = (math_fn1)ldso_lookup("fabs");
    math_fn1 floor_fn = (math_fn1)ldso_lookup("floor");

    if (!check("sqrt", sqrt_fn, 144.0, 12.0)) {
        return 1;
    }
    if (!check("fabs", fabs_fn, -3.5, 3.5)) {
        return 1;
    }
    if (!check("floor", floor_fn, 2.75, 2.0)) {
        return 1;
    }

    /* Un simbolo que la libreria no define tiene que dar 0, no una direccion
     * cualquiera: un lookup que "siempre funciona" esconde un .dynsym mal
     * recorrido. */
    if (ldso_lookup("no_existe_este_simbolo") != 0) {
        eprintf("ldtest: un simbolo inexistente devolvio una direccion\n");
        return 1;
    }
    return 0;
}
