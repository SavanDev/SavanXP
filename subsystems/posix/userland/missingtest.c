/* Que cargar una libreria que no esta diga el nombre del archivo.
 *
 * El caso mas comun de todos y el que mas cuesta adivinar sin mensaje: un
 * DT_NEEDED que no esta en el volumen. El sintoma sin diagnostico es un codigo de
 * paso, y un codigo de paso no dice que archivo falta.
 *
 * El archivo que se pide no existe y no va a existir nunca: no es que este mal
 * puesto en el build, es que el nombre esta bien escrito y el volumen no lo tiene. */
#include "libc.h"
#include <savanxp/ldso.h>

int main(void)
{
    const int before = ldso_count();

    const int why = ldso_load("/disk/lib/libnothere-at-all.so.0.4");
    if (why == 0) {
        eprintf("missingtest: cargo una libreria que no existe\n");
        return 1;
    }
    /* -2 es el paso de savanxp_open. Si fuera -1 seria el cupo lleno, que es otro
     * problema con otro arreglo. */
    if (why != -2) {
        eprintf("missingtest: fallo en el paso %d, y el de un archivo ausente es -2\n", why);
        return 1;
    }
    /* Y que no haya dejado un hueco a medias: un fallo tiene que dejar la tabla como
     * estaba, o las 31 cargas siguientes fallan por un motivo distinto. */
    if (ldso_count() != before) {
        eprintf("missingtest: el fallo dejo %d imagenes, habia %d\n", ldso_count(), before);
        return 1;
    }
    /* Y que el archivo que se pidio ahora exista de verdad en el volumen. Sin esto
     * la prueba pasaria por un motivo equivocado. */
    if (savanxp_open("/disk/lib/libnothere-at-all.so.0.4") >= 0) {
        eprintf("missingtest: el archivo ausente si se puede abrir\n");
        return 1;
    }
    return 0;
}