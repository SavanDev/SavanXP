/* Que una dependencia compartida se cargue UNA vez, no una por rama.
 *
 * El diamante: top necesita left y right, y los dos necesitan leaf. El recorrido
 * de DT_NEEDED tiene un cursor, `needed_done`, justamente para no traer leaf dos
 * veces -- sin el, left y right quedarian apuntando a copias distintas de leaf, y
 * cada copia con sus propios datos relocalizados.
 *
 * Esto no se podia comprobar antes. `ldtest` prueba una cadena de dos, que es un
 * caso donde el problema no puede aparecer: en una cadena, una dependencia se
 * nombra una vez. El diamante es el caso donde el bug estaria.
 *
 * Lo que se afirma no es que las cuatro carguen --eso lo dice el codigo de
 * retorno-- sino CUANTAS imagenes quedan mapeadas. Si leaf se carga dos veces, hay
 * una imagen de mas y el numero lo delata. */
#include "libc.h"
#include <savanxp/ldso.h>

/* El ejecutable ocupa un hueco, asi que antes de cargar nada hay una imagen. */
#define EXPECTED_AFTER_TOP 5 /* ejecutable, leaf, left, right, top */

typedef int (*leaf_fn)(int);
typedef int (*top_fn)(int);

int main(void)
{
    const int before = ldso_count();
    if (before != 1) {
        eprintf("diamondtest: se esperaba 1 imagen antes de empezar, hay %d\n", before);
        return 1;
    }

    if (ldso_load("/disk/lib/libdia_top.so.0.4") != 0) {
        eprintf("diamondtest: no se cargo libdia_top.so.0.4\n");
        return 1;
    }

    const int after = ldso_count();
    if (after != EXPECTED_AFTER_TOP) {
        eprintf("diamondtest: tras cargar top hay %d imagenes y deberian ser %d\n",
                after, EXPECTED_AFTER_TOP);
        eprintf("diamondtest: %d significa que leaf se cargo mas de una vez\n",
                after - EXPECTED_AFTER_TOP);
        return 1;
    }

    /* Y que las dos ramas seeing la MISMA leaf, no dos. Si el dedup falla, esto
     * sigue funcionando --cada rama tiene la suya-- asi que el conteo es la
     * afirmacion; esto solo confirma que la cadena se resolvio entera. */
    top_fn top = (top_fn)ldso_lookup("dia_top");
    leaf_fn leaf = (leaf_fn)ldso_lookup("dia_leaf");
    if (top == 0 || leaf == 0) {
        eprintf("diamondtest: no se resolvio dia_top o dia_leaf\n");
        return 1;
    }
    /* dia_leaf(1) = 2; dia_left(1) = 4; dia_right(1) = 6; dia_top(1) = 10. */
    if (top(1) != 10) {
        eprintf("diamondtest: dia_top(1) dio %d, se esperaba 10\n", top(1));
        return 1;
    }
    /* Un token de EXITO, y no solo de error. needstest lleva el suyo desde el
     * principio y por eso hay un escenario que lo ejecuta; estos cuatro se
     * compilaron, entraron en la imagen y no los ejecuto nadie nunca, porque un
     * programa que solo habla cuando algo va mal no permite comprobar que algo
     * va bien. Un token de exito es lo que convierte una prueba en algo que se
     * puede automatizar. */
    puts_fd(1, "DIAMONDTEST ONCED\n");
    return 0;
}