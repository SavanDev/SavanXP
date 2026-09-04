/* Test del cargador de ejecutables.
 *
 * El kernel dejo de copiar la imagen entera a un buffer intermedio: ahora la
 * lee del archivo directo sobre las paginas del proceso, de a una. Lo que un
 * copiado asi puede romper no se ve con un binario chico:
 *
 *   1. contenido que cruza muchas paginas -- un arreglo grande en .rodata
 *      tiene que llegar byte a byte, sin saltos ni repeticiones en los bordes;
 *   2. el corte entre file_size y memory_size -- el .bss no esta en el archivo
 *      y tiene que quedar en cero, sin arrastrar lo que hubiera en la pagina;
 *   3. .data, que si esta en el archivo y ademas es escribible.
 *
 * El binario se mira a si mismo: si el loader se equivoco en cualquiera de las
 * tres, lo que este programa lee de su propia imagen no coincide.
 */

#include <stdio.h>
#include <string.h>

/* 32 paginas de contenido conocido, generado por una formula para no depender
 * de un blob y para que un desplazamiento de un solo byte se note. */
#define IMAGE_PATTERN_BYTES (128 * 1024)
#define IMAGE_BSS_BYTES (64 * 1024)

static unsigned char image_pattern_byte(unsigned long index) {
    return (unsigned char)((index * 31u) ^ (index >> 8) ^ 0xa5u);
}

/* La macro repite la formula en tiempo de compilacion. Se escribe asi y no con
 * un memset para que el contenido viaje DE VERDAD en el archivo. */
#define P1(i) (unsigned char)(((i) * 31u) ^ ((i) >> 8) ^ 0xa5u)
#define P8(i) P1(i), P1(i + 1), P1(i + 2), P1(i + 3), P1(i + 4), P1(i + 5), P1(i + 6), P1(i + 7)
#define P64(i) P8(i), P8(i + 8), P8(i + 16), P8(i + 24), P8(i + 32), P8(i + 40), P8(i + 48), P8(i + 56)
#define P512(i) P64(i), P64(i + 64), P64(i + 128), P64(i + 192), P64(i + 256), P64(i + 320), P64(i + 384), P64(i + 448)
#define P4096(i) P512(i), P512(i + 512), P512(i + 1024), P512(i + 1536), P512(i + 2048), P512(i + 2560), P512(i + 3072), P512(i + 3584)
#define P32768(i) P4096(i), P4096(i + 4096), P4096(i + 8192), P4096(i + 12288), P4096(i + 16384), P4096(i + 20480), P4096(i + 24576), P4096(i + 28672)

static const unsigned char g_pattern[IMAGE_PATTERN_BYTES] = {
    P32768(0u), P32768(32768u), P32768(65536u), P32768(98304u)};

/* .bss: no viaja en el archivo, el loader lo tiene que dejar en cero. */
static unsigned char g_zeroed[IMAGE_BSS_BYTES];

/* .data: viaja en el archivo y ademas es escribible. */
static unsigned long g_initialized[8] = {
    0x0123456789abcdefUL, 0xfedcba9876543210UL, 1UL, 2UL, 3UL, 4UL, 5UL, 6UL};

static int g_failures = 0;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("imagetest: FALLO %s\n", what);
        g_failures += 1;
    }
}

int main(void) {
    unsigned long index = 0;
    unsigned long mismatches = 0;
    unsigned long first_mismatch = 0;

    printf("imagetest: arrancando\n");

    /* 1. .rodata completo, byte a byte. */
    for (index = 0; index < IMAGE_PATTERN_BYTES; ++index) {
        if (g_pattern[index] != image_pattern_byte(index)) {
            if (mismatches == 0) {
                first_mismatch = index;
            }
            mismatches += 1;
        }
    }
    if (mismatches != 0) {
        printf("imagetest: %lu bytes distintos, el primero en %lu (pagina %lu)\n",
               mismatches, first_mismatch, first_mismatch / 4096u);
    }
    check(mismatches == 0, "rodata de 128 KiB intacto tras el streaming");

    /* 2. .bss en cero, incluida la parte que comparte pagina con .data. */
    mismatches = 0;
    for (index = 0; index < IMAGE_BSS_BYTES; ++index) {
        if (g_zeroed[index] != 0) {
            if (mismatches == 0) {
                first_mismatch = index;
            }
            mismatches += 1;
        }
    }
    if (mismatches != 0) {
        printf("imagetest: %lu bytes de bss sucios, el primero en %lu\n",
               mismatches, first_mismatch);
    }
    check(mismatches == 0, "bss en cero (memory_size mas alla de file_size)");

    /* 3. .data con sus valores iniciales, y escribible. */
    check(g_initialized[0] == 0x0123456789abcdefUL && g_initialized[1] == 0xfedcba9876543210UL,
          "data con sus valores iniciales");
    g_initialized[0] = 0xdeadbeefUL;
    check(g_initialized[0] == 0xdeadbeefUL, "data escribible");

    /* 4. El patron tambien se lee bien desde el medio, no solo en orden. */
    check(g_pattern[IMAGE_PATTERN_BYTES - 1] == image_pattern_byte(IMAGE_PATTERN_BYTES - 1),
          "ultimo byte del patron");
    check(g_pattern[4096] == image_pattern_byte(4096), "primer byte de la segunda pagina");
    check(g_pattern[4095] == image_pattern_byte(4095), "ultimo byte de la primera pagina");

    if (g_failures != 0) {
        printf("IMAGETEST FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("IMAGETEST PASS\n");
    return 0;
}
