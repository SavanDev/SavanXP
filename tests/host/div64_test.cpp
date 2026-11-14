#include <cassert>
#include <cstdint>
#include <limits>

extern "C" uint64_t __udivdi3(uint64_t, uint64_t);
extern "C" uint64_t __umoddi3(uint64_t, uint64_t);
extern "C" int64_t __divdi3(int64_t, int64_t);
extern "C" int64_t __moddi3(int64_t, int64_t);

int main() {
    const uint64_t values[] = {0, 1, 2, 3, 0xffffffff, 0x100000000,
        0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff};
    for (uint64_t dividend : values) {
        for (uint64_t divisor : values) {
            if (divisor == 0) continue;
            assert(__udivdi3(dividend, divisor) == dividend / divisor);
            assert(__umoddi3(dividend, divisor) == dividend % divisor);
            const int64_t left = static_cast<int64_t>(dividend);
            const int64_t right = static_cast<int64_t>(divisor);
            if (left == std::numeric_limits<int64_t>::min() && right == -1) continue;
            assert(__divdi3(left, right) == left / right);
            assert(__moddi3(left, right) == left % right);
        }
    }
    uint64_t state = 0x123456789abcdef0;
    for (unsigned i = 0; i < 2000; ++i) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        uint64_t dividend = state;
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        uint64_t divisor = state | 1;
        assert(__udivdi3(dividend, divisor) == dividend / divisor);
        assert(__umoddi3(dividend, divisor) == dividend % divisor);
        int64_t left = static_cast<int64_t>(dividend);
        int64_t right = static_cast<int64_t>(divisor);
        assert(__divdi3(left, right) == left / right);
        assert(__moddi3(left, right) == left % right);
    }
}
