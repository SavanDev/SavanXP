/*
 * kernel_string_stubs.cpp -- Implementaciones de kernel/string.hpp para los
 * tests de host. En el kernel viven en runtime.cpp, que no se puede linkear
 * aca, y no alcanza con la libc del host: las de string.hpp tienen linkage C++
 * y chocarian con las declaraciones C de <string.h>. Por eso este TU las define
 * a mano y NO incluye <string.h>. Las mem* si son extern "C" y las pone la libc.
 *
 * Va aparte de los stubs de cada test porque lo necesitan todos: dos copias de
 * esto es exactamente la clase de duplicacion que ya costo un bug en el bitmap
 * de SxFS.
 */

#include "kernel/string.hpp"

size_t strlen(const char* text) {
    size_t length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

int strcmp(const char* left, const char* right) {
    while (*left != 0 && *left == *right) {
        ++left;
        ++right;
    }
    return static_cast<int>(static_cast<unsigned char>(*left)) -
        static_cast<int>(static_cast<unsigned char>(*right));
}

int strncmp(const char* left, const char* right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if (a != b || a == 0) {
            return static_cast<int>(a) - static_cast<int>(b);
        }
    }
    return 0;
}

char* strcpy(char* destination, const char* source) {
    char* cursor = destination;
    while ((*cursor++ = *source++) != 0) {
    }
    return destination;
}
