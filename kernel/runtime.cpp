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

extern "C" int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

extern "C" void __cxa_pure_virtual() {
    for (;;) {
        asm volatile("hlt");
    }
}

void* __dso_handle = nullptr;

