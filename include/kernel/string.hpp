#pragma once

#include <stddef.h>

extern "C" void* memcpy(void* destination, const void* source, size_t count);
extern "C" void* memset(void* destination, int value, size_t count);
extern "C" void* memmove(void* destination, const void* source, size_t count);
extern "C" int memcmp(const void* left, const void* right, size_t count);

size_t strlen(const char* text);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, size_t count);
char* strcpy(char* destination, const char* source);
