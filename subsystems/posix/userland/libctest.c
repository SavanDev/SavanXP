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

int main(void) {
    const struct file_ops ops = {
        .close = close,
        .read = read,
        .write = write,
        .remove = "campo llamado remove",
    };

    test_string_and_memory();
    test_format();
    test_sorting();
    test_files(&ops);
    test_stdio_stream();
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
