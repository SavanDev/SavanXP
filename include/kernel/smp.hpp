#pragma once

namespace smp {

// Wait for an interrupt, releasing the kernel lock while asleep if held.
void wait_for_interrupt();

} // namespace smp
