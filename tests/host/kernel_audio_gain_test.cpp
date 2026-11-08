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

bool mock_ready(void*) { return true; }
bool mock_info(void*, savanxp_audio_info& info) { info = {48000,2,16,4,4096,16384,SAVANXP_AUDIO_BACKEND_AC97,0}; return true; }
int mock_submit(void*, uint64_t, uint32_t) { return 0; }
int mock_capture(void*, void*, uint32_t) { return 0; }
void mock_stop(void*) {}
uint64_t chosen = 0x123;
bool mock_endpoint(void*, uint32_t index, savanxp_audio_endpoint& endpoint) {
    if (index > 1) return false;
    endpoint = {};
    endpoint.id = 0x123 + index; endpoint.device_id = 0x120;
    endpoint.flags = SAVANXP_AUDIO_PLAYBACK | SAVANXP_AUDIO_AVAILABLE |
        (chosen == endpoint.id ? SAVANXP_AUDIO_SELECTED : 0);
    return true;
}
int mock_select(void*, uint64_t id, uint32_t) {
    if (id == 0x124) return -SX_EIO;
    chosen = id; return 0;
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

    check(audio::set_master_volume(25) && audio::set_capture_volume(50), "independent playback and recording gains");
    memcpy(buffer,full_scale,sizeof(buffer));
    audio::apply_capture_gain(buffer,sizeof(buffer));
    check(buffer[0] == 16383 && buffer[1] == -16384 && audio::master_volume() == 25,
          "input half gain changes samples without changing playback level");
    check(!audio::set_capture_volume(101) && audio::capture_volume() == 50, "invalid input gain preserves level");
    audio::set_capture_muted(true);
    memcpy(buffer,full_scale,sizeof(buffer)); audio::apply_capture_gain(buffer,sizeof(buffer));
    check(!buffer[0] && !buffer[1] && audio::capture_volume() == 50 && !audio::master_muted(),
          "input mute preserves input gain and leaves playback unmuted");
    audio::set_capture_muted(false);
    memcpy(buffer,full_scale,sizeof(buffer)); audio::apply_capture_gain(buffer,sizeof(buffer));
    check(buffer[0] == 16383 && buffer[1] == -16384, "unmuting input restores its independent gain");
    check(audio::set_capture_volume(100) && audio::set_master_volume(100), "restore independent default gains");

    const audio::Backend fallback = {mock_ready,mock_info,mock_ready,mock_submit,mock_stop,
        mock_ready,mock_ready,mock_capture,mock_stop};
    audio::set_backend(fallback);
    savanxp_audio_endpoint output = {}, input = {}, again = {};
    check(audio::endpoint(0,output) && audio::endpoint(1,input) && !audio::endpoint(2,again),
          "legacy backend enumerates output and independent input");
    check(output.id && input.id != output.id && input.device_id == output.device_id &&
          (output.flags & SAVANXP_AUDIO_PLAYBACK) && (input.flags & SAVANXP_AUDIO_CAPTURE),
          "legacy endpoint identities and directions are distinct");
    check(audio::endpoint(0,again) && again.id == output.id, "endpoint identity remains stable across reads");
    check(audio::select_endpoint(output.id,SAVANXP_AUDIO_PLAYBACK) == 0, "selecting the default is idempotent");
    check(audio::select_endpoint(output.id,SAVANXP_AUDIO_CAPTURE) == -SX_ENOENT, "refuse wrong direction");
    check(audio::select_endpoint(output.id,3) == -SX_EINVAL, "refuse combined directions");
    const audio::Backend routed = {mock_ready,mock_info,mock_ready,mock_submit,mock_stop,
        nullptr,nullptr,nullptr,nullptr,mock_endpoint,mock_select};
    audio::set_backend(routed);
    check(audio::endpoint(0,output) && audio::endpoint(1,input), "backend exposes both real connectors");
    check(audio::select_endpoint(input.id,SAVANXP_AUDIO_PLAYBACK) == -SX_EIO,
          "propagate failed hardware transition");
    check(audio::endpoint(0,again) && (again.flags & SAVANXP_AUDIO_SELECTED), "failed selection retains effective output");
    check(audio::select_endpoint(0x999,SAVANXP_AUDIO_PLAYBACK) == -SX_ENOENT, "unknown stable id cannot select another connector");

    printf("kernel-audio-gain: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
