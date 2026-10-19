#include "egatext.hpp"

namespace i386 {

namespace egatext {

namespace {

bool g_ready = false;
uint16_t g_column = 0;
uint16_t g_row = 0;

inline volatile uint16_t* text_memory() {
    return reinterpret_cast<volatile uint16_t*>(kTextBase);
}

void put_entry(uint16_t column, uint16_t row, char character) {
    text_memory()[static_cast<uint32_t>(row) * kColumns + column] =
        static_cast<uint16_t>((static_cast<uint16_t>(kAttribute) << 8u) |
                              static_cast<uint8_t>(character));
}

void scroll_up() {
    volatile uint16_t* memory = text_memory();
    for (uint16_t row = 1; row < kRows; ++row) {
        for (uint16_t column = 0; column < kColumns; ++column) {
            memory[static_cast<uint32_t>(row - 1) * kColumns + column] =
                memory[static_cast<uint32_t>(row) * kColumns + column];
        }
    }
    for (uint16_t column = 0; column < kColumns; ++column) {
        put_entry(column, static_cast<uint16_t>(kRows - 1), ' ');
    }
    if (g_row > 0) {
        --g_row;
    }
}

}  // namespace

void initialize(uint32_t width, uint32_t height, uint8_t color_type, uint64_t address) {
    g_ready = (width == kColumns && height == kRows && color_type == kTypeEgaText &&
               address == kTextBase);
    g_column = 0;
    g_row = 0;
    if (g_ready) {
        clear();
    }
}

bool ready() {
    return g_ready;
}

void clear() {
    if (!g_ready) {
        return;
    }
    for (uint16_t row = 0; row < kRows; ++row) {
        for (uint16_t column = 0; column < kColumns; ++column) {
            put_entry(column, row, ' ');
        }
    }
    g_column = 0;
    g_row = 0;
}

void write_char(char character) {
    if (!g_ready) {
        return;
    }
    if (character == '\r') {
        g_column = 0;
        return;
    }
    if (character == '\n') {
        /* A diferencia del hardware VGA, aqui el '\n' solo baja: serial::
         * manda \r\n, pero hay llamadas directas con '\n' y dejar la columna
         * donde estuviera cortaria la linea en el medio. */
        g_column = 0;
        if (++g_row >= kRows) {
            scroll_up();
        }
        return;
    }
    if (character == '\b') {
        if (g_column > 0) {
            --g_column;
            put_entry(g_column, g_row, ' ');
        }
        return;
    }
    put_entry(g_column, g_row, character);
    if (++g_column >= kColumns) {
        g_column = 0;
        if (++g_row >= kRows) {
            scroll_up();
        }
    }
}

void write(const char* text) {
    for (; *text != '\0'; ++text) {
        write_char(*text);
    }
}

}  // namespace egatext

}  // namespace i386
