#pragma once
#include <stdint.h>

namespace i386::smp {
// Bring-up only: APs have private TSS/stacks and accept IPIs, but remain parked.
void initialize(uint32_t multiboot_information);
uint32_t cpu_count();
uint32_t online_count();
bool ipi_test_passed();
}
