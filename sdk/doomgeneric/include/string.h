#pragma once

#include <stddef.h>

#define memcpy sx_memcpy
#define memset sx_memset
#define memcmp sx_memcmp
#define memmove sx_memmove
#define strlen sx_strlen
#define strcmp sx_strcmp
#define strncmp sx_strncmp
#define strcpy sx_strcpy
#define strncpy sx_strncpy
#define strchr sx_strchr
#define strrchr sx_strrchr
#define strstr sx_strstr
#define strdup sx_strdup

void *sx_memcpy(void *destination, const void *source, size_t count);
void *sx_memset(void *destination, int value, size_t count);
int sx_memcmp(const void *left, const void *right, size_t count);
void *sx_memmove(void *destination, const void *source, size_t count);
size_t sx_strlen(const char *text);
int sx_strcmp(const char *left, const char *right);
int sx_strncmp(const char *left, const char *right, size_t count);
char *sx_strcpy(char *destination, const char *source);
char *sx_strncpy(char *destination, const char *source, size_t count);
char *sx_strchr(const char *text, int character);
char *sx_strrchr(const char *text, int character);
char *sx_strstr(const char *haystack, const char *needle);
char *sx_strdup(const char *text);
