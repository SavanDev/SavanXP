#pragma once

namespace power {

// Registra el device /dev/power que expone apagado/reinicio a userspace.
void initialize();
bool ready();

} // namespace power
