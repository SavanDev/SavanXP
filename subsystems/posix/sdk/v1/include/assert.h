#pragma once

/* C11 pide que <assert.h> defina static_assert como macro de _Static_assert.
 * No es un detalle: el configure de FFmpeg lo usa para verificar el layout de
 * sus structs y, sin esto, decide que el compilador no soporta C11 y se corta.
 * En C23 pasa a ser palabra clave, asi que ahi no hay nada que definir. */
#if !defined(__cplusplus) && !defined(static_assert) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define static_assert _Static_assert
#endif

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
/* Imprime que fallo y termina el proceso. Antes era `((void)(expression))`, que
 * evaluaba la condicion y descartaba el resultado: un assert que no se cumple
 * no se notaba, que es lo contrario de para lo que existe. */
void sx_assert_failed(const char* expression, const char* file, int line);

#define assert(expression) \
    ((expression) ? (void)0 : sx_assert_failed(#expression, __FILE__, __LINE__))
#endif
