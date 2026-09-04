#pragma once

#include <stdarg.h>
#include <stddef.h>

#include "sys/types.h"

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

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 512

int fgetc(FILE* stream);
int getc(FILE* stream);
int getchar(void);
int fputc(int character, FILE* stream);
int ungetc(int character, FILE* stream);
int fileno(FILE* stream);
FILE* fdopen(int fd, const char* mode);
int setvbuf(FILE* stream, char* buffer, int mode, size_t size);
void setbuf(FILE* stream, char* buffer);
void rewind(FILE* stream);
int vsprintf(char* buffer, const char* format, va_list args);
void perror(const char* prefix);

/* off_t es de 64 bits, asi que fseeko/ftello no agregan alcance sobre
 * fseek/ftell; existen porque un port las nombra. El tope real de tamano de
 * archivo lo pone el st_size de 32 bits del kernel. */
int fseeko(FILE* stream, off_t offset, int whence);
off_t ftello(FILE* stream);
int sscanf(const char* input, const char* format, ...);
int vsscanf(const char* input, const char* format, va_list args);
