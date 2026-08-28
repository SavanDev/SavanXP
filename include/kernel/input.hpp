#pragma once

#include <stdint.h>

#include "savanxp/syscall.h"

namespace input {

enum class MouseSource : uint8_t {
    ps2 = 0,
    virtio_tablet = 1,
};

/* Derivados de savanxp/syscall.h y no redefinidos: estos flags viajan tal cual
 * a userland en savanxp_input_event.modifiers, asi que dos listas separadas
 * podrian divergir sin que nada se queje. */
enum ModifierFlags : uint32_t {
    modifier_shift = SAVANXP_KEY_MOD_SHIFT,
    modifier_ctrl = SAVANXP_KEY_MOD_CTRL,
    modifier_alt = SAVANXP_KEY_MOD_ALT,
    modifier_alt_gr = SAVANXP_KEY_MOD_ALT_GR,
    modifier_caps_lock = SAVANXP_KEY_MOD_CAPS_LOCK,
    modifier_num_lock = SAVANXP_KEY_MOD_NUM_LOCK,
    modifier_scroll_lock = SAVANXP_KEY_MOD_SCROLL_LOCK,
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
