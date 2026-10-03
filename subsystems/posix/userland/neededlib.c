/* Una libreria inocua, para probar que se puede pedir.
 *
 * Solo define una funcion. No esta rota ni es Minimal a proposito: el caso
 * interesante no es una libreria que falle al cargar, sino una que NO esta en el
 * volumen, y eso lo provoca el escenario quitando el archivo de la imagen. */
#include "libc.h"

int needed_answer(void);

int needed_answer(void)
{
    return 42;
}
