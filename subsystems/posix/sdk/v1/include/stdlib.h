#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define alloca __builtin_alloca

extern char** environ;

void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* pointer, size_t size);
void free(void* pointer);
int atoi(const char* text);
double atof(const char* text);
int abs(int value);
char* getenv(const char* name);
int system(const char* command);
void abort(void);
void exit(int code) __attribute__((noreturn));
long strtol(const char* text, char** endptr, int base);
unsigned long strtoul(const char* text, char** endptr, int base);
void* bsearch(const void* key, const void* base, size_t count, size_t size,
    int (*compar)(const void*, const void*));
void qsort(void* base, size_t count, size_t size, int (*compar)(const void*, const void*));

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

long long strtoll(const char* text, char** endptr, int base);
unsigned long long strtoull(const char* text, char** endptr, int base);
long atol(const char* text);
long long atoll(const char* text);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

/* alignment tiene que ser potencia de dos. malloc ya devuelve memoria alineada
 * a 16 (max_align_t de x86-64); estas existen para lo que pida mas. */
int posix_memalign(void** out_pointer, size_t alignment, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
void* memalign(size_t alignment, size_t size);

/* Solo existen con -Sse: sin SSE el target no tiene ABI de punto flotante y el
 * link se caeria igual ante cualquier operacion con double. */
#if defined(__SSE2__)
double strtod(const char* text, char** endptr);
float strtof(const char* text, char** endptr);
#endif
