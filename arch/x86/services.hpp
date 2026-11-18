#pragma once
#include <stdint.h>
namespace i386::services {
uint32_t initialize_firmware(uint32_t multiboot_info);
void initialize(uint32_t rsdp_physical);
}
// The reference network stack waits inside a non-preemptible kernel call.
// Only device IRQs run during this wait; the user context is not rescheduled.
namespace smp { void wait_for_interrupt(); }

namespace i386 { struct ContextFrame; bool native_syscall(ContextFrame& frame); }
