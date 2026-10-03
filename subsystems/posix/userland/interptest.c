/* Comprueba que la ruta del interprete (PT_INTERP) llega desde el kernel hasta
 * crt0 y sobrevive hasta que un programa la lee.
 *
 * Este binario esta enlazado con --dynamic-linker, asi que declara un PT_INTERP
 * de verdad. Todavia NO arranca el interprete: el kernel sigue cargando la
 * imagen entera y crt0 va derecho a main. Lo que se prueba aca es el primer
 * eslabon -- que la ruta llegue -- que es lo que despues habilita todo lo demas. */

#include "libc.h"

int main(void) {
    const char* path = savanxp_interpreter_path();
    if (path == 0) {
        eprintf("interptest: no llego la ruta del interprete\n");
        return 1;
    }
    if (strcmp(path, "/disk/lib/ld.so.0.4") != 0) {
        eprintf("interptest: llego '%s', se esperaba '/disk/lib/ld.so.0.4'\n", path);
        return 1;
    }
    /* Tiene que estar terminada en NUL dentro de la cadena: el kernel copia
     * p_filesz bytes y trusts que eso incluye el terminador. */
    size_t length = 0;
    while (path[length] != '\0') {
        length++;
    }
    if (length == 0 || length > 512) {
        eprintf("interptest: largo de ruta raro (%lu)\n", (unsigned long)length);
        return 1;
    }
    return 0;
}
