#pragma once
#include <stdint.h>

namespace i386::lapic {
constexpr uint8_t kPingVector = 64;
constexpr uint8_t kTimerVector = 65;
constexpr uint8_t kRescheduleVector = 66;
bool calibrate_timer();
bool start_timer();
bool initialize(uint32_t address);
bool initialize_secondary();
bool ready();
uint32_t id();
uint32_t initial_id();
void acknowledge();
bool send(uint32_t destination, uint32_t command);
}
