#include "kernel/string.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.hpp"

// Acceso de 64 bits que no promete alineacion ni exclusividad de aliasing: x86
// permite loads/stores desalineados y may_alias evita que el optimizador asuma
// que este uint64_t no pisa los bytes de al lado.
using UnalignedWord = uint64_t __attribute__((may_alias, aligned(1)));

extern "C" void* memmove(void* destination, const void* source, size_t count) {
    auto* dst = static_cast<uint8_t*>(destination);
    const auto* src = static_cast<const uint8_t*>(source);

    if (dst == src || count == 0) {
        return destination;
    }

    // Sin solape hacia adelante: el movsq ascendente de memcpy es correcto.
    if (dst < src) {
        return memcpy(destination, source, count);
    }

    // Solape hacia atras. Se copia descendente de a 8 bytes a mano en vez de
    // prender DF y usar rep movs: dejar la bandera prendida aunque sea un
    // instante rompe cualquier interrupcion que entre en el medio.
    size_t remaining = count;
    while (remaining >= sizeof(uint64_t)) {
        remaining -= sizeof(uint64_t);
        *reinterpret_cast<UnalignedWord*>(dst + remaining) =
            *reinterpret_cast<const UnalignedWord*>(src + remaining);
    }
    while (remaining > 0) {
        --remaining;
        dst[remaining] = src[remaining];
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
    arch::halt_forever();
}

void* __dso_handle = nullptr;
