#include "libc.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>

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

static int expect_format(const char* name, const char* expected, const char* label) {
    char buffer[64];
    int length = clipboard_get_format(name, buffer, (int)sizeof(buffer));
    int wanted = (int)strlen(expected);
    if (length != wanted || memcmp(buffer, expected, (size_t)wanted) != 0) {
        eprintf("cliptest: %s: format '%s' gave length %d, expected '%s'\n", label, name, length, expected);
        return 0;
    }
    return 1;
}

static int expect_formats(unsigned int count, unsigned int text_length, unsigned int format, const char* label) {
    struct savanxp_clipboard_info info;
    if (clipboard_get_info(&info) < 0 || info.format_count != count || info.length != text_length ||
        info.format != format) {
        eprintf("cliptest: %s: expected %u formats, text length %u, format %u\n", label, count, text_length, format);
        return 0;
    }
    return 1;
}

/* Several formats of one content (docs/desktop/CLIPBOARD.md): the set is
 * replaced whole or not at all, each format is read by name, and plain text
 * stays the SAVANXP_CLIPBOARD_TEXT entry. */
static int test_formats(void) {
    static const char text[] = "/home/a.txt\n";
    static const char uris[] = "file:///home/a.txt\r\n";
    struct clipboard_format both[2] = {
        {SAVANXP_CLIPBOARD_URI_LIST, uris, sizeof(uris) - 1},
        {SAVANXP_CLIPBOARD_TEXT, text, sizeof(text) - 1},
    };
    if (clipboard_set_formats(both, 2) < 0) {
        return fail("set_formats with two formats failed");
    }
    if (!expect_formats(2, sizeof(text) - 1, SAVANXP_CLIPBOARD_FORMAT_TEXT, "two formats") ||
        !expect_text(text, "text of a two-format set") ||
        !expect_format(SAVANXP_CLIPBOARD_URI_LIST, uris, "uri list") ||
        !expect_format(SAVANXP_CLIPBOARD_TEXT, text, "text by name")) {
        return 0;
    }

    /* Enumeration keeps the writer's order, and ends with ENOENT. */
    char name[SAVANXP_CLIPBOARD_NAME_CAPACITY];
    if (clipboard_enum_format(0, name, (int)sizeof(name)) != (int)sizeof(uris) - 1 ||
        strcmp(name, SAVANXP_CLIPBOARD_URI_LIST) != 0 ||
        clipboard_enum_format(1, name, (int)sizeof(name)) != (int)sizeof(text) - 1 ||
        strcmp(name, SAVANXP_CLIPBOARD_TEXT) != 0) {
        return fail("enumeration does not follow the writer's order");
    }
    int past = clipboard_enum_format(2, name, (int)sizeof(name));
    if (past >= 0 || result_error_code(past) != SAVANXP_ENOENT) {
        return fail("enumeration past the last format should be ENOENT");
    }

    /* A short buffer gets the prefix and the full length. */
    char small[4];
    if (clipboard_get_format(SAVANXP_CLIPBOARD_URI_LIST, small, (int)sizeof(small)) != (int)sizeof(uris) - 1 ||
        memcmp(small, "file", 4) != 0) {
        return fail("a short get_format should copy the prefix and report the full length");
    }
    int missing = clipboard_get_format("image/bmp", small, (int)sizeof(small));
    if (missing >= 0 || result_error_code(missing) != SAVANXP_ENOENT) {
        return fail("a format the clipboard does not hold should be ENOENT");
    }

    /* Refused sets leave the previous one whole: a repeated name, a name
     * with a control character, and a total over the capacity. */
    struct clipboard_format twice[2] = {
        {SAVANXP_CLIPBOARD_TEXT, "x", 1},
        {SAVANXP_CLIPBOARD_TEXT, "y", 1},
    };
    int duplicate = clipboard_set_formats(twice, 2);
    struct clipboard_format control = {"text/\tplain", "x", 1};
    int bad_name = clipboard_set_formats(&control, 1);
    char* half = (char*)malloc(SAVANXP_CLIPBOARD_CAPACITY / 2 + 1);
    if (half == 0) {
        return fail("could not allocate half the capacity");
    }
    memset(half, 'h', SAVANXP_CLIPBOARD_CAPACITY / 2 + 1);
    struct clipboard_format large[2] = {
        {SAVANXP_CLIPBOARD_TEXT, half, SAVANXP_CLIPBOARD_CAPACITY / 2 + 1},
        {"application/octet-stream", half, SAVANXP_CLIPBOARD_CAPACITY / 2 + 1},
    };
    int too_large = clipboard_set_formats(large, 2);
    free(half);
    if (result_error_code(duplicate) != SAVANXP_EINVAL || result_error_code(bad_name) != SAVANXP_EINVAL ||
        result_error_code(too_large) != SAVANXP_ENOSPC) {
        eprintf("cliptest: refused sets gave %d %d %d\n", duplicate, bad_name, too_large);
        return 0;
    }
    if (!expect_formats(2, sizeof(text) - 1, SAVANXP_CLIPBOARD_FORMAT_TEXT, "after refused sets") ||
        !expect_format(SAVANXP_CLIPBOARD_URI_LIST, uris, "uri list after refused sets")) {
        return 0;
    }

    /* A set without text: plain read finds nothing to paste. */
    if (clipboard_set_formats(both, 1) < 0) {
        return fail("set_formats with one non-text format failed");
    }
    char buffer[16];
    if (!expect_formats(1, 0, SAVANXP_CLIPBOARD_FORMAT_OTHER, "no text") ||
        clipboard_get_text(buffer, (int)sizeof(buffer)) != 0 || buffer[0] != '\0') {
        return fail("a set without text should paste as nothing");
    }

    /* Plain text replaces every format. */
    if (clipboard_set_text("plain") < 0 || !expect_formats(1, 5, SAVANXP_CLIPBOARD_FORMAT_TEXT, "plain text") ||
        !expect_format(SAVANXP_CLIPBOARD_TEXT, "plain", "plain text by name")) {
        return 0;
    }

    /* Writing formats needs write access, like CLEAR. */
    int read_only = savanxp_open_mode("/dev/clipboard", SAVANXP_OPEN_READ);
    if (read_only < 0) {
        return fail("could not open the clipboard read-only");
    }
    struct savanxp_clipboard_entry entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, SAVANXP_CLIPBOARD_TEXT, sizeof(SAVANXP_CLIPBOARD_TEXT));
    entry.data = (uint64_t)(uintptr_t)"denied";
    entry.length = 6;
    struct savanxp_clipboard_set request;
    memset(&request, 0, sizeof(request));
    request.entries = (uint64_t)(uintptr_t)&entry;
    request.count = 1;
    long denied = savanxp_ioctl(read_only, CLIP_IOC_SET_FORMATS, (unsigned long)&request);
    savanxp_close(read_only);
    if (denied >= 0 || result_error_code(denied) != SAVANXP_EACCES) {
        eprintf("cliptest: SET_FORMATS read-only was not refused (%ld)\n", denied);
        return 0;
    }
    return clipboard_clear() >= 0;
}

static int clipboard_readable(int fd, int timeout_ms) {
    struct pollfd entry = {fd, POLLIN, 0};
    int result = poll(&entry, 1, timeout_ms);
    return result == 1 && (entry.revents & POLLIN) != 0;
}

/* Following the clipboard (docs/desktop/CLIPBOARD.md): a descriptor is
 * readable while the content changed since it last asked GET_INFO, so a
 * blocked poll wakes when another process copies. */
static int test_changes(void) {
    int fd = savanxp_open_mode("/dev/clipboard", SAVANXP_OPEN_READ);
    if (fd < 0) {
        return fail("could not open the clipboard to follow it");
    }
    struct savanxp_clipboard_info info;
    int ok = clipboard_readable(fd, 0);
    if (!ok) {
        eprintf("cliptest: a descriptor just opened should be readable\n");
    }
    if (ok && (savanxp_ioctl(fd, CLIP_IOC_GET_INFO, (unsigned long)&info) < 0 || clipboard_readable(fd, 0))) {
        eprintf("cliptest: after GET_INFO the descriptor should not be readable\n");
        ok = 0;
    }
    /* Reading the content is not asking: it stays not readable. */
    char buffer[8];
    if (ok && (savanxp_read(fd, buffer, sizeof(buffer)) < 0 || clipboard_readable(fd, 0))) {
        eprintf("cliptest: a read should leave the descriptor not readable\n");
        ok = 0;
    }

    /* Another process copies while this one sleeps in poll. */
    long child = ok ? savanxp_fork() : -1;
    if (ok && child < 0) {
        eprintf("cliptest: fork for the change test failed\n");
        ok = 0;
    }
    if (child == 0) {
        sleep_ms(50);
        exit(clipboard_set_text("changed elsewhere") < 0 ? 1 : 0);
    }
    if (ok && !clipboard_readable(fd, 5000)) {
        eprintf("cliptest: a copy by another process did not wake the poll\n");
        ok = 0;
    }
    int status = -1;
    if (child > 0 && (savanxp_waitpid((int)child, &status) < 0 || status != 0)) {
        eprintf("cliptest: the child could not copy\n");
        ok = 0;
    }
    unsigned long long before = 0;
    if (ok) {
        (void)savanxp_ioctl(fd, CLIP_IOC_GET_INFO, (unsigned long)&info);
        before = (unsigned long long)info.sequence;
        if (clipboard_readable(fd, 0)) {
            eprintf("cliptest: asking again should clear the change\n");
            ok = 0;
        }
    }

    /* CLEAR is a change too. */
    if (ok && (clipboard_clear() < 0 || !clipboard_readable(fd, 0))) {
        eprintf("cliptest: CLEAR should make the descriptor readable\n");
        ok = 0;
    }
    if (ok && (savanxp_ioctl(fd, CLIP_IOC_GET_INFO, (unsigned long)&info) < 0 ||
               (unsigned long long)info.sequence <= before)) {
        eprintf("cliptest: the sequence did not rise with CLEAR\n");
        ok = 0;
    }
    savanxp_close(fd);
    return ok;
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

    /* Going over the capacity fails instead of truncating silently. The
     * buffers come from the heap: the capacity is a megabyte. */
    char* oversized = (char*)malloc(SAVANXP_CLIPBOARD_CAPACITY + 64);
    char* exact = (char*)malloc(SAVANXP_CLIPBOARD_CAPACITY + 1);
    if (oversized == 0 || exact == 0) {
        return fail("could not allocate the capacity buffers");
    }
    memset(oversized, 'a', SAVANXP_CLIPBOARD_CAPACITY + 63);
    oversized[SAVANXP_CLIPBOARD_CAPACITY + 63] = '\0';
    int overflow = clipboard_set_text(oversized);
    if (overflow >= 0) {
        return fail("a set_text larger than the capacity should fail");
    }
    if (!expect_text("0123456789", "the content survives the failed set")) {
        return 1;
    }

    /* Exactly the capacity fits. */
    memset(exact, 'b', SAVANXP_CLIPBOARD_CAPACITY);
    exact[SAVANXP_CLIPBOARD_CAPACITY] = '\0';
    if (clipboard_set_text(exact) < 0) {
        return fail("a set_text of exactly the capacity should fit");
    }
    if (!expect_info(SAVANXP_CLIPBOARD_CAPACITY, SAVANXP_CLIPBOARD_FORMAT_TEXT, "exact capacity")) {
        return 1;
    }
    free(oversized);
    free(exact);

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

    /* Un descriptor read-only puede consultar informacion, pero no ejecutar
     * un ioctl mutante aunque el handler del device siga registrado. */
    int read_only = savanxp_open_mode("/dev/clipboard", SAVANXP_OPEN_READ);
    if (read_only < 0) {
        return fail("no se pudo abrir clipboard read-only");
    }
    struct savanxp_clipboard_info read_info = {};
    if (savanxp_ioctl(read_only, CLIP_IOC_GET_INFO, (unsigned long)&read_info) < 0) {
        savanxp_close(read_only);
        return fail("GET_INFO read-only fallo");
    }
    long denied = savanxp_ioctl(read_only, CLIP_IOC_CLEAR, 0);
    savanxp_close(read_only);
    if (denied >= 0 || result_error_code(denied) != SAVANXP_EACCES) {
        eprintf("cliptest: CLEAR read-only no fue rechazado (%ld)\n", denied);
        return 1;
    }

    if (!test_formats()) {
        return 1;
    }

    if (!test_changes()) {
        return 1;
    }

    puts_out("cliptest: PASS\n");
    return 0;
}
