#include <stdint.h>

#if defined(__PIC__)
// i386 accesses even its stack guard through a GOT pointer. Resolve the main
// image's RELATIVE entries before calling any protected C function. This is
// the self-relocation step normally performed by an ELF interpreter; SavanXP
// embeds that interpreter in the executable, as in the reference design.
unsigned int sx_bootstrap_relocated;
__attribute__((no_stack_protector))
static int mapped(const uint32_t* programs, uint32_t count, uint32_t at, uint32_t size, int write) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* ph = programs + i * 8;
        if (ph[0] != 1 || (write && !(ph[6] & 2)) || at < ph[2]) continue;
        const uint32_t offset = at - ph[2];
        if (offset <= ph[5] && size <= ph[5] - offset) return 1;
    }
    return 0;
}
__attribute__((no_stack_protector, visibility("hidden")))
int sx_bootstrap_relative(uint32_t base) {
    const uint32_t* header = (const uint32_t*)base;
    const uint16_t* halves = (const uint16_t*)base;
    if (header[0] != 0x464c457f || halves[8] != 3) return 0;
    const uint32_t* programs = (const uint32_t*)(base + header[7]);
    const uint32_t count = halves[22];
    if (halves[21] != 32 || count > 24) return -1;
    uint32_t bias = base, dynamic = 0, dynamic_size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* ph = programs + i * 8;
        if (ph[0] == 1 && ph[1] == 0) bias = base - (ph[2] & ~4095u);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* ph = programs + i * 8;
        if (ph[0] == 2) { dynamic = ph[2] + bias; dynamic_size = ph[5]; break; }
    }
    if (!dynamic || !mapped(programs, count, dynamic - bias, dynamic_size, 0)) return -1;
    uint32_t rel = 0, bytes = 0;
    for (uint32_t i = 0; i + 8 <= dynamic_size; i += 8) {
        const uint32_t* entry = (const uint32_t*)(dynamic + i);
        if (!entry[0]) break;
        if (entry[0] == 17) rel = entry[1];
        if (entry[0] == 18) bytes = entry[1];
    }
    if (bytes % 8 || (bytes && !mapped(programs, count, rel, bytes, 0))) return -1;
    const uint32_t* entries = (const uint32_t*)(rel + bias);
    for (uint32_t i = 0; i < bytes / 8; ++i) {
        if ((entries[i * 2 + 1] & 255u) == 8) {
            if (!mapped(programs, count, entries[i * 2], 4, 1)) return -1;
            *(uint32_t*)(bias + entries[i * 2]) += bias;
        }
    }
    sx_bootstrap_relocated = 1;
    return 0;
}
#endif
