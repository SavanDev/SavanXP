# The kernel's random number generator

`kernel/random.cpp` is the system's cryptographically secure random number
generator: what TLS keys, nonces and every other secret come from. It is the
first part of N5 in
[WEBVIEWER_ROADMAP.md](../roadmaps/open/WEBVIEWER_ROADMAP.md): validated HTTPS
needs randomness nobody can predict, and the tree had none. `arch::random_u64`
(RDRAND, or the TSC on old CPUs) stays for stack canaries and placement
decisions at early boot, and is not this.

## How it works

The design is Linux's since 5.17, in miniature:

- **The pool** is a running BLAKE2s (RFC 7693). Every input is hashed into
  it with its source and length. Each source says how many bits of entropy
  its bytes carry; the pool counts those credited bits, up to 1024.
- **The generator** is a ChaCha20 (RFC 8439) key. The first time the pool
  holds 256 credited bits, its digest becomes the key and the generator is
  **seeded**. From then on, once a minute at most, fresh credited entropy is
  folded in: `key = BLAKE2s(key || pool)`.
- **Output** uses fast key erasure: each request runs one ChaCha20 block
  under the current key, takes the first half as the next key and the second
  half as a one-time key, and only then generates the bytes from that
  one-time key. The old key is gone before a byte leaves, so a later
  compromise of the kernel's memory cannot recover earlier output.

Both primitives live header-only in `include/kernel/random_crypto.hpp` and
are checked on the host by `random-crypto-test`: RFC 8439's ChaCha20 block,
RFC 7693's BLAKE2s("abc"), and Python `hashlib` digests at every length that
crosses a block boundary, plain and keyed, fed whole, byte by byte and in odd
pieces.

## Sources, and what is credited

| Source | Credited | Where |
|---|---|---|
| virtio-rng | 8 bits per byte | `kernel/virtio_rng.cpp`: the host's entropy (QEMU's `virtio-rng-pci`, backed by the host's `/dev/urandom`). One 64-byte request is kept posted; the BSP tick takes it when filled and posts it again. |
| RDSEED | 64 bits per word | `arch::hardware_seed_u64`: the CPU's conditioned entropy source, 8 words at boot. |
| RDRAND | never | `arch::hardware_random_u64`: the output of the CPU's own generator, mixed in but not counted. A CPU with RDRAND and no RDSEED does not seed the system by itself. |
| timing | never | the TSC and the monotonic clock, at boot and every 64 ticks. |

Only sources whose output is entropy rather than the expansion of a seed
count. A machine with none of the credited ones -- no virtio-rng and a CPU
without RDSEED -- **is never seeded, and says so**: the boot log reads
`random: not seeded: no entropy source (N of 256 bits)`, and every request
fails. There is deliberately no fallback to something weaker: a TLS
connection that cannot get real randomness must fail, not proceed with keys
an attacker can guess. Otherwise the log reads `random: seeded from
virtio-rng rdseed (1 reseeds)`.

At boot, `random::initialize()` runs after PCI and before the first process:
it mixes timing, RDSEED and RDRAND, brings virtio-rng up and waits up to
250 ms for its first bytes.

## Interfaces

| Subsystem | Call | Notes |
|---|---|---|
| SUN | `sxn_random_bytes(buffer, length)` (`SXN_SYS_RANDOM_BYTES`) | at most `SXN_RANDOM_MAX` (4096); the length, `-EAGAIN` unseeded, `-EINVAL` |
| SUN | `sxn_random_status(&status)` (`SXN_SYS_RANDOM_STATUS`) | seeded, sources, credited bits waiting, reseeds |
| POSIX | `getrandom(buffer, length, flags)` (`<sys/random.h>`, `SAVANXP_SYS_GETRANDOM`) | a short read past 4096; `GRND_NONBLOCK` and `GRND_RANDOM` change nothing; `EAGAIN` unseeded whatever the flags |
| POSIX | `getentropy(buffer, length)` (`<unistd.h>`) | up to 256 bytes, `EINVAL` past that |
| POSIX | `random_status(&status)` (`savanxp/libc.h`, `SAVANXP_SYS_RANDOM_STATUS`) | the same `struct savanxp_random_status` as SUN's |
| Linux (SSL) | `getrandom(2)` | up to 256 bytes per call; `EAGAIN` unseeded, except with `GRND_INSECURE`, which asks for the CPU's unvetted source and gets it |
| ELF loader | `AT_RANDOM` | the generator's 16 bytes once seeded |

Nothing blocks. Linux's `getrandom` without `GRND_NONBLOCK` waits for the
seed; here seeding is decided at boot -- a source either answered or there
is none -- so waiting would mean waiting forever.

## Tests

- `random-crypto-test` (host): the primitives against the RFCs and hashlib.
- `smoke native-test`: `nativetest` checks the status (seeded, from
  virtio-rng), the limits, that two draws differ, and that 64 KiB of output
  has every byte value within loose bounds -- which proves nothing about
  cryptographic quality, and catches a stuck, zeroed or skipped buffer.
- `smoke random-unseeded`: a machine with no virtio-rng and a CPU without
  RDRAND or RDSEED (`run_qemu.py --no-rng`): unseeded, no credited source,
  `-EAGAIN`, and the caller's buffer untouched.
- `smoke smoke`: `libctest` calls `getrandom` and `getentropy` as a port
  would, and the core smoke checks the POSIX status.

Every QEMU machine the tools start has `virtio-rng-pci` unless `--no-rng`
removes it.

## Not here

`/dev/random` and `/dev/urandom` device files, reseeding from interrupt
timing, and saving a seed across reboots. RDRAND-only machines stay
unseeded; whether to trust RDRAND there is a decision for when one matters.
