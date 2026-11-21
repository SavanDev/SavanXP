#pragma once

// The two primitives behind the kernel's random number generator
// (docs/kernel/RANDOM.md): BLAKE2s (RFC 7693) condenses whatever entropy
// arrives into a key, and ChaCha20 (RFC 8439) expands a key into output. The
// same pair Linux's generator uses. Header-only and freestanding, so the host
// test (tests/host/random_crypto_test.cpp) checks them against the RFCs'
// vectors exactly as the kernel compiles them.

#include <stddef.h>
#include <stdint.h>

namespace random_crypto {

inline uint32_t rotl32(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

inline uint32_t rotr32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

inline uint32_t load_le32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

inline void store_le32(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

// Writes zeroes the compiler may not drop: key material must not linger.
inline void wipe(void* data, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
    for (size_t index = 0; index < length; ++index) {
        bytes[index] = 0;
    }
}

// ---- ChaCha20 ------------------------------------------------------------------

constexpr size_t kChachaKeyBytes = 32;
constexpr size_t kChachaNonceBytes = 12;
constexpr size_t kChachaBlockBytes = 64;

inline void chacha_quarter(uint32_t* x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);
}

// One 64-byte block of the keystream for (key, counter, nonce), RFC 8439 2.3.
inline void chacha20_block(const uint8_t key[kChachaKeyBytes], uint32_t counter,
                           const uint8_t nonce[kChachaNonceBytes], uint8_t out[kChachaBlockBytes]) {
    uint32_t state[16];
    uint32_t working[16];

    state[0] = 0x61707865u;
    state[1] = 0x3320646eu;
    state[2] = 0x79622d32u;
    state[3] = 0x6b206574u;
    for (int index = 0; index < 8; ++index) {
        state[4 + index] = load_le32(key + 4 * index);
    }
    state[12] = counter;
    for (int index = 0; index < 3; ++index) {
        state[13 + index] = load_le32(nonce + 4 * index);
    }
    for (int index = 0; index < 16; ++index) {
        working[index] = state[index];
    }
    for (int round = 0; round < 10; ++round) {
        chacha_quarter(working, 0, 4, 8, 12);
        chacha_quarter(working, 1, 5, 9, 13);
        chacha_quarter(working, 2, 6, 10, 14);
        chacha_quarter(working, 3, 7, 11, 15);
        chacha_quarter(working, 0, 5, 10, 15);
        chacha_quarter(working, 1, 6, 11, 12);
        chacha_quarter(working, 2, 7, 8, 13);
        chacha_quarter(working, 3, 4, 9, 14);
    }
    for (int index = 0; index < 16; ++index) {
        store_le32(out + 4 * index, working[index] + state[index]);
    }
    wipe(state, sizeof(state));
    wipe(working, sizeof(working));
}

// ---- BLAKE2s ---------------------------------------------------------------------

constexpr size_t kBlake2sBlockBytes = 64;
constexpr size_t kBlake2sOutBytes = 32;

constexpr uint32_t kBlake2sIv[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr uint8_t kBlake2sSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

struct Blake2s {
    uint32_t h[8];
    uint32_t t[2];
    uint8_t buffer[kBlake2sBlockBytes];
    size_t buffered;
    size_t out_bytes;
};

inline void blake2s_compress(Blake2s& state, const uint8_t block[kBlake2sBlockBytes], bool last) {
    uint32_t m[16];
    uint32_t v[16];

    for (int index = 0; index < 16; ++index) {
        m[index] = load_le32(block + 4 * index);
    }
    for (int index = 0; index < 8; ++index) {
        v[index] = state.h[index];
        v[8 + index] = kBlake2sIv[index];
    }
    v[12] ^= state.t[0];
    v[13] ^= state.t[1];
    if (last) {
        v[14] = ~v[14];
    }
    for (int round = 0; round < 10; ++round) {
        const uint8_t* s = kBlake2sSigma[round];
        auto g = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
            v[a] = v[a] + v[b] + x; v[d] = rotr32(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];     v[b] = rotr32(v[b] ^ v[c], 12);
            v[a] = v[a] + v[b] + y; v[d] = rotr32(v[d] ^ v[a], 8);
            v[c] = v[c] + v[d];     v[b] = rotr32(v[b] ^ v[c], 7);
        };
        g(0, 4, 8, 12, m[s[0]], m[s[1]]);
        g(1, 5, 9, 13, m[s[2]], m[s[3]]);
        g(2, 6, 10, 14, m[s[4]], m[s[5]]);
        g(3, 7, 11, 15, m[s[6]], m[s[7]]);
        g(0, 5, 10, 15, m[s[8]], m[s[9]]);
        g(1, 6, 11, 12, m[s[10]], m[s[11]]);
        g(2, 7, 8, 13, m[s[12]], m[s[13]]);
        g(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int index = 0; index < 8; ++index) {
        state.h[index] ^= v[index] ^ v[8 + index];
    }
    wipe(m, sizeof(m));
    wipe(v, sizeof(v));
}

inline void blake2s_add_length(Blake2s& state, uint32_t bytes) {
    state.t[0] += bytes;
    if (state.t[0] < bytes) {
        state.t[1] += 1;
    }
}

inline void blake2s_update(Blake2s& state, const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    while (length > 0) {
        // A full buffer is compressed only once more bytes follow: the last
        // block must be compressed with the final flag.
        if (state.buffered == kBlake2sBlockBytes) {
            blake2s_add_length(state, kBlake2sBlockBytes);
            blake2s_compress(state, state.buffer, false);
            state.buffered = 0;
        }
        size_t take = kBlake2sBlockBytes - state.buffered;
        if (take > length) {
            take = length;
        }
        for (size_t index = 0; index < take; ++index) {
            state.buffer[state.buffered + index] = bytes[index];
        }
        state.buffered += take;
        bytes += take;
        length -= take;
    }
}

// Starts a BLAKE2s of `out_bytes` (1..32), keyed when key_bytes is not 0
// (up to 32), RFC 7693 section 3.3.
inline void blake2s_init(Blake2s& state, size_t out_bytes, const void* key = nullptr, size_t key_bytes = 0) {
    for (int index = 0; index < 8; ++index) {
        state.h[index] = kBlake2sIv[index];
    }
    state.h[0] ^= 0x01010000u ^ (static_cast<uint32_t>(key_bytes) << 8) ^ static_cast<uint32_t>(out_bytes);
    state.t[0] = 0;
    state.t[1] = 0;
    state.buffered = 0;
    state.out_bytes = out_bytes;
    for (size_t index = 0; index < kBlake2sBlockBytes; ++index) {
        state.buffer[index] = 0;
    }
    if (key_bytes > 0) {
        uint8_t block[kBlake2sBlockBytes] = {};
        const uint8_t* key_data = static_cast<const uint8_t*>(key);
        for (size_t index = 0; index < key_bytes; ++index) {
            block[index] = key_data[index];
        }
        blake2s_update(state, block, kBlake2sBlockBytes);
        wipe(block, sizeof(block));
    }
}

inline void blake2s_final(Blake2s& state, uint8_t* out) {
    uint8_t digest[kBlake2sOutBytes];

    blake2s_add_length(state, static_cast<uint32_t>(state.buffered));
    for (size_t index = state.buffered; index < kBlake2sBlockBytes; ++index) {
        state.buffer[index] = 0;
    }
    blake2s_compress(state, state.buffer, true);
    for (int index = 0; index < 8; ++index) {
        store_le32(digest + 4 * index, state.h[index]);
    }
    for (size_t index = 0; index < state.out_bytes; ++index) {
        out[index] = digest[index];
    }
    wipe(digest, sizeof(digest));
    wipe(&state, sizeof(state));
}

} // namespace random_crypto
