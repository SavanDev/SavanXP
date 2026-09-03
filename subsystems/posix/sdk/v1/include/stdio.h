#pragma once

#include <stdarg.h>
#include <stddef.h>

typedef struct sx_FILE FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)


FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
size_t fread(void* buffer, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fflush(FILE* stream);
int fprintf(FILE* stream, const char* format, ...);
int vfprintf(FILE* stream, const char* format, va_list args);
int printf(const char* format, ...);
int sprintf(char* buffer, const char* format, ...);
int vprintf(const char* format, va_list args);
int snprintf(char* buffer, size_t size, const char* format, ...);
int vsnprintf(char* buffer, size_t size, const char* format, va_list args);
char* fgets(char* buffer, int size, FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
int fputs(const char* text, FILE* stream);
int putc(int character, FILE* stream);
int putchar(int character);
int puts(const char* text);
int remove(const char* path);
int rename(const char* old_path, const char* new_path);
