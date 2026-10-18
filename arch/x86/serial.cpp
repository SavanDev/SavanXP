#include "serial.hpp"

#include <stddef.h>

namespace i386 {

namespace {

constexpr uint16_t kPort = 0x3F8; /* COM1 */
constexpr uint16_t kData = kPort + 0;
constexpr uint16_t kInterruptEnable = kPort + 1;
constexpr uint16_t kFifoControl = kPort + 2;
constexpr uint16_t kLineControl = kPort + 3;
constexpr uint16_t kModemControl = kPort + 4;
constexpr uint16_t kLineStatus = kPort + 5;

constexpr uint8_t kLineStatusTransmitterEmpty = 0x20;

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

}

void serial::init() {
    outb(kInterruptEnable, 0x00); /* sin interrupciones */
    outb(kLineControl, 0x80);     /* DLAB: los dos primeros puertos son el divisor */
    outb(kData, 0x01);            /* 115200 baudios */
    outb(kInterruptEnable, 0x00);
    outb(kLineControl, 0x03);     /* 8 bits, sin paridad, 1 de parada */
    outb(kFifoControl, 0xC7);     /* FIFO on, limpiar, umbral 14 */
    outb(kModemControl, 0x03);    /* DTR + RTS */
}

void serial::write_char(char character) {
    /* La espera tiene limite: en una maquina sin UART el arranque no se
     * cuelga, solo pierde las lineas. */
    for (uint32_t spin = 0; spin < 65536; ++spin) {
        if ((inb(kLineStatus) & kLineStatusTransmitterEmpty) != 0) {
            break;
        }
    }
    outb(kData, static_cast<uint8_t>(character));
}

void serial::write(const char* text) {
    for (; *text != '\0'; ++text) {
        if (*text == '\n') {
            write_char('\r');
        }
        write_char(*text);
    }
}

void serial::write_hex(uint32_t value) {
    static const char kDigits[] = "0123456789abcdef";
    write("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        write_char(kDigits[(value >> shift) & 0xFu]);
    }
}

void serial::write_hex64(uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        write_char(kDigits[(value >> shift) & 0xFu]);
    }
}

void serial::write_dec(uint32_t value) {
    char buffer[10];
    size_t length = 0;
    do {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && length < sizeof(buffer));
    while (length > 0) {
        write_char(buffer[--length]);
    }
}

}
