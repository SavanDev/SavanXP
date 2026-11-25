#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

namespace i386 {

namespace ps2_32 {

/* The reference kernel/ps2.cpp drives the 8042 with the ES/EN layouts. Its
 * decoded events queue here: canonical TTY consumers receive lines; raw
 * clients read savanxp_input_event records from /dev/input0 (diagnostics can
 * explicitly duplicate that descriptor to stdin). */

void initialize();
// Decode the bytes that the reference IRQ handlers queued.
void poll();
// Retained virtio keyboard/tablet drivers feed the same native event queues.
void inject_mouse_event(const savanxp_mouse_event& event);
bool has_mouse_event();
bool pop_mouse_event(savanxp_mouse_event& event);

/* An event is pending for the READ syscall. */
bool has_pending_event();

/* Move the oldest event to `out`; false when there was none. */
bool pop_event(struct savanxp_input_event& out);

/* Clear raw queues at scheduler startup and graphics-session boundaries.
 * Spawning an ordinary program does not clear the shared queue. */
void clear_queue();

/* Keyboard layout (SAVANXP_KEYBOARD_LAYOUT_*), owned by the reference ps2::. */
bool set_layout(int layout);
int get_layout();

}  // namespace ps2_32

}  // namespace i386
