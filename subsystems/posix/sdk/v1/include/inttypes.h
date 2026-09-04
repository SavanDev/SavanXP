#pragma once

#include <stdint.h>

/* Los especificadores de printf/scanf por ancho.
 *
 * Faltaban por completo, y el sintoma no era "PRId64 no existe": como el macro
 * no estaba definido, `"%" PRId64` quedaba como una cadena seguida de un
 * identificador suelto, o sea un error de sintaxis a varias lineas de la causa.
 *
 * El modelo de datos es LP64: int64_t es `long`, asi que el modificador de 64
 * bits es "l" y no "ll".
 */

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIdLEAST8 "d"
#define PRIdLEAST16 "d"
#define PRIdLEAST32 "d"
#define PRIdLEAST64 "ld"
#define PRIdFAST8 "d"
#define PRIdFAST16 "d"
#define PRIdFAST32 "d"
#define PRIdFAST64 "ld"
#define PRIdMAX "ld"
#define PRIdPTR "ld"

#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIiLEAST8 "i"
#define PRIiLEAST16 "i"
#define PRIiLEAST32 "i"
#define PRIiLEAST64 "li"
#define PRIiFAST8 "i"
#define PRIiFAST16 "i"
#define PRIiFAST32 "i"
#define PRIiFAST64 "li"
#define PRIiMAX "li"
#define PRIiPTR "li"

#define PRIo8 "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIoMAX "lo"
#define PRIoPTR "lo"

#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIuLEAST8 "u"
#define PRIuLEAST16 "u"
#define PRIuLEAST32 "u"
#define PRIuLEAST64 "lu"
#define PRIuFAST8 "u"
#define PRIuFAST16 "u"
#define PRIuFAST32 "u"
#define PRIuFAST64 "lu"
#define PRIuMAX "lu"
#define PRIuPTR "lu"

#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIxLEAST8 "x"
#define PRIxLEAST16 "x"
#define PRIxLEAST32 "x"
#define PRIxLEAST64 "lx"
#define PRIxFAST8 "x"
#define PRIxFAST16 "x"
#define PRIxFAST32 "x"
#define PRIxFAST64 "lx"
#define PRIxMAX "lx"
#define PRIxPTR "lx"

#define PRIX8 "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIXMAX "lX"
#define PRIXPTR "lX"

#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNdMAX "ld"
#define SCNdPTR "ld"

#define SCNi8 "hhi"
#define SCNi16 "hi"
#define SCNi32 "i"
#define SCNi64 "li"
#define SCNiMAX "li"
#define SCNiPTR "li"

#define SCNo8 "hho"
#define SCNo16 "ho"
#define SCNo32 "o"
#define SCNo64 "lo"
#define SCNoMAX "lo"
#define SCNoPTR "lo"

#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNuMAX "lu"
#define SCNuPTR "lu"

#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNx32 "x"
#define SCNx64 "lx"
#define SCNxMAX "lx"
#define SCNxPTR "lx"

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator);
intmax_t strtoimax(const char* text, char** endptr, int base);
uintmax_t strtoumax(const char* text, char** endptr, int base);
