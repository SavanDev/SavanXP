#pragma once

#include <stddef.h>


void* memcpy(void* destination, const void* source, size_t count);
void* mempcpy(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);
int memcmp(const void* left, const void* right, size_t count);
void* memmove(void* destination, const void* source, size_t count);
size_t strlen(const char* text);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, size_t count);
char* strcpy(char* destination, const char* source);
char* strncpy(char* destination, const char* source, size_t count);
char* strchr(const char* text, int character);
char* strchrnul(const char* text, int character);
char* strpbrk(const char* text, const char* accept);
char* strrchr(const char* text, int character);
char* strstr(const char* haystack, const char* needle);
size_t strcspn(const char* text, const char* reject);
size_t strspn(const char* text, const char* accept);
char* strerror(int error_number);
char* strtok_r(char* text, const char* delimiters, char** save_ptr);
char* stpncpy(char* destination, const char* source, size_t count);
char* strdup(const char* text);
void* memchr(const void* block, int value, size_t count);
size_t strnlen(const char* text, size_t limit);
char* strcat(char* destination, const char* source);
char* strncat(char* destination, const char* source, size_t count);
char* strtok(char* text, const char* delimiters);
char* strsep(char** text, const char* delimiters);
char* strcasestr(const char* haystack, const char* needle);
