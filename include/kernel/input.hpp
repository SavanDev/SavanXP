#pragma once

#include <stdint.h>

namespace input {

enum class MouseSource : uint8_t {
    ps2 = 0,
    virtio_tablet = 1,
};

enum ModifierFlags : uint32_t {
    modifier_shift = 1u << 0,
    modifier_ctrl = 1u << 1,
    modifier_alt = 1u << 2,
    modifier_alt_gr = 1u << 3,
    modifier_caps_lock = 1u << 4,
    modifier_num_lock = 1u << 5,
    modifier_scroll_lock = 1u << 6,
};

struct KeyEvent {
    uint32_t key;
    bool pressed;
    char ascii;
    uint32_t modifiers;
};

struct MouseEvent {
    int32_t delta_x;
    int32_t delta_y;
    uint32_t buttons;
    MouseSource source;
};

void initialize();
void poll();
void submit_key_event(const KeyEvent& event);
void submit_mouse_event(const MouseEvent& event);

} // namespace input
