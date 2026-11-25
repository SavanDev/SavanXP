#pragma once

#include <stddef.h>

namespace tty {

struct TtyDevice {
    char pending_line[256];
    size_t pending_length;
    bool line_ready;
};

} // namespace tty
