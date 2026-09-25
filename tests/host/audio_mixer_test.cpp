#include <stdint.h>

extern "C" {
#include <stdio.h>
#include <string.h>
}

#include "savanxp/audio.h"

namespace {

int g_failures = 0;
int g_checks = 0;
uint32_t g_now_ms = 1000;
int g_write_fails = 0;
size_t g_last_write_bytes = 0;
size_t g_last_write_frames = 0;
const int16_t* g_last_write_samples = nullptr;
int g_close_count = 0;

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

extern "C" {

long audio_open(void) {
    return 41;
}

long audio_get_info(int fd, struct savanxp_audio_info* info) {
    if (fd != 41 || info == nullptr) {
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->sample_rate_hz = 44100;
    info->channels = 2;
    info->bits_per_sample = 16;
    info->frame_bytes = 4;
    info->period_bytes = 1764;
    info->buffer_bytes = 17640;
    return 0;
}

long savanxp_close(int fd) {
    if (fd == 41) {
        ++g_close_count;
    }
    return 0;
}

long savanxp_write(int fd, const void* buffer, size_t count) {
    if (fd != 41 || buffer == nullptr || (count % 4u) != 0) {
        return -1;
    }
    if (g_write_fails) {
        return -5;
    }
    g_last_write_bytes = count;
    g_last_write_frames = count / 4u;
    g_last_write_samples = static_cast<const int16_t*>(buffer);
    return static_cast<long>(count);
}

unsigned long uptime_ms(void) {
    return g_now_ms;
}

} // extern "C"

int main() {
    printf("AUDIO MIXER TEST START\n");
    struct sx_audio_mixer mixer = {};
    const unsigned char samples[] = {128, 255, 0};
    long result;

    check(sx_audio_mixer_init(&mixer, 2, 25) == 0, "el mixer abre el device PCM");
    check(sx_audio_mixer_active(&mixer) && mixer.voice_count == 2,
          "inicializa las voces solicitadas");
    check(sx_audio_mixer_start_voice(&mixer, 0, samples, sizeof(samples), 44100,
                                     SX_AUDIO_PITCH_NORMAL_Q16, 127, 0) == 0,
          "una voz unsigned-8 mono arranca");
    check(mixer.voices[0].step_fixed == SX_AUDIO_PITCH_NORMAL_Q16,
          "el pitch normal conserva el ratio de sample rate");
    check(sx_audio_mixer_set_pan(&mixer, 0, 127, 0) == 0,
          "el paneo se puede actualizar sin reiniciar la voz");

    g_now_ms += 10;
    result = sx_audio_mixer_update(&mixer);
    check(result == 1 && g_last_write_frames == 441 && g_last_write_bytes == 1764,
          "calcula los frames por reloj y escribe PCM stereo");
    check(g_last_write_samples[0] == 0 && g_last_write_samples[1] == 0 &&
              g_last_write_samples[2] == 32512 && g_last_write_samples[3] == 0 &&
              g_last_write_samples[4] == -32768 && g_last_write_samples[5] == 0,
          "convierte unsigned-8 a S16 y aplica el paneo izquierdo");
    check(!sx_audio_mixer_voice_playing(&mixer, 0),
          "la voz termina al consumir sus samples");

    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "conserva el resto fraccionario de 1 ms");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "acumula el reloj sin perder frames");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "acumula tres milisegundos sin redondear prematuramente");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "conserva la fraccion durante cuatro milisegundos");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el quinto milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el sexto milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el septimo milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el octavo milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el noveno milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 45,
          "redondea el siguiente frame al completar la fraccion");

    g_now_ms += 1000;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 1102,
          "limita el catch-up a la ventana maxima configurada");

    g_write_fails = 1;
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) < 0 && !sx_audio_mixer_active(&mixer),
          "un write fallido desactiva el mixer");
    check(!sx_audio_mixer_voice_playing(&mixer, 0),
          "desactivar el mixer detiene las voces");

    sx_audio_mixer_destroy(&mixer);
    check(g_close_count == 1 && mixer.fd == -1 && mixer.voices == nullptr,
          "destroy cierra el device y libera el estado");

    printf(g_failures == 0 ? "AUDIO MIXER TEST PASS\n" : "AUDIO MIXER TEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
