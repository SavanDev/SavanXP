#include "kernel/random.hpp"

#include "arch/cpu.hpp"
#include "kernel/console.hpp"
#include "kernel/random_crypto.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"
#include "kernel/virtio_rng.hpp"

namespace {

// Fresh entropy is folded into the key at most this often once seeded, so a
// noisy source cannot make the generator rekey on every call.
constexpr uint64_t kReseedIntervalMs = 60000;
// How long boot waits for virtio-rng's first bytes.
constexpr uint64_t kBootWaitNs = 250ull * 1000000ull;
// Credited bits the pool keeps count of; more says nothing new.
constexpr uint32_t kPoolBitsMax = 1024;

random_crypto::Blake2s g_pool = {};
uint32_t g_pool_bits = 0;
uint8_t g_key[random_crypto::kChachaKeyBytes] = {};
uint64_t g_generation = 0;
bool g_seeded = false;
uint32_t g_sources = 0;
uint32_t g_reseeds = 0;
uint64_t g_last_reseed_ms = 0;
uint64_t g_timing_ticks = 0;

void pool_restart() {
    random_crypto::blake2s_init(g_pool, random_crypto::kBlake2sOutBytes);
}

// The pool's digest becomes part of the key: key = BLAKE2s(key || pool).
void reseed(uint64_t now_ms) {
    uint8_t seed[random_crypto::kBlake2sOutBytes];
    random_crypto::Blake2s mix;

    random_crypto::blake2s_final(g_pool, seed);
    pool_restart();
    random_crypto::blake2s_init(mix, sizeof(g_key));
    random_crypto::blake2s_update(mix, g_key, sizeof(g_key));
    random_crypto::blake2s_update(mix, seed, sizeof(seed));
    random_crypto::blake2s_final(mix, g_key);
    random_crypto::wipe(seed, sizeof(seed));
    g_pool_bits = 0;
    g_seeded = true;
    g_reseeds += 1;
    g_last_reseed_ms = now_ms;
}

void mix_cpu_sources() {
    uint64_t value = 0;

    for (int index = 0; index < 8; ++index) {
        if (arch::hardware_seed_u64(value)) {
            random::add_entropy(&value, sizeof(value), 64, random::kSourceRdseed);
        }
    }
    for (int index = 0; index < 8; ++index) {
        if (arch::hardware_random_u64(value)) {
            random::add_entropy(&value, sizeof(value), 0, random::kSourceRdrand);
        }
    }
    random_crypto::wipe(&value, sizeof(value));
}

void mix_timing() {
    const uint64_t sample[2] = {arch::read_timestamp_counter(), timer::monotonic_ns()};
    random::add_entropy(sample, sizeof(sample), 0, random::kSourceTiming);
}

} // namespace

namespace random {

void add_entropy(const void* data, size_t length, uint32_t credited_bits, uint32_t source) {
    const uint32_t header[2] = {source, static_cast<uint32_t>(length)};

    if (data == nullptr || length == 0) {
        return;
    }
    random_crypto::blake2s_update(g_pool, header, sizeof(header));
    random_crypto::blake2s_update(g_pool, data, length);
    g_sources |= source;
    if (credited_bits > static_cast<uint32_t>(length) * 8u) {
        credited_bits = static_cast<uint32_t>(length) * 8u;
    }
    g_pool_bits = g_pool_bits + credited_bits > kPoolBitsMax ? kPoolBitsMax : g_pool_bits + credited_bits;
    if (!g_seeded && g_pool_bits >= kSeedBits) {
        reseed(timer::monotonic_ms());
    }
}

void initialize() {
    pool_restart();
    mix_timing();
    mix_cpu_sources();
    if (virtio_rng::initialize()) {
        const uint64_t deadline = timer::monotonic_ns() + kBootWaitNs;
        // Even when RDSEED already seeded the generator, the host's bytes
        // are worth having before the first process asks.
        while ((g_sources & kSourceVirtioRng) == 0 && timer::monotonic_ns() < deadline) {
            if (!virtio_rng::poll()) {
                asm volatile("pause");
            }
        }
    }
    mix_timing();
    if (g_seeded) {
        console::printf("random: seeded from%s%s (%u reseeds)\n",
            (g_sources & kSourceVirtioRng) != 0 ? " virtio-rng" : "",
            (g_sources & kSourceRdseed) != 0 ? " rdseed" : "", static_cast<unsigned>(g_reseeds));
    } else {
        console::printf("random: not seeded: no entropy source (%u of %u bits)\n",
            static_cast<unsigned>(g_pool_bits), static_cast<unsigned>(kSeedBits));
    }
}

void tick(uint64_t now_ms) {
    (void)virtio_rng::poll();
    // A timing sample now and then: never credited, but it costs an
    // attacker who knows every other input.
    if ((++g_timing_ticks & 63u) == 0) {
        mix_timing();
    }
    if (g_seeded && g_pool_bits >= kSeedBits && now_ms - g_last_reseed_ms >= kReseedIntervalMs) {
        reseed(now_ms);
    }
}

bool seeded() { return g_seeded; }

bool fill(void* out, size_t length) {
    uint8_t nonce[random_crypto::kChachaNonceBytes] = {};
    uint8_t block[random_crypto::kChachaBlockBytes];
    uint8_t* bytes = static_cast<uint8_t*>(out);

    if (!g_seeded || length > kMaxRequest || (out == nullptr && length != 0)) {
        return false;
    }
    // Fast key erasure: the first block gives the next key and a one-time
    // output key; the old key is gone before a byte leaves. A later
    // compromise of the state cannot recover what was handed out before.
    g_generation += 1;
    for (int index = 0; index < 8; ++index) {
        nonce[index] = static_cast<uint8_t>(g_generation >> (8 * index));
    }
    random_crypto::chacha20_block(g_key, 0, nonce, block);
    for (size_t index = 0; index < sizeof(g_key); ++index) {
        g_key[index] = block[index];
    }
    {
        uint8_t output_key[random_crypto::kChachaKeyBytes];
        uint8_t zero_nonce[random_crypto::kChachaNonceBytes] = {};
        uint32_t counter = 0;

        for (size_t index = 0; index < sizeof(output_key); ++index) {
            output_key[index] = block[sizeof(g_key) + index];
        }
        while (length > 0) {
            const size_t take = length < sizeof(block) ? length : sizeof(block);
            random_crypto::chacha20_block(output_key, counter++, zero_nonce, block);
            memcpy(bytes, block, take);
            bytes += take;
            length -= take;
        }
        random_crypto::wipe(output_key, sizeof(output_key));
    }
    random_crypto::wipe(block, sizeof(block));
    return true;
}

void status(Status& out) {
    out.seeded = g_seeded;
    out.sources = g_sources;
    out.pool_bits = g_pool_bits;
    out.reseeds = g_reseeds;
}

} // namespace random
