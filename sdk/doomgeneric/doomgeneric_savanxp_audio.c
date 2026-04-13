#include "savanxp/libc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#define SX_AUDIO_PITCH_NORMAL 128u
#define SX_AUDIO_FIXED_SHIFT 16u
#define SX_AUDIO_FIXED_ONE (1u << SX_AUDIO_FIXED_SHIFT)
#define SX_AUDIO_MAX_DELTA_MS 100u

typedef struct sx_audio_sample {
    uint32_t sample_rate_hz;
    uint32_t sample_count;
    unsigned char *samples;
} sx_audio_sample_t;

typedef struct sx_audio_channel {
    const sx_audio_sample_t *sample;
    uint32_t position_fixed;
    uint32_t step_fixed;
    int left_gain;
    int right_gain;
    int active;
} sx_audio_channel_t;

static snddevice_t g_dg_sound_devices[] = { SNDDEVICE_SB };
static snddevice_t g_dg_music_devices[] = { SNDDEVICE_NONE };

static struct savanxp_audio_info g_audio_info = {0};
static sx_audio_channel_t *g_audio_channels = 0;
static int16_t *g_mix_buffer = 0;
static size_t g_mix_buffer_frames = 0;
static int g_audio_fd = -1;
static int g_use_sfx_prefix = 1;
static uint32_t g_mix_frame_remainder = 0;
static uint32_t g_last_update_ms = 0;

int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;
char *timidity_cfg_path = "";

static uint16_t sx_read_u16le(const unsigned char *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t sx_read_u32le(const unsigned char *data) {
    return (uint32_t)data[0]
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

static void sx_audio_reset_channel(sx_audio_channel_t *channel) {
    if (channel == 0) {
        return;
    }

    channel->sample = 0;
    channel->position_fixed = 0;
    channel->step_fixed = SX_AUDIO_FIXED_ONE;
    channel->left_gain = 0;
    channel->right_gain = 0;
    channel->active = 0;
}

static void sx_audio_reset_all_channels(void) {
    int index;

    if (g_audio_channels == 0) {
        return;
    }

    for (index = 0; index < snd_channels; ++index) {
        sx_audio_reset_channel(&g_audio_channels[index]);
    }
}

static void sx_audio_close_device(void) {
    if (g_audio_fd >= 0) {
        (void)close(g_audio_fd);
        g_audio_fd = -1;
    }
    memset(&g_audio_info, 0, sizeof(g_audio_info));
    g_mix_frame_remainder = 0;
    g_last_update_ms = 0;
}

static void sx_audio_shutdown_samples(void) {
    int index;

    for (index = 0; index < NUMSFX; ++index) {
        sfxinfo_t *sfx = &S_sfx[index];
        if (sfx->link == NULL && sfx->driver_data != NULL) {
            sx_audio_sample_t *sample = (sx_audio_sample_t *)sfx->driver_data;
            if (sample->samples != NULL) {
                free(sample->samples);
            }
            free(sample);
        }
        sfx->driver_data = NULL;
    }
}

static sfxinfo_t *sx_audio_root_sfx(sfxinfo_t *sfxinfo) {
    sfxinfo_t *current = sfxinfo;

    while (current != NULL && current->link != NULL) {
        current = current->link;
    }

    return current;
}

static int sx_audio_build_lump_name(sfxinfo_t *sfxinfo, char *buffer, size_t buffer_size) {
    sfxinfo_t *base = sx_audio_root_sfx(sfxinfo);
    const char *name;
    int written;

    if (base == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    name = base->name;
    if (name == NULL || strcmp(name, "none") == 0) {
        return -1;
    }

    if (g_use_sfx_prefix) {
        written = snprintf(buffer, buffer_size, "ds%s", name);
    } else {
        written = snprintf(buffer, buffer_size, "%s", name);
    }

    return written > 0 && (size_t)written < buffer_size ? 0 : -1;
}

static int sx_audio_resolve_lumpnum(sfxinfo_t *sfxinfo) {
    sfxinfo_t *base = sx_audio_root_sfx(sfxinfo);
    char lump_name[16];
    int lumpnum;

    if (base == NULL) {
        return -1;
    }

    if (base->lumpnum >= 0) {
        return base->lumpnum;
    }

    if (sx_audio_build_lump_name(base, lump_name, sizeof(lump_name)) < 0) {
        return -1;
    }

    lumpnum = W_CheckNumForName(lump_name);
    if (lumpnum < 0) {
        return -1;
    }

    base->lumpnum = lumpnum;
    return lumpnum;
}

static sx_audio_sample_t *sx_audio_decode_sample(sfxinfo_t *sfxinfo) {
    sfxinfo_t *base = sx_audio_root_sfx(sfxinfo);
    const unsigned char *lump_data;
    sx_audio_sample_t *sample;
    unsigned char *sample_bytes;
    int lumpnum;
    int lump_length;
    uint32_t declared_count;
    uint32_t actual_count;
    uint16_t format;
    uint16_t sample_rate_hz;

    if (base == NULL) {
        return 0;
    }

    if (base->driver_data != NULL) {
        return (sx_audio_sample_t *)base->driver_data;
    }

    lumpnum = sx_audio_resolve_lumpnum(base);
    if (lumpnum < 0) {
        return 0;
    }

    lump_length = W_LumpLength((unsigned int)lumpnum);
    if (lump_length < 8) {
        return 0;
    }

    lump_data = (const unsigned char *)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (lump_data == 0) {
        return 0;
    }

    format = sx_read_u16le(lump_data);
    sample_rate_hz = sx_read_u16le(lump_data + 2);
    declared_count = sx_read_u32le(lump_data + 4);

    if (format != 3u || sample_rate_hz == 0u) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    actual_count = (uint32_t)(lump_length - 8);
    if (declared_count != 0u && declared_count < actual_count) {
        actual_count = declared_count;
    }
    if (actual_count == 0u) {
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    sample = (sx_audio_sample_t *)calloc(1, sizeof(*sample));
    sample_bytes = (unsigned char *)malloc(actual_count);
    if (sample == 0 || sample_bytes == 0) {
        if (sample != 0) {
            free(sample);
        }
        if (sample_bytes != 0) {
            free(sample_bytes);
        }
        W_ReleaseLumpNum(lumpnum);
        return 0;
    }

    memcpy(sample_bytes, lump_data + 8, actual_count);
    W_ReleaseLumpNum(lumpnum);

    sample->sample_rate_hz = (uint32_t)sample_rate_hz;
    sample->sample_count = actual_count;
    sample->samples = sample_bytes;
    base->driver_data = sample;
    return sample;
}

static void sx_audio_set_pan(int *left_gain, int *right_gain, int volume, int separation) {
    int clamped_sep = separation;
    int clamped_vol = volume;

    if (left_gain == 0 || right_gain == 0) {
        return;
    }

    if (clamped_sep < 0) {
        clamped_sep = 0;
    } else if (clamped_sep > 254) {
        clamped_sep = 254;
    }

    if (clamped_vol < 0) {
        clamped_vol = 0;
    } else if (clamped_vol > 127) {
        clamped_vol = 127;
    }

    *left_gain = (clamped_vol * (254 - clamped_sep)) / 254;
    *right_gain = (clamped_vol * clamped_sep) / 254;
}

static uint32_t sx_audio_pitch_to_step(const sx_audio_sample_t *sample, int pitch) {
    uint64_t numerator;
    uint32_t pitch_scale = pitch > 0 ? (uint32_t)pitch : SX_AUDIO_PITCH_NORMAL;
    uint32_t denominator;

    if (sample == 0 || sample->sample_rate_hz == 0u || g_audio_info.sample_rate_hz == 0u) {
        return SX_AUDIO_FIXED_ONE;
    }

    denominator = SX_AUDIO_PITCH_NORMAL * g_audio_info.sample_rate_hz;
    numerator = ((uint64_t)sample->sample_rate_hz * (uint64_t)pitch_scale) << SX_AUDIO_FIXED_SHIFT;

    if (denominator == 0u) {
        return SX_AUDIO_FIXED_ONE;
    }

    numerator /= denominator;
    if (numerator == 0u) {
        return 1u;
    }

    return (uint32_t)numerator;
}

static size_t sx_audio_period_frames(void) {
    if (g_audio_info.frame_bytes == 0u) {
        return 0;
    }

    return (size_t)(g_audio_info.period_bytes / g_audio_info.frame_bytes);
}

static size_t sx_audio_frames_per_update(void) {
    uint32_t now_ms;
    uint32_t delta_ms;
    uint64_t numerator;

    if (g_audio_info.sample_rate_hz == 0u) {
        return 0;
    }

    now_ms = (uint32_t)uptime_ms();
    if (g_last_update_ms == 0u) {
        g_last_update_ms = now_ms;
        return sx_audio_period_frames();
    }

    delta_ms = now_ms - g_last_update_ms;
    g_last_update_ms = now_ms;

    if (delta_ms == 0u) {
        return 0;
    }

    if (delta_ms > SX_AUDIO_MAX_DELTA_MS) {
        delta_ms = SX_AUDIO_MAX_DELTA_MS;
    }

    numerator = (uint64_t)delta_ms * (uint64_t)g_audio_info.sample_rate_hz + (uint64_t)g_mix_frame_remainder;
    g_mix_frame_remainder = (uint32_t)(numerator % 1000u);
    return (size_t)(numerator / 1000u);
}

static int sx_audio_ensure_mix_capacity(size_t frames) {
    int16_t *new_buffer;

    if (frames <= g_mix_buffer_frames) {
        return 1;
    }

    new_buffer = (int16_t *)realloc(g_mix_buffer, frames * g_audio_info.channels * sizeof(int16_t));
    if (new_buffer == 0) {
        return 0;
    }

    g_mix_buffer = new_buffer;
    g_mix_buffer_frames = frames;
    return 1;
}

static size_t sx_audio_mix_frames(int16_t *destination, size_t frames_to_mix) {
    size_t frame;

    if (destination == 0 || frames_to_mix == 0 || g_audio_channels == 0) {
        return 0;
    }

    memset(destination, 0, frames_to_mix * g_audio_info.channels * sizeof(int16_t));

    for (frame = 0; frame < frames_to_mix; ++frame) {
        int channel_index;
        int mixed_left = 0;
        int mixed_right = 0;

        for (channel_index = 0; channel_index < snd_channels; ++channel_index) {
            sx_audio_channel_t *channel = &g_audio_channels[channel_index];
            uint32_t sample_index;
            int sample_value;

            if (!channel->active || channel->sample == 0) {
                continue;
            }

            sample_index = channel->position_fixed >> SX_AUDIO_FIXED_SHIFT;
            if (sample_index >= channel->sample->sample_count) {
                sx_audio_reset_channel(channel);
                continue;
            }

            sample_value = ((int)channel->sample->samples[sample_index] - 128) << 8;
            mixed_left += (sample_value * channel->left_gain) / 127;
            mixed_right += (sample_value * channel->right_gain) / 127;

            channel->position_fixed += channel->step_fixed;
            if ((channel->position_fixed >> SX_AUDIO_FIXED_SHIFT) >= channel->sample->sample_count) {
                sx_audio_reset_channel(channel);
            }
        }

        if (mixed_left < -32768) {
            mixed_left = -32768;
        } else if (mixed_left > 32767) {
            mixed_left = 32767;
        }
        if (mixed_right < -32768) {
            mixed_right = -32768;
        } else if (mixed_right > 32767) {
            mixed_right = 32767;
        }

        destination[(frame * g_audio_info.channels) + 0] = (int16_t)mixed_left;
        destination[(frame * g_audio_info.channels) + 1] = (int16_t)mixed_right;
    }

    return frames_to_mix;
}

static void sx_audio_disable_runtime(void) {
    sx_audio_close_device();
    sx_audio_reset_all_channels();
}

static boolean DG_Sound_Init(boolean use_sfx_prefix) {
    struct savanxp_audio_info info = {0};

    g_audio_fd = (int)audio_open();
    if (g_audio_fd < 0) {
        g_audio_fd = -1;
        return false;
    }

    if (audio_get_info(g_audio_fd, &info) < 0 ||
        info.sample_rate_hz == 0u ||
        info.channels != 2u ||
        info.bits_per_sample != 16u ||
        info.frame_bytes != 4u) {
        sx_audio_close_device();
        return false;
    }

    g_audio_info = info;
    g_use_sfx_prefix = use_sfx_prefix ? 1 : 0;
    snd_samplerate = (int)g_audio_info.sample_rate_hz;
    g_last_update_ms = (uint32_t)uptime_ms();

    g_audio_channels = (sx_audio_channel_t *)calloc((size_t)snd_channels, sizeof(*g_audio_channels));
    if (g_audio_channels == 0) {
        sx_audio_close_device();
        return false;
    }

    return true;
}

static void DG_Sound_Shutdown(void) {
    if (g_mix_buffer != 0) {
        free(g_mix_buffer);
        g_mix_buffer = 0;
    }
    g_mix_buffer_frames = 0;

    if (g_audio_channels != 0) {
        free(g_audio_channels);
        g_audio_channels = 0;
    }

    sx_audio_shutdown_samples();
    sx_audio_close_device();
}

static int DG_Sound_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    return sx_audio_resolve_lumpnum(sfxinfo);
}

static void DG_Sound_Update(void) {
    size_t frames_to_mix;
    size_t period_frames;
    size_t frames_to_write;
    unsigned long total_bytes;

    if (g_audio_fd < 0 || g_audio_channels == 0) {
        return;
    }

    frames_to_mix = sx_audio_frames_per_update();
    if (frames_to_mix == 0) {
        return;
    }

    if (!sx_audio_ensure_mix_capacity(frames_to_mix)) {
        return;
    }

    period_frames = sx_audio_period_frames();
    frames_to_mix = sx_audio_mix_frames(g_mix_buffer, frames_to_mix);
    if (frames_to_mix == 0) {
        return;
    }

    if (period_frames != 0u) {
        frames_to_write = frames_to_mix - (frames_to_mix % period_frames);
        if (frames_to_write == 0u) {
            frames_to_write = frames_to_mix;
        }
    } else {
        frames_to_write = frames_to_mix;
    }

    total_bytes = (unsigned long)(frames_to_write * g_audio_info.frame_bytes);
    if (write(g_audio_fd, g_mix_buffer, total_bytes) != (long)total_bytes) {
        sx_audio_disable_runtime();
    }
}

static void DG_Sound_UpdateSoundParams(int channel, int vol, int sep) {
    if (g_audio_channels == 0 || channel < 0 || channel >= snd_channels) {
        return;
    }

    sx_audio_set_pan(&g_audio_channels[channel].left_gain, &g_audio_channels[channel].right_gain, vol, sep);
}

static int DG_Sound_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    sx_audio_sample_t *sample;
    sfxinfo_t *base;
    int pitch = SX_AUDIO_PITCH_NORMAL;

    if (g_audio_channels == 0 || channel < 0 || channel >= snd_channels) {
        return -1;
    }

    base = sx_audio_root_sfx(sfxinfo);
    sample = sx_audio_decode_sample(sfxinfo);
    if (sample == 0 || base == 0) {
        return -1;
    }

    if (sfxinfo != NULL && sfxinfo->link != NULL && sfxinfo->pitch > 0) {
        pitch = sfxinfo->pitch;
    }

    g_audio_channels[channel].sample = sample;
    g_audio_channels[channel].position_fixed = 0u;
    g_audio_channels[channel].step_fixed = sx_audio_pitch_to_step(sample, pitch);
    sx_audio_set_pan(&g_audio_channels[channel].left_gain, &g_audio_channels[channel].right_gain, vol, sep);
    g_audio_channels[channel].active = 1;
    return channel;
}

static void DG_Sound_StopSound(int channel) {
    if (g_audio_channels == 0 || channel < 0 || channel >= snd_channels) {
        return;
    }

    sx_audio_reset_channel(&g_audio_channels[channel]);
}

static boolean DG_Sound_IsPlaying(int channel) {
    if (g_audio_channels == 0 || channel < 0 || channel >= snd_channels) {
        return false;
    }

    return g_audio_channels[channel].active ? true : false;
}

static void DG_Sound_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    int index;

    if (sounds == 0 || num_sounds <= 0) {
        return;
    }

    for (index = 0; index < num_sounds; ++index) {
        (void)sx_audio_decode_sample(&sounds[index]);
    }
}

static boolean DG_Music_Init(void) {
    return true;
}

static void DG_Music_Shutdown(void) {
}

static void DG_Music_SetVolume(int volume) {
    (void)volume;
}

static void DG_Music_Pause(void) {
}

static void DG_Music_Resume(void) {
}

static void *DG_Music_RegisterSong(void *data, int len) {
    (void)data;
    (void)len;
    return 0;
}

static void DG_Music_UnRegisterSong(void *handle) {
    (void)handle;
}

static void DG_Music_PlaySong(void *handle, boolean looping) {
    (void)handle;
    (void)looping;
}

static void DG_Music_StopSong(void) {
}

static boolean DG_Music_IsPlaying(void) {
    return false;
}

static void DG_Music_Poll(void) {
}

sound_module_t DG_sound_module = {
    g_dg_sound_devices,
    1,
    DG_Sound_Init,
    DG_Sound_Shutdown,
    DG_Sound_GetSfxLumpNum,
    DG_Sound_Update,
    DG_Sound_UpdateSoundParams,
    DG_Sound_StartSound,
    DG_Sound_StopSound,
    DG_Sound_IsPlaying,
    DG_Sound_CacheSounds,
};

music_module_t DG_music_module = {
    g_dg_music_devices,
    1,
    DG_Music_Init,
    DG_Music_Shutdown,
    DG_Music_SetVolume,
    DG_Music_Pause,
    DG_Music_Resume,
    DG_Music_RegisterSong,
    DG_Music_UnRegisterSong,
    DG_Music_PlaySong,
    DG_Music_StopSong,
    DG_Music_IsPlaying,
    DG_Music_Poll,
};
