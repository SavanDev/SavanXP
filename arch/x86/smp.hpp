#pragma once
#include <stdint.h>

namespace i386::smp {
void initialize(uint32_t multiboot_information);
uint32_t cpu_count();
uint32_t online_count();
bool ipi_test_passed();
bool may_schedule(uint32_t index);
bool can_schedule(uint32_t index);
uint32_t scheduling_count();
void start_scheduling();
void kick(uint32_t index);
void lock_kernel();
void unlock_kernel();
bool kernel_locked_here();
}
