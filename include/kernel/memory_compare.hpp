#pragma once
#include <stddef.h>
#include <stdint.h>

namespace kernel_memory {
// x86 allows unaligned loads; may_alias preserves the byte-array contract.
using UnalignedWord = uint64_t __attribute__((may_alias, aligned(1)));
inline int compare(const void* left, const void* right, size_t count) {
    auto* lhs = static_cast<const uint8_t*>(left);
    auto* rhs = static_cast<const uint8_t*>(right);
    size_t words = count/sizeof(UnalignedWord);
    if (words) {
        bool different;
        // As with memcpy, kernel entry points guarantee DF=0. The string
        // instruction avoids a C loop's per-word overhead in unoptimized builds.
        asm volatile("repe cmpsq" : "+S"(lhs), "+D"(rhs), "+c"(words), "=@ccnz"(different) : : "memory");
        if (different) { lhs -= sizeof(UnalignedWord); rhs -= sizeof(UnalignedWord); count = sizeof(UnalignedWord); }
        else count %= sizeof(UnalignedWord);
    }
    // A mismatching word must still use its first differing unsigned byte:
    // comparing the little-endian integer would reverse lexical order.
    while (count--) {
        if (*lhs != *rhs) return *lhs < *rhs ? -1 : 1;
        ++lhs; ++rhs;
    }
    return 0;
}
}
