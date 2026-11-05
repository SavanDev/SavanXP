#pragma once

/* CRC-32 as GPT, zlib and Ethernet compute it (reflected, polynomial
 * 0xEDB88320, initial and final XOR 0xFFFFFFFF). Header-only and freestanding,
 * so the kernel's partition reader, libsxdisk's writer and the host tests
 * share one definition: a table one side writes is a table the other accepts.
 *
 * Bitwise, without a table: it runs over a GPT header and a 16 KiB entry array
 * a few times per disk, which is not worth 1 KiB of table. */

#include <stddef.h>
#include <stdint.h>

/* Continues a CRC over more bytes: sx_crc32_update(sx_crc32(a), b) is the CRC
 * of a followed by b. Start from 0. */
static inline uint32_t sx_crc32_update(uint32_t crc, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    crc = ~crc;
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static inline uint32_t sx_crc32(const void* data, size_t size) {
    return sx_crc32_update(0, data, size);
}
