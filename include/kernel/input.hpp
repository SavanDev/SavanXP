#pragma once

#include <stdint.h>

#include "abi/savanxp_abi.h"

/* The input layer (docs/kernel/INPUT.md). Drivers register here and report
 * what they see; the layer turns it into events for the console or the
 * graphics session. No driver knows another one exists, and nothing outside
 * the drivers names a driver. */
namespace input {

/* Derived from savanxp/syscall.h, not redefined: these flags travel to
 * userland unchanged in savanxp_input_event.modifiers, so two separate lists
 * could drift apart without anything complaining. */
enum ModifierFlags : uint32_t {
    modifier_shift = SAVANXP_KEY_MOD_SHIFT,
    modifier_ctrl = SAVANXP_KEY_MOD_CTRL,
    modifier_alt = SAVANXP_KEY_MOD_ALT,
    modifier_alt_gr = SAVANXP_KEY_MOD_ALT_GR,
    modifier_caps_lock = SAVANXP_KEY_MOD_CAPS_LOCK,
    modifier_num_lock = SAVANXP_KEY_MOD_NUM_LOCK,
    modifier_scroll_lock = SAVANXP_KEY_MOD_SCROLL_LOCK,
};

constexpr uint32_t kLockModifiers = modifier_caps_lock | modifier_num_lock | modifier_scroll_lock;

struct KeyEvent {
    uint32_t key;
    bool pressed;
    char ascii;
    uint32_t modifiers;
};

struct MouseEvent {
    int32_t delta_x;
    int32_t delta_y;
    // Wheel ticks since the previous event; positive = away from the user,
    // same as savanxp_mouse_event.wheel. Zero on devices without a wheel.
    int32_t wheel;
    uint32_t buttons;
    // True pointer position in screen pixels, valid only with has_absolute.
    // Absolute devices send it; a relative one leaves it zero.
    int32_t absolute_x;
    int32_t absolute_y;
    bool has_absolute;
};

/* One registered input driver. A driver may provide a keyboard, a pointer or
 * both. Registration order is priority: a driver that finds a keyboard (or a
 * pointer) already registered leaves its own silent, because QEMU feeds the
 * host's input to every backend at once and each key would arrive twice. */
struct Driver {
    const char* name;
    bool keyboard;
    bool pointer;
    // Drains whatever the device has queued. Called from the timer tick and
    // from blocking reads.
    void (*poll)();
    // Optional. The lock state changed (a mask of kLockModifiers). Called
    // with the kernel lock held, maybe from an interrupt: record it, do the
    // device I/O from poll.
    void (*set_keyboard_leds)(uint32_t lock_modifiers);
    // Optional, absolute pointers only: the screen size their range maps to.
    void (*set_screen_extent)(uint32_t width, uint32_t height);
    // Optional, absolute pointers only: forget the last position, so the
    // next sample is reported as a jump instead of a delta.
    void (*reset_pointer)();
};

void initialize();
bool register_driver(const Driver& driver);
bool keyboard_available();
bool pointer_available();

void poll();
void poll_pointers();
void set_screen_extent(uint32_t width, uint32_t height);
void reset_pointers();

// What drivers report.
void submit_mouse_event(const MouseEvent& event);

/* The keyboard core (kernel/keyboard.cpp): modifiers, lock keys, layouts and
 * the translation to SAVANXP_KEY_* and text. Keyboard drivers report physical
 * keys as PC/AT scancode set 1 positions (7 bits, plus the 0xE0 prefix as
 * `extended`); a driver whose hardware speaks another code maps it first. */
void submit_scancode(uint8_t scancode, bool extended, bool pressed);
// For keys that have no set 1 position of their own (Pause, Print Screen).
void submit_key(uint32_t key, bool pressed);
bool set_keyboard_layout(int layout);
int keyboard_layout();

// Used by the keyboard core.
void submit_key_event(const KeyEvent& event);
void notify_keyboard_leds(uint32_t lock_modifiers);

} // namespace input
