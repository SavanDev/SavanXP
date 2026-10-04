/* Que el limite de librerias se respete y se diga por que.
 *
 * Este programa se compila con SAVANXP_LD_MAX_LIBRARIES=4, porque probar el limite
 * de 32 exigiria 33 libreras de prueba en el volumen, y lo que se quiere probar es
 * el CAMINO, no el numero. Es el mismo ldso.c con otra constante: ldso.c se
 * compila dentro de cada programa, as que un -D alcanza.
 *
 * El limite incluye el ejecutable, que ocupa el hueco 0, asi que con 4 hay hueco
 * para tres librerias y la cuarta tiene que ser rechazada con -1.
 *
 * El mensaje importa tanto como el codigo: "no cabe una mas" y "el archivo no
 * esta" son -1 y -2, y son problemas distintos. Uno se arregla encogiendo la
 * cadena, el otro poniendo el archivo. */
#include "libc.h"
#include <savanxp/ldso.h>

/* libchaintop referencia exe_answer, que el ejecutable tiene que definir. Aqui solo
 * se carga por su cuenta de dependencias --lo que importa es que ocupa un hueco y
 * arrastra libchainbase--, pero el enlazador exige que el simbolo exista. */
int exe_answer(void)
{
    return 100;
}

int main(void)
{
    /* Ejecutable + tres librerias: el tope. */
    static const char* const fills[] = {
        "/disk/lib/libmath.so.0.4",
        "/disk/lib/libchainbase.so.0.4",
        "/disk/lib/libchaintop.so.0.4",
    };
    for (unsigned i = 0; i < sizeof(fills) / sizeof(fills[0]); ++i) {
        if (ldso_load(fills[i]) != 0) {
            eprintf("slottest: no se cargo %s con el tope en %d\n", fills[i], ldso_count());
            return 1;
        }
    }
    const int full = ldso_count();
    if (full != 4) {
        eprintf("slottest: se esperaban 4 imagenes con el tope, hay %d\n", full);
        return 1;
    }

    /* La cuarta tiene que ser rechazada, y con -1: el cupo lleno. */
    const int why = ldso_load("/disk/lib/libsxgfx.so.0.4");
    if (why != -1) {
        eprintf("slottest: la cuarta dio %d, y el cupo lleno es -1\n", why);
        return 1;
    }
    /* Rechazada significa rechazada: el hueco no se consume. */
    if (ldso_count() != full) {
        eprintf("slottest: el rechazo dejo %d imagenes, habia %d\n", ldso_count(), full);
        return 1;
    }
    return 0;
}