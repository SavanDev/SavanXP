#pragma once

// The kernel's cryptographically secure random number generator
// (docs/kernel/RANDOM.md): an entropy pool condensed with BLAKE2s, and a
// ChaCha20 generator keyed from it with fast key erasure. It refuses to
// produce output until 256 bits of real entropy have been credited --
// virtio-rng or RDSEED -- and says so instead of falling back to something
// weaker. Everything runs under the kernel lock: from syscalls, the BSP tick
// and boot.

#include <stddef.h>
#include <stdint.h>

namespace random {

// Sources, as the status reports them (SAVANXP_RANDOM_SOURCE_*).
enum Source : uint32_t {
    kSourceVirtioRng = 1u << 0, // credited
    kSourceRdseed = 1u << 1,    // credited
    kSourceRdrand = 1u << 2,    // mixed, never credited
    kSourceTiming = 1u << 3,    // mixed, never credited
};

// Bits of credited entropy the generator needs before its first output.
constexpr uint32_t kSeedBits = 256;
// The most a single call may ask for, as getrandom(2) allows per call here.
constexpr size_t kMaxRequest = 4096;

struct Status {
    bool seeded;
    uint32_t sources;  // every source that contributed, credited or not
    uint32_t pool_bits; // credited bits in the pool since the last reseed
    uint32_t reseeds;
};

// At boot, after PCI: mixes what the machine offers, brings virtio-rng up,
// waits a moment for its first bytes, and seeds if it can. Logs either
// "random: seeded from ..." or "random: not seeded: ...".
void initialize();

// Mixes `length` bytes into the pool, crediting `credited_bits` of entropy.
void add_entropy(const void* data, size_t length, uint32_t credited_bits, uint32_t source);

// The BSP tick: collects virtio-rng's bytes and some timing, and reseeds
// the generator once a minute when fresh entropy is waiting.
void tick(uint64_t now_ms);

bool seeded();

// Fills `out` with `length` bytes (at most kMaxRequest). False, writing
// nothing, until the generator is seeded.
bool fill(void* out, size_t length);

void status(Status& out);

} // namespace random
