#include "kernel/string.hpp"

#include <stddef.h>
#include <stdint.h>

extern "C" void* memcpy(void* destination, const void* source, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(source);

    for (size_t index = 0; index < count; ++index) {
        dst[index] = src[index];
    }

    return destination;
}

extern "C" void* memset(void* destination, int value, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);

    for (size_t index = 0; index < count; ++index) {
        dst[index] = static_cast<uint8_t>(value);
    }

    return destination;
}

extern "C" void* memmove(void* destination, const void* source, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(source);

    if (dst == src || count == 0) {
        return destination;
    }

    if (dst < src) {
        for (size_t index = 0; index < count; ++index) {
            dst[index] = src[index];
        }
        return destination;
    }

    for (size_t index = count; index > 0; --index) {
        dst[index - 1] = src[index - 1];
    }

    return destination;
}

extern "C" int memcmp(const void* left, const void* right, size_t count) {
    const auto* lhs = static_cast<const uint8_t*>(left);
    const auto* rhs = static_cast<const uint8_t*>(right);

    for (size_t index = 0; index < count; ++index) {
        if (lhs[index] != rhs[index]) {
            return lhs[index] < rhs[index] ? -1 : 1;
        }
    }

    return 0;
}

size_t strlen(const char* text) {
    size_t length = 0;
    while (text != nullptr && text[length] != '\0') {
        ++length;
    }
    return length;
}

int strcmp(const char* left, const char* right) {
    size_t index = 0;
    for (;;) {
        const unsigned char lhs = left != nullptr ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char rhs = right != nullptr ? static_cast<unsigned char>(right[index]) : 0;
        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
        if (lhs == 0) {
            return 0;
        }
        ++index;
    }
}

int strncmp(const char* left, const char* right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        const unsigned char lhs = left != nullptr ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char rhs = right != nullptr ? static_cast<unsigned char>(right[index]) : 0;
        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
        if (lhs == 0) {
            return 0;
        }
    }
    return 0;
}

char* strcpy(char* destination, const char* source) {
    size_t index = 0;
    while (source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return destination;
}

extern "C" int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

extern "C" void __cxa_pure_virtual() {
    for (;;) {
        asm volatile("hlt");
    }
}

void* __dso_handle = nullptr;
