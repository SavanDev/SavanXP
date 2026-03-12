#include "kernel/input.hpp"

#include "kernel/ps2.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"

namespace input {

void initialize() {
}

void poll() {
    ps2::poll();
}

void submit_key_event(const KeyEvent& event) {
    if (ui::graphics_active()) {
        ui::handle_key_event(event.key, event.pressed, event.pressed ? event.ascii : 0);
        return;
    }

    tty::handle_key_event(event);
}

} // namespace input
