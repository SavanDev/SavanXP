#pragma once

#include <stdint.h>

/*
 * Consola serie del bootstrap i386: COM1 a 115200 8N1, sin interrupciones.
 * Es el unico canal de diagnostico del arranque (docs/REAL_HARDWARE.md).
 */

namespace i386 {

namespace serial {

void init();
// IRQ traces stay on COM1: framebuffer scrolling in a keyboard handler can
// delay PIT delivery long enough to overflow the raw event queue.
bool set_console_mirroring(bool enabled);
class SerialOnly {
    bool previous_;
public:
    SerialOnly() : previous_(set_console_mirroring(false)) {}
    ~SerialOnly() { set_console_mirroring(previous_); }
};

void write_char(char character);
void write(const char* text);

void write_hex(uint32_t value);   /* 0x + 8 digitos */
void write_hex64(uint64_t value); /* 0x + 16 digitos */
void write_dec(uint32_t value);

}

}
