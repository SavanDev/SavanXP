// The relative pointer speed (include/kernel/pointer_motion.hpp), compiled as
// the kernel compiles it: what each speed does to a run of counts.
#include <cstdio>
#include <cstdlib>

#include "kernel/pointer_motion.hpp"

namespace {

void check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "pointer-motion-test: FAIL %s\n", why);
        std::exit(1);
    }
}

// The pixels `count` events of `delta` move at `speed`, from a fresh remainder.
int64_t run(int32_t delta, int count, int speed) {
    int32_t remainder = 0;
    int64_t moved = 0;
    for (int i = 0; i < count; ++i) {
        moved += pointer_motion::scale(delta, speed, remainder);
    }
    return moved;
}

} // namespace

int main() {
    int32_t remainder = 0;

    check(pointer_motion::kNumerators[SAVANXP_POINTER_SPEED_DEFAULT - 1] == pointer_motion::kDenominator,
          "the default speed is one to one");
    for (int speed = SAVANXP_POINTER_SPEED_MIN + 1; speed <= SAVANXP_POINTER_SPEED_MAX; ++speed) {
        check(pointer_motion::kNumerators[speed - 1] > pointer_motion::kNumerators[speed - 2],
              "every speed is faster than the one before");
    }

    for (int32_t delta = -200; delta <= 200; ++delta) {
        remainder = 0;
        check(pointer_motion::scale(delta, SAVANXP_POINTER_SPEED_DEFAULT, remainder) == delta && remainder == 0,
              "the default speed leaves every delta alone");
    }

    // Over a long run every speed moves its exact multiple, nothing lost to
    // rounding: the fractions add up across events.
    for (int speed = SAVANXP_POINTER_SPEED_MIN; speed <= SAVANXP_POINTER_SPEED_MAX; ++speed) {
        const int64_t expected = 3200 * static_cast<int64_t>(pointer_motion::kNumerators[speed - 1]) /
            pointer_motion::kDenominator;
        check(run(1, 3200, speed) == expected, "slow single counts add up to the exact distance");
        check(run(-1, 3200, speed) == -expected, "the same distance the other way");
        check(run(7, 3200, speed) == 7 * expected, "larger deltas scale by the same factor");
    }
    check(run(1, 31, SAVANXP_POINTER_SPEED_MIN) == 0 && run(1, 32, SAVANXP_POINTER_SPEED_MIN) == 1,
          "the slowest speed moves one pixel every 32 counts");
    check(run(2, 1, SAVANXP_POINTER_SPEED_MAX) == 7, "the fastest speed moves 3.5 pixels a count");

    // A fraction collected one way is dropped when the mouse turns, so the
    // first count back moves as soon as it would from rest.
    remainder = 0;
    for (int i = 0; i < 31; ++i) {
        (void)pointer_motion::scale(1, SAVANXP_POINTER_SPEED_MIN, remainder);
    }
    check(remainder == 31, "the slow fraction is kept between events");
    check(pointer_motion::scale(-1, SAVANXP_POINTER_SPEED_MIN, remainder) == 0 && remainder == -1,
          "turning back drops the fraction of the other direction");
    check(pointer_motion::scale(0, SAVANXP_POINTER_SPEED_MIN, remainder) == 0 && remainder == -1,
          "an axis that does not move keeps its fraction");

    remainder = 0;
    check(pointer_motion::scale(INT32_MAX, SAVANXP_POINTER_SPEED_MAX, remainder) == INT32_MAX,
          "a huge delta saturates instead of wrapping");
    remainder = 5;
    check(pointer_motion::scale(9, 0, remainder) == 9 && remainder == 0 &&
          pointer_motion::scale(9, SAVANXP_POINTER_SPEED_MAX + 1, remainder) == 9,
          "an invalid speed passes deltas through");
    check(!pointer_motion::valid_speed(0) && !pointer_motion::valid_speed(21) &&
          pointer_motion::valid_speed(1) && pointer_motion::valid_speed(20),
          "the valid range is 1..20");

    std::printf("pointer-motion-test: ok\n");
    return 0;
}
