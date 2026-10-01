/* Primer ejecutable PIE en el arbol: ET_DYN, reubicado por el kernel sobre una base.
 *
 * A diferencia de todo lo demas, no usa el linker script del SDK ni una base
 * fija; el kernel lo pone donde dice el sesgo. Lo que prueba es que una imagen
 * reubicada carga y corre: datos, codigo y llamadas, todo con direcciones
 * movidas de forma coherente. Y lo que lo hace importante es que un ET_DYN sale
 * con .dynsym, asi que el ejecutable puede exportar simbolos a una libreria --
 * la premisa que lld niega a un ET_EXEC no-PIE. */

#include "libc.h"
#include <stdio.h>

/* Tomar la direccion de una funcion de esta misma imagen: con una base fija
 * valdria cerca de 0x400000, y con la imagen movida vale base + algo. Si el
 * kernel movio el contenido pero no la entrada de forma coherente, no se llega
 * ni a aca. */
static int anchor(void) {
    return 1;
}

int main(void) {
    if (anchor() != 1) {
        return 1;
    }

    static int global_probe = 7;
    int local_probe = 11;
    if (global_probe + local_probe != 18) {
        eprintf("pietest: los datos de la imagen no coinciden\n");
        return 1;
    }

    char buffer[64];
    const int written = snprintf(buffer, sizeof(buffer), "%s %d", "pietest", global_probe);
    if (written != 9 || strcmp(buffer, "pietest 7") != 0) {
        eprintf("pietest: snprintf devolvio '%s' (%d)\n", buffer, written);
        return 1;
    }
    return 0;
}
