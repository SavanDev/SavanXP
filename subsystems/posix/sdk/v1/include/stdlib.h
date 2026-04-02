#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define malloc sx_malloc
#define calloc sx_calloc
#define realloc sx_realloc
#define free sx_free
#define atoi sx_atoi
#define atof sx_atof
#define abs sx_abs
#define getenv sx_getenv
#define system sx_system
#define abort sx_abort
#define exit sx_exit
#define strtol sx_strtol
#define strtoul sx_strtoul
#define bsearch sx_bsearch
#define qsort sx_qsort
#define alloca __builtin_alloca

extern char** environ;

void* sx_malloc(size_t size);
void* sx_calloc(size_t count, size_t size);
void* sx_realloc(void* pointer, size_t size);
void sx_free(void* pointer);
int sx_atoi(const char* text);
double sx_atof(const char* text);
int sx_abs(int value);
char* sx_getenv(const char* name);
int sx_system(const char* command);
void sx_abort(void);
void sx_exit(int code) __attribute__((noreturn));
long sx_strtol(const char* text, char** endptr, int base);
unsigned long sx_strtoul(const char* text, char** endptr, int base);
void* sx_bsearch(const void* key, const void* base, size_t count, size_t size,
    int (*compar)(const void*, const void*));
void sx_qsort(void* base, size_t count, size_t size, int (*compar)(const void*, const void*));
