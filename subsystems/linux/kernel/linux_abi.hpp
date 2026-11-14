#pragma once

#include <stdint.h>

#include "abi/savanxp_abi.h"

/*
 * The Linux x86-64 ABI as SSL needs it: numbers, error values and layouts,
 * written here from the documented ABI (arch/x86/entry/syscalls/syscall_64.tbl
 * and include/uapi in the Linux tree). Nothing in this header comes from the
 * POSIX SDK: a Linux value and a SavanXP value with the same name are
 * different things, even when the number is the same.
 */

namespace ssl::abi {

// System call numbers.
constexpr uint64_t kSysWrite = 1;
constexpr uint64_t kSysExit = 60;
constexpr uint64_t kSysExitGroup = 231;

// Errors, returned negated in RAX.
constexpr int64_t kEBADF = 9;
constexpr int64_t kEFAULT = 14;
constexpr int64_t kENOSYS = 38;

// Every number the subsystem tracks fits below this; the tables in
// syscall_64.tbl stop well before it.
constexpr uint32_t kSyscallLimit = 512;

/* The kernel's error space (sx_status) uses Linux's numbers, so a kernel
 * result passes through unchanged. That matters where the layer does not see
 * the result: a write that blocks on a pipe is finished by whoever wakes the
 * thread, and that code writes the kernel's value into RAX. These asserts are
 * what makes passing it through correct; a kernel error renumbered breaks the
 * build here instead of a Linux program. */
static_assert(SX_EPERM == 1 && SX_ENOENT == 2 && SX_EINTR == 4 && SX_EIO == 5 && SX_E2BIG == 7 &&
              SX_ENOEXEC == 8 && SX_EBADF == 9 && SX_ECHILD == 10 && SX_EAGAIN == 11 &&
              SX_ENOMEM == 12 && SX_EACCES == 13 && SX_EBUSY == 16 && SX_EEXIST == 17 &&
              SX_EXDEV == 18 && SX_ENODEV == 19 && SX_ENOTDIR == 20 && SX_EISDIR == 21 &&
              SX_EINVAL == 22 && SX_ENOTTY == 25 && SX_ENOSPC == 28 && SX_EROFS == 30 &&
              SX_EPIPE == 32 && SX_ENAMETOOLONG == 36 && SX_ENOSYS == 38 && SX_ENOTEMPTY == 39 &&
              SX_ECONNRESET == 104 && SX_ETIMEDOUT == 110 && SX_ECONNREFUSED == 111,
              "the kernel's error numbers must stay Linux's for SSL to pass them through");

} // namespace ssl::abi
