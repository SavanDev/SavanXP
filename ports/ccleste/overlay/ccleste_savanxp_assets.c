/*
 * Asset decoding for the SavanXP ccleste port. See ccleste_savanxp_assets.h.
 */
#include "savanxp/libc.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ccleste_savanxp_assets.h"

/* Both upstream BMPs are well under 8 KiB and the largest sound effect is
 * 188 KiB, so a single generous ceiling is simpler than growing a buffer and it
 * keeps a corrupt header from turning into a huge allocation. */
#define SX_ASSET_MAX_BYTES (512u * 1024u)

static uint16_t sx_read_u16le(const unsigned char* data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t sx_read_u32le(const unsigned char* data) {
    return (uint32_t)data[0]
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

/* Reads a whole regular file into a fresh buffer. The caller owns it. */
static int sx_asset_slurp(const char* path, unsigned char** out_data, uint32_t* out_size) {
    struct stat info;
    unsigned char* data;
    uint32_t size;
    uint32_t filled = 0;
    int fd;

    *out_data = 0;
    *out_size = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        eprintf("ccleste: no se pudo abrir %s\n", path);
        return -1;
    }
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size == 0) {
        eprintf("ccleste: %s no es un archivo regular con contenido\n", path);
        close(fd);
        return -1;
    }
    if (info.st_size > SX_ASSET_MAX_BYTES) {
        eprintf("ccleste: %s excede el limite de %u bytes\n", path, SX_ASSET_MAX_BYTES);
        close(fd);
        return -1;
    }

    size = (uint32_t)info.st_size;
    data = (unsigned char*)malloc(size);
    if (data == 0) {
        eprintf("ccleste: sin memoria para %s (%u bytes)\n", path, (unsigned int)size);
        close(fd);
        return -1;
    }

    while (filled < size) {
        ssize_t got = read(fd, data + filled, size - filled);
        if (got <= 0) {
            eprintf("ccleste: lectura incompleta de %s\n", path);
            free(data);
            close(fd);
            return -1;
        }
        filled += (uint32_t)got;
    }

    close(fd);
    *out_data = data;
    *out_size = size;
    return 0;
}

/* --- BMP ------------------------------------------------------------------ */

int sx_bmp_load(const char* path, struct sx_bmp* out) {
    unsigned char* data = 0;
    uint32_t size = 0;
    uint32_t pixel_offset;
    uint32_t header_size;
    int32_t width;
    int32_t height;
    uint16_t bpp;
    uint32_t compression;
    uint32_t stride;
    unsigned char* pixels;
    int32_t y;

    out->pixels = 0;
    out->width = 0;
    out->height = 0;

    if (sx_asset_slurp(path, &data, &size) < 0) {
        return -1;
    }

    if (size < 54u || data[0] != 'B' || data[1] != 'M') {
        eprintf("ccleste: %s no es un BMP\n", path);
        free(data);
        return -1;
    }

    pixel_offset = sx_read_u32le(data + 10);
    header_size = sx_read_u32le(data + 14);
    width = (int32_t)sx_read_u32le(data + 18);
    height = (int32_t)sx_read_u32le(data + 22);
    bpp = sx_read_u16le(data + 28);
    compression = sx_read_u32le(data + 30);

    /* BI_RGB only, and a positive height only: the game has no use for a
     * top-down sheet, and handling the sign would hide a swapped file. */
    if (header_size < 40u || compression != 0u) {
        eprintf("ccleste: %s usa un BMP comprimido (no soportado)\n", path);
        free(data);
        return -1;
    }
    if (width <= 0 || height <= 0 || (bpp != 1u && bpp != 4u && bpp != 8u)) {
        eprintf("ccleste: %s tiene dimensiones o profundidad no soportadas (%dx%d, %u bpp)\n",
                path, (int)width, (int)height, (unsigned int)bpp);
        free(data);
        return -1;
    }

    /* BMP rows are padded to a 4-byte boundary. */
    stride = (((uint32_t)width * (uint32_t)bpp + 31u) / 32u) * 4u;
    if (pixel_offset > size || stride * (uint32_t)height > size - pixel_offset) {
        eprintf("ccleste: %s tiene datos de pixeles truncados\n", path);
        free(data);
        return -1;
    }

    pixels = (unsigned char*)malloc((size_t)width * (size_t)height);
    if (pixels == 0) {
        eprintf("ccleste: sin memoria para la hoja de %s\n", path);
        free(data);
        return -1;
    }

    for (y = 0; y < height; ++y) {
        const unsigned char* source = data + pixel_offset + stride * (uint32_t)y;
        unsigned char* target = pixels + (size_t)width * (size_t)y;
        int32_t x;

        for (x = 0; x < width; ++x) {
            switch (bpp) {
                case 1u:
                    target[x] = (unsigned char)((source[x >> 3] >> (7u - (x & 7))) & 1u);
                    break;
                case 4u:
                    target[x] = (unsigned char)((x & 1) == 0 ? (source[x >> 1] & 0x0fu)
                                                             : (source[x >> 1] >> 4));
                    break;
                default:
                    target[x] = source[x];
                    break;
            }
        }
    }

    free(data);
    out->pixels = pixels;
    out->width = width;
    out->height = height;
    return 0;
}

void sx_bmp_release(struct sx_bmp* bmp) {
    if (bmp == 0) {
        return;
    }
    free(bmp->pixels);
    bmp->pixels = 0;
    bmp->width = 0;
    bmp->height = 0;
}

/* --- WAV ------------------------------------------------------------------ */

int sx_wav_load(const char* path, struct sx_wav* out) {
    unsigned char* data = 0;
    uint32_t size = 0;
    uint32_t offset;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    const unsigned char* frames = 0;
    uint32_t frame_bytes = 0;
    uint32_t frame_count = 0;
    unsigned char* samples;
    uint32_t i;

    out->samples = 0;
    out->sample_count = 0;
    out->sample_rate_hz = 0;

    if (sx_asset_slurp(path, &data, &size) < 0) {
        return -1;
    }

    if (size < 44u || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        eprintf("ccleste: %s no es un WAV RIFF\n", path);
        free(data);
        return -1;
    }

    /* Walk the chunk list instead of assuming the canonical 44-byte header:
     * the upstream files carry a LIST chunk between fmt and data. */
    offset = 12u;
    while (offset + 8u <= size) {
        uint32_t chunk_size = sx_read_u32le(data + offset + 4);

        if (memcmp(data + offset, "fmt ", 4) == 0) {
            if (chunk_size < 16u || offset + 8u + 16u > size) {
                eprintf("ccleste: %s tiene un chunk fmt incompleto\n", path);
                free(data);
                return -1;
            }
            format = sx_read_u16le(data + offset + 8);
            channels = sx_read_u16le(data + offset + 10);
            rate = sx_read_u32le(data + offset + 12);
            bits = sx_read_u16le(data + offset + 22);
        } else if (memcmp(data + offset, "data", 4) == 0) {
            if (offset + 8u > size) {
                eprintf("ccleste: %s tiene un chunk data truncado\n", path);
                free(data);
                return -1;
            }
            if (chunk_size > size - offset - 8u) {
                chunk_size = size - offset - 8u;
            }
            frames = data + offset + 8u;
            frame_bytes = chunk_size;
        }

        /* Chunks are word aligned. */
        offset += 8u + chunk_size + (chunk_size & 1u);
    }

    if (format != 1u || channels != 1u || rate == 0u || frames == 0) {
        eprintf("ccleste: %s no es PCM mono (format=%u canales=%u)\n",
                path, (unsigned int)format, (unsigned int)channels);
        free(data);
        return -1;
    }
    if (bits != 8u && bits != 16u) {
        eprintf("ccleste: %s usa %u bits por muestra (solo 8 o 16)\n", path, (unsigned int)bits);
        free(data);
        return -1;
    }

    frame_count = frame_bytes / (uint32_t)(bits / 8u);
    if (frame_count == 0u) {
        eprintf("ccleste: %s no tiene muestras\n", path);
        free(data);
        return -1;
    }

    samples = (unsigned char*)malloc(frame_count);
    if (samples == 0) {
        eprintf("ccleste: sin memoria para las muestras de %s\n", path);
        free(data);
        return -1;
    }

    /* The SDK mixer takes unsigned 8-bit mono, so 16-bit input keeps only the
     * high byte: that is the whole dynamic range the mixer can express anyway. */
    if (bits == 8u) {
        memcpy(samples, frames, frame_count);
    } else {
        for (i = 0; i < frame_count; ++i) {
            samples[i] = frames[2u * i + 1u];
        }
    }

    free(data);
    out->samples = samples;
    out->sample_count = frame_count;
    out->sample_rate_hz = rate;
    return 0;
}

void sx_wav_release(struct sx_wav* wav) {
    if (wav == 0) {
        return;
    }
    free(wav->samples);
    wav->samples = 0;
    wav->sample_count = 0;
    wav->sample_rate_hz = 0;
}
