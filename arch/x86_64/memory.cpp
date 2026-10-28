#include <stddef.h>
#include <stdint.h>

// The kernel is built with -mgeneral-regs-only (no SSE), so the widest copy
// unit available is 8 bytes. Instead of a C loop -- which unoptimized costs half
// a dozen instructions PER BYTE -- these use the string instructions, which
// move 8 bytes per microcode iteration and pay no loop overhead. It is the
// difference between dragging the 4 MiB of a 1280x800 frame one byte at a time
// or eight.
//
// Requirement: DF=0. The cld at every kernel entry point guarantees it
// (context.S and the DEFINE_ISR_* macros in cpu_init.cpp).
//
// The rest of the C runtime (memmove, memcmp, strlen, ...) is portable C and
// stays in kernel/runtime.cpp.
extern "C" void* memcpy(void* destination, const void* source, size_t count) {
    void* cursor = destination;
    const void* origin = source;
    size_t words = count >> 3;
    size_t tail = count & 7u;

    asm volatile("rep movsq" : "+D"(cursor), "+S"(origin), "+c"(words) : : "memory");
    asm volatile("rep movsb" : "+D"(cursor), "+S"(origin), "+c"(tail) : : "memory");

    return destination;
}

extern "C" void* memset(void* destination, int value, size_t count) {
    const uint8_t byte = static_cast<uint8_t>(value);
    // Copies the byte into all 8 positions so that stosq writes the same pattern.
    const uint64_t pattern = static_cast<uint64_t>(byte) * 0x0101010101010101ULL;
    void* cursor = destination;
    size_t words = count >> 3;
    size_t tail = count & 7u;

    asm volatile("rep stosq" : "+D"(cursor), "+c"(words) : "a"(pattern) : "memory");
    asm volatile("rep stosb" : "+D"(cursor), "+c"(tail) : "a"(byte) : "memory");

    return destination;
}
