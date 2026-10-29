#include "kernel/input.hpp"

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_abi.h"

/* The keyboard core: what a key means, independent of the device that
 * reported it. Drivers hand in set 1 positions (input::submit_scancode); this
 * file keeps the modifier and lock state, applies the layout and emits
 * input::KeyEvent. See docs/kernel/INPUT.md. */

namespace {

bool g_left_shift_pressed = false;
bool g_right_shift_pressed = false;
bool g_left_ctrl_pressed = false;
bool g_right_ctrl_pressed = false;
bool g_left_alt_pressed = false;
bool g_right_alt_pressed = false;
bool g_caps_lock_enabled = false;
bool g_num_lock_enabled = false;
bool g_scroll_lock_enabled = false;
// Which keys are down, by set 1 position (bit 7 for the 0xE0 ones): a make
// code for a key already down is the keyboard repeating it.
bool g_keys_down[256] = {};

bool keyboard_shift_active() {
    return g_left_shift_pressed || g_right_shift_pressed;
}

bool keyboard_ctrl_active() {
    return g_left_ctrl_pressed || g_right_ctrl_pressed;
}

bool keyboard_alt_active() {
    return g_left_alt_pressed || g_right_alt_pressed;
}

bool keyboard_alt_gr_active() {
    return g_right_alt_pressed;
}

uint32_t lock_modifiers() {
    uint32_t modifiers = 0;
    if (g_caps_lock_enabled) {
        modifiers |= input::modifier_caps_lock;
    }
    if (g_num_lock_enabled) {
        modifiers |= input::modifier_num_lock;
    }
    if (g_scroll_lock_enabled) {
        modifiers |= input::modifier_scroll_lock;
    }
    return modifiers;
}

uint32_t current_modifiers() {
    uint32_t modifiers = lock_modifiers();
    if (keyboard_shift_active()) {
        modifiers |= input::modifier_shift;
    }
    if (keyboard_ctrl_active()) {
        modifiers |= input::modifier_ctrl;
    }
    if (keyboard_alt_active()) {
        modifiers |= input::modifier_alt;
    }
    if (keyboard_alt_gr_active()) {
        modifiers |= input::modifier_alt_gr;
    }
    return modifiers;
}

bool is_alpha_character(char value) {
    return value >= 'a' && value <= 'z';
}

/* Scancode -> ASCII tables, one family per layout, indexed by set 1
 * scancode. kNormalEs/kShiftedEs/kAltGrEs are the original layout.
 * kNormalEn/kShiftedEn/kAltGrEn are standard US QWERTY derived from the
 * canonical set 1; English has no AltGr, so that table is all zeros and falls
 * back to normal/shifted below. */
const char kNormalEs[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 0, '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '\'', '+', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '{', '|', 0, '}', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' ', 0
};
const char kShiftedEs[] = {
    0, 27, '!', '"', '#', '$', '%', '&', '/', '(', ')', '=', '?', 0, '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '"', '*', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '[', 0, 0, ']', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' ', 0
};
const char kAltGrEs[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0,
    0, '@', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '~', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '^', 0, 0, '`', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '*', 0, ' ', 0
};
const char kNormalEn[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z',
    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
};
const char kShiftedEn[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0
};
const char kAltGrEn[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

struct LayoutTables {
    const char* normal;
    const char* shifted;
    const char* alt_gr;
};

const LayoutTables kLayoutTables[2] = {
    {kNormalEs, kShiftedEs, kAltGrEs},
    {kNormalEn, kShiftedEn, kAltGrEn},
};

/* The layout is a user preference, not key state: it lives apart from the
 * modifier flags above and nothing that resets them sends it back to
 * English. */
savanxp_keyboard_layout g_active_layout = SAVANXP_KEYBOARD_LAYOUT_ES;

char translate_main_block(uint8_t scancode, bool shift_active, bool caps_lock_active, bool alt_gr_active) {
    const LayoutTables& tables = kLayoutTables[g_active_layout];

    if (scancode >= sizeof(kNormalEs)) {
        return 0;
    }

    if (alt_gr_active && tables.alt_gr[scancode] != 0) {
        return tables.alt_gr[scancode];
    }

    const char normal = tables.normal[scancode];
    const char shifted = tables.shifted[scancode];
    if (normal == 0) {
        return 0;
    }

    if (is_alpha_character(normal)) {
        const bool uppercase = shift_active != caps_lock_active;
        return uppercase ? shifted : normal;
    }

    return shift_active ? shifted : normal;
}

char translate_keypad(uint8_t scancode, bool shift_active, bool num_lock_active) {
    switch (scancode) {
        case 0x37: return '*';
        case 0x4a: return '-';
        case 0x4e: return '+';
        case 0x47: return (!shift_active && num_lock_active) ? '7' : 0;
        case 0x48: return (!shift_active && num_lock_active) ? '8' : 0;
        case 0x49: return (!shift_active && num_lock_active) ? '9' : 0;
        case 0x4b: return (!shift_active && num_lock_active) ? '4' : 0;
        case 0x4c: return (!shift_active && num_lock_active) ? '5' : 0;
        case 0x4d: return (!shift_active && num_lock_active) ? '6' : 0;
        case 0x4f: return (!shift_active && num_lock_active) ? '1' : 0;
        case 0x50: return (!shift_active && num_lock_active) ? '2' : 0;
        case 0x51: return (!shift_active && num_lock_active) ? '3' : 0;
        case 0x52: return (!shift_active && num_lock_active) ? '0' : 0;
        case 0x53: return (!shift_active && num_lock_active) ? '.' : 0;
        default: return 0;
    }
}

char translate_ascii(uint8_t scancode, bool extended) {
    const bool shift_active = keyboard_shift_active();
    if (extended) {
        switch (scancode) {
            case 0x1c: return '\n';
            case 0x35: return '/';
            default: return 0;
        }
    }

    const char keypad = translate_keypad(scancode, shift_active, g_num_lock_enabled);
    if (keypad != 0) {
        return keypad;
    }

    return translate_main_block(scancode, shift_active, g_caps_lock_enabled, keyboard_alt_gr_active());
}

uint32_t translate_key_code(uint8_t scancode, bool extended) {
    if (extended) {
        switch (scancode) {
            case 0x1c: return SAVANXP_KEY_ENTER;
            case 0x1d: return SAVANXP_KEY_CTRL;
            case 0x35: return SAVANXP_KEY_NONE;
            case 0x38: return SAVANXP_KEY_ALT_GR;
            case 0x47: return SAVANXP_KEY_HOME;
            case 0x48: return SAVANXP_KEY_UP;
            case 0x49: return SAVANXP_KEY_PAGE_UP;
            case 0x4b: return SAVANXP_KEY_LEFT;
            case 0x4d: return SAVANXP_KEY_RIGHT;
            case 0x4f: return SAVANXP_KEY_END;
            case 0x50: return SAVANXP_KEY_DOWN;
            case 0x51: return SAVANXP_KEY_PAGE_DOWN;
            case 0x52: return SAVANXP_KEY_INSERT;
            case 0x53: return SAVANXP_KEY_DELETE;
            case 0x5b:
            case 0x5c:
                return SAVANXP_KEY_SUPER;
            case 0x5d:
                return SAVANXP_KEY_MENU;
            default:
                return SAVANXP_KEY_NONE;
        }
    }

    switch (scancode) {
        case 0x01: return SAVANXP_KEY_ESC;
        case 0x0e: return SAVANXP_KEY_BACKSPACE;
        case 0x0f: return SAVANXP_KEY_TAB;
        case 0x1c: return SAVANXP_KEY_ENTER;
        case 0x1d: return SAVANXP_KEY_CTRL;
        case 0x2a:
        case 0x36:
            return SAVANXP_KEY_SHIFT;
        case 0x38: return SAVANXP_KEY_ALT;
        case 0x3a: return SAVANXP_KEY_CAPSLOCK;
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
        case 0x45: return SAVANXP_KEY_NUMLOCK;
        case 0x46: return SAVANXP_KEY_SCROLLLOCK;
        case 0x47: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_HOME;
        case 0x48: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_UP;
        case 0x49: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_PAGE_UP;
        case 0x4b: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_LEFT;
        case 0x4d: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_RIGHT;
        case 0x4f: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_END;
        case 0x50: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_DOWN;
        case 0x51: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_PAGE_DOWN;
        case 0x52: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_INSERT;
        case 0x53: return g_num_lock_enabled ? SAVANXP_KEY_NONE : SAVANXP_KEY_DELETE;
        case 0x57: return SAVANXP_KEY_F11;
        case 0x58: return SAVANXP_KEY_F12;
        default: return SAVANXP_KEY_NONE;
    }
}

void emit_key_event(uint32_t key, bool pressed, char ascii_key, char ascii_text, bool repeat = false) {
    if (key == SAVANXP_KEY_NONE && ascii_key == 0) {
        return;
    }

    input::submit_key_event({
        .key = key != SAVANXP_KEY_NONE ? key : static_cast<uint32_t>(static_cast<uint8_t>(ascii_key)),
        .pressed = pressed,
        .ascii = static_cast<char>(pressed ? ascii_text : 0),
        .modifiers = current_modifiers() | (repeat ? input::modifier_repeat : 0u),
    });
}

void update_modifier_state(uint8_t scancode, bool extended, bool pressed) {
    if (extended) {
        switch (scancode) {
            case 0x1d:
                g_right_ctrl_pressed = pressed;
                return;
            case 0x38:
                g_right_alt_pressed = pressed;
                return;
            default:
                return;
        }
    }

    switch (scancode) {
        case 0x1d:
            g_left_ctrl_pressed = pressed;
            return;
        case 0x2a:
            g_left_shift_pressed = pressed;
            return;
        case 0x36:
            g_right_shift_pressed = pressed;
            return;
        case 0x38:
            g_left_alt_pressed = pressed;
            return;
        default:
            return;
    }
}

void update_lock_state(uint8_t scancode, bool pressed) {
    if (!pressed) {
        return;
    }

    switch (scancode) {
        case 0x3a:
            g_caps_lock_enabled = !g_caps_lock_enabled;
            break;
        case 0x45:
            g_num_lock_enabled = !g_num_lock_enabled;
            break;
        case 0x46:
            g_scroll_lock_enabled = !g_scroll_lock_enabled;
            break;
        default:
            return;
    }
    input::notify_keyboard_leds(lock_modifiers());
}

} // namespace

namespace input {

void submit_scancode(uint8_t scancode, bool extended, bool pressed) {
    scancode = static_cast<uint8_t>(scancode & 0x7fu);

    update_modifier_state(scancode, extended, pressed);
    if (!extended) {
        update_lock_state(scancode, pressed);
    }

    const size_t position = scancode | (extended ? 0x80u : 0u);
    const bool repeat = pressed && g_keys_down[position];
    g_keys_down[position] = pressed;

    const uint32_t key_code = translate_key_code(scancode, extended);
    const char translated = translate_ascii(scancode, extended);
    emit_key_event(key_code, pressed, translated, translated, repeat);
}

void submit_key(uint32_t key, bool pressed) {
    emit_key_event(key, pressed, 0, 0);
}

bool set_keyboard_layout(int layout) {
    if (layout != SAVANXP_KEYBOARD_LAYOUT_ES && layout != SAVANXP_KEYBOARD_LAYOUT_EN) {
        return false;
    }
    g_active_layout = static_cast<savanxp_keyboard_layout>(layout);
    return true;
}

int keyboard_layout() {
    return static_cast<int>(g_active_layout);
}

} // namespace input
