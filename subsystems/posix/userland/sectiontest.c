#include "libc.h"

/* Codigo de maquina minimo y posicion independiente, escrito a mano para que el
 * test no dependa de como compilo el compilador una funcion equivalente:
 *
 *   b8 3c 00 00 00   mov eax, 60
 *   c3               ret
 *
 * No toca la memoria ni recibe nada, asi que devuelve lo mismo desde
 * cualquier pagina en la que se lo copie. Es lo que se ejecuta abajo: el dato
 * que importa no es que valga 60, es que una pagina mapeada R+X llegue a
 * ejecutar y no a pegarse un #PF en el primer fetch. */
static const unsigned char kExecProbe[] = { 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xc3 };
#define kExecProbeResult 60L

/* Hijos que saturan la tabla global de secciones a la vez. Cada uno topa con su
 * propio limite de vistas (64), asi que 8 piden hasta 512 y la tabla se llena
 * antes de que el ultimo termine. */
#define kFillerChildren 8

/* Ranuras de la pagina de barrera, en este orden:
 *   [0, kFillerChildren)                    cuentas por hijo
 *   [kFillerChildren, kTallyRelease)        aviso por hijo, uno cada uno
 *   kTallyRelease                           bandera de salida, la escribe el padre */
#define kTallyRelease (kFillerChildren * 2)
#define kTallySlots (kTallyRelease + 1)

/* Intentos de barrera, 1 ms cada uno: ~20 s, de sobra para ocho hijos que solo
 * tienen que reservar secciones de 4 KiB. */
#define kBarrierAttempts 20000

/* Crea secciones hasta que el sistema las niegue, y devuelve cuantas logro
 * dejar vivas. La vista es lo que las mantiene vivas: cerrar el handle no las
 * destruye mientras esten mapeadas en algun lado.
 *
 * El handle se cierra en cada vuelta justamente para no acumular descriptores:
 * el tope de fds por proceso es 64 y el de vistas tambien, asi que sin esto el
 * hijo toparia antes por descriptor que por tabla global. */
static unsigned long fill_sections_until_full(void) {
    unsigned long count = 0;

    for (;;) {
        const long section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (section < 0) {
            break;
        }
        void* view = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        savanxp_close((int)section);
        if (view == 0 || result_is_error((long)view)) {
            break;
        }
        /* Cada vista vive hasta que el proceso termina, asi que count es
         * tambien cuantas secciones quedan vivas cuando se returns. */
        count++;
    }

    return count;
}

/* Los dos bloques de abajo se escribieron antes que estos helpers; se declaran
 * aqui para que el orden del archivo no los obligue a ir al final. */
static int expect_success(long result, const char* label);
static int expect_pointer(void* value, const char* label);
static int expect_refused(void* value, const char* label);

/* Abre un archivo que el smoke garantiza presente, para usar de respaldo de
 * prueba. No importa cual: el test compara el contenido de la seccion contra
 * lo que read() trae del mismo descriptor, asi que le sirve cualquiera. */
static long open_known_file(void) {
    static const char* const paths[] = {
        "/disk/bin/sectiontest",
        "/bin/sectiontest",
        "/disk/bin/true",
        "/bin/true",
    };
    for (unsigned index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        const long fd = savanxp_open(paths[index]);
        if (fd >= 0) {
            return fd;
        }
    }    return -1;
}

/* --- secciones respaldadas por archivo -----------------------------------
 *
 * Un backing de solo lectura: se abre un descriptor, la seccion se llena con el
 * contenido del archivo, y despues solo se puede leer. No hay forma de pedir
 * escritura, porque los segmentos que el cargador tiene que escribir son
 * secciones anonimas aparte. */
static int test_file_backed_section(void) {
    const long fd = open_known_file();
    if (fd < 0) {
        eprintf("sectiontest: no se encontro ningun archivo conocido para el respaldo\n");
        return 0;
    }

    /* El grant se pide al CREAR la seccion, no al mapear. Acquire lo lleva para
     * que la vista ejecutable se pueda pedir despues; una seccion creada sin
     * exec no la concede ni aunque se la pida la vista. */
    const long section = section_open((int)fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC);
    if (!expect_success(section, "open file-backed section")) {
        savanxp_close((int)fd);
        return 0;
    }

    /* Escritura sobre el respaldo se rechaza con EINVAL, no con ENOMEM: el
     * rechazo es una decision de diseno, no falta de memoria. */
    if (!expect_refused((void*)(long)section_open((int)fd, SAVANXP_SECTION_WRITE),
                        "un respaldo escribible")) {
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }

    void* runnable = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC);
    if (!expect_pointer(runnable, "map file-backed view")) {
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }

    /* El contenido tiene que ser el del archivo. Se comparan los primeros 512
     * bytes contra read() del mismo descriptor: si la seccion salio de otro lado
     * o desalineada, el primer byte difiere. */
    unsigned char from_read[512];
    const long got = savanxp_read(fd, from_read, sizeof(from_read));    if (got <= 0) {
        eprintf("sectiontest: read del archivo fallo (%s)\n", result_error_string(got));
        unmap_view(runnable);
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }
    const unsigned char* mapped = (const unsigned char*)runnable;
    int mismatch = 0;
    for (long index = 0; index < got; ++index) {
        if (mapped[index] != from_read[index]) {
            eprintf("sectiontest: byte %ld difiere (seccion %02x, archivo %02x)\n",
                    index, mapped[index], from_read[index]);
            mismatch = 1;
            break;
        }
    }
    if (mismatch) {
        unmap_view(runnable);
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }

    /* Los primeros cuatro bytes de cualquier ejecutable del sistema son el
     * ELF magic. No prueba que el codigo CORRA desde aca -- eso llega con el
     * cargador -- pero si que la pagina quedo mapeada y legible como codigo. */
    if (mapped[0] != 0x7f || mapped[1] != 0x45 || mapped[2] != 0x4c || mapped[3] != 0x46) {
        eprintf("sectiontest: la seccion respaldada no arranca con ELF magic (%02x %02x %02x %02x)\n",
                mapped[0], mapped[1], mapped[2], mapped[3]);
        unmap_view(runnable);
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }

    /* Sin cache de identidad todavia: dos aperturas del mismo archivo dan dos
     * secciones distintas. Esto es lo que va a cambiar cuando la identidad viva
     * en la tabla global, y el test va a tener que cambiar con ella. */
    const long second = section_open((int)fd, SAVANXP_SECTION_READ);
    if (!expect_success(second, "reopen file-backed section")) {
        unmap_view(runnable);
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }
    void* second_view = map_view((int)second, SAVANXP_SECTION_READ);
    if (!expect_pointer(second_view, "map second file-backed view")) {
        savanxp_close((int)second);
        unmap_view(runnable);
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }
    if (second_view == runnable) {
        eprintf("sectiontest: dos secciones del mismo archivo salieron en la misma direccion\n");
        unmap_view(second_view);
        savanxp_close((int)second);
        unmap_view(runnable);
        savanxp_close((int)section);
        savanxp_close((int)fd);
        return 0;
    }

    unmap_view(second_view);
    savanxp_close((int)second);
    unmap_view(runnable);
    savanxp_close((int)section);
    savanxp_close((int)fd);
    return 1;
}

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("sectiontest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_pointer(void* value, const char* label) {
    const long result = (long)value;
    if (value == 0 || result_is_error(result)) {
        eprintf("sectiontest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

/* map_view devuelve el resultado crudo del syscall, asi que un rechazo llega
 * como un errno negativo casteado a puntero -- no como NULL. Por eso esto
 * compara con result_is_error y no con != 0. */
static int expect_refused(void* value, const char* label) {
    const long result = (long)value;
    if (value != 0 && !result_is_error(result)) {
        eprintf("sectiontest: %s fue aceptado (%p)\n", label, value);
        return 0;
    }
    return 1;
}

int main(void) {
    long section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_success(section, "create section")) {
        return 1;
    }

    void* first_view = map_view((int)section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_pointer(first_view, "map first view")) {
        savanxp_close((int)section);
        return 1;
    }

    volatile unsigned char* shared = (volatile unsigned char*)first_view;
    shared[0] = 0x11;

    long child_ready = event_create(SAVANXP_EVENT_AUTO_RESET);
    long parent_ready = event_create(SAVANXP_EVENT_AUTO_RESET);
    if (!expect_success(child_ready, "create child_ready") ||
        !expect_success(parent_ready, "create parent_ready")) {
        unmap_view(first_view);
        savanxp_close((int)section);
        return 1;
    }

    long pid = savanxp_fork();
    if (pid < 0) {
        eprintf("sectiontest: fork failed (%s)\n", result_error_string(pid));
        savanxp_close((int)parent_ready);
        savanxp_close((int)child_ready);
        unmap_view(first_view);
        savanxp_close((int)section);
        return 1;
    }

    if (pid == 0) {
        if (shared[0] != 0x11) {
            eprintf("sectiontest: child initial byte mismatch (%u)\n", (unsigned)shared[0]);
            return 2;
        }

        shared[0] = 0x22;
        if (!expect_success(event_set((int)child_ready), "child signal ready")) {
            return 3;
        }
        if (!expect_success(wait_one((int)parent_ready, 1000), "child wait parent")) {
            return 4;
        }
        if (shared[0] != 0x33) {
            eprintf("sectiontest: child shared byte mismatch after parent write (%u)\n", (unsigned)shared[0]);
            return 5;
        }
        if (!expect_success(unmap_view((void*)shared), "child unmap view")) {
            return 6;
        }
        savanxp_close((int)parent_ready);
        savanxp_close((int)child_ready);
        savanxp_close((int)section);
        return 0;
    }

    if (!expect_success(wait_one((int)child_ready, 1000), "parent wait child")) {
        return 1;
    }
    if (shared[0] != 0x22) {
        eprintf("sectiontest: parent shared byte mismatch after child write (%u)\n", (unsigned)shared[0]);
        return 1;
    }

    void* second_view = map_view((int)section, SAVANXP_SECTION_READ);
    if (!expect_pointer(second_view, "map second view")) {
        return 1;
    }
    if (((volatile unsigned char*)second_view)[0] != 0x22) {
        eprintf("sectiontest: second view mismatch (%u)\n", (unsigned)((volatile unsigned char*)second_view)[0]);
        return 1;
    }

    shared[0] = 0x33;
    if (((volatile unsigned char*)second_view)[0] != 0x33) {
        eprintf("sectiontest: second view did not reflect parent write (%u)\n", (unsigned)((volatile unsigned char*)second_view)[0]);
        return 1;
    }
    if (!expect_success(event_set((int)parent_ready), "parent signal child")) {
        return 1;
    }

    int status = -1;
    if (savanxp_waitpid((int)pid, &status) < 0) {
        eprintf("sectiontest: waitpid failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("sectiontest: child status %d\n", status);
        return 1;
    }

    if (!expect_success(unmap_view(second_view), "parent unmap second view")) {
        return 1;
    }
    if (!expect_success(unmap_view(first_view), "parent unmap first view")) {
        return 1;
    }

    savanxp_close((int)parent_ready);
    savanxp_close((int)child_ready);
    savanxp_close((int)section);

    /* --- vistas ejecutables -------------------------------------------------
     *
     * La seccion pide los tres permisos en su GRANT, pero las vistas se piden
     * separadas: una RW donde se escriben los bytes y una R+X desde donde se
     * ejecutan. Es lo que va a hacer el cargador -- escribir la imagen en una
     * vista y ejecutarla desde otra -- y de paso muestra donde vive W^X: en la
     * VISTA, no en la seccion. Un grant que concede los tres no produce una
     * pagina escribible y ejecutable a la vez. */
    long code_section = section_create(
        4096,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE | SAVANXP_SECTION_EXEC);
    if (!expect_success(code_section, "create code section")) {
        return 1;
    }

    void* staging = map_view((int)code_section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_pointer(staging, "map staging view")) {
        savanxp_close((int)code_section);
        return 1;
    }
    memcpy(staging, kExecProbe, sizeof(kExecProbe));

    void* runnable = map_view((int)code_section, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC);
    if (!expect_pointer(runnable, "map executable view")) {
        unmap_view(staging);
        savanxp_close((int)code_section);
        return 1;
    }

    /* El codigo sale de una pagina sin permiso de escritura: sin el bit de
     * ejecucion el acceso habria sido un #PF y el proceso habria muerto con el
     * primer byte, antes de que esto llegue a comparar nada. */
    long (*probe)(void);
    memcpy(&probe, &runnable, sizeof(probe));
    if (probe() != kExecProbeResult) {
        eprintf("sectiontest: el codigo ejecutable devolvio %ld, se esperaba %ld\n", probe(), (long)kExecProbeResult);
        unmap_view(runnable);
        unmap_view(staging);
        savanxp_close((int)code_section);
        return 1;
    }

    /* W^X se rechaza, y con EINVAL y no con el ENOMEM que daria el rechazo de
     * vm::map_section_view: el error tiene que poder leerse desde el cargador. */
    if (!expect_refused(
            map_view((int)code_section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE | SAVANXP_SECTION_EXEC),
            "una vista escribible y ejecutable")) {
        unmap_view(runnable);
        unmap_view(staging);
        savanxp_close((int)code_section);
        return 1;
    }

    /* Y una seccion que no concede ejecucion no puede mapearsela, aunque el
     * proceso se la pida. */
    long plain_section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_success(plain_section, "create plain section")) {
        unmap_view(runnable);
        unmap_view(staging);
        savanxp_close((int)code_section);
        return 1;
    }
    if (!expect_refused(
            map_view((int)plain_section, SAVANXP_SECTION_READ | SAVANXP_SECTION_EXEC),
            "ejecucion que la seccion no concede")) {
        savanxp_close((int)plain_section);
        unmap_view(runnable);
        unmap_view(staging);
        savanxp_close((int)code_section);
        return 1;
    }
    savanxp_close((int)plain_section);

    /* Mapear y desmapear en ciclo no tiene que agotar los lugares de vista:
     * si unmap_view no liberara el lugar, el ultimo map_view fallaria. Y
     * tampoco en paralelo -- con 48 a la vez, este bloque se cae si el tope
     * vuelve a ser 32. */
    void* held_views[48];
    for (int round = 0; round < 48; ++round) {
        /* RW, no R+X: el objetivo aca es el tope de vistas, y una vista R+X
         * rightly no se puede escribir. */
        held_views[round] = map_view((int)code_section, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
        if (!expect_pointer(held_views[round], "map held view")) {
            for (int done = 0; done < round; ++done) {
                (void)unmap_view(held_views[done]);
            }
            unmap_view(runnable);
            unmap_view(staging);
            savanxp_close((int)code_section);
            return 1;
        }
    }
    /* Las 48 son vistas distintas de la MISMA seccion, asi que las 48 tienen
     * que ver el mismo byte: comparten la pagina fisica, no cada una la suya.
     * Se planta el marcador por una sola y se lee por las 48. */
    ((volatile unsigned char*)held_views[0])[0] = 0x5a;
    for (int round = 0; round < 48; ++round) {
        if (((volatile unsigned char*)held_views[round])[0] != 0x5a) {
            eprintf("sectiontest: la vista %d de 48 no ve la pagina compartida\n", round);
            for (int done = 0; done < 48; ++done) {
                (void)unmap_view(held_views[done]);
            }
            unmap_view(runnable);
            unmap_view(staging);
            savanxp_close((int)code_section);
            return 1;
        }
    }
    for (int round = 0; round < 48; ++round) {
        if (!expect_success(unmap_view(held_views[round]), "unmap held view")) {
            return 1;
        }
    }

    if (!expect_success(unmap_view(runnable), "unmap executable view")) {
        return 1;
    }
    if (!expect_success(unmap_view(staging), "unmap staging view")) {
        return 1;
    }
    savanxp_close((int)code_section);

    if (!test_file_backed_section()) {
        return 1;
    }

    /* --- la tabla global de secciones, llenada entre varios ---------------
     *
     * Un proceso solo nunca alcanza esa tabla: sus secciones viven mientras
     * tenga vistas mapeadas, y de ahi el tope son 64. Se llena entre varios
     * procesos, que es como se llena de verdad -- cada uno aporta sus arenas
     * y sus librerias.
     *
     * Los hijos necesitan una barrera, y por un motivo que no es obvio: sin
     * ella NO se solapan. El padre forkea sin esperar, asi que el hijo 0 puede
     * terminar y liberar sus secciones antes de que el padre siquiera forkee al
     * 7, y la suma contaria creaciones sucesivas en vez de secciones vivas al
     * mismo tiempo. Con la barrera todos las tienen vivas mientras el padre
     * mira el tally, que es lo que hay que medir.
     *
     * La barrera vive en la pagina compartida, no en eventos. Un evento auto
     * reset es binario: ocho event_set seguido se colapsan en una senal y el
     * padre se queda esperando las otras siete para siempre. Cada hijo tiene
     * ademas su propia ranura de aviso, porque un += compartido entre cores es
     * un read-modify-write que pierde actualizaciones. */
    long tally_section = section_create(4096, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_success(tally_section, "create tally section")) {
        return 1;
    }
    volatile unsigned long* tally = (volatile unsigned long*)map_view(
        (int)tally_section,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (!expect_pointer((void*)tally, "map tally view")) {
        return 1;
    }
    /* Se ponen TODAS las ranuras a cero, incluida la de salida. La pagina viene
     * de cero, pero confiar en eso para la bandera que decide si los hijos
     * esperan o no es confiar en que el test mienta. */
    for (int slot = 0; slot < kTallySlots; ++slot) {
        tally[slot] = 0;
    }

    for (int child_id = 0; child_id < kFillerChildren; ++child_id) {
        const long pid = savanxp_fork();
        if (pid < 0) {
            eprintf("sectiontest: fork del relleno %d fallo (%s)\n", child_id, result_error_string(pid));
            return 1;
        }
        if (pid != 0) {
            continue;
        }
        tally[child_id] = fill_sections_until_full();
        tally[kFillerChildren + child_id] = 1;
        /* Sigue vivo con TODO lo que tiene tomado hasta que el padre lo deja. */
        while (tally[kTallyRelease] == 0) {
            (void)sleep_ms(1);
        }
        return 0;
    }

    int announced = 0;
    for (int waited = 0; waited < kBarrierAttempts && announced != kFillerChildren; ++waited) {
        announced = 0;
        for (int slot = 0; slot < kFillerChildren; ++slot) {
            if (tally[kFillerChildren + slot] == 1) {
                announced++;
            }
        }
        if (announced != kFillerChildren) {
            (void)sleep_ms(1);
        }
    }

    int failed = 0;
    if (announced != kFillerChildren) {
        /* Un hijo que no llego a la barrera es otra cosa que una tabla chica, y
         * si el segundo mensaje tapa al primero el diagnostico miente. */
        eprintf("sectiontest: solo %d de %d hijos llegaron a la barrera\n", announced, kFillerChildren);
        failed = 1;
    } else {
        /* Los ocho tienen sus secciones vivas AHORA. Con el tope viejo (64)
         * esta suma no podia pasar de 64; con el nuevo tiene que llegar ademas
         * a haber encontrado la tabla llena y devuelto ENOMEM limpio. */
        unsigned long total = 0;
        for (int slot = 0; slot < kFillerChildren; ++slot) {
            total += tally[slot];
        }
        if (total <= 64) {
            eprintf("sectiontest: la tabla global solo llego a %lu secciones vivas\n", total);
            failed = 1;
        } else {
            eprintf("sectiontest: %lu secciones vivas entre %d procesos, tabla llena limpio\n", total, kFillerChildren);
        }
    }

    /* Suelta y recoge SIEMPRE, incluso cuando failed ya es 1. Los hijos esperan
     * en la bandera de salida: si el test vuelve antes de ponerla quedan ocho
     * procesos girando con sleep hasta morir, quitandole timeslice a todo lo que
     * viene despues en el smoke. */
    tally[kTallyRelease] = 1;
    for (int slot = 0; slot < kFillerChildren; ++slot) {
        int status = -1;
        /* -1 es "cualquier hijo" en este kernel; 0 seria el pid 0. */
        if (savanxp_waitpid(-1, &status) < 0 || status != 0) {
            /* Un hijo que se muere por falta de memoria en vez de devolver un
             * errno limpio es un bug del kernel, no de este test. */
            eprintf("sectiontest: el hijo de relleno %d termino con status %d\n", slot, status);
            failed = 1;
        }
    }

    unmap_view((void*)tally);
    savanxp_close((int)tally_section);
    return failed;
}