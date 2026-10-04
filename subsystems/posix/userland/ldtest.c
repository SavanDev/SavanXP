/* Prueba del cargador: mapea libmath.so.0.4, lo reubica y llama funciones de
 * verdad a traves de punteros resueltos por nombre.
 *
 * Lo que prueba de verdad es la ultima parte: que el codigo corra desde una
 * seccion respaldada por el archivo y que sus datos reubicados esten donde el
 * codigo los busca. Un .so con GOT no arranca si eso esta mal. */

#include "libc.h"
#include <savanxp/ldso.h>

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

/* El canario esta vivo de verdad, no solo resuelto.
 *
 * libmath se compila con -fstack-protector-strong, asi que referencia
 * __stack_chk_guard, que vive en el ejecutable y lo inicializa crt0. Que la
 * reubicacion encuentre la variable no dice que tenga un valor: si la
 * reubicacion apuntara a cualquier lado y las dos mitades de la comparacion
 * coincidieran por casualidad, la proteccion no detectaria nada.
 *
 * Se comprueba el VALOR a traves de la direccion que devolvio la resolucion, y
 * ademas que las dos mitades sean distintas: crt0 siembra el canario con dos
 * registros distintos justamente para que un smashed stack de 8 bytes no lo
 * alcance. */
static int check_canary(void) {
    const unsigned long* guard = (const unsigned long*)ldso_lookup("__stack_chk_guard");
    if (guard == 0) {
        eprintf("ldtest: __stack_chk_guard no se resolvio contra el ejecutable\n");
        return 0;
    }
    if (guard[0] == 0) {
        eprintf("ldtest: el canario es cero, crt0 no lo inicializo\n");
        return 0;
    }
    if (guard[0] == guard[1]) {
        eprintf("ldtest: las dos mitades del canario son iguales\n");
        return 0;
    }
    return 1;
}

/* Definido acá, no en una libreria. libchaintop lo llama sin declararlo
 * ejecutable de otra manera: para resolverlo, el ejecutable tiene que estar en
 * el ambito de simbolos del cargador, con su .dynsym leida. */
int exe_answer(void) {
    return 100;
}

/* La cadena: libchaintop declara DT_NEEDED libchainbase y llama a chain_base.
 *
 * Probar que chain_top da lo correcto exige las tres piezas a la vez: que el
 * cargador trova la dependencia por nombre, que la mapea, y que el GOT de
 * chain_top se relleno con la direccion de chain_base ya corrida. Si cualquiera
 * de las tres falta, la llamada vuelve con basura o no vuelve.
 *
 * chain_base vive en la dependencia, asi que buscarla desde el ejecutable es
 * tambien la prueba de que el ambito de simbolos abarca la cadena y no solo la
 * libreria que se pidio. */
static int check_chain(void) {
    const int why = ldso_load("/disk/lib/libchaintop.so.0.4");
    if (why != 0) {
        eprintf("ldtest: no se cargo libchaintop.so.0.4 (paso %d)\n", -why);
        return 0;
    }
    typedef int (*chain_fn)(int);

    chain_fn top = (chain_fn)ldso_lookup("chain_top");
    if (top == 0) {
        eprintf("ldtest: chain_top no se encontro\n");
        return 0;
    }
    /* 20 -> chain_base(20) = 21 -> 21 * 2 = 42, mas exe_answer() = 100. */
    if (top(20) != 142) {
        eprintf("ldtest: chain_top(20) dio %d, se esperaba 142\n", top(20));
        return 0;
    }

    /* chain_base vive en la dependencia, asi que resolverla desde el
     * ejecutable tambien prueba que el ambito abarca la cadena. */
    chain_fn base = (chain_fn)ldso_lookup("chain_base");
    if (base == 0) {
        eprintf("ldtest: chain_base no se encontro, la cadena no se cargo\n");
        return 0;
    }
    if (base(1) != 2) {
        eprintf("ldtest: chain_base(1) dio %d, se esperaba 2\n", base(1));
        return 0;
    }
    return 1;
}

int main(void) {
    if (!ldso_loaded()) {
        const int why = ldso_load("/disk/lib/libmath.so.0.4");
        if (why != 0) {
            return (why == -7) ? (70 + g_lib_reloc_step) : (80 - why);
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

    if (ldso_lookup("no_existe_este_simbolo") != 0) {
        eprintf("ldtest: un simbolo inexistente devolvio una direccion\n");
        return 1;
    }
    if (!check_canary()) {
        return 1;
    }
    return check_chain() ? 0 : 1;
}
