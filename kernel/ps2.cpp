#include "kernel/ps2.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/input.hpp"
#include "kernel/virtio_input.hpp"
#include "shared/syscall.h"

namespace {

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;
constexpr uint16_t kCommandPort = 0x64;

constexpr uint8_t kStatusOutputReady = 1u << 0;
constexpr uint8_t kStatusInputBusy = 1u << 1;
constexpr uint8_t kStatusAuxiliaryOutput = 1u << 5;

constexpr uint8_t kControllerCommandReadConfig = 0x20;
constexpr uint8_t kControllerCommandWriteConfig = 0x60;
constexpr uint8_t kControllerCommandWriteSecondPort = 0xd4;
constexpr uint8_t kControllerCommandSelfTest = 0xaa;
constexpr uint8_t kControllerCommandTestFirstPort = 0xab;
constexpr uint8_t kControllerCommandEnableSecondPort = 0xa8;
constexpr uint8_t kControllerCommandTestSecondPort = 0xa9;
constexpr uint8_t kControllerCommandDisableFirstPort = 0xad;
constexpr uint8_t kControllerCommandEnableFirstPort = 0xae;
constexpr uint8_t kControllerCommandDisableSecondPort = 0xa7;

constexpr uint8_t kConfigIrqFirstPort = 1u << 0;
constexpr uint8_t kConfigIrqSecondPort = 1u << 1;
constexpr uint8_t kConfigDisableFirstPortClock = 1u << 4;
constexpr uint8_t kConfigDisableSecondPortClock = 1u << 5;
constexpr uint8_t kConfigTranslation = 1u << 6;

constexpr uint8_t kKeyboardCommandSetLeds = 0xed;
constexpr uint8_t kKeyboardCommandSetScancode = 0xf0;
constexpr uint8_t kKeyboardCommandDisableScanning = 0xf5;
constexpr uint8_t kKeyboardCommandSetDefaults = 0xf6;
constexpr uint8_t kKeyboardCommandEnableScanning = 0xf4;
constexpr uint8_t kKeyboardCommandReset = 0xff;

constexpr uint8_t kMouseCommandSetDefaults = 0xf6;
constexpr uint8_t kMouseCommandEnableDataReporting = 0xf4;
constexpr uint8_t kMouseCommandReset = 0xff;

constexpr uint8_t kKeyboardResponseAck = 0xfa;
constexpr uint8_t kKeyboardResponseResend = 0xfe;
constexpr uint8_t kKeyboardResponseBatOk = 0xaa;
constexpr uint8_t kKeyboardResponseSelfTestFailed0 = 0xfc;
constexpr uint8_t kKeyboardResponseSelfTestFailed1 = 0xfd;
constexpr uint8_t kKeyboardResponseBufferError0 = 0x00;
constexpr uint8_t kKeyboardResponseBufferError1 = 0xff;

constexpr uint8_t kMouseResponseAck = 0xfa;
constexpr uint8_t kMouseResponseResend = 0xfe;
constexpr uint8_t kMouseResponseBatOk = 0xaa;

constexpr uint8_t kScancodeSet2 = 0x02;

constexpr size_t kRawQueueCapacity = 256;
constexpr uint32_t kIoWaitSpinCount = 100000;
constexpr uint32_t kCommandRetryCount = 3;
struct RawByte {
    uint8_t value;
    bool auxiliary;
};

bool g_ready = false;
bool g_mouse_ready = false;

RawByte g_raw_queue[kRawQueueCapacity] = {};
size_t g_raw_read_index = 0;
size_t g_raw_write_index = 0;
size_t g_raw_count = 0;
bool g_raw_overflow = false;

bool g_extended_prefix = false;
uint8_t g_pause_sequence_index = 0;
uint8_t g_print_screen_state = 0;

bool g_left_shift_pressed = false;
bool g_right_shift_pressed = false;
bool g_left_ctrl_pressed = false;
bool g_right_ctrl_pressed = false;
bool g_left_alt_pressed = false;
bool g_right_alt_pressed = false;
bool g_caps_lock_enabled = false;
bool g_num_lock_enabled = false;
bool g_scroll_lock_enabled = false;
bool g_led_sync_pending = false;

uint8_t g_mouse_packet[3] = {};
uint8_t g_mouse_packet_index = 0;

inline void out8(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t in8(uint16_t port) {
    uint8_t value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool wait_input_ready() {
    for (uint32_t spin = 0; spin < kIoWaitSpinCount; ++spin) {
        if ((in8(kStatusPort) & kStatusInputBusy) == 0) {
            return true;
        }
    }
    return false;
}

bool wait_output_ready() {
    for (uint32_t spin = 0; spin < kIoWaitSpinCount; ++spin) {
        if ((in8(kStatusPort) & kStatusOutputReady) != 0) {
            return true;
        }
    }
    return false;
}

bool controller_write(uint8_t value) {
    if (!wait_input_ready()) {
        return false;
    }
    out8(kCommandPort, value);
    return true;
}

bool data_write(uint8_t value) {
    if (!wait_input_ready()) {
        return false;
    }
    out8(kDataPort, value);
    return true;
}

bool data_read(uint8_t& value) {
    if (!wait_output_ready()) {
        return false;
    }
    value = in8(kDataPort);
    return true;
}

bool read_output_byte(uint8_t& value, bool& auxiliary) {
    if (!wait_output_ready()) {
        return false;
    }

    const uint8_t status = in8(kStatusPort);
    auxiliary = (status & kStatusAuxiliaryOutput) != 0;
    value = in8(kDataPort);
    return true;
}

void flush_controller_output() {
    while ((in8(kStatusPort) & kStatusOutputReady) != 0) {
        (void)in8(kDataPort);
    }
}

void reset_decoder_state() {
    g_extended_prefix = false;
    g_pause_sequence_index = 0;
    g_print_screen_state = 0;
}

void reset_input_state() {
    g_left_shift_pressed = false;
    g_right_shift_pressed = false;
    g_left_ctrl_pressed = false;
    g_right_ctrl_pressed = false;
    g_left_alt_pressed = false;
    g_right_alt_pressed = false;
    g_caps_lock_enabled = false;
    g_num_lock_enabled = false;
    g_scroll_lock_enabled = false;
    g_led_sync_pending = false;
    reset_decoder_state();
    g_mouse_packet[0] = 0;
    g_mouse_packet[1] = 0;
    g_mouse_packet[2] = 0;
    g_mouse_packet_index = 0;
}

void clear_raw_queue() {
    g_raw_read_index = 0;
    g_raw_write_index = 0;
    g_raw_count = 0;
    g_raw_overflow = false;
}

void enqueue_raw_byte(uint8_t value, bool auxiliary) {
    if (g_raw_count == kRawQueueCapacity) {
        g_raw_overflow = true;
        return;
    }

    g_raw_queue[g_raw_write_index] = {
        .value = value,
        .auxiliary = auxiliary,
    };
    g_raw_write_index = (g_raw_write_index + 1) % kRawQueueCapacity;
    g_raw_count += 1;
}

bool dequeue_raw_byte(RawByte& value) {
    if (g_raw_count == 0) {
        return false;
    }

    value = g_raw_queue[g_raw_read_index];
    g_raw_read_index = (g_raw_read_index + 1) % kRawQueueCapacity;
    g_raw_count -= 1;
    return true;
}

void drain_controller_output_to_queue() {
    while ((in8(kStatusPort) & kStatusOutputReady) != 0) {
        const uint8_t status = in8(kStatusPort);
        enqueue_raw_byte(in8(kDataPort), (status & kStatusAuxiliaryOutput) != 0);
    }
}

bool controller_read_config(uint8_t& config) {
    if (!controller_write(kControllerCommandReadConfig)) {
        return false;
    }
    return data_read(config);
}

bool controller_write_config(uint8_t config) {
    if (!controller_write(kControllerCommandWriteConfig)) {
        return false;
    }
    return data_write(config);
}

bool controller_expect_byte(uint8_t expected) {
    uint8_t value = 0;
    return data_read(value) && value == expected;
}

bool keyboard_wait_for_ack() {
    for (;;) {
        uint8_t value = 0;
        if (!data_read(value)) {
            return false;
        }

        if (value == kKeyboardResponseAck) {
            return true;
        }
        if (value == kKeyboardResponseResend ||
            value == kKeyboardResponseSelfTestFailed0 ||
            value == kKeyboardResponseSelfTestFailed1 ||
            value == kKeyboardResponseBufferError0 ||
            value == kKeyboardResponseBufferError1) {
            return false;
        }
    }
}

bool keyboard_send_byte_with_ack(uint8_t value) {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!data_write(value)) {
            return false;
        }
        if (keyboard_wait_for_ack()) {
            return true;
        }
    }
    return false;
}

bool keyboard_send_command(uint8_t command) {
    return keyboard_send_byte_with_ack(command);
}

bool keyboard_send_command(uint8_t command, uint8_t argument) {
    return keyboard_send_byte_with_ack(command) && keyboard_send_byte_with_ack(argument);
}

bool write_mouse_data(uint8_t value) {
    return controller_write(kControllerCommandWriteSecondPort) && data_write(value);
}

bool mouse_wait_for_ack() {
    for (;;) {
        uint8_t value = 0;
        bool auxiliary = false;
        if (!read_output_byte(value, auxiliary)) {
            return false;
        }
        if (!auxiliary) {
            continue;
        }
        if (value == kMouseResponseAck) {
            return true;
        }
        if (value == kMouseResponseResend) {
            return false;
        }
    }
}

bool mouse_send_byte_with_ack(uint8_t value) {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!write_mouse_data(value)) {
            return false;
        }
        if (mouse_wait_for_ack()) {
            return true;
        }
    }
    return false;
}

bool keyboard_reset_device() {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!data_write(kKeyboardCommandReset)) {
            return false;
        }

        uint8_t response = 0;
        if (!data_read(response)) {
            return false;
        }
        if (response == kKeyboardResponseResend) {
            continue;
        }
        if (response != kKeyboardResponseAck) {
            return false;
        }

        if (!data_read(response)) {
            return false;
        }
        return response == kKeyboardResponseBatOk;
    }
    return false;
}

bool mouse_reset_device() {
    for (uint32_t attempt = 0; attempt < kCommandRetryCount; ++attempt) {
        if (!write_mouse_data(kMouseCommandReset)) {
            return false;
        }

        uint8_t response = 0;
        bool auxiliary = false;
        if (!read_output_byte(response, auxiliary)) {
            return false;
        }
        if (!auxiliary) {
            continue;
        }
        if (response == kMouseResponseResend) {
            continue;
        }
        if (response != kMouseResponseAck) {
            return false;
        }

        if (!read_output_byte(response, auxiliary) || !auxiliary || response != kMouseResponseBatOk) {
            return false;
        }
        if (!read_output_byte(response, auxiliary) || !auxiliary) {
            return false;
        }
        return true;
    }
    return false;
}

uint8_t current_led_mask() {
    return static_cast<uint8_t>(
        (g_scroll_lock_enabled ? 0x01u : 0u) |
        (g_num_lock_enabled ? 0x02u : 0u) |
        (g_caps_lock_enabled ? 0x04u : 0u));
}

bool synchronize_leds_locked() {
    return keyboard_send_command(kKeyboardCommandDisableScanning) &&
        keyboard_send_command(kKeyboardCommandSetLeds, current_led_mask()) &&
        keyboard_send_command(kKeyboardCommandEnableScanning);
}

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

uint32_t current_modifiers() {
    uint32_t modifiers = 0;
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

bool is_alpha_character(char value) {
    return value >= 'a' && value <= 'z';
}

char translate_main_block(uint8_t scancode, bool shift_active, bool caps_lock_active, bool alt_gr_active) {
    static const char kNormal[] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 0, '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '\'', '+', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '{', '|', 0, '}', 'z',
        'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' ', 0
    };
    static const char kShifted[] = {
        0, 27, '!', '"', '#', '$', '%', '&', '/', '(', ')', '=', '?', 0, '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '"', '*', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '[', 0, 0, ']', 'Z',
        'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' ', 0
    };
    static const char kAltGr[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0,
        0, '@', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '~', 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '^', 0, 0, '`', 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '*', 0, ' ', 0
    };

    if (scancode >= sizeof(kNormal)) {
        return 0;
    }

    if (alt_gr_active && kAltGr[scancode] != 0) {
        return kAltGr[scancode];
    }

    const char normal = kNormal[scancode];
    const char shifted = kShifted[scancode];
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

void emit_key_event(uint32_t key, bool pressed, char ascii_key, char ascii_text) {
    if (key == SAVANXP_KEY_NONE && ascii_key == 0) {
        return;
    }

    input::submit_key_event({
        .key = key != SAVANXP_KEY_NONE ? key : static_cast<uint32_t>(static_cast<uint8_t>(ascii_key)),
        .pressed = pressed,
        .ascii = static_cast<char>(pressed ? ascii_text : 0),
        .modifiers = current_modifiers(),
    });
}

void emit_mouse_event(int32_t delta_x, int32_t delta_y, uint32_t buttons) {
    input::submit_mouse_event({
        .delta_x = delta_x,
        .delta_y = delta_y,
        .buttons = buttons,
        .source = input::MouseSource::ps2,
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
            g_led_sync_pending = true;
            return;
        case 0x45:
            g_num_lock_enabled = !g_num_lock_enabled;
            g_led_sync_pending = true;
            return;
        case 0x46:
            g_scroll_lock_enabled = !g_scroll_lock_enabled;
            g_led_sync_pending = true;
            return;
        default:
            return;
    }
}

void handle_make_break_code(uint8_t raw_code, bool extended) {
    const bool released = (raw_code & 0x80u) != 0;
    const uint8_t scancode = static_cast<uint8_t>(raw_code & 0x7fu);

    update_modifier_state(scancode, extended, !released);
    if (!extended) {
        update_lock_state(scancode, !released);
    }

    const uint32_t key_code = translate_key_code(scancode, extended);
    const char translated = translate_ascii(scancode, extended);
    emit_key_event(key_code, !released, translated, translated);
}

void process_pause_byte(uint8_t byte) {
    static const uint8_t kPauseSequence[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};

    if (byte != kPauseSequence[g_pause_sequence_index]) {
        g_pause_sequence_index = 0;
        return;
    }

    g_pause_sequence_index += 1;
    if (g_pause_sequence_index == sizeof(kPauseSequence)) {
        emit_key_event(SAVANXP_KEY_PAUSE, true, 0, 0);
        g_pause_sequence_index = 0;
    }
}

void process_print_screen_state(uint8_t byte, bool& handled) {
    handled = false;

    if (g_print_screen_state == 1) {
        handled = true;
        if (byte == 0xe0) {
            g_print_screen_state = 3;
            return;
        }
        g_print_screen_state = 0;
        return;
    }
    if (g_print_screen_state == 2) {
        handled = true;
        if (byte == 0xe0) {
            g_print_screen_state = 4;
            return;
        }
        g_print_screen_state = 0;
        return;
    }
    if (g_print_screen_state == 3) {
        handled = true;
        if (byte == 0x37) {
            emit_key_event(SAVANXP_KEY_PRINT_SCREEN, true, 0, 0);
        }
        g_print_screen_state = 0;
        return;
    }
    if (g_print_screen_state == 4) {
        handled = true;
        if (byte == 0xaa) {
            emit_key_event(SAVANXP_KEY_PRINT_SCREEN, false, 0, 0);
        }
        g_print_screen_state = 0;
    }
}

void process_keyboard_byte(uint8_t byte) {
    if (byte == 0x54 || byte == 0xd4) {
        emit_key_event(SAVANXP_KEY_PRINT_SCREEN, byte == 0x54, 0, 0);
        return;
    }
    if (byte == kKeyboardResponseAck || byte == kKeyboardResponseResend) {
        return;
    }
    if (byte == kKeyboardResponseBufferError0 || byte == kKeyboardResponseBufferError1) {
        reset_decoder_state();
        return;
    }
    if (byte == kKeyboardResponseSelfTestFailed0 || byte == kKeyboardResponseSelfTestFailed1) {
        g_ready = false;
        reset_decoder_state();
        console::write_line("ps2: keyboard reported a self-test failure");
        return;
    }

    if (g_pause_sequence_index != 0 || byte == 0xe1) {
        process_pause_byte(byte);
        return;
    }

    bool handled_print_screen = false;
    process_print_screen_state(byte, handled_print_screen);
    if (handled_print_screen) {
        return;
    }

    if (byte == 0xe0) {
        g_extended_prefix = true;
        return;
    }

    if (g_extended_prefix) {
        g_extended_prefix = false;
        if (byte == 0x37) {
            emit_key_event(SAVANXP_KEY_PRINT_SCREEN, true, 0, 0);
            return;
        }
        if (byte == 0xb7 || byte == 0xaa) {
            emit_key_event(SAVANXP_KEY_PRINT_SCREEN, false, 0, 0);
            return;
        }
        if (byte == 0x2a) {
            g_print_screen_state = 1;
            return;
        }
        if (byte == 0xb7) {
            g_print_screen_state = 2;
            return;
        }
        handle_make_break_code(byte, true);
        return;
    }

    handle_make_break_code(byte, false);
}

int32_t decode_mouse_delta(uint8_t value, bool negative) {
    int32_t delta = static_cast<int32_t>(value);
    if (negative) {
        delta -= 0x100;
    }
    return delta;
}

void process_mouse_packet() {
    const uint8_t first = g_mouse_packet[0];
    const uint8_t second = g_mouse_packet[1];
    const uint8_t third = g_mouse_packet[2];

    if ((first & 0x08u) == 0) {
        return;
    }
    if ((first & 0xc0u) != 0) {
        return;
    }

    uint32_t buttons = 0;
    if ((first & 0x01u) != 0) {
        buttons |= SAVANXP_MOUSE_BUTTON_LEFT;
    }
    if ((first & 0x02u) != 0) {
        buttons |= SAVANXP_MOUSE_BUTTON_RIGHT;
    }
    if ((first & 0x04u) != 0) {
        buttons |= SAVANXP_MOUSE_BUTTON_MIDDLE;
    }

    const int32_t delta_x = decode_mouse_delta(second, (first & 0x10u) != 0);
    const int32_t raw_delta_y = decode_mouse_delta(third, (first & 0x20u) != 0);
    emit_mouse_event(delta_x, -raw_delta_y, buttons);
}

void process_mouse_byte(uint8_t byte) {
    if (byte == kMouseResponseAck || byte == kMouseResponseResend) {
        return;
    }

    if (g_mouse_packet_index == 0 && (byte & 0x08u) == 0) {
        return;
    }

    g_mouse_packet[g_mouse_packet_index++] = byte;
    if (g_mouse_packet_index < 3) {
        return;
    }

    process_mouse_packet();
    g_mouse_packet_index = 0;
}

void service_pending_led_update() {
    if (!g_led_sync_pending || !g_ready) {
        return;
    }

    arch::x86_64::disable_interrupts();
    if (g_raw_count != 0 || (in8(kStatusPort) & kStatusOutputReady) != 0) {
        arch::x86_64::enable_interrupts();
        return;
    }

    if (synchronize_leds_locked()) {
        g_led_sync_pending = false;
    }
    arch::x86_64::enable_interrupts();
}

bool detect_second_port() {
    uint8_t config = 0;
    if (!controller_write(kControllerCommandEnableSecondPort)) {
        return false;
    }
    if (!controller_read_config(config)) {
        return false;
    }
    const bool second_port_present = (config & kConfigDisableSecondPortClock) == 0;
    if (!controller_write(kControllerCommandDisableSecondPort)) {
        return false;
    }
    return second_port_present;
}

bool initialize_keyboard() {
    flush_controller_output();
    if (!keyboard_reset_device()) {
        console::write_line("ps2: keyboard reset failed");
        return false;
    }
    if (!keyboard_send_command(kKeyboardCommandSetDefaults)) {
        console::write_line("ps2: failed to restore keyboard defaults");
        return false;
    }
    if (!keyboard_send_command(kKeyboardCommandSetScancode, kScancodeSet2)) {
        console::write_line("ps2: failed to select keyboard scancode set 2");
        return false;
    }
    if (!synchronize_leds_locked()) {
        console::write_line("ps2: failed to synchronize keyboard LEDs");
        return false;
    }
    return true;
}

bool initialize_mouse() {
    flush_controller_output();
    if (!mouse_reset_device()) {
        console::write_line("ps2: mouse reset failed");
        return false;
    }
    if (!mouse_send_byte_with_ack(kMouseCommandSetDefaults)) {
        console::write_line("ps2: failed to restore mouse defaults");
        return false;
    }
    if (!mouse_send_byte_with_ack(kMouseCommandEnableDataReporting)) {
        console::write_line("ps2: failed to enable mouse data reporting");
        return false;
    }
    return true;
}

bool initialize_controller(bool& second_port_present) {
    second_port_present = false;
    flush_controller_output();

    if (!controller_write(kControllerCommandDisableFirstPort) ||
        !controller_write(kControllerCommandDisableSecondPort)) {
        console::write_line("ps2: failed to disable controller ports");
        return false;
    }

    flush_controller_output();

    if (!controller_write(kControllerCommandSelfTest) || !controller_expect_byte(0x55)) {
        console::write_line("ps2: controller self-test failed");
        return false;
    }

    if (!controller_write(kControllerCommandTestFirstPort) || !controller_expect_byte(0x00)) {
        console::write_line("ps2: first PS/2 port test failed");
        return false;
    }

    second_port_present = detect_second_port();
    if (second_port_present) {
        if (!controller_write(kControllerCommandEnableSecondPort)) {
            second_port_present = false;
        } else if (!controller_write(kControllerCommandTestSecondPort) || !controller_expect_byte(0x00)) {
            console::write_line("ps2: second PS/2 port test failed");
            second_port_present = false;
        } else if (!controller_write(kControllerCommandDisableSecondPort)) {
            second_port_present = false;
        }
    }

    uint8_t config = 0;
    if (!controller_read_config(config)) {
        console::write_line("ps2: failed to read controller config");
        return false;
    }

    config &= static_cast<uint8_t>(~(kConfigIrqFirstPort | kConfigIrqSecondPort));
    config &= static_cast<uint8_t>(~kConfigDisableFirstPortClock);
    if (second_port_present) {
        config &= static_cast<uint8_t>(~kConfigDisableSecondPortClock);
    } else {
        config |= kConfigDisableSecondPortClock;
    }
    config |= kConfigTranslation;
    if (!controller_write_config(config)) {
        console::write_line("ps2: failed to write controller config");
        return false;
    }

    if (!controller_write(kControllerCommandEnableFirstPort)) {
        console::write_line("ps2: failed to enable first PS/2 port");
        return false;
    }
    if (second_port_present && !controller_write(kControllerCommandEnableSecondPort)) {
        console::write_line("ps2: failed to enable second PS/2 port");
        second_port_present = false;
    }

    if (!initialize_keyboard()) {
        return false;
    }

    const bool prefer_virtio_mouse = virtio_input::mouse_ready();
    if (second_port_present && prefer_virtio_mouse) {
        console::write_line("ps2: skipping auxiliary mouse because virtio-input is active");
    }

    g_mouse_ready = second_port_present && !prefer_virtio_mouse && initialize_mouse();

    if (!controller_read_config(config)) {
        console::write_line("ps2: failed to reload controller config");
        return false;
    }

    config |= kConfigIrqFirstPort;
    if (g_mouse_ready) {
        config |= kConfigIrqSecondPort;
        config &= static_cast<uint8_t>(~kConfigDisableSecondPortClock);
    } else {
        config &= static_cast<uint8_t>(~kConfigIrqSecondPort);
        config |= kConfigDisableSecondPortClock;
        (void)controller_write(kControllerCommandDisableSecondPort);
    }
    config &= static_cast<uint8_t>(~kConfigDisableFirstPortClock);
    config |= kConfigTranslation;
    if (!controller_write_config(config)) {
        console::write_line("ps2: failed to enable PS/2 IRQs");
        return false;
    }

    flush_controller_output();
    return true;
}

void keyboard_irq() {
    drain_controller_output_to_queue();
}

void mouse_irq() {
    drain_controller_output_to_queue();
}

} // namespace

namespace ps2 {

void initialize() {
    g_ready = false;
    g_mouse_ready = false;
    clear_raw_queue();
    reset_input_state();

    bool second_port_present = false;
    if (!initialize_controller(second_port_present)) {
        return;
    }

    if (!arch::x86_64::register_irq_handler(1, keyboard_irq)) {
        console::write_line("ps2: failed to register irq1");
        return;
    }
    if (g_mouse_ready && !arch::x86_64::register_irq_handler(12, mouse_irq)) {
        console::write_line("ps2: failed to register irq12");
        g_mouse_ready = false;
    }

    arch::x86_64::enable_irq(1);
    if (g_mouse_ready) {
        arch::x86_64::enable_irq(12);
    }
    g_ready = true;
}

bool ready() {
    return g_ready;
}

bool mouse_ready() {
    return g_mouse_ready;
}

void poll() {
    drain_controller_output_to_queue();

    if (g_raw_overflow) {
        g_raw_overflow = false;
        console::write_line("ps2: raw input queue overflow");
    }

    RawByte value = {};
    while (dequeue_raw_byte(value)) {
        if (value.auxiliary) {
            if (g_mouse_ready) {
                process_mouse_byte(value.value);
            }
        } else {
            process_keyboard_byte(value.value);
        }
    }

    service_pending_led_update();
}

} // namespace ps2
