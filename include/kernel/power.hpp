#pragma once

namespace power {

// Registers the /dev/power device that exposes power-off and reboot to user space.
void initialize();
bool ready();

// The one way the machine is powered off or reset, for /dev/power and the
// ACPI power button alike: every mounted volume synced and marked clean, the
// writeback queue drained, every block driver told, then S5 or a reset. A
// step that fails is logged and the rest still runs. Called with the big
// kernel lock held (a system call or an interrupt), so no filesystem
// mutation is half done.
[[noreturn]] void shutdown();
[[noreturn]] void reboot();

} // namespace power
