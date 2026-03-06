#include "kernel/panic.hpp"

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"

[[noreturn]] void panic(const char* message) {
    console::write_line("");
    console::write_line("*** PANIC ***");
    console::write_line(message);
    arch::x86_64::halt_forever();
}

