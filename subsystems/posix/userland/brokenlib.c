/* Libreria con una dependencia que nadie define.
 *
 * Existe para probar el diagnostico del enlazador: llama a una funcion que no esta
 * en libmath, en libgfx2d, ni en el ejecutable. El enlazador tiene que fallar al
 * cargarla Y decir cual es el simbolo, no solo "paso 5".
 *
 * Sin esto, un simbolo mal escrito en una dependencia nueva se manifestaba como un
 * "paso 5" sin nombre, que no dice nada hasta que uno abre un volcado de la
 * .dynsym a mano. */
#include "libc.h"

extern int sx_missing_symbol_nobody_defines(int value);

int broken_missing(void)
{
    /* No existe en ninguna imagen. El nombre es unico a proposito: si el
     * diagnostico lo imprimiera, no puede ser algo que ya estuviera puesto. */
    return sx_missing_symbol_nobody_defines(7);
}
