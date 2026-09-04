/* Test de la superficie de libc que consume codigo de terceros.
 *
 * A proposito incluye SOLO headers estandar: ni savanxp/libc.h ni "libc.h".
 * Un port (FFmpeg, ccleste, lo que venga) no conoce otra cosa, y hasta que los
 * headers dejaron de renombrar con #define esto no se podia ni escribir:
 *   - un campo de struct llamado `close` o `read` se convertia en `sx_close` /
 *     `sx_read` en las unidades que incluian <unistd.h> y quedaba con el nombre
 *     original en las que no;
 *   - los simbolos estandar (malloc, qsort, snprintf, stat, ...) directamente
 *     no existian en el link: solo estaban los sx_*.
 *
 * Por eso el test mezcla las dos cosas: nombres estandar usados como campos y
 * como funciones, y las convenciones POSIX de retorno (-1 y errno, no -errno).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIBCTEST_PATH "/disk/tmp/libctest.txt"

static int g_failures = 0;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("libctest: FALLO %s\n", what);
        g_failures += 1;
    }
}

/* Campos con nombres de funciones de libc: el caso que rompia con los macros. */
struct file_ops {
    int (*close)(int fd);
    ssize_t (*read)(int fd, void* buffer, size_t count);
    ssize_t (*write)(int fd, const void* buffer, size_t count);
    const char* remove;
};

static int compare_ints(const void* left, const void* right) {
    const int a = *(const int*)left;
    const int b = *(const int*)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void test_string_and_memory(void) {
    char buffer[64];
    char* copy = 0;
    void* block = 0;

    check(strlen("savanxp") == 7, "strlen");
    check(strcmp("a", "a") == 0 && strcmp("a", "b") < 0, "strcmp");
    check(strncmp("abcd", "abce", 3) == 0, "strncmp");
    check(strchr("a/b", '/') != 0, "strchr");
    check(strstr("hola mundo", "mundo") != 0, "strstr");

    memset(buffer, 'x', sizeof(buffer));
    check(buffer[0] == 'x' && buffer[63] == 'x', "memset");
    memcpy(buffer, "hola", 5);
    check(strcmp(buffer, "hola") == 0, "memcpy");
    memmove(buffer + 1, buffer, 5);
    check(strcmp(buffer + 1, "hola") == 0, "memmove");
    check(memcmp("abc", "abc", 3) == 0, "memcmp");

    copy = strdup("duplicado");
    check(copy != 0 && strcmp(copy, "duplicado") == 0, "strdup");
    free(copy);

    block = malloc(4096);
    check(block != 0, "malloc");
    block = realloc(block, 8192);
    check(block != 0, "realloc");
    memset(block, 0, 8192);
    free(block);

    block = calloc(32, 16);
    check(block != 0 && ((const unsigned char*)block)[511] == 0, "calloc");
    free(block);
}

static void test_format(void) {
    char buffer[96];
    int written = snprintf(buffer, sizeof(buffer), "%s|%d|%u|%x|%c|%ld|%05d|%-4s|",
                           "s", -7, 7u, 255u, 'z', 123456789L, 42, "pad");
    check(written > 0, "snprintf devuelve el largo");
    check(strcmp(buffer, "s|-7|7|ff|z|123456789|00042|pad |") == 0, "snprintf formatea");
    if (strcmp(buffer, "s|-7|7|ff|z|123456789|00042|pad |") != 0) {
        printf("libctest:   obtuvo '%s'\n", buffer);
    }
}

static void test_sorting(void) {
    int values[] = {5, 2, 9, 1, 7};
    const int needle = 7;
    const int* found = 0;

    qsort(values, 5, sizeof(values[0]), compare_ints);
    check(values[0] == 1 && values[4] == 9, "qsort");
    found = (const int*)bsearch(&needle, values, 5, sizeof(values[0]), compare_ints);
    check(found != 0 && *found == 7, "bsearch");
}

static void test_files(const struct file_ops* ops) {
    struct stat info;
    char buffer[32] = {0};
    int fd = open(LIBCTEST_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    check(fd >= 0, "open O_CREAT");
    if (fd < 0) {
        return;
    }
    check(ops->write(fd, "contenido", 9) == 9, "write");
    check(ops->close(fd) == 0, "close devuelve 0");

    check(stat(LIBCTEST_PATH, &info) == 0, "stat");
    check(info.st_size == 9, "stat st_size");

    fd = open(LIBCTEST_PATH, O_RDONLY);
    check(fd >= 0, "open O_RDONLY");
    if (fd >= 0) {
        check(ops->read(fd, buffer, sizeof(buffer) - 1) == 9, "read");
        check(strcmp(buffer, "contenido") == 0, "read contenido");
        check(lseek(fd, 4, SEEK_SET) == 4, "lseek");
        check(fstat(fd, &info) == 0 && info.st_size == 9, "fstat");
        check(ops->close(fd) == 0, "close del lector");
    }

    check(unlink(LIBCTEST_PATH) == 0, "unlink");
}

static void test_stdio_stream(void) {
    FILE* stream = fopen(LIBCTEST_PATH, "w");
    char line[32] = {0};

    check(stream != 0, "fopen w");
    if (stream == 0) {
        return;
    }
    check(fprintf(stream, "linea %d\n", 1) > 0, "fprintf");
    check(fclose(stream) == 0, "fclose");

    stream = fopen(LIBCTEST_PATH, "r");
    check(stream != 0, "fopen r");
    if (stream != 0) {
        check(fgets(line, sizeof(line), stream) != 0, "fgets");
        check(strcmp(line, "linea 1\n") == 0, "fgets contenido");
        check(fclose(stream) == 0, "fclose del lector");
    }
    check(remove(LIBCTEST_PATH) == 0, "remove");
}

/* La convencion POSIX: -1 y errno, no el -errno crudo del kernel. */
static void test_error_convention(void) {
    int fd = open("/disk/no/existe/de/verdad", O_RDONLY);
    check(fd == -1, "open fallido devuelve -1");
    check(errno == ENOENT, "open fallido deja errno=ENOENT");

    errno = 0;
    check(close(4242) == -1, "close de fd invalido devuelve -1");
    check(errno == EBADF, "close invalido deja errno=EBADF");
}

static void test_directories(void) {
    DIR* directory = opendir("/disk/bin");
    int entries = 0;

    check(directory != 0, "opendir");
    if (directory == 0) {
        return;
    }
    while (readdir(directory) != 0) {
        entries += 1;
    }
    check(entries > 0, "readdir devuelve entradas");
    check(closedir(directory) == 0, "closedir");
}

static void test_string_extra(void) {
    char buffer[64];
    char scratch[64];
    char* cursor = 0;
    char* token = 0;

    check(memchr("abcdef", 'c', 6) != 0, "memchr encuentra");
    check(memchr("abcdef", 'z', 6) == 0, "memchr no encuentra");
    check(strnlen("abc", 10) == 3, "strnlen corto");
    check(strnlen("abcdef", 3) == 3, "strnlen topeado");

    strcpy(buffer, "hola");
    strcat(buffer, " mundo");
    check(strcmp(buffer, "hola mundo") == 0, "strcat");
    strcpy(buffer, "hola");
    strncat(buffer, " mundo", 3);
    check(strcmp(buffer, "hola mu") == 0, "strncat");

    strcpy(scratch, "uno,dos,tres");
    token = strtok(scratch, ",");
    check(token != 0 && strcmp(token, "uno") == 0, "strtok primero");
    token = strtok(0, ",");
    check(token != 0 && strcmp(token, "dos") == 0, "strtok segundo");
    token = strtok(0, ",");
    check(token != 0 && strcmp(token, "tres") == 0, "strtok tercero");
    check(strtok(0, ",") == 0, "strtok agotado");

    strcpy(scratch, "a:b::c");
    cursor = scratch;
    check(strcmp(strsep(&cursor, ":"), "a") == 0, "strsep primero");
    check(strcmp(strsep(&cursor, ":"), "b") == 0, "strsep segundo");
    check(strcmp(strsep(&cursor, ":"), "") == 0, "strsep campo vacio");
    check(strcmp(strsep(&cursor, ":"), "c") == 0, "strsep ultimo");
    check(strsep(&cursor, ":") == 0, "strsep agotado");

    check(strcasestr("Hola Mundo", "mundo") != 0, "strcasestr encuentra");
    check(strcasestr("Hola Mundo", "chau") == 0, "strcasestr no encuentra");
}

static void test_stdlib_extra(void) {
    char* end = 0;
    void* aligned = 0;
    div_t quotient;

    check(strtoll("-9007199254740993", &end, 10) == -9007199254740993LL, "strtoll 64 bits");
    check(strtoull("18446744073709551615", &end, 10) == 18446744073709551615ULL,
          "strtoull maximo");
    check(strtol("0x1f", &end, 0) == 31, "strtol base automatica hex");
    check(strtol("017", &end, 0) == 15, "strtol base automatica octal");
    check(atol("-12345") == -12345L, "atol");
    check(atoll("-12345678901") == -12345678901LL, "atoll");
    check(labs(-7L) == 7L, "labs");
    check(llabs(-7LL) == 7LL, "llabs");

    quotient = div(7, 2);
    check(quotient.quot == 3 && quotient.rem == 1, "div");
    check(ldiv(-7L, 2L).quot == -3L, "ldiv");

    /* Desbordar tiene que saturar y avisar por ERANGE, no envolver. */
    errno = 0;
    check(strtoul("99999999999999999999999", &end, 10) == (unsigned long)-1,
          "strtoul satura");
    check(errno == ERANGE, "strtoul deja ERANGE");

    /* malloc alinea a 16: es el max_align_t de x86-64. */
    {
        void* blocks[8];
        int index = 0;
        int all_aligned = 1;
        for (index = 0; index < 8; ++index) {
            blocks[index] = malloc(index * 7 + 1);
            if (blocks[index] == 0 || ((unsigned long)blocks[index] & 15u) != 0) {
                all_aligned = 0;
            }
        }
        check(all_aligned, "malloc alinea a 16");
        for (index = 0; index < 8; ++index) {
            free(blocks[index]);
        }
    }

    check(posix_memalign(&aligned, 64, 1000) == 0, "posix_memalign");
    check(aligned != 0 && ((unsigned long)aligned & 63u) == 0, "posix_memalign alinea a 64");
    if (aligned != 0) {
        memset(aligned, 0xab, 1000);
        free(aligned);
    }
    check(posix_memalign(&aligned, 3, 16) != 0, "posix_memalign rechaza no potencia de dos");

    aligned = aligned_alloc(256, 512);
    check(aligned != 0 && ((unsigned long)aligned & 255u) == 0, "aligned_alloc");
    free(aligned);

    /* Intercalar alineadas y normales: free tiene que distinguirlas. */
    {
        void* a = aligned_alloc(128, 64);
        void* b = malloc(64);
        void* c = aligned_alloc(32, 64);
        check(a != 0 && b != 0 && c != 0, "mezcla alineada/normal");
        free(b);
        free(a);
        free(c);
    }
}

static void test_stdio_extra(void) {
    FILE* stream = fopen(LIBCTEST_PATH, "w");
    char line[64] = {0};
    int fd = -1;

    check(stream != 0, "fopen para stdio extra");
    if (stream == 0) {
        return;
    }
    check(fputc('A', stream) == 'A', "fputc");
    check(fileno(stream) >= 0, "fileno");
    check(fwrite("BCDEF\n", 1, 6, stream) == 6, "fwrite");
    check(fclose(stream) == 0, "fclose tras fputc");

    stream = fopen(LIBCTEST_PATH, "r");
    check(stream != 0, "fopen lectura");
    if (stream == 0) {
        return;
    }
    check(fgetc(stream) == 'A', "fgetc");
    check(ungetc('A', stream) == 'A', "ungetc");
    check(fgetc(stream) == 'A', "fgetc tras ungetc");
    check(getc(stream) == 'B', "getc");
    check(ftell(stream) == 2, "ftell cuenta el buffer");
    rewind(stream);
    check(ftell(stream) == 0, "rewind");
    check(fgets(line, sizeof(line), stream) != 0 && strcmp(line, "ABCDEF\n") == 0,
          "fgets con buffer");
    check(fseeko(stream, 2, SEEK_SET) == 0 && ftello(stream) == 2, "fseeko/ftello");
    check(setvbuf(stream, 0, _IONBF, 0) == 0, "setvbuf aceptado");
    check(fclose(stream) == 0, "fclose lectura");

    fd = open(LIBCTEST_PATH, O_RDONLY);
    check(fd >= 0, "open para fdopen");
    if (fd >= 0) {
        FILE* adopted = fdopen(fd, "r");
        check(adopted != 0, "fdopen");
        if (adopted != 0) {
            check(fgetc(adopted) == 'A', "fgetc sobre fdopen");
            fclose(adopted);
        } else {
            close(fd);
        }
    }
    unlink(LIBCTEST_PATH);
}

static void test_scan(void) {
    int a = 0;
    int b = 0;
    unsigned int hex = 0;
    char word[32] = {0};
    char letter = 0;
    char set[32] = {0};
    int consumed = 0;

    check(sscanf("12 34", "%d %d", &a, &b) == 2 && a == 12 && b == 34, "sscanf dos enteros");
    check(sscanf("ff", "%x", &hex) == 1 && hex == 255u, "sscanf hex");
    check(sscanf("hola mundo", "%s", word) == 1 && strcmp(word, "hola") == 0, "sscanf cadena");
    check(sscanf("abc", "%c", &letter) == 1 && letter == 'a', "sscanf caracter");
    check(sscanf("12345", "%3d", &a) == 1 && a == 123, "sscanf con ancho");
    check(sscanf("9 8", "%*d %d", &a) == 1 && a == 8, "sscanf con supresion");
    check(sscanf("clave=valor", "%[^=]", set) == 1 && strcmp(set, "clave") == 0,
          "sscanf scanset negado");
    check(sscanf("abc123", "%[a-z]", set) == 1 && strcmp(set, "abc") == 0,
          "sscanf scanset con rango");
    check(sscanf("ab 7", "%s %n%d", word, &consumed, &a) == 2 && consumed == 3 && a == 7,
          "sscanf con %n");
    check(sscanf("x=5", "x=%d", &a) == 1 && a == 5, "sscanf con literal");
    check(sscanf("nada", "%d", &a) == 0, "sscanf sin coincidencia");

    {
        long long big = 0;
        check(sscanf("-9007199254740993", "%lld", &big) == 1 &&
                  big == -9007199254740993LL,
              "sscanf %lld");
    }
}

static void test_time(void) {
    struct tm fields;
    struct tm roundtrip;
    char text[64];
    time_t stamp = 0;

    memset(&fields, 0, sizeof(fields));
    fields.tm_year = 2026 - 1900;
    fields.tm_mon = 8;   /* septiembre */
    fields.tm_mday = 3;
    fields.tm_hour = 14;
    fields.tm_min = 30;
    fields.tm_sec = 15;

    stamp = timegm(&fields);
    check(stamp == 1788445815L, "timegm contra un valor conocido");
    check(fields.tm_wday == 4, "timegm normaliza el dia de semana (jueves)");

    check(gmtime_r(&stamp, &roundtrip) != 0, "gmtime_r");
    check(roundtrip.tm_year == 2026 - 1900 && roundtrip.tm_mon == 8 &&
              roundtrip.tm_mday == 3 && roundtrip.tm_hour == 14 &&
              roundtrip.tm_min == 30 && roundtrip.tm_sec == 15,
          "gmtime_r revierte a timegm");
    check(roundtrip.tm_yday == 245, "tm_yday");

    check(strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &roundtrip) == 19,
          "strftime devuelve el largo");
    check(strcmp(text, "2026-09-03 14:30:15") == 0, "strftime formatea");
    check(strftime(text, sizeof(text), "%F %T", &roundtrip) == 19, "strftime %F %T");
    check(strcmp(text, "2026-09-03 14:30:15") == 0, "strftime %F %T contenido");
    check(strftime(text, sizeof(text), "%a %b", &roundtrip) > 0, "strftime nombres");
    check(strcmp(text, "Thu Sep") == 0, "strftime nombres contenido");
    check(strftime(text, 4, "%Y", &roundtrip) == 0, "strftime avisa si no entra");

    /* La epoca misma, y una fecha anterior a 1970 (division hacia abajo). */
    {
        time_t epoch = 0;
        check(gmtime_r(&epoch, &roundtrip) != 0 && roundtrip.tm_year == 70 &&
                  roundtrip.tm_mon == 0 && roundtrip.tm_mday == 1 &&
                  roundtrip.tm_wday == 4,
              "gmtime_r en la epoca");
    }
    {
        time_t before = -86400L;
        check(gmtime_r(&before, &roundtrip) != 0 && roundtrip.tm_year == 69 &&
                  roundtrip.tm_mon == 11 && roundtrip.tm_mday == 31 &&
                  roundtrip.tm_hour == 0,
              "gmtime_r antes de 1970");
    }

    /* time() tiene que dar una fecha de verdad, no el uptime. */
    {
        const time_t now = time(0);
        check(now > 1735689600L, "time() devuelve epoca real, no uptime");
    }

    check(mktime(&fields) == stamp, "mktime coincide con timegm");
    check(localtime(&stamp) != 0, "localtime");
}

int main(void) {
    const struct file_ops ops = {
        .close = close,
        .read = read,
        .write = write,
        .remove = "campo llamado remove",
    };

    test_string_and_memory();
    test_string_extra();
    test_format();
    test_sorting();
    test_stdlib_extra();
    test_files(&ops);
    test_stdio_stream();
    test_stdio_extra();
    test_scan();
    test_time();
    test_error_convention();
    test_directories();

    check(getpid() > 0, "getpid");
    check(strcmp(ops.remove, "campo llamado remove") == 0, "campo remove intacto");

    if (g_failures != 0) {
        printf("LIBCTEST FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("LIBCTEST PASS\n");
    return 0;
}
