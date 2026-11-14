#pragma once

#include <stddef.h>
#include <stdint.h>
#include "context.hpp"

namespace i386 {

namespace syscall {

/* Numeros que entiende este kernel, los mismos del SDK (savanxp/syscall.h):
 * solo los que los programas del objetivo i386 usan. */
constexpr uint32_t kSysRead = 0;
constexpr uint32_t kSysWrite = 1;
constexpr uint32_t kSysOpen = 2;
constexpr uint32_t kSysClose = 3;
constexpr uint32_t kSysSpawn = 5;
constexpr uint32_t kSysWaitPid = 6;
constexpr uint32_t kSysExit = 7;
constexpr uint32_t kSysYield = 8;
constexpr uint32_t kSysUptimeMs = 9;
constexpr uint32_t kSysSleepMs = 11;
constexpr uint32_t kSysIoctl = 24;
constexpr uint32_t kSysGetPid = 30;
constexpr uint32_t kSysSystemInfo = 35;

constexpr uint32_t kStdinFd = 0;
constexpr uint32_t kStdoutFd = 1;
constexpr uint32_t kStderrFd = 2;
/* One GPU descriptor for the single fullscreen client. */
constexpr uint32_t kGpuFd = 3;
constexpr const char* kGpuPath = "/dev/gpu0";

/* Set the result or block the process; the trap wrapper selects a return frame. */
void handle(ContextFrame& frame);

}

}