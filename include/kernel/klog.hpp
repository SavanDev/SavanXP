#pragma once

#include <stddef.h>
#include <stdint.h>

#include "abi/savanxp_native_abi.h"
#include "kernel/credentials.hpp"

/*
 * The system log (docs/kernel/SYSTEM_LOG.md).
 *
 * The first kBootKept lines that left for the serial port and a ring of the
 * latest ones after them, kCapacity in all, each one with when it started,
 * who wrote it and the writer's name. On real hardware
 * there is often no serial cable: this is what lets a program in the system
 * show the same log QEMU prints to the terminal.
 *
 * The console feeds it: console::write_char and console::serial_write hand it
 * every character they send. Text is the kernel's unless a ScopedSource says a
 * process is writing it. Like the console, it relies on the kernel lock and
 * takes none of its own.
 */
namespace klog {

constexpr size_t kCapacity = 2048;
// Of those, the first lines ever written, kept for the life of the system.
constexpr size_t kBootKept = SXN_LOG_BOOT_KEPT;

void write_char(char character);
void write(const char* text, size_t length);

// While it lives, the text the console takes is this process's: a program's
// stdout on the text console, /dev/serial, sxn_log.
class ScopedSource {
public:
    ScopedSource(uint32_t pid, uint32_t uid, const char* name);
    ~ScopedSource();
    ScopedSource(const ScopedSource&) = delete;
    ScopedSource& operator=(const ScopedSource&) = delete;

private:
    uint32_t previous_pid_;
    uint32_t previous_uid_;
    const char* previous_name_;
};

// While it lives, the console's text is not logged: the text console's echo
// of what is typed, which is input rather than a message.
class ScopedMute {
public:
    ScopedMute();
    ~ScopedMute();
    ScopedMute(const ScopedMute&) = delete;
    ScopedMute& operator=(const ScopedMute&) = delete;
};

// The first record `reader` may see whose sequence is >= `sequence`, or the
// oldest one when that sequence is gone. False when there is none. Every
// reader sees the kernel's and the system's lines; another user's programs'
// only the system does.
bool next(const cred::Credentials& reader, uint64_t sequence, sxn_log_record& out);

} // namespace klog
