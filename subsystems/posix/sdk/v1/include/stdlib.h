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
