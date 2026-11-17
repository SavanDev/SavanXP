#pragma once
#include "process.hpp"
namespace i386::waits {
bool syscall(ContextFrame& frame);
void service();
void clear(process::Process& task);
}
