#include "kernel/string.hpp"

#include <stddef.h>
#include <stdint.h>

// La palabra de las instrucciones de string depende del modo: 8 bytes en
// x86_64 y 4 en i386 (rep movsq/stosq no existen en modo de 32 bits).
#if defined(__x86_64__)
using CopyWord = uint64_t;
#else
using CopyWord = uint32_t;
#endif

// Desplazamiento de esa palabra (3 = 8 bytes, 2 = 4 bytes).
constexpr unsigned kCopyWordShift = sizeof(CopyWord) == 8 ? 3 : 2;

// Acceso que no promete alineacion ni exclusividad de aliasing: x86 permite
// loads/stores desalineados y may_alias evita que el optimizador asuma que
// esta palabra no pisa los bytes de al lado.
using UnalignedWord = CopyWord __attribute__((may_alias, aligned(1)));

// El kernel se compila con -mgeneral-regs-only (sin SSE), asi que la unidad de
// copia mas ancha disponible es CopyWord. En vez de un lazo en C -- que sin
// optimizacion cuesta media docena de instrucciones POR BYTE -- se usan las
// instrucciones de string, que mueven esa palabra por iteracion de microcodigo
// y no pagan overhead de lazo. Es la diferencia entre arrastrar los 4 MiB de un
// frame de 1280x800 de a un byte o de a palabra.
//
// Requisito: DF=0. Lo garantizan los cld de los puntos de entrada al kernel
// (entry32.S en i386; context.S y las macros DEFINE_ISR_* de cpu_init.cpp en
// x86_64).
extern "C" void* memcpy(void* destination, const void* source, size_t count) {
    void* cursor = destination;
    const void* origin = source;
    size_t words = count >> kCopyWordShift;
    size_t tail = count & ((1u << kCopyWordShift) - 1u);

#if defined(__x86_64__)
    asm volatile("rep movsq" : "+D"(cursor), "+S"(origin), "+c"(words) : : "memory");
#else
    asm volatile("rep movsl" : "+D"(cursor), "+S"(origin), "+c"(words) : : "memory");
#endif
    asm volatile("rep movsb" : "+D"(cursor), "+S"(origin), "+c"(tail) : : "memory");

    return destination;
}

extern "C" void* memset(void* destination, int value, size_t count) {
    const uint8_t byte = static_cast<uint8_t>(value);
    // Replica el byte en todas las posiciones para que la instruccion de string
    // escriba el mismo patron.
#if defined(__x86_64__)
    const CopyWord pattern = static_cast<CopyWord>(byte) * 0x0101010101010101ULL;
#else
    const CopyWord pattern = static_cast<CopyWord>(byte) * 0x01010101u;
#endif
    void* cursor = destination;
    size_t words = count >> kCopyWordShift;
    size_t tail = count & ((1u << kCopyWordShift) - 1u);

#if defined(__x86_64__)
    asm volatile("rep stosq" : "+D"(cursor), "+c"(words) : "a"(pattern) : "memory");
#else
    asm volatile("rep stosl" : "+D"(cursor), "+c"(words) : "a"(pattern) : "memory");
#endif
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

    // Solape hacia atras. Se copia descendente de a palabra a mano en vez de
    // prender DF y usar rep movs: dejar la bandera prendida aunque sea un
    // instante rompe cualquier interrupcion que entre en el medio.
    size_t remaining = count;
    while (remaining >= sizeof(CopyWord)) {
        remaining -= sizeof(CopyWord);
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
