#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tty {

struct TtyDevice {
    char pending_line[256];
    size_t pending_length;
    bool line_ready;
    bool shift_pressed;
    bool ctrl_pressed;
    bool extended_prefix;
};

void initialize();
TtyDevice& main();
void clear();
void handle_input_char(char character);
void handle_backspace();
void submit_line();
size_t read_line(char* buffer, size_t capacity);
void write(const char* text);
void write_char(char character);

} // namespace tty
