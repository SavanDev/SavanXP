#pragma once

#include <stddef.h>

extern "C" void* memcpy(void* destination, const void* source, size_t count);
extern "C" void* memset(void* destination, int value, size_t count);
extern "C" void* memmove(void* destination, const void* source, size_t count);
extern "C" int memcmp(const void* left, const void* right, size_t count);

