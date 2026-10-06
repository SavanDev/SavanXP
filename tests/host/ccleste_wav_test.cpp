#include <stdint.h>

extern "C" {
#include <stdio.h>
#include <string.h>
}

extern "C" {
#include "ccleste_savanxp_assets.h"
}

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    fflush(0);
    return ok;
}

void put_u16le(unsigned char* out, uint16_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
}

void put_u32le(unsigned char* out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

/* WAV PCM mono minimo en memoria: RIFF + fmt(16) + data, sin chunks extra. */
size_t build_wav(
    unsigned char* out,
    uint16_t bits,
    const unsigned char* frames,
    size_t frame_bytes)
{
    memcpy(out + 0, "RIFF", 4);
    put_u32le(out + 4, (uint32_t)(36 + frame_bytes));
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    put_u32le(out + 16, 16);
    put_u16le(out + 20, 1);
    put_u16le(out + 22, 1);
    put_u32le(out + 24, 22050);
    put_u32le(out + 28, (uint32_t)(22050 * bits / 8));
    put_u16le(out + 32, (uint16_t)(bits / 8));
    put_u16le(out + 34, bits);
    memcpy(out + 36, "data", 4);
    put_u32le(out + 40, (uint32_t)frame_bytes);
    memcpy(out + 44, frames, frame_bytes);
    return 44 + frame_bytes;
}

} // namespace

extern "C" void eprintf(const char* format, ...) {
    (void)format;
}

int main() {
    printf("CCLESTE WAV TEST START\n");
    {
        /* 8 bits sin signo: pasa tal cual, silencio en 128. */
        const unsigned char frames[] = {0, 128, 255, 128, 64};
        unsigned char file[64];
        struct sx_wav wav = {};
        size_t size = build_wav(file, 8, frames, sizeof(frames));

        check(sx_wav_decode(file, (uint32_t)size, "wav8", &wav) == 0,
            "decodifica el wav de 8 bits");
        check(wav.sample_count == 5 && wav.sample_rate_hz == 22050, "lee conteo y rate");
        check(wav.samples[0] == 0 && wav.samples[1] == 128 && wav.samples[2] == 255 &&
                  wav.samples[3] == 128 && wav.samples[4] == 64,
            "el u8 pasa intacto, silencio en 128");
        sx_wav_release(&wav);
    }
    {
        /* 16 bits con signo: solo el byte alto, RECENTRADO. Sin el XOR el
         * silencio (0x0000) sonaba como -32768 constante y todo venia
         * corrido media escala: la saturacion que ningun volumen arreglaba. */
        const int16_t s16[] = {0, 0x1000, -4096, 32767, -32768};
        unsigned char frames[10];
        unsigned char file[64];
        struct sx_wav wav = {};
        size_t size;
        for (size_t i = 0; i < 5; ++i) {
            put_u16le(frames + 2 * i, (uint16_t)s16[i]);
        }
        size = build_wav(file, 16, frames, sizeof(frames));

        check(sx_wav_decode(file, (uint32_t)size, "wav16", &wav) == 0,
            "decodifica el wav de 16 bits");
        check(wav.sample_count == 5 && wav.sample_rate_hz == 22050, "lee conteo y rate");
        check(wav.samples[0] == 128 && wav.samples[1] == 144 && wav.samples[2] == 112 &&
                  wav.samples[3] == 255 && wav.samples[4] == 0,
            "el byte alto se recentra: silencio en 128, extremos en su lugar");
        sx_wav_release(&wav);
    }
    {
        /* Basura: falla limpio sin colgar ni ensuciar la salida. */
        const unsigned char junk[] = {'N', 'O', 'P', 'E'};
        struct sx_wav wav = {};
        wav.samples = (unsigned char*)0x1;

        check(sx_wav_decode(junk, sizeof(junk), "roto", &wav) != 0 && wav.samples == 0,
            "rechaza lo que no es WAV");
        sx_wav_release(&wav);
    }

    printf(g_failures == 0 ? "CCLESTE WAV TEST PASS\n" : "CCLESTE WAV TEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
