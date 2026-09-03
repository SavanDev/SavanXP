#include "libc.h"

/*
 * Portapapeles del sistema (/dev/clipboard, ver savanxp/syscall.h).
 *
 * Lo que se valida aca es la semantica de VALOR, que es lo que lo separa de un
 * archivo cualquiera: un write reemplaza todo (no hace append), un read
 * devuelve desde el principio siempre (no hay cursor), y el contenido cruza el
 * limite de proceso -- que es la razon de existir de un portapapeles y lo
 * unico que no se puede probar sin un segundo proceso.
 */

static int fail(const char* label) {
    eprintf("cliptest: %s\n", label);
    return 0;
}

static int expect_info(unsigned int length, unsigned int format, const char* label) {
    struct savanxp_clipboard_info info;
    int result = clipboard_get_info(&info);
    if (result < 0) {
        eprintf("cliptest: get_info fallo en %s (%s)\n", label, result_error_string(result));
        return 0;
    }
    if (info.length != length || info.format != format) {
        eprintf("cliptest: %s esperaba length=%u format=%u, hay length=%u format=%u\n",
                label, length, format, info.length, info.format);
        return 0;
    }
    if (info.capacity != SAVANXP_CLIPBOARD_CAPACITY) {
        eprintf("cliptest: %s capacity=%u inesperada\n", label, info.capacity);
        return 0;
    }
    return 1;
}

static unsigned long sequence_now(void) {
    struct savanxp_clipboard_info info;
    if (clipboard_get_info(&info) < 0) {
        return 0;
    }
    return (unsigned long)info.sequence;
}

static int expect_text(const char* expected, const char* label) {
    char buffer[64];
    int length = clipboard_get_text(buffer, (int)sizeof(buffer));
    if (length < 0) {
        eprintf("cliptest: get_text fallo en %s (%s)\n", label, result_error_string(length));
        return 0;
    }
    if (strcmp(buffer, expected) != 0) {
        eprintf("cliptest: %s esperaba '%s', hay '%s'\n", label, expected, buffer);
        return 0;
    }
    if (length != (int)strlen(expected)) {
        eprintf("cliptest: %s largo reportado %d, esperaba %d\n", label, length, (int)strlen(expected));
        return 0;
    }
    return 1;
}

int main(void) {
    if (clipboard_clear() < 0) {
        return fail("clear inicial fallo");
    }
    if (!expect_info(0, SAVANXP_CLIPBOARD_FORMAT_EMPTY, "despues de clear")) {
        return 1;
    }

    /* Copiar y pegar. */
    if (clipboard_set_text("hola mundo") < 0) {
        return fail("set_text fallo");
    }
    if (!expect_text("hola mundo", "set/get basico") ||
        !expect_info(10, SAVANXP_CLIPBOARD_FORMAT_TEXT, "despues de set")) {
        return 1;
    }

    /* Sin cursor: leer de nuevo devuelve lo mismo, no cero bytes. */
    if (!expect_text("hola mundo", "segunda lectura")) {
        return 1;
    }

    /* Valor, no append: el write nuevo reemplaza al anterior entero. Si hiciera
     * append quedaria "hola mundoxy" y el largo daria 12. */
    if (clipboard_set_text("xy") < 0) {
        return fail("set_text de reemplazo fallo");
    }
    if (!expect_text("xy", "reemplazo") ||
        !expect_info(2, SAVANXP_CLIPBOARD_FORMAT_TEXT, "despues del reemplazo")) {
        return 1;
    }

    /* La secuencia avanza con cada cambio: es lo que deja a un menu saber si
     * tiene que rehabilitar Pegar sin releer el contenido. */
    unsigned long before = sequence_now();
    if (clipboard_set_text("otro") < 0) {
        return fail("set_text para secuencia fallo");
    }
    unsigned long after = sequence_now();
    if (after <= before) {
        eprintf("cliptest: la secuencia no avanzo (%lu -> %lu)\n", before, after);
        return 1;
    }

    /* Cadena vacia = portapapeles vacio, y el formato vuelve a EMPTY. */
    if (clipboard_set_text("") < 0) {
        return fail("set_text vacio fallo");
    }
    if (!expect_info(0, SAVANXP_CLIPBOARD_FORMAT_EMPTY, "despues del vacio")) {
        return 1;
    }

    /* Truncamiento informado: el buffer chico se lleva el prefijo terminado en
     * NUL, pero el retorno dice el largo REAL para que el llamador se entere. */
    if (clipboard_set_text("0123456789") < 0) {
        return fail("set_text para truncamiento fallo");
    }
    char small[4];
    int reported = clipboard_get_text(small, (int)sizeof(small));
    if (reported != 10) {
        eprintf("cliptest: truncamiento reporto %d, esperaba 10\n", reported);
        return 1;
    }
    if (strcmp(small, "012") != 0) {
        eprintf("cliptest: truncamiento copio '%s', esperaba '012'\n", small);
        return 1;
    }

    /* Pasarse de capacidad falla en vez de truncar en silencio. */
    static char oversized[SAVANXP_CLIPBOARD_CAPACITY + 64];
    for (unsigned int index = 0; index < sizeof(oversized) - 1; ++index) {
        oversized[index] = 'a';
    }
    oversized[sizeof(oversized) - 1] = '\0';
    int overflow = clipboard_set_text(oversized);
    if (overflow >= 0) {
        return fail("un set_text mas grande que la capacidad tendria que fallar");
    }
    if (!expect_text("0123456789", "el contenido sobrevive al set fallido")) {
        return 1;
    }

    /* Justo la capacidad entra. */
    static char exact[SAVANXP_CLIPBOARD_CAPACITY + 1];
    for (unsigned int index = 0; index < SAVANXP_CLIPBOARD_CAPACITY; ++index) {
        exact[index] = 'b';
    }
    exact[SAVANXP_CLIPBOARD_CAPACITY] = '\0';
    if (clipboard_set_text(exact) < 0) {
        return fail("set_text de exactamente la capacidad tendria que entrar");
    }
    if (!expect_info(SAVANXP_CLIPBOARD_CAPACITY, SAVANXP_CLIPBOARD_FORMAT_TEXT, "capacidad exacta")) {
        return 1;
    }

    /* Lo que importa de verdad: que cruce procesos. El hijo copia y el padre,
     * que nunca vio ese buffer, lo pega. */
    if (clipboard_set_text("del padre") < 0) {
        return fail("set_text previo al fork fallo");
    }
    long child = savanxp_fork();
    if (child < 0) {
        return fail("fork fallo");
    }
    if (child == 0) {
        if (clipboard_set_text("copiado por el hijo") < 0) {
            exit(1);
        }
        exit(0);
    }
    int status = -1;
    if (savanxp_waitpid((int)child, &status) < 0 || status != 0) {
        return fail("el hijo no pudo copiar");
    }
    if (!expect_text("copiado por el hijo", "cruce de procesos")) {
        return 1;
    }

    if (clipboard_clear() < 0) {
        return fail("clear final fallo");
    }

    puts_out("cliptest: PASS\n");
    return 0;
}
