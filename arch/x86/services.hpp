#pragma once
#include <stdint.h>
namespace i386::services {
uint32_t initialize_firmware(uint32_t multiboot_info);
// The reference boot stages: "Starting ACPI" before the PCI scan, and
// "Initializing devices" after the display binds.
void start_acpi(uint32_t rsdp_physical);
void initialize_devices();
}
namespace i386 { struct ContextFrame; bool native_syscall(ContextFrame& frame); }
