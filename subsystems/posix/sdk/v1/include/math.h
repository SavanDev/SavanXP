#pragma once

/* Las cuatro sx_* de abajo son la excepcion historica a todo lo demas: las
 * implementa posix.c con asm de x87 sobre operandos en memoria, que no depende
 * del ABI de punto flotante, y por eso existen tambien sin SSE. Siguen
 * declaradas siempre para no romper a quien ya las llama por su nombre. */
double sx_fabs(double value);
double sx_sin(double value);
double sx_tan(double value);
double sx_atan(double value);

#define M_E 2.71828182845904523536
#define M_LN2 0.69314718055994530942
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_SQRT2 1.41421356237309504880

#if defined(__SSE2__)

/* Camino con -Sse: la biblioteca de verdad, en runtime/math.c. El link solo
 * resuelve estos simbolos si la app se construyo con ese switch, que es
 * justamente lo que se quiere -- una app sin SSE no puede ni llamarlas, porque
 * clang le pasaria los argumentos por una convencion que math.c no habla. */

#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double fabs(double x);
double copysign(double x, double y);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double ldexp(double x, int n);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);

float fabsf(float x);
float copysignf(float x, float y);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fmodf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atanf(float x);
float atan2f(float y, float x);
float ldexpf(float x, int n);
float expf(float x);
float exp2f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float x, float y);


#else

/* Camino sin SSE (el default). Los nombres estandar se redirigen a las
 * variantes x87, que es lo que estos macros venian haciendo desde siempre: solo
 * hay version double y solo estas cuatro. Cualquier otra funcion de math.h
 * queda sin declarar a proposito, para que el error salga al compilar y no como
 * un simbolo suelto en el link. */
#define fabs sx_fabs
#define sin sx_sin
#define tan sx_tan
#define atan sx_atan

#endif
