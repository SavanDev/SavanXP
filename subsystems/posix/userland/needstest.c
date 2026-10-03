/* Lo que hace un programa cuando le falta una libreria.
 *
 * Este es el caso que decide como va a funcionar el reproductor cuando FFmpeg pase
 * a ser libreria, asi que las dos cosas que se comprueban aqui son la misma
 * pregunta: si el arranque del programa depende de que la libreria este.
 *
 * La primera es que ldso_missing() diga CUAL falta. El codigo de paso dice donde
 * fallo, y donde fallo es "en algun punto de la cadena", que no ayuda a nadie: el
 * nombre es lo que se puede decir en una ventana.
 *
 * La segunda es que main se ejecute. El cargador no es fatal --sx_start_dynamic
 * imprime el fallo y vuelve, y crt0 llama a main igual-- asi que un programa con un
 * DT_NEEDED irresoluble arranca. Eso es lo que permite abrir una ventana y explicar
 * el problema, en vez de desaparecer sin dejar rastro. Y es una propiedad que no
 * estaba probada: si alguien la cambia por un abort(), esto falla.
 *
 * Se ejecuta con la libreria BORRADA del volumen, que es lo que hace el escenario.
 * Con la libreria presente el mismo programa tiene que encontrar todo en su sitio,
 * y eso lo comprueba la mitad de abajo. */
#include "libc.h"
#include "ldso.h"

/* NO se define needed_answer aqui a proposito. Una version anterior de este
 * archivo lo definiia, y la comprobacion de abajo --"con la libreria ausente el
 * simbolo no se puede resolver"-- era tautologica: el ejecutable lo tenia siempre.
 * El simbolo tiene que existir SOLO en la libreria, o el test no comprueba nada. */

int main(void)
{
    const char* missing = ldso_missing();
    if (missing == 0) {
        /* Con la libreria en su sitio esto es lo que tiene que pasar. */
        if (ldso_lookup("needed_answer") == 0) {
            eprintf("needstest: la libreria cargo pero needed_answer no se resolvio\n");
            return 1;
        }
        return 0;
    }

    /* Sin ella: el nombre tiene que ser el de la que falta, no el de la que la
     * pidio. */
    if (strcmp(missing, "libneeded.so.0.4") != 0) {
        eprintf("needstest: ldso_missing() dio \"%s\", se esperaba libneeded.so.0.4\n",
                missing);
        return 1;
    }
    /* Y no se puede seguir usando: si la aplicacion llama a una funcion de la
     * libreria que no cargo, la entrada del GOT sigue en cero. */
    if (ldso_lookup("needed_answer") != 0) {
        eprintf("needstest: needed_answer se resolvio con la libreria ausente\n");
        return 1;
    }
    puts_fd(1, "NEEDSTEST SURVIVED\n");
    return 0;
}