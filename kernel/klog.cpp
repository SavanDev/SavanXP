#include "kernel/klog.hpp"

#include "kernel/string.hpp"
#include "kernel/timer.hpp"

/*
 * The system log: what the console sends to the serial port, kept as lines.
 * docs/kernel/SYSTEM_LOG.md explains the format and who may read what.
 */

namespace {

constexpr size_t kTextMax = SXN_LOG_TEXT_MAX;
constexpr size_t kNameMax = SXN_LOG_SOURCE_MAX;

// The line each writer is in the middle of. Writers interleave -- a driver
// logs while a program is halfway through a line -- so each keeps its own
// until its line break. Slot 0 is the kernel's; the rest go to processes and
// the least recently used one is committed when a new writer needs room.
constexpr size_t kPendingSlots = 8;

struct Pending {
    bool used;
    uint32_t pid;
    uint32_t uid;
    uint32_t flags;
    uint64_t start_ns;
    uint64_t touched;
    size_t length;
    char name[kNameMax];
    char text[kTextMax];
};

sxn_log_record g_ring[klog::kCapacity];
// The writer's uid for each record, which decides who may read it. Kept out
// of the record because the reader never sees it.
uint32_t g_ring_uid[klog::kCapacity];
uint64_t g_last_sequence = 0;

// The first kBootKept records are the boot: the processor, the PCI devices,
// what each driver found. They are never overwritten, so a machine that has
// been up for hours still shows how it started. The rest of the array is the
// ring for every later line.
constexpr size_t kBootKept = klog::kBootKept;
constexpr size_t kRolling = klog::kCapacity - kBootKept;
static_assert(kBootKept < klog::kCapacity, "the ring needs room past the boot lines");

size_t slot_of(uint64_t sequence) {
    if (sequence <= kBootKept) {
        return static_cast<size_t>(sequence - 1);
    }
    return kBootKept + static_cast<size_t>((sequence - kBootKept - 1) % kRolling);
}

Pending g_pending[kPendingSlots];
uint64_t g_touch_clock = 0;

// Who writes now; pid 0 is the kernel.
uint32_t g_source_pid = 0;
uint32_t g_source_uid = 0;
const char* g_source_name = nullptr;
uint32_t g_mute_depth = 0;

void copy_name(char* destination, const char* source) {
    size_t index = 0;
    if (source != nullptr) {
        for (; index + 1 < kNameMax && source[index] != '\0'; ++index) {
            destination[index] = source[index];
        }
    }
    destination[index] = '\0';
}

bool same_name(const char* stored, const char* name) {
    char copy[kNameMax];
    copy_name(copy, name);
    return strcmp(stored, copy) == 0;
}

void commit(Pending& pending, uint32_t extra_flags) {
    // An empty line carries nothing; the kernel prints a few as spacing.
    if (pending.length == 0 && (pending.flags & SXN_LOG_CONTINUED) == 0) {
        pending.flags = 0;
        return;
    }

    const uint64_t sequence = ++g_last_sequence;
    const size_t index = slot_of(sequence);
    sxn_log_record& record = g_ring[index];

    record.sequence = sequence;
    record.time_ns = pending.start_ns;
    record.pid = pending.pid;
    record.flags = pending.flags | extra_flags;
    memcpy(record.source, pending.name, kNameMax);
    memcpy(record.text, pending.text, pending.length);
    memset(record.text + pending.length, 0, kTextMax - pending.length);
    g_ring_uid[index] = pending.uid;

    pending.length = 0;
    pending.flags = 0;
}

Pending& kernel_slot() {
    Pending& slot = g_pending[0];
    if (!slot.used) {
        slot.used = true;
        copy_name(slot.name, "kernel");
    }
    return slot;
}

Pending& current_slot() {
    if (g_source_pid == 0) {
        return kernel_slot();
    }

    // The name is part of the key: a pid that comes back under another name
    // is another process, and gets a slot of its own.
    Pending* free_slot = nullptr;
    Pending* oldest = nullptr;
    for (size_t index = 1; index < kPendingSlots; ++index) {
        Pending& slot = g_pending[index];
        if (!slot.used) {
            if (free_slot == nullptr) {
                free_slot = &slot;
            }
            continue;
        }
        if (slot.pid == g_source_pid && same_name(slot.name, g_source_name)) {
            slot.touched = ++g_touch_clock;
            return slot;
        }
        if (oldest == nullptr || slot.touched < oldest->touched) {
            oldest = &slot;
        }
    }

    // No room: the writer heard from least recently gives up its slot, and
    // whatever it left half written is committed as it was.
    Pending* victim = free_slot != nullptr ? free_slot : oldest;
    if (victim->used && victim->length != 0) {
        commit(*victim, SXN_LOG_UNTERMINATED);
    }
    victim->used = true;
    victim->pid = g_source_pid;
    victim->uid = g_source_uid;
    victim->flags = 0;
    victim->length = 0;
    victim->touched = ++g_touch_clock;
    copy_name(victim->name, g_source_name);
    return *victim;
}

} // namespace

namespace klog {

void write_char(char character) {
    if (g_mute_depth != 0 || character == '\r') {
        return;
    }

    Pending& pending = current_slot();
    if (character == '\n') {
        commit(pending, 0);
        return;
    }
    if (character == '\b') {
        if (pending.length != 0) {
            --pending.length;
        }
        return;
    }
    // A tab would split the line into a list's columns, and other control
    // bytes draw nothing useful.
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7f) {
        character = ' ';
    }

    if (pending.length + 1 >= kTextMax) {
        commit(pending, 0);
        pending.flags = SXN_LOG_CONTINUED;
    }
    if (pending.length == 0) {
        pending.start_ns = timer::monotonic_ns();
    }
    pending.text[pending.length++] = character;
}

void write(const char* text, size_t length) {
    if (text == nullptr) {
        return;
    }
    for (size_t index = 0; index < length; ++index) {
        write_char(text[index]);
    }
}

ScopedSource::ScopedSource(uint32_t pid, uint32_t uid, const char* name)
    : previous_pid_(g_source_pid), previous_uid_(g_source_uid), previous_name_(g_source_name) {
    g_source_pid = pid;
    g_source_uid = uid;
    g_source_name = name;
}

ScopedSource::~ScopedSource() {
    g_source_pid = previous_pid_;
    g_source_uid = previous_uid_;
    g_source_name = previous_name_;
}

ScopedMute::ScopedMute() {
    ++g_mute_depth;
}

ScopedMute::~ScopedMute() {
    --g_mute_depth;
}

bool next(const cred::Credentials& reader, uint64_t sequence, sxn_log_record& out) {
    if (g_last_sequence == 0) {
        return false;
    }
    if (sequence == 0) {
        sequence = 1;
    }
    // Past the boot lines, the oldest the ring still holds; the sequences in
    // between were overwritten.
    const uint64_t oldest_rolling = g_last_sequence > kCapacity
        ? g_last_sequence - kRolling + 1
        : kBootKept + 1;
    if (sequence > kBootKept && sequence < oldest_rolling) {
        sequence = oldest_rolling;
    }

    const bool system = cred::acts_as_system(reader);
    for (; sequence <= g_last_sequence; ++sequence) {
        if (sequence == kBootKept + 1 && sequence < oldest_rolling) {
            sequence = oldest_rolling;
        }
        const size_t index = slot_of(sequence);
        const sxn_log_record& record = g_ring[index];
        const uint32_t owner = g_ring_uid[index];
        // Another user's programs are theirs; the kernel's and the system's
        // lines are everyone's, since they are the system's state.
        if (record.pid == 0 || owner == 0 || system || owner == reader.uid || owner == reader.euid) {
            out = record;
            return true;
        }
    }
    return false;
}

} // namespace klog
