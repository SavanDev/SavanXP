#include "ps2_32.hpp"

#include <stdint.h>

#include "fb32.hpp"
#include "savanxp/syscall.h"
#include "serial.hpp"
#include "kernel/virtio_input.hpp"
#include "kernel/input.hpp"
#include "kernel/ps2.hpp"

/*
 * The reference kernel/ps2.cpp owns the 8042 controller, its IRQ routing, the
 * ES/EN layouts, lock keys and LEDs, the keypad, AltGr and the IntelliMouse
 * packets. It reports decoded events through input::submit_key_event and
 * input::submit_mouse_event. On x86_64 kernel/input.cpp hands those to the
 * kernel UI or TTY; i386 has neither, so this adapter queues them for
 * /dev/input0, the pointer device and the canonical TTY in arch/x86/io.cpp.
 */

namespace i386 {

namespace ps2_32 {

namespace {

constexpr uint32_t kMouseQueueCapacity = 512;
savanxp_mouse_event g_mouse_events[kMouseQueueCapacity];
uint32_t g_mouse_head, g_mouse_count;

/* Producers run from the reference poll path and from virtio-input; device
 * reads drain them. Queue mutations preserve the caller's interrupt state. */
constexpr size_t kQueueCapacity = 512;
savanxp_input_event g_queue[kQueueCapacity] = {};
size_t g_read_index = 0;
size_t g_write_index = 0;
size_t g_pending = 0;

uint32_t save_and_disable() {
    uint32_t value;
    asm volatile("pushf; pop %0; cli" : "=r"(value) : : "memory");
    return value;
}

void restore(uint32_t flags) {
    if ((flags & (1u << 9)) != 0) {
        asm volatile("sti");
    }
}

void push_event(uint32_t type, uint32_t key, int32_t ascii, uint32_t modifiers) {
    const uint32_t flags = save_and_disable();
    if (g_pending >= kQueueCapacity) {
        g_read_index = (g_read_index + 1) % kQueueCapacity;
        --g_pending;
    }
    g_queue[g_write_index].type = type;
    g_queue[g_write_index].key = key;
    g_queue[g_write_index].ascii = ascii;
    g_queue[g_write_index].modifiers = modifiers;
    g_write_index = (g_write_index + 1) % kQueueCapacity;
    ++g_pending;
    restore(flags);
}

void write_int(int32_t value) {
    if (value < 0) {
        serial::write_char('-');
        value = -value;
    }
    serial::write_dec(static_cast<uint32_t>(value));
}

}  // namespace

void inject_mouse_event(const savanxp_mouse_event& event) {
    const uint32_t flags = save_and_disable();
    if (g_mouse_count == kMouseQueueCapacity) {
        const int32_t dropped_wheel = g_mouse_events[g_mouse_head].wheel;
        g_mouse_head = (g_mouse_head + 1) % kMouseQueueCapacity;
        --g_mouse_count;
        g_mouse_events[g_mouse_head].wheel += dropped_wheel;
    }
    g_mouse_events[(g_mouse_head + g_mouse_count++) % kMouseQueueCapacity] = event;
    restore(flags);
}

bool has_pending_event() {
    return g_pending != 0;
}

bool pop_event(savanxp_input_event& out) {
    const uint32_t flags = save_and_disable();
    if (g_pending == 0) {
        restore(flags);
        return false;
    }
    out = g_queue[g_read_index];
    g_read_index = (g_read_index + 1) % kQueueCapacity;
    --g_pending;
    restore(flags);
    return true;
}

void clear_queue() {
    /* Reset both queues at boot/session boundaries. */
    const uint32_t flags = save_and_disable();
    g_read_index = 0;
    g_write_index = 0;
    g_pending = 0;
    g_mouse_head = g_mouse_count = 0;
    restore(flags);
}

bool set_layout(int layout) {
    return ps2::set_layout(layout);
}

int get_layout() {
    return ps2::get_layout();
}

bool has_mouse_event() { return g_mouse_count != 0; }

bool pop_mouse_event(savanxp_mouse_event& event) {
    const uint32_t flags = save_and_disable();
    if (!g_mouse_count) {
        restore(flags);
        return false;
    }
    event = g_mouse_events[g_mouse_head];
    g_mouse_head = (g_mouse_head + 1) % kMouseQueueCapacity;
    --g_mouse_count;
    restore(flags);
    return true;
}

void initialize() {
    ps2::initialize();
    if (!ps2::ready()) {
        serial::write("ps2: controller unavailable\n");
        return;
    }
    serial::write("ps2: teclado listo (irq 1, scancodes set 1)\n");
    serial::write(ps2::mouse_ready() ? "ps2: mouse ready (irq 12)\n" : "ps2: mouse unavailable\n");
}

void poll() {
    ps2::poll();
}

}  // namespace ps2_32

}  // namespace i386

namespace input {

/* Same contract as the reference: ASCII travels only with a press, and the
 * key code of a printable key is its ASCII value. The serial trace is the
 * i386 diagnostic that the console boot and desktop checks read. */
void submit_key_event(const KeyEvent& event) {
    i386::ps2_32::push_event(event.pressed ? SAVANXP_INPUT_EVENT_KEY_DOWN : SAVANXP_INPUT_EVENT_KEY_UP,
                             event.key, event.ascii, event.modifiers);
    i386::serial::SerialOnly trace;
    i386::serial::write("ps2: tecla ");
    i386::serial::write(event.pressed ? "down " : "up ");
    i386::serial::write_hex(event.key);
    if (event.ascii >= 32 && event.ascii < 127) {
        i386::serial::write(" '");
        i386::serial::write_char(event.ascii);
        i386::serial::write("'");
    }
    i386::serial::write("\n");
}

void submit_mouse_event(const MouseEvent& event) {
    // As in kernel/input.cpp, a virtio pointer supersedes the PS/2 mouse.
    if (virtio_input::mouse_ready() && event.source == MouseSource::ps2) {
        return;
    }
    int32_t x, y;
    i386::fb32::pointer_position(x, y);
    i386::fb32::pointer_move(event.has_absolute ? event.absolute_x - x : event.delta_x,
                             event.has_absolute ? event.absolute_y - y : event.delta_y);
    i386::fb32::pointer_set_buttons(event.buttons);
    i386::ps2_32::inject_mouse_event({event.delta_x, event.delta_y, event.wheel, event.buttons,
        event.absolute_x, event.absolute_y, event.has_absolute ? SAVANXP_MOUSE_FLAG_ABSOLUTE : 0u});
    if (event.source != MouseSource::ps2) {
        return;
    }
    i386::serial::SerialOnly trace;
    i386::fb32::pointer_position(x, y);
    i386::serial::write("ps2: mouse dx ");
    i386::ps2_32::write_int(event.delta_x);
    i386::serial::write(" dy ");
    i386::ps2_32::write_int(-event.delta_y);
    i386::serial::write(" buttons ");
    i386::serial::write_dec(event.buttons);
    i386::serial::write(" at ");
    i386::serial::write_dec(static_cast<uint32_t>(x));
    i386::serial::write(",");
    i386::serial::write_dec(static_cast<uint32_t>(y));
    i386::serial::write("\n");
}

}  // namespace input
