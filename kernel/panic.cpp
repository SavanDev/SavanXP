#include "kernel/panic.hpp"

#include "arch/cpu.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/display.hpp"
#include "kernel/smp.hpp"

extern "C" [[noreturn]] void __stack_chk_fail() {
    const auto caller = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    console::printf("kernel: stack canary failure at 0x%llx\n", caller);
    panic("kernel: stack canary failure");
}

[[noreturn]] void panic(const char* message) {
    arch::disable_interrupts();
    uint32_t targets = 0;
    const uint32_t stopped = smp::stop_other_cores(targets);
    console::serial_flush_synchronous();
    boot_screen::finish();
    // The desktop may be showing a buffer the console does not write into,
    // or a mode change may have been cut short: make the console visible
    // first (FB4, docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md).
    display::emergency_console();
    console::set_framebuffer_console_enabled(true);
    console::write_line("");
    console::write_line("*** PANIC ***");
    console::write_line(message);
    smp::report_stop(stopped, targets);
    arch::halt_forever();
}
