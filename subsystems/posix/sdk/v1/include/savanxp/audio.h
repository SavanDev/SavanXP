#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Playback remains a raw PCM device in the kernel. This optional SDK module
 * owns the userland half shared by legacy sound engines: a wall-clock frame
 * sink, a growable interleaved stereo buffer, and mono unsigned-8-bit voices
 * with source-rate/pitch conversion and classic volume/separation panning.
 *
 * Link runtime/audio.c or build an external app with `build-user.ps1 -Audio`.
 */
#define SX_AUDIO_PITCH_NORMAL_Q16 65536u
#define SX_AUDIO_DEFAULT_MAX_DELTA_MS 100u
#define SX_AUDIO_MAX_CATCHUP_MS 1000u
#define SX_AUDIO_MIXER_MAX_VOICES 256u

struct sx_audio_voice {
    const unsigned char* samples;
    uint32_t sample_count;
    uint32_t sample_rate_hz;
    uint64_t position_fixed;
    uint64_t step_fixed;
    int left_gain;
    int right_gain;
    int active;
};

struct sx_audio_mixer {
    int fd;
    struct savanxp_audio_info info;
    struct sx_audio_voice* voices;
    size_t voice_count;
    int16_t* mix_buffer;
    size_t mix_buffer_frames;
    uint32_t frame_remainder;
    uint32_t last_update_ms;
    uint32_t max_delta_ms;
    int clock_valid;
    int initialized;
};

/* Opens /dev/audio0 and allocates `voice_count` voices. The current device
 * contract is PCM S16LE stereo; zero selects the default 100 ms catch-up cap,
 * and the cap is bounded at one second. Returns 0 or a negative error. */
int sx_audio_mixer_init(
    struct sx_audio_mixer* mixer,
    size_t voice_count,
    uint32_t max_delta_ms);
void sx_audio_mixer_destroy(struct sx_audio_mixer* mixer);
void sx_audio_mixer_disable(struct sx_audio_mixer* mixer);
int sx_audio_mixer_active(const struct sx_audio_mixer* mixer);

/* Volume is 0..127 and separation is 0..254 (left..right), the convention used
 * by Doom and other DOS-era engines. Values outside the range are clamped.
 * `samples` is borrowed until the voice ends or is stopped. */
int sx_audio_mixer_set_pan(
    struct sx_audio_mixer* mixer,
    size_t voice_index,
    int volume,
    int separation);
int sx_audio_mixer_start_voice(
    struct sx_audio_mixer* mixer,
    size_t voice_index,
    const unsigned char* samples,
    uint32_t sample_count,
    uint32_t sample_rate_hz,
    uint32_t pitch_q16,
    int volume,
    int separation);
int sx_audio_mixer_stop_voice(struct sx_audio_mixer* mixer, size_t voice_index);
int sx_audio_mixer_voice_playing(const struct sx_audio_mixer* mixer, size_t voice_index);

/* Mixes and writes exactly the wall-clock frames since the previous call.
 * Returns 1 after a successful write, 0 when no frame is due, and a negative
 * error after disabling the device and stopping all voices. */
long sx_audio_mixer_update(struct sx_audio_mixer* mixer);

#ifdef __cplusplus
}
#endif
