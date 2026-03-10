#include "kernel/ps2.hpp"

#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"
#include "shared/syscall.h"

namespace {

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;
constexpr uint16_t kCommandPort = 0x64;
constexpr uint8_t kStatusOutputReady = 1u << 0;
constexpr uint8_t kStatusInputBusy = 1u << 1;

bool g_ready = false;
bool g_extended = false;
bool g_alt_gr_pressed = false;

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool wait_input_ready() {
    for (uint32_t spin = 0; spin < 100000; ++spin) {
        if ((in8(kStatusPort) & kStatusInputBusy) == 0) {
            return true;
        }
    }
    return false;
}

bool wait_output_ready() {
    for (uint32_t spin = 0; spin < 100000; ++spin) {
        if ((in8(kStatusPort) & kStatusOutputReady) != 0) {
            return true;
        }
    }
    return false;
}

void controller_write(uint8_t value) {
    if (!wait_input_ready()) {
        return;
    }
    out8(kCommandPort, value);
}

void data_write(uint8_t value) {
    if (!wait_input_ready()) {
        return;
    }
    out8(kDataPort, value);
}

uint8_t data_read() {
    if (!wait_output_ready()) {
        return 0;
    }
    return in8(kDataPort);
}

char translate_scancode(uint8_t scancode, bool shifted, bool alt_gr) {
    if (scancode == 0x56) {
        if (alt_gr) {
            return '|';
        }
        return shifted ? '>' : '<';
    }

    static const char kNormal[] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z',
        'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
    };
    static const char kShifted[] = {
        0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z',
        'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0
    };

    if (scancode >= sizeof(kNormal)) {
        return 0;
    }
    return shifted ? kShifted[scancode] : kNormal[scancode];
}

uint32_t special_key(uint8_t scancode, bool extended) {
    if (extended) {
        switch (scancode) {
            case 0x48: return SAVANXP_KEY_UP;
            case 0x50: return SAVANXP_KEY_DOWN;
            case 0x4b: return SAVANXP_KEY_LEFT;
            case 0x4d: return SAVANXP_KEY_RIGHT;
            case 0x47: return SAVANXP_KEY_HOME;
            case 0x4f: return SAVANXP_KEY_END;
            case 0x49: return SAVANXP_KEY_PAGE_UP;
            case 0x51: return SAVANXP_KEY_PAGE_DOWN;
            case 0x52: return SAVANXP_KEY_INSERT;
            case 0x53: return SAVANXP_KEY_DELETE;
            case 0x1d: return SAVANXP_KEY_CTRL;
            case 0x38: return SAVANXP_KEY_ALT;
            default: return SAVANXP_KEY_NONE;
        }
    }

    switch (scancode) {
        case 0x01: return SAVANXP_KEY_ESC;
        case 0x0e: return SAVANXP_KEY_BACKSPACE;
        case 0x0f: return SAVANXP_KEY_TAB;
        case 0x1c: return SAVANXP_KEY_ENTER;
        case 0x2a:
        case 0x36:
            return SAVANXP_KEY_SHIFT;
        case 0x1d:
            return SAVANXP_KEY_CTRL;
        case 0x38:
            return SAVANXP_KEY_ALT;
        case 0x3a:
            return SAVANXP_KEY_CAPSLOCK;
        case 0x3b: return SAVANXP_KEY_F1;
        case 0x3c: return SAVANXP_KEY_F2;
        case 0x3d: return SAVANXP_KEY_F3;
        case 0x3e: return SAVANXP_KEY_F4;
        case 0x3f: return SAVANXP_KEY_F5;
        case 0x40: return SAVANXP_KEY_F6;
        case 0x41: return SAVANXP_KEY_F7;
        case 0x42: return SAVANXP_KEY_F8;
        case 0x43: return SAVANXP_KEY_F9;
        case 0x44: return SAVANXP_KEY_F10;
        case 0x57: return SAVANXP_KEY_F11;
        case 0x58: return SAVANXP_KEY_F12;
        default: return SAVANXP_KEY_NONE;
    }
}

void process_scancode(uint8_t scancode) {
    if (scancode == 0xe0) {
        g_extended = true;
        return;
    }

    tty::TtyDevice& device = tty::main();
    const bool released = (scancode & 0x80u) != 0;
    const uint8_t key = static_cast<uint8_t>(scancode & 0x7fu);

    if (g_extended) {
        if (key == 0x38) {
            g_alt_gr_pressed = !released;
        }

        const uint32_t key_code = special_key(key, true);
        if (key_code != SAVANXP_KEY_NONE) {
            ui::handle_key_event(key_code, !released, 0);
            g_extended = false;
            return;
        }
        g_extended = false;
        return;
    }

    const uint32_t key_code = special_key(key, false);
    if (key_code == SAVANXP_KEY_SHIFT) {
        device.shift_pressed = !released;
    }
    if (key_code == SAVANXP_KEY_CTRL) {
        device.ctrl_pressed = !released;
    }
    if (ui::graphics_active()) {
        const char translated = translate_scancode(key, device.shift_pressed, g_alt_gr_pressed);
        ui::handle_key_event(
            key_code != SAVANXP_KEY_NONE ? key_code : static_cast<uint32_t>(static_cast<uint8_t>(translated)),
            !released,
            released ? 0 : translated
        );
        return;
    }

    if (released) {
        return;
    }

    if (key_code == SAVANXP_KEY_BACKSPACE) {
        tty::handle_backspace();
        return;
    }

    if (key_code == SAVANXP_KEY_ENTER) {
        tty::submit_line();
        return;
    }

    const char translated = translate_scancode(key, device.shift_pressed, g_alt_gr_pressed);
    if (device.ctrl_pressed) {
        if (translated == 'l' || translated == 'L') {
            tty::clear();
            return;
        }
    }

    tty::handle_input_char(translated);
}

void keyboard_irq() {
    process_scancode(in8(kDataPort));
}

void initialize_controller() {
    while ((in8(kStatusPort) & kStatusOutputReady) != 0) {
        (void)in8(kDataPort);
    }

    controller_write(0xad);
    controller_write(0xa7);
    controller_write(0x20);
    uint8_t config = data_read();
    config |= 0x01;
    config &= static_cast<uint8_t>(~0x10u);
    controller_write(0x60);
    data_write(config);
    controller_write(0xae);
    data_write(0xf4);
    (void)data_read();
}

} // namespace

namespace ps2 {

void initialize() {
    g_ready = false;
    initialize_controller();

    if (!arch::x86_64::register_irq_handler(1, keyboard_irq)) {
        console::write_line("ps2: failed to register irq1");
        return;
    }

    arch::x86_64::enable_irq(1);
    g_ready = true;
}

bool ready() {
    return g_ready;
}

void poll() {
    while ((in8(kStatusPort) & kStatusOutputReady) != 0) {
        process_scancode(in8(kDataPort));
    }
}

} // namespace ps2
