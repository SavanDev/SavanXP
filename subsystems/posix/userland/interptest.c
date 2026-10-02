/* Comprueba que la ruta del interprete (PT_INTERP) llega desde el kernel hasta
 * crt0 y sobrevive hasta que un programa la lee.
 *
 * Este binario esta enlazado con --dynamic-linker, asi que declara un PT_INTERP
 * de verdad, y con el perfil PIE, asi que trae el cargador enlazado: crt0 corre
 * el interprete antes de main y la ruta queda en el cargador.
 *
 * Lo que se prueba aca es que la ruta llega desde el kernel, pasa por crt0 sin
 * morir, llega al cargador, y sobrevive hasta que un programa la lee. Antes de que
 * la ruta viviera en el cargador esto solo comprobaba los dos primeros tramos, y
 * lo que importaba --que llegue hasta quien la usa-- no estaba comprobado por
 * nada. */

#include "libc.h"
#include "ldso.h"

int main(void) {
    /* La ruta la guarda el cargador, no libc. crt0 se la pasa antes de main y
     * queda disponible para consultarla desde aca. */
    const char* path = ldso_interpreter_path();
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
    /* Y que el interprete CORRIO. La ruta se guarda antes de intentar enlazar, asi
     * que este programa pasaria con el cargador roto: encontraria la cadena y no
     * sabria que despues fallo todo. La ruta es el primer eslabon, no el unico. */
    if (!ldso_loaded()) {
        eprintf("interptest: llego la ruta pero no hay ninguna libreria cargada\n");
        return 1;
    }
    return 0;
}
