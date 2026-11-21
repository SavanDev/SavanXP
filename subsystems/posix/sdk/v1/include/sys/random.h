#pragma once

/* getrandom(2) over the kernel's cryptographic generator
 * (docs/kernel/RANDOM.md). At most 4096 bytes per call; while the generator
 * is not seeded it fails with EAGAIN whatever the flags, since there is no
 * weaker pool to fall back on. GRND_NONBLOCK and GRND_RANDOM are accepted
 * and change nothing. */

#include "sys/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM 0x0002

ssize_t getrandom(void* buffer, size_t length, unsigned int flags);

#ifdef __cplusplus
}
#endif
