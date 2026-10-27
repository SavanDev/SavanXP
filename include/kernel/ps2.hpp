#pragma once

namespace ps2 {

// Brings up the 8042 and registers with the input layer whatever it drives:
// the keyboard and the mouse, unless another driver already provides one.
void initialize();
void poll();

} // namespace ps2
