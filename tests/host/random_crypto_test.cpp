// The kernel generator's primitives (include/kernel/random_crypto.hpp)
// against independent references: RFC 8439's ChaCha20 block, RFC 7693's
// BLAKE2s "abc", and BLAKE2s digests from Python's hashlib at every length
// that crosses a block boundary, plain and keyed, fed whole and in pieces.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel/random_crypto.hpp"

static int g_failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "random-crypto-test: FAIL %s\n", what);
        g_failures += 1;
    }
}

static void from_hex(const char* hex, uint8_t* out, size_t bytes) {
    for (size_t index = 0; index < bytes; ++index) {
        unsigned value = 0;
        std::sscanf(hex + 2 * index, "%2x", &value);
        out[index] = static_cast<uint8_t>(value);
    }
}

static bool matches(const uint8_t* got, const char* hex, size_t bytes) {
    uint8_t want[64];
    from_hex(hex, want, bytes);
    return std::memcmp(got, want, bytes) == 0;
}

static void test_chacha20() {
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t block[64];

    for (int index = 0; index < 32; ++index) key[index] = static_cast<uint8_t>(index);
    from_hex("000000090000004a00000000", nonce, sizeof(nonce));
    random_crypto::chacha20_block(key, 1, nonce, block);
    check(matches(block,
              "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
              "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e",
              64),
        "RFC 8439 2.3.2 block");
    uint8_t other[64];
    random_crypto::chacha20_block(key, 2, nonce, other);
    check(std::memcmp(block, other, 64) != 0, "another counter gives another block");
}

struct Digest {
    size_t length;
    const char* plain;
    const char* keyed16;
};

static const Digest kDigests[] = {
    {0, "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9", "9536f9b267655743dee97b8a670f9f53"},
    {1, "a28ac19d6bcbe2cd1d7de183485768d598e996b07889b9b11f418cb1b4a4fb0d", "4559b111ae96763f9a75fc80c088718a"},
    {63, "de27df0e375d83c49f1af9ca8270f9f2fe7b70bf800fc01672db0e9746021ebf", "1052ac94e3acb1a81c87d50cb3cebd68"},
    {64, "5377e4ff957bda4d4535f4879876b71a61056c4cec31e78397c66ec47a86a130", "27337ab7d92116d773ea23fded157e28"},
    {65, "19b1b26fba093f4a670d8913e1b71cbb2916dfa701018cc6b05785c966593374", "0b9ae39a44ce901047c9d568694a26d0"},
    {128, "83470c75afa23d90cd7659906e4b47daa278131fbb225241dd37a40fd5355ac7", "1823d609afa81d714dab2d1412152ff4"},
    {129, "26fd8892ab0a5f12d4acbff1efd7dcd92b99e2d377f888f07e3cb81b232b11cf", "cc6a2619c2328be9b586db8900ec43d2"},
    {1000, "02a016193469710efadf8fb005ca19b509331cb847df5598cc0794bded669681", "55c7574f0da0352ecb127f2f70e0aa6a"},
};

static void test_blake2s() {
    uint8_t out[32];
    uint8_t key[32];
    static uint8_t data[1000];
    random_crypto::Blake2s state;

    random_crypto::blake2s_init(state, 32);
    random_crypto::blake2s_update(state, "abc", 3);
    random_crypto::blake2s_final(state, out);
    check(matches(out, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982", 32),
        "RFC 7693 appendix B, BLAKE2s(\"abc\")");

    for (int index = 0; index < 32; ++index) key[index] = static_cast<uint8_t>(index);
    random_crypto::blake2s_init(state, 32, key, 32);
    random_crypto::blake2s_final(state, out);
    check(matches(out, "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49", 32),
        "keyed BLAKE2s of the empty message");

    for (size_t index = 0; index < sizeof(data); ++index) data[index] = static_cast<uint8_t>(index * 7 + 3);
    for (const Digest& digest : kDigests) {
        char what[96];
        // Whole.
        random_crypto::blake2s_init(state, 32);
        random_crypto::blake2s_update(state, data, digest.length);
        random_crypto::blake2s_final(state, out);
        std::snprintf(what, sizeof(what), "BLAKE2s of %zu bytes", digest.length);
        check(matches(out, digest.plain, 32), what);
        // One byte at a time.
        random_crypto::blake2s_init(state, 32);
        for (size_t index = 0; index < digest.length; ++index) {
            random_crypto::blake2s_update(state, data + index, 1);
        }
        random_crypto::blake2s_final(state, out);
        std::snprintf(what, sizeof(what), "BLAKE2s of %zu bytes fed one at a time", digest.length);
        check(matches(out, digest.plain, 32), what);
        // Keyed, 16-byte digest, in pieces of 13.
        random_crypto::blake2s_init(state, 16, key, 32);
        for (size_t index = 0; index < digest.length; index += 13) {
            size_t take = digest.length - index < 13 ? digest.length - index : 13;
            random_crypto::blake2s_update(state, data + index, take);
        }
        random_crypto::blake2s_final(state, out);
        std::snprintf(what, sizeof(what), "keyed 16-byte BLAKE2s of %zu bytes", digest.length);
        check(matches(out, digest.keyed16, 16), what);
    }
}

int main() {
    test_chacha20();
    test_blake2s();
    if (g_failures != 0) {
        std::fprintf(stderr, "random-crypto-test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("random-crypto-test: OK\n");
    return 0;
}
