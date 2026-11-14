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
constexpr uint64_t kSysRead = 0;
constexpr uint64_t kSysWrite = 1;
constexpr uint64_t kSysOpen = 2;
constexpr uint64_t kSysClose = 3;
constexpr uint64_t kSysStat = 4;
constexpr uint64_t kSysFstat = 5;
constexpr uint64_t kSysLseek = 8;
constexpr uint64_t kSysMmap = 9;
constexpr uint64_t kSysMunmap = 11;
constexpr uint64_t kSysBrk = 12;
constexpr uint64_t kSysIoctl = 16;
constexpr uint64_t kSysWritev = 20;
constexpr uint64_t kSysMremap = 25;
constexpr uint64_t kSysNanosleep = 35;
constexpr uint64_t kSysExit = 60;
constexpr uint64_t kSysUnlink = 87;
constexpr uint64_t kSysArchPrctl = 158;
constexpr uint64_t kSysSetTidAddress = 218;
constexpr uint64_t kSysClockGettime = 228;
constexpr uint64_t kSysExitGroup = 231;

// Errors, returned negated in RAX.
constexpr int64_t kEPERM = 1;
constexpr int64_t kENOENT = 2;
constexpr int64_t kEBADF = 9;
constexpr int64_t kENOMEM = 12;
constexpr int64_t kEACCES = 13;
constexpr int64_t kEFAULT = 14;
constexpr int64_t kEEXIST = 17;
constexpr int64_t kENODEV = 19;
constexpr int64_t kEISDIR = 21;
constexpr int64_t kEINVAL = 22;
constexpr int64_t kEMFILE = 24;
constexpr int64_t kENOTTY = 25;
constexpr int64_t kESPIPE = 29;
constexpr int64_t kENAMETOOLONG = 36;
constexpr int64_t kENOSYS = 38;

// The most one read or write moves (MAX_RW_COUNT): a page short of 2 GiB.
constexpr uint64_t kMaxRwCount = 0x7ffff000;

// mmap and mremap (include/uapi/asm-generic/mman-common.h, mman.h).
constexpr uint64_t kProtRead = 0x1;
constexpr uint64_t kProtWrite = 0x2;
constexpr uint64_t kProtExec = 0x4;
// Accepted and meaningless on x86, as on Linux.
constexpr uint64_t kProtSem = 0x8;
constexpr uint64_t kMapShared = 0x01;
constexpr uint64_t kMapPrivate = 0x02;
constexpr uint64_t kMapSharedValidate = 0x03;
constexpr uint64_t kMapType = 0x0f;
constexpr uint64_t kMapFixed = 0x10;
constexpr uint64_t kMapAnonymous = 0x20;
constexpr uint64_t kMapDenyWrite = 0x0800;
constexpr uint64_t kMapExecutable = 0x1000;
constexpr uint64_t kMapNoReserve = 0x4000;
constexpr uint64_t kMapStack = 0x20000;
constexpr uint64_t kMapFixedNoReplace = 0x100000;
constexpr uint64_t kMremapMayMove = 1;
constexpr uint64_t kMremapFixed = 2;
constexpr uint64_t kMremapDontUnmap = 4;

// open (include/uapi/asm-generic/fcntl.h), lseek.
constexpr uint64_t kOpenAccessMode = 03;
constexpr uint64_t kOpenReadOnly = 00;
constexpr uint64_t kOpenWriteOnly = 01;
constexpr uint64_t kOpenReadWrite = 02;
constexpr uint64_t kOpenCreate = 0100;
constexpr uint64_t kOpenNoCtty = 0400;
constexpr uint64_t kOpenTruncate = 01000;
constexpr uint64_t kOpenLargeFile = 0100000;
constexpr uint64_t kOpenCloseOnExec = 02000000;
constexpr uint64_t kSeekSet = 0;
constexpr uint64_t kSeekCur = 1;
constexpr uint64_t kSeekEnd = 2;

// struct stat of x86-64 (arch/x86/include/uapi/asm/stat.h).
struct Stat {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad0;
    uint64_t rdev;
    int64_t size;
    int64_t blksize;
    int64_t blocks;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    int64_t unused[3];
};
static_assert(sizeof(Stat) == 144, "x86-64 struct stat is 144 bytes");
constexpr uint32_t kModeSocket = 0140000;
constexpr uint32_t kModeRegular = 0100000;
constexpr uint32_t kModeDirectory = 0040000;
constexpr uint32_t kModeCharacter = 0020000;
constexpr uint32_t kModeFifo = 0010000;

// Clocks and times (include/uapi/linux/time.h).
constexpr uint64_t kClockRealtime = 0;
constexpr uint64_t kClockMonotonic = 1;
// The clock ids Linux has; one past the last.
constexpr uint64_t kClockLimit = 12;
struct Timespec {
    int64_t seconds;
    int64_t nanoseconds;
};
static_assert(sizeof(Timespec) == 16, "struct timespec is two 64-bit words");

// arch_prctl (arch/x86/include/uapi/asm/prctl.h).
constexpr uint64_t kArchSetFs = 0x1002;

// The vectors of readv and writev (UIO_MAXIOV), each a struct iovec.
constexpr uint64_t kIovMax = 1024;
struct Iovec {
    uint64_t base;
    uint64_t length;
};
static_assert(sizeof(Iovec) == 16, "struct iovec is two 64-bit words");

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
