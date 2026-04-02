#include "kernel/tty.hpp"

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/input.hpp"
#include "kernel/process.hpp"
#include "kernel/string.hpp"
#include "shared/syscall.h"

namespace {

tty::TtyDevice g_main_tty = {};

} // namespace

namespace tty {

void initialize() {
    memset(&g_main_tty, 0, sizeof(g_main_tty));
}

TtyDevice& main() {
    return g_main_tty;
}

void clear() {
    g_main_tty.pending_length = 0;
    g_main_tty.line_ready = false;
    console::clear();
}

void handle_key_event(const input::KeyEvent& event) {
    if (!event.pressed) {
        return;
    }

    if (event.key == SAVANXP_KEY_BACKSPACE) {
        handle_backspace();
        return;
    }

    if (event.key == SAVANXP_KEY_ENTER) {
        submit_line();
        return;
    }

    if ((event.modifiers & input::modifier_ctrl) != 0 &&
        (event.ascii == 'l' || event.ascii == 'L')) {
        clear();
        return;
    }

    handle_input_char(event.ascii);
}

void handle_input_char(char character) {
    if (character == '\0') {
        return;
    }

    if (g_main_tty.pending_length + 1 >= sizeof(g_main_tty.pending_line)) {
        return;
    }

    g_main_tty.pending_line[g_main_tty.pending_length++] = character;
    console::write_char(character);
}

void handle_backspace() {
    if (g_main_tty.pending_length == 0) {
        return;
    }

    --g_main_tty.pending_length;
    g_main_tty.pending_line[g_main_tty.pending_length] = '\0';
    console::write_char('\b');
}

void submit_line() {
    console::write_char('\n');
    g_main_tty.pending_line[g_main_tty.pending_length] = '\0';
    g_main_tty.line_ready = true;
    process::notify_tty_line_ready();
}

size_t read_line(char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }

    while (!g_main_tty.line_ready) {
        device::service_background();
        input::poll();
        arch::x86_64::enable_interrupts();
        arch::x86_64::halt_once();
    }

    const size_t to_copy = g_main_tty.pending_length < (capacity - 1)
        ? g_main_tty.pending_length
        : (capacity - 1);
    memcpy(buffer, g_main_tty.pending_line, to_copy);
    buffer[to_copy] = '\0';

    memset(g_main_tty.pending_line, 0, sizeof(g_main_tty.pending_line));
    g_main_tty.pending_length = 0;
    g_main_tty.line_ready = false;
    return to_copy;
}

void write(const char* text) {
    console::write(text);
}

void write_char(char character) {
    console::write_char(character);
}

} // namespace tty
