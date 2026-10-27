#include "kernel/input.hpp"

#include <stddef.h>

#include "kernel/console.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"

namespace {

constexpr size_t kMaxDrivers = 4;

input::Driver g_drivers[kMaxDrivers] = {};
size_t g_driver_count = 0;
bool g_keyboard_available = false;
bool g_pointer_available = false;

} // namespace

namespace input {

void initialize() {
    g_driver_count = 0;
    g_keyboard_available = false;
    g_pointer_available = false;
}

bool register_driver(const Driver& driver) {
    if (g_driver_count >= kMaxDrivers || (!driver.keyboard && !driver.pointer)) {
        console::write_line("input: driver table full or driver provides nothing");
        return false;
    }
    g_drivers[g_driver_count++] = driver;
    g_keyboard_available = g_keyboard_available || driver.keyboard;
    g_pointer_available = g_pointer_available || driver.pointer;
    return true;
}

bool keyboard_available() {
    return g_keyboard_available;
}

bool pointer_available() {
    return g_pointer_available;
}

void poll() {
    for (size_t i = 0; i < g_driver_count; ++i) {
        if (g_drivers[i].poll != nullptr) {
            g_drivers[i].poll();
        }
    }
}

void poll_pointers() {
    for (size_t i = 0; i < g_driver_count; ++i) {
        if (g_drivers[i].pointer && g_drivers[i].poll != nullptr) {
            g_drivers[i].poll();
        }
    }
}

void set_screen_extent(uint32_t width, uint32_t height) {
    for (size_t i = 0; i < g_driver_count; ++i) {
        if (g_drivers[i].set_screen_extent != nullptr) {
            g_drivers[i].set_screen_extent(width, height);
        }
    }
}

void reset_pointers() {
    for (size_t i = 0; i < g_driver_count; ++i) {
        if (g_drivers[i].reset_pointer != nullptr) {
            g_drivers[i].reset_pointer();
        }
    }
}

void notify_keyboard_leds(uint32_t lock_modifiers) {
    for (size_t i = 0; i < g_driver_count; ++i) {
        if (g_drivers[i].keyboard && g_drivers[i].set_keyboard_leds != nullptr) {
            g_drivers[i].set_keyboard_leds(lock_modifiers);
        }
    }
}

void submit_key_event(const KeyEvent& event) {
    if (ui::graphics_active()) {
        ui::handle_key_event(event.key, event.pressed, event.pressed ? event.ascii : 0, event.modifiers);
        return;
    }

    tty::handle_key_event(event);
}

void submit_mouse_event(const MouseEvent& event) {
    if (!ui::graphics_active()) {
        return;
    }

    ui::handle_mouse_event(event.delta_x, event.delta_y, event.wheel, event.buttons, event.absolute_x, event.absolute_y, event.has_absolute);
}

} // namespace input
