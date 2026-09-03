#pragma once

namespace ps2 {

void initialize();
bool ready();
bool mouse_ready();
bool set_layout(int layout);
int get_layout();
void poll();

} // namespace ps2
