#pragma once

#include <stdint.h>

namespace input {

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

void initialize();
void poll();
void submit_key_event(const KeyEvent& event);

} // namespace input
