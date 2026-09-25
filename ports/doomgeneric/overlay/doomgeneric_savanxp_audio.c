#include "savanxp/libc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/audio.h"

#include "doomtype.h"
#include "i_sound.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#define SX_SOUND_PITCH_NORMAL 128u

typedef struct sx_audio_sample {
    uint32_t sample_rate_hz;
    uint32_t sample_count;
    unsigned char *samples;
} sx_audio_sample_t;

static snddevice_t g_dg_sound_devices[] = { SNDDEVICE_SB };
static snddevice_t g_dg_music_devices[] = { SNDDEVICE_NONE };

static struct sx_audio_mixer g_audio_mixer = { .fd = -1 };
static int g_use_sfx_prefix = 1;

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

static boolean DG_Sound_Init(boolean use_sfx_prefix) {
    if (sx_audio_mixer_init(&g_audio_mixer,
                            (size_t)snd_channels,
                            SX_AUDIO_DEFAULT_MAX_DELTA_MS) < 0) {
        return false;
    }

    g_use_sfx_prefix = use_sfx_prefix ? 1 : 0;
    snd_samplerate = (int)g_audio_mixer.info.sample_rate_hz;
    return true;
}

static void DG_Sound_Shutdown(void) {
    sx_audio_mixer_destroy(&g_audio_mixer);
    sx_audio_shutdown_samples();
}

static int DG_Sound_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    return sx_audio_resolve_lumpnum(sfxinfo);
}

static void DG_Sound_Update(void) {
    (void)sx_audio_mixer_update(&g_audio_mixer);
}

static void DG_Sound_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= snd_channels) {
        return;
    }
    (void)sx_audio_mixer_set_pan(&g_audio_mixer, (size_t)channel, vol, sep);
}

static int DG_Sound_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    sx_audio_sample_t *sample;
    sfxinfo_t *base;
    uint32_t pitch_q16 = SX_AUDIO_PITCH_NORMAL_Q16;

    if (channel < 0 || channel >= snd_channels) {
        return -1;
    }

    base = sx_audio_root_sfx(sfxinfo);
    sample = sx_audio_decode_sample(sfxinfo);
    if (sample == 0 || base == 0) {
        return -1;
    }

    if (sfxinfo != NULL && sfxinfo->link != NULL && sfxinfo->pitch > 0) {
        uint64_t converted_pitch = ((uint64_t)(uint32_t)sfxinfo->pitch << 16) /
                                    SX_SOUND_PITCH_NORMAL;
        pitch_q16 = converted_pitch > UINT32_MAX ? UINT32_MAX : (uint32_t)converted_pitch;
    }

    if (sx_audio_mixer_start_voice(&g_audio_mixer,
                                   (size_t)channel,
                                   sample->samples,
                                   sample->sample_count,
                                   sample->sample_rate_hz,
                                   pitch_q16,
                                   vol,
                                   sep) < 0) {
        return -1;
    }
    return channel;
}

static void DG_Sound_StopSound(int channel) {
    if (channel < 0 || channel >= snd_channels) {
        return;
    }
    (void)sx_audio_mixer_stop_voice(&g_audio_mixer, (size_t)channel);
}

static boolean DG_Sound_IsPlaying(int channel) {
    if (channel < 0 || channel >= snd_channels) {
        return false;
    }
    return sx_audio_mixer_voice_playing(&g_audio_mixer, (size_t)channel) ? true : false;
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
