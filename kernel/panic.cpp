#include "kernel/panic.hpp"

#include "kernel/boot_screen.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"

extern "C" [[noreturn]] void __stack_chk_fail() {
    uint64_t caller = 0;
    asm volatile("movq 8(%%rsp), %0" : "=r"(caller));
    console::printf("kernel: stack canary failure at 0x%llx\n", caller);
    panic("kernel: stack canary failure");
}

[[noreturn]] void panic(const char* message) {
    boot_screen::finish();
    console::set_framebuffer_console_enabled(true);
    console::write_line("");
    console::write_line("*** PANIC ***");
    console::write_line(message);
    arch::x86_64::halt_forever();
}
