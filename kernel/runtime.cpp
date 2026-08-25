#include "kernel/string.hpp"

#include <stddef.h>
#include <stdint.h>

// Acceso de 64 bits que no promete alineacion ni exclusividad de aliasing: x86
// permite loads/stores desalineados y may_alias evita que el optimizador asuma
// que este uint64_t no pisa los bytes de al lado.
using UnalignedWord = uint64_t __attribute__((may_alias, aligned(1)));

// El kernel se compila con -mgeneral-regs-only (sin SSE), asi que la unidad de
// copia mas ancha disponible son 8 bytes. En vez de un lazo en C -- que sin
// optimizacion cuesta media docena de instrucciones POR BYTE -- se usan las
// instrucciones de string, que mueven 8 bytes por iteracion de microcodigo y no
// pagan overhead de lazo. Es la diferencia entre arrastrar los 4 MiB de un
// frame de 1280x800 de a un byte o de a ocho.
//
// Requisito: DF=0. Lo garantizan los cld de los puntos de entrada al kernel
// (context.S y las macros DEFINE_ISR_* de cpu_init.cpp).
extern "C" void* memcpy(void* destination, const void* source, size_t count) {
    void* cursor = destination;
    const void* origin = source;
    size_t words = count >> 3;
    size_t tail = count & 7u;

    asm volatile("rep movsq" : "+D"(cursor), "+S"(origin), "+c"(words) : : "memory");
    asm volatile("rep movsb" : "+D"(cursor), "+S"(origin), "+c"(tail) : : "memory");

    return destination;
}

extern "C" void* memset(void* destination, int value, size_t count) {
    const uint8_t byte = static_cast<uint8_t>(value);
    // Replica el byte en las 8 posiciones para que stosq escriba el mismo patron.
    const uint64_t pattern = static_cast<uint64_t>(byte) * 0x0101010101010101ULL;
    void* cursor = destination;
    size_t words = count >> 3;
    size_t tail = count & 7u;

    asm volatile("rep stosq" : "+D"(cursor), "+c"(words) : "a"(pattern) : "memory");
    asm volatile("rep stosb" : "+D"(cursor), "+c"(tail) : "a"(byte) : "memory");

    return destination;
}

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
    for (;;) {
        asm volatile("hlt");
    }
}

void* __dso_handle = nullptr;
