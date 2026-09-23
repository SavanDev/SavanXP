#include "savanxp/audio.h"

#include "savanxp/libc.h"

#include <stdlib.h>
#include <string.h>

static int sx_audio_voice_index_valid(
    const struct sx_audio_mixer* mixer,
    size_t voice_index)
{
    return mixer != 0 && mixer->initialized && mixer->voices != 0 &&
        voice_index < mixer->voice_count;
}

static void sx_audio_voice_set_pan(
    struct sx_audio_voice* voice,
    int volume,
    int separation)
{
    if (voice == 0)
    {
        return;
    }
    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 127)
    {
        volume = 127;
    }
    if (separation < 0)
    {
        separation = 0;
    }
    else if (separation > 254)
    {
        separation = 254;
    }
    voice->left_gain = (volume * (254 - separation)) / 254;
    voice->right_gain = (volume * separation) / 254;
}

void sx_audio_mixer_disable(struct sx_audio_mixer* mixer)
{
    size_t index;

    if (mixer == 0)
    {
        return;
    }
    if (mixer->initialized && mixer->fd >= 0)
    {
        (void)savanxp_close(mixer->fd);
    }
    mixer->fd = -1;
    mixer->initialized = 0;
    memset(&mixer->info, 0, sizeof(mixer->info));
    mixer->frame_remainder = 0;
    mixer->last_update_ms = 0;
    mixer->clock_valid = 0;
    if (mixer->voices != 0)
    {
        for (index = 0; index < mixer->voice_count; ++index)
        {
            mixer->voices[index].active = 0;
        }
    }
}

int sx_audio_mixer_init(
    struct sx_audio_mixer* mixer,
    size_t voice_count,
    uint32_t max_delta_ms)
{
    struct savanxp_audio_info info = {0};
    int fd;

    if (mixer == 0)
    {
        return -SAVANXP_EINVAL;
    }
    if (mixer->initialized || mixer->voices != 0 || mixer->mix_buffer != 0)
    {
        sx_audio_mixer_destroy(mixer);
    }
    memset(mixer, 0, sizeof(*mixer));
    mixer->fd = -1;
    if (voice_count == 0 || voice_count > SX_AUDIO_MIXER_MAX_VOICES)
    {
        return -SAVANXP_EINVAL;
    }
    mixer->max_delta_ms = max_delta_ms != 0 ? max_delta_ms : SX_AUDIO_DEFAULT_MAX_DELTA_MS;
    if (mixer->max_delta_ms > SX_AUDIO_MAX_CATCHUP_MS)
    {
        mixer->max_delta_ms = SX_AUDIO_MAX_CATCHUP_MS;
    }

    fd = (int)audio_open();
    if (fd < 0)
    {
        return fd;
    }
    if (audio_get_info(fd, &info) < 0 ||
        info.sample_rate_hz == 0 ||
        info.channels != 2 ||
        info.bits_per_sample != 16 ||
        info.frame_bytes != 4)
    {
        (void)savanxp_close(fd);
        return -SAVANXP_EINVAL;
    }

    mixer->voices = (struct sx_audio_voice*)calloc(voice_count, sizeof(*mixer->voices));
    if (mixer->voices == 0)
    {
        (void)savanxp_close(fd);
        mixer->voice_count = 0;
        return -SAVANXP_ENOMEM;
    }
    mixer->fd = fd;
    mixer->info = info;
    mixer->voice_count = voice_count;
    mixer->initialized = 1;
    mixer->last_update_ms = (uint32_t)uptime_ms();
    mixer->clock_valid = 1;
    return 0;
}

void sx_audio_mixer_destroy(struct sx_audio_mixer* mixer)
{
    if (mixer == 0)
    {
        return;
    }
    sx_audio_mixer_disable(mixer);
    if (mixer->mix_buffer != 0)
    {
        free(mixer->mix_buffer);
    }
    if (mixer->voices != 0)
    {
        free(mixer->voices);
    }
    memset(mixer, 0, sizeof(*mixer));
    mixer->fd = -1;
}

int sx_audio_mixer_active(const struct sx_audio_mixer* mixer)
{
    return mixer != 0 && mixer->initialized && mixer->fd >= 0;
}

int sx_audio_mixer_set_pan(
    struct sx_audio_mixer* mixer,
    size_t voice_index,
    int volume,
    int separation)
{
    if (!sx_audio_voice_index_valid(mixer, voice_index))
    {
        return -SAVANXP_EINVAL;
    }
    sx_audio_voice_set_pan(&mixer->voices[voice_index], volume, separation);
    return 0;
}

int sx_audio_mixer_start_voice(
    struct sx_audio_mixer* mixer,
    size_t voice_index,
    const unsigned char* samples,
    uint32_t sample_count,
    uint32_t sample_rate_hz,
    uint32_t pitch_q16,
    int volume,
    int separation)
{
    struct sx_audio_voice* voice;
    uint64_t numerator;
    uint64_t step;

    if (!sx_audio_voice_index_valid(mixer, voice_index) || mixer->fd < 0 ||
        samples == 0 || sample_count == 0 || sample_rate_hz == 0 || pitch_q16 == 0)
    {
        return -SAVANXP_EINVAL;
    }

    numerator = (uint64_t)sample_rate_hz * (uint64_t)pitch_q16;
    step = numerator / mixer->info.sample_rate_hz;
    if (step == 0)
    {
        step = 1;
    }

    voice = &mixer->voices[voice_index];
    voice->samples = samples;
    voice->sample_count = sample_count;
    voice->sample_rate_hz = sample_rate_hz;
    voice->position_fixed = 0;
    voice->step_fixed = step;
    sx_audio_voice_set_pan(voice, volume, separation);
    voice->active = 1;
    return 0;
}

int sx_audio_mixer_stop_voice(struct sx_audio_mixer* mixer, size_t voice_index)
{
    if (!sx_audio_voice_index_valid(mixer, voice_index))
    {
        return -SAVANXP_EINVAL;
    }
    memset(&mixer->voices[voice_index], 0, sizeof(mixer->voices[voice_index]));
    return 0;
}

int sx_audio_mixer_voice_playing(const struct sx_audio_mixer* mixer, size_t voice_index)
{
    return sx_audio_voice_index_valid(mixer, voice_index) &&
        mixer->voices[voice_index].active;
}

static int sx_audio_mixer_ensure_capacity(struct sx_audio_mixer* mixer, size_t frames)
{
    int16_t* buffer;
    size_t bytes;

    if (frames <= mixer->mix_buffer_frames)
    {
        return 1;
    }
    if (frames > SIZE_MAX / mixer->info.channels / sizeof(int16_t))
    {
        return 0;
    }
    bytes = frames * mixer->info.channels * sizeof(int16_t);
    buffer = (int16_t*)realloc(mixer->mix_buffer, bytes);
    if (buffer == 0)
    {
        return 0;
    }
    mixer->mix_buffer = buffer;
    mixer->mix_buffer_frames = frames;
    return 1;
}

static size_t sx_audio_mixer_frames_due(struct sx_audio_mixer* mixer)
{
    uint32_t now = (uint32_t)uptime_ms();
    uint32_t delta_ms;
    uint64_t numerator;

    if (!mixer->clock_valid)
    {
        mixer->last_update_ms = now;
        mixer->clock_valid = 1;
        return 0;
    }
    delta_ms = now - mixer->last_update_ms;
    mixer->last_update_ms = now;
    if (delta_ms == 0)
    {
        return 0;
    }
    if (delta_ms > mixer->max_delta_ms)
    {
        delta_ms = mixer->max_delta_ms;
    }

    numerator = (uint64_t)delta_ms * (uint64_t)mixer->info.sample_rate_hz +
        (uint64_t)mixer->frame_remainder;
    mixer->frame_remainder = (uint32_t)(numerator % 1000u);
    return (size_t)(numerator / 1000u);
}

static int16_t sx_audio_clamp_sample(int mixed)
{
    if (mixed < -32768)
    {
        return -32768;
    }
    if (mixed > 32767)
    {
        return 32767;
    }
    return (int16_t)mixed;
}

static void sx_audio_mixer_render(
    struct sx_audio_mixer* mixer,
    size_t frames)
{
    size_t frame;

    memset(mixer->mix_buffer, 0, frames * mixer->info.channels * sizeof(int16_t));
    for (frame = 0; frame < frames; ++frame)
    {
        int mixed_left = 0;
        int mixed_right = 0;
        size_t voice_index;

        for (voice_index = 0; voice_index < mixer->voice_count; ++voice_index)
        {
            struct sx_audio_voice* voice = &mixer->voices[voice_index];
            uint64_t sample_index_wide;
            uint32_t sample_index;
            int sample_value;

            if (!voice->active || voice->samples == 0)
            {
                continue;
            }
            sample_index_wide = voice->position_fixed >> 16;
            if (sample_index_wide >= (uint64_t)voice->sample_count)
            {
                voice->active = 0;
                continue;
            }
            sample_index = (uint32_t)sample_index_wide;

            sample_value = ((int)voice->samples[sample_index] - 128) << 8;
            mixed_left += (sample_value * voice->left_gain) / 127;
            mixed_right += (sample_value * voice->right_gain) / 127;
            if (voice->position_fixed > UINT64_MAX - voice->step_fixed)
            {
                voice->active = 0;
                continue;
            }
            voice->position_fixed += voice->step_fixed;
            if ((voice->position_fixed >> 16) >= (uint64_t)voice->sample_count)
            {
                voice->active = 0;
            }
        }

        mixer->mix_buffer[(frame * mixer->info.channels) + 0] =
            sx_audio_clamp_sample(mixed_left);
        mixer->mix_buffer[(frame * mixer->info.channels) + 1] =
            sx_audio_clamp_sample(mixed_right);
    }
}

long sx_audio_mixer_update(struct sx_audio_mixer* mixer)
{
    size_t frames;
    size_t bytes;
    long written;

    if (mixer == 0 || !mixer->initialized || mixer->fd < 0 || mixer->voices == 0)
    {
        return 0;
    }
    frames = sx_audio_mixer_frames_due(mixer);
    if (frames == 0)
    {
        return 0;
    }
    if (!sx_audio_mixer_ensure_capacity(mixer, frames))
    {
        sx_audio_mixer_disable(mixer);
        return -SAVANXP_ENOMEM;
    }

    sx_audio_mixer_render(mixer, frames);
    if (frames > SIZE_MAX / mixer->info.frame_bytes)
    {
        sx_audio_mixer_disable(mixer);
        return -SAVANXP_EINVAL;
    }
    bytes = frames * (size_t)mixer->info.frame_bytes;
    written = savanxp_write(mixer->fd, mixer->mix_buffer, bytes);
    if (written != (long)bytes)
    {
        long error = written < 0 ? written : -SAVANXP_EIO;
        sx_audio_mixer_disable(mixer);
        return error;
    }
    return 1;
}
