/*
 * Asset decoding for the SavanXP ccleste port.
 *
 * The upstream frontend hands its decoded surfaces straight to SDL, so this
 * side only has to produce what the game actually reads: palette indices for
 * the two BMPs, and unsigned 8-bit mono PCM for the sound effects the SDK
 * mixer plays. The upstream BMP palettes are deliberately ignored -- every
 * pixel in this game is drawn through the mutable 16-entry PICO-8 palette, so
 * the colours baked into the files are dead weight.
 *
 * Part of the SavanXP port overlay; the engine itself is unmodified upstream.
 */
#ifndef CCLESTE_SAVANXP_ASSETS_H
#define CCLESTE_SAVANXP_ASSETS_H

#include <stdint.h>

struct sx_bmp {
    unsigned char* pixels; /* width * height palette indices, row major, top-down */
    int width;
    int height;
};

/* Decodes an uncompressed paletted BMP (1, 4 or 8 bpp) into 8-bit indices.
 * Returns 0, or -1 with a reason already logged. */
int sx_bmp_load(const char* path, struct sx_bmp* out);
void sx_bmp_release(struct sx_bmp* bmp);

struct sx_wav {
    unsigned char* samples; /* unsigned 8-bit mono, silence at 128 */
    uint32_t sample_count;
    uint32_t sample_rate_hz;
};

/* Decodes a RIFF/WAVE PCM file into unsigned 8-bit mono samples. Accepts the
 * 8-bit unsigned and 16-bit signed mono forms; anything else is rejected rather
 * than guessed at. Returns 0, or -1 with a reason already logged. */
int sx_wav_load(const char* path, struct sx_wav* out);
void sx_wav_release(struct sx_wav* wav);

#endif
