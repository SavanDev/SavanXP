#pragma once
#include <stdint.h>

#include "abi/savanxp_abi.h"

/* Relative pointer speed (docs/kernel/INPUT.md#pointer-speed). Plain
 * arithmetic, so the host test compiles it as the kernel does. */
namespace pointer_motion {

constexpr int32_t kDenominator = 32;

// The multiplier of each speed, in 32nds: 1/32 at 1, one to one at 10, 3.5 at
// 20. The steps of Windows' pointer speed slider without "enhance pointer
// precision", so a speed feels the same on both.
constexpr int32_t kNumerators[SAVANXP_POINTER_SPEED_MAX] = {
    1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112,
};

inline bool valid_speed(int speed) {
    return speed >= SAVANXP_POINTER_SPEED_MIN && speed <= SAVANXP_POINTER_SPEED_MAX;
}

// One axis. `remainder` keeps the fraction a scaled delta did not move, so a
// slow speed still moves the pointer one pixel for every few counts instead of
// rounding each one to zero. A move the other way drops the fraction: it
// belongs to the opposite direction and would only delay the turn.
inline int32_t scale(int32_t delta, int speed, int32_t& remainder) {
    if (!valid_speed(speed) || speed == SAVANXP_POINTER_SPEED_DEFAULT) {
        remainder = 0;
        return delta;
    }
    if ((delta > 0 && remainder < 0) || (delta < 0 && remainder > 0)) {
        remainder = 0;
    }
    const int64_t total = static_cast<int64_t>(delta) * kNumerators[speed - 1] + remainder;
    int64_t moved = total / kDenominator;
    remainder = static_cast<int32_t>(total - moved * kDenominator);
    if (moved > INT32_MAX) {
        moved = INT32_MAX;
    } else if (moved < INT32_MIN) {
        moved = INT32_MIN;
    }
    return static_cast<int32_t>(moved);
}

} // namespace pointer_motion
