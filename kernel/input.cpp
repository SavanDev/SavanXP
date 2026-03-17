#include "kernel/input.hpp"

#include "kernel/ps2.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"
#include "kernel/virtio_input.hpp"

namespace input {

void initialize() {
}

void poll() {
    virtio_input::poll();
    ps2::poll();
}

void submit_key_event(const KeyEvent& event) {
    if (ui::graphics_active()) {
        ui::handle_key_event(event.key, event.pressed, event.pressed ? event.ascii : 0);
        return;
    }

    tty::handle_key_event(event);
}

void submit_mouse_event(const MouseEvent& event) {
    if (virtio_input::mouse_ready() && event.source == input::MouseSource::ps2) {
        return;
    }
    if (!ui::graphics_active()) {
        return;
    }

    ui::handle_mouse_event(event.delta_x, event.delta_y, event.buttons);
}

} // namespace input
