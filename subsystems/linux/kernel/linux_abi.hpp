#pragma once

#include <stdint.h>

/*
 * The Linux x86-64 ABI as SSL needs it: numbers, error values and layouts,
 * written here from the documented ABI (arch/x86/entry/syscalls/syscall_64.tbl
 * and include/uapi in the Linux tree). Nothing in this header comes from the
 * POSIX SDK: a Linux value and a SavanXP value with the same name are
 * different numbers.
 */

namespace ssl::abi {

// Errors are returned negated in RAX, as the kernel's own convention.
constexpr int64_t kENOSYS = 38;

// Every number the subsystem tracks fits below this; the tables in
// syscall_64.tbl stop well before it.
constexpr uint32_t kSyscallLimit = 512;

} // namespace ssl::abi
