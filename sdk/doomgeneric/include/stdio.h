#pragma once

#include <stdarg.h>
#include <stddef.h>

typedef struct sx_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)

#define fopen sx_fopen
#define fclose sx_fclose
#define fread sx_fread
#define fwrite sx_fwrite
#define fseek sx_fseek
#define ftell sx_ftell
#define fflush sx_fflush
#define fprintf sx_fprintf
#define vfprintf sx_vfprintf
#define printf sx_printf
#define snprintf sx_snprintf
#define vsnprintf sx_vsnprintf
#define fgets sx_fgets
#define feof sx_feof
#define putchar sx_putchar
#define puts sx_puts
#define remove sx_remove
#define rename sx_rename

FILE *sx_fopen(const char *path, const char *mode);
int sx_fclose(FILE *stream);
size_t sx_fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t sx_fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int sx_fseek(FILE *stream, long offset, int whence);
long sx_ftell(FILE *stream);
int sx_fflush(FILE *stream);
int sx_fprintf(FILE *stream, const char *format, ...);
int sx_vfprintf(FILE *stream, const char *format, va_list args);
int sx_printf(const char *format, ...);
int sx_snprintf(char *buffer, size_t size, const char *format, ...);
int sx_vsnprintf(char *buffer, size_t size, const char *format, va_list args);
char *sx_fgets(char *buffer, int size, FILE *stream);
int sx_feof(FILE *stream);
int sx_putchar(int character);
int sx_puts(const char *text);
int sx_remove(const char *path);
int sx_rename(const char *old_path, const char *new_path);
