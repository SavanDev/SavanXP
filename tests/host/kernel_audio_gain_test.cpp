#include <stdint.h>

extern "C" {
#include <stdio.h>
#include <string.h>
}

#include "kernel/audio.hpp"

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

} // namespace

int main() {
    int16_t full_scale[4] = {32767, -32768, 16384, -16384};
    int16_t buffer[4];
    int16_t untouched[4] = {1000, -1000, 2000, -2000};

    check(audio::master_volume() == 100u, "arranca a 100");
    check(!audio::master_muted(), "arranca sin mutear");
    check(!audio::set_master_volume(101u), "rechaza mas de 100");
    check(audio::master_volume() == 100u, "el rechazo no mueve el nivel");

    memcpy(buffer, full_scale, sizeof(buffer));
    audio::apply_playback_gain(buffer, sizeof(buffer));
    check(memcmp(buffer, full_scale, sizeof(buffer)) == 0, "100 es identidad");

    check(audio::set_master_volume(50u), "acepta 50");
    memcpy(buffer, full_scale, sizeof(buffer));
    audio::apply_playback_gain(buffer, sizeof(buffer));
    check(buffer[0] == 16383, "50 atenua el positivo");
    check(buffer[1] == -16384, "50 atenua el negativo");
    check(buffer[2] == 8192, "50 atenua a la mitad");
    check(buffer[3] == -8192, "50 atenua a la mitad negativa");

    check(audio::set_master_volume(0u), "acepta 0");
    memcpy(buffer, full_scale, sizeof(buffer));
    audio::apply_playback_gain(buffer, sizeof(buffer));
    check(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 0,
        "0 silencia");

    check(audio::set_master_volume(100u), "vuelve a 100");
    audio::set_master_muted(true);
    check(audio::master_muted(), "mutea");
    memcpy(buffer, full_scale, sizeof(buffer));
    audio::apply_playback_gain(buffer, sizeof(buffer));
    check(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 0,
        "mudo silencia aunque el nivel sea 100");
    audio::set_master_muted(false);
    memcpy(buffer, full_scale, sizeof(buffer));
    audio::apply_playback_gain(buffer, sizeof(buffer));
    check(memcmp(buffer, full_scale, sizeof(buffer)) == 0, "unmute restaura");

    memcpy(buffer, untouched, sizeof(buffer));
    audio::apply_playback_gain(buffer, sizeof(buffer) - 1);
    check(buffer[3] == untouched[3], "el resto impar se deja tal cual");
    audio::apply_playback_gain(nullptr, 8u);
    check(true, "nullptr no cuelga");

    printf("kernel-audio-gain: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
