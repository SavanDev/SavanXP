#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decodificador PNG del sistema, en libsximg.so.0.4.
 *
 * Cubre el subconjunto que una app necesita para abrir imagenes reales: PNG
 * de 8 bits, color verdadero (RGB) o color verdadero con alfa (RGBA), sin
 * entrelazado, con los cinco filtros y los tres tipos de bloque deflate. El
 * flujo zlib se verifica (cabecera y Adler-32); los CRC de chunk no se
 * comprueban. Lo demas se rechaza: paleta, grises, 1/2/4/16 bpp, tRNS,
 * entrelazado Adam7 y cualquier perfil fuera de 8-bit truecolor.
 *
 * El alfa se integra sobre blanco, que es el fondo del documento de Paint.
 * La salida es top-down en 0x00RRGGBB, como SXGUI_RGB.
 *
 * Implementacion propia de referencia (RFC 1951 + spec PNG): sin codigo de
 * terceros, sin tablas horneadas, sin estado global. La libreria no depende
 * de libc mas alla de malloc/free/memcpy/memset, que resuelve contra el
 * ejecutable como el resto de las librerias del sistema.
 *
 * Sin codificador: escribir PNG (aunque sea con bloques almacenados, que no
 * necesitan Huffman de compresion) queda para despues. Sin streaming: el PNG
 * entero se decodifica de una vez.
 */

#define SX_PNG_MAX_DIMENSION 4096u

struct sx_png_image {
    uint32_t *pixels; /* w*h en 0x00RRGGBB, filas de arriba abajo; NULL si vacia */
    uint32_t width;
    uint32_t height;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Decodifica un PNG en memoria. Devuelve 0 y deja los pixeles en `out`
 * (llamar a sx_png_free), o < 0 si los datos no son un PNG del subconjunto
 * o si no hay memoria. `data` solo se lee durante la llamada. */
int sx_png_decode(const unsigned char *data, size_t size, struct sx_png_image *out);

/* Libera los pixeles y deja la estructura vacia. Acepta una imagen vacia. */
void sx_png_free(struct sx_png_image *image);

#ifdef __cplusplus
}
#endif
