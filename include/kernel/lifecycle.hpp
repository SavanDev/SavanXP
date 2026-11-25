#pragma once

// What every driver's start and stop share (docs/kernel/DRIVER_LIFECYCLE.md):
// a record of what a start acquired, given back in reverse when the start
// fails or the device stops, and failure injection at each stage of a start.
// No hardware access and no kernel state beyond the injection, so the host
// tests compile it as the kernel does.

#include <stddef.h>
#include <stdint.h>

namespace lifecycle {

// Gives back one thing a stage took: frees a buffer, takes a handler off,
// releases the function.
using Release = void (*)(void* context);

// At most this many acquisitions per start.
constexpr size_t kMaxSteps = 16;

// What one start acquired, in the order it did. Each stage that took
// something pushes how to give it back; unwind() gives everything back, last
// taken first, so a failed start releases exactly what it got and a stop
// releases the rest in the same order.
class Acquired {
public:
    // False, and nothing recorded, when the record is full: the caller gives
    // back what it just took itself and fails the start.
    bool push(const char* stage, Release release, void* context);
    // Every release, last pushed first; the record is empty after.
    void unwind();
    size_t count() const { return count_; }
    // The stage that pushed entry `index`, for a log line; nullptr past the end.
    const char* stage(size_t index) const;

private:
    struct Step {
        const char* stage;
        Release release;
        void* context;
    };
    Step steps_[kMaxSteps] = {};
    size_t count_ = 0;
};

// The next start of `driver` fails at stage `stage`, counting from 1; 0
// clears it. One injection at a time, for the self-tests.
void inject_failure(const char* driver, unsigned stage);
// Called by a start at the top of stage `stage` (counting from 1): true when
// that stage must fail now. A hit clears the injection, so it fails once.
bool injected(const char* driver, unsigned stage);

} // namespace lifecycle
