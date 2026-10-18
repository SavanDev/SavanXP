#pragma once

#include <stdint.h>

/*
 * Consola serie del bootstrap i386: COM1 a 115200 8N1, sin interrupciones.
 * Es el unico canal de diagnostico del arranque (docs/REAL_HARDWARE.md).
 */

namespace i386 {

namespace serial {

void init();

void write_char(char character);
void write(const char* text);

void write_hex(uint32_t value);   /* 0x + 8 digitos */
void write_hex64(uint64_t value); /* 0x + 16 digitos */
void write_dec(uint32_t value);

}

}
