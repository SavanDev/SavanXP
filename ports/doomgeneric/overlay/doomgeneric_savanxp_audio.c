#include "savanxp/libc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/audio.h"
#include "savanxp/ldso.h"
#include "savanxp/midi.h"

#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#define SX_SOUND_PITCH_NORMAL 128u

/* La musica entra por libsxmidi.so.0.4: los lumps MUS del WAD se convierten a
 * MIDI con el mus2mid de upstream y el sintetizador los renderiza a PCM una vez
 * por tema, que despues suena en loop en una voz dedicada. No hay streaming ni
 * callback nuevo en el mixer: es el mismo camino que ya usa ccleste, con la
 * diferencia de que el sintetizador es una libreria del sistema y no un decoder
 * de terceros. Ver docs/MIDI.md. */
#define DG_MUSIC_RATE SX_MIDI_DEFAULT_RATE
#define DG_MUSIC_CENTRE 127

/* El tema se sintetiza por adelantado en trozos y suena en loop mientras se
 * termina de llenar. Antes se renderizaba entero de una vez: 96 s de musica
 * cuestan ~120 ms en nativo y cerca de un segundo bajo TCG, y eso se veia como
 * un congelamiento en cada cambio de musica. A ~780x tiempo real, adelantar un
 * segundo cuesta ~1 ms y seguir el ritmo sobra dentro de un frame. */
#define DG_MUSIC_LEAD_FRAMES (DG_MUSIC_RATE * 1u)
#define DG_MUSIC_CHUNK_FRAMES (DG_MUSIC_RATE / 4u)

typedef struct sx_audio_sample {
    uint32_t sample_rate_hz;
    uint32_t sample_count;
    unsigned char *samples;
} sx_audio_sample_t;

static snddevice_t g_dg_sound_devices[] = { SNDDEVICE_SB };
static snddevice_t g_dg_music_devices[] = { SNDDEVICE_SB };

static struct sx_audio_mixer g_audio_mixer = { .fd = -1 };
static int g_use_sfx_prefix = 1;

/* Voz dedicada a la musica, inmediatamente despues de las de efectos: el
 * round-robin de SFX valida channel < snd_channels, asi que nunca la roba. */
static size_t g_music_voice = 0;
static struct sx_midi_song *g_music_song = 0;
static unsigned char *g_music_pcm = 0;
static uint32_t g_music_pcm_frames = 0;
static uint32_t g_music_total_frames = 0;
static uint32_t g_music_rendered = 0;
static int g_music_streaming = 0;
static int g_music_volume = 127;
static int g_music_paused = 0;
static int g_music_playing = 0;
static int g_music_missing_reported = 0;

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
                            (size_t)snd_channels + 1u,
                            SX_AUDIO_DEFAULT_MAX_DELTA_MS) < 0) {
        return false;
    }
    sx_audio_mixer_set_client_name(&g_audio_mixer, "doomgeneric");
    g_music_voice = (size_t)snd_channels;

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

static void dg_music_release_pcm(void) {
    if (g_music_pcm != 0) {
        free(g_music_pcm);
        g_music_pcm = 0;
    }
    g_music_pcm_frames = 0;
    g_music_rendered = 0;
    g_music_streaming = 0;
}

/* Sintetiza hasta `budget` frames mas del tema. La voz del mixer lee el mismo
 * buffer mientras esto corre, asi que el trozo que falta se queda en silencio
 * (128) en vez de en basura: el buffer se limpia antes de arrancar la voz. */
static void dg_music_fill(uint32_t budget) {
    while (g_music_streaming && budget > 0u &&
           g_music_rendered < g_music_total_frames) {
        uint32_t want = g_music_total_frames - g_music_rendered;
        size_t got;

        if (want > budget) {
            want = budget;
        }
        got = sx_midi_song_render(g_music_song, g_music_pcm + g_music_rendered,
                                  want, DG_MUSIC_RATE, 0);
        if (got == 0) {
            break;
        }
        g_music_rendered += (uint32_t)got;
        budget -= (uint32_t)got;
    }
    if (g_music_rendered >= g_music_total_frames) {
        g_music_streaming = 0;
    }
}

static void dg_music_apply_volume(void) {
    if (!sx_audio_mixer_active(&g_audio_mixer) ||
        !sx_audio_mixer_voice_playing(&g_audio_mixer, g_music_voice)) {
        return;
    }
    (void)sx_audio_mixer_set_pan(&g_audio_mixer, g_music_voice,
                                 g_music_paused ? 0 : g_music_volume, DG_MUSIC_CENTRE);
}

static boolean DG_Music_Init(void) {
    /* El mixer ya existe: I_InitSound inicializa los efectos antes que la
     * musica, asi que aca solo se hereda el estado. */
    g_music_song = 0;
    g_music_pcm = 0;
    g_music_pcm_frames = 0;
    g_music_playing = 0;
    g_music_paused = 0;
    return true;
}

static void DG_Music_Shutdown(void) {
    (void)sx_audio_mixer_stop_voice(&g_audio_mixer, g_music_voice);
    g_music_playing = 0;
    dg_music_release_pcm();
    if (g_music_song != 0) {
        sx_midi_song_destroy(g_music_song);
        g_music_song = 0;
    }
}

static void DG_Music_SetVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 127) {
        volume = 127;
    }
    g_music_volume = volume;
    dg_music_apply_volume();
}

static void DG_Music_Pause(void) {
    g_music_paused = 1;
    dg_music_apply_volume();
}

static void DG_Music_Resume(void) {
    g_music_paused = 0;
    dg_music_apply_volume();
}

/* El banco OPL2 del WAD: el lump GENMIDI tal cual. Solo se lee durante
 * `sx_midi_song_create_with_bank`, asi que se libera justo despues. Sin lump
 * (o ilegible) el tema cae en las familias, que es lo que suena cuando el WAD
 * no trae banco. */
static void dg_music_bank(const unsigned char **out_data, size_t *out_len,
                          int *out_lump) {
    int lump;
    int length;

    if (out_data != 0) {
        *out_data = 0;
    }
    if (out_len != 0) {
        *out_len = 0;
    }
    if (out_lump != 0) {
        *out_lump = -1;
    }
    lump = W_CheckNumForName((char *)"GENMIDI");
    if (lump < 0) {
        return;
    }
    length = W_LumpLength((unsigned int)lump);
    if (length <= 0) {
        return;
    }
    if (out_data != 0) {
        *out_data = (const unsigned char *)W_CacheLumpNum(lump, PU_STATIC);
        if (*out_data == 0) {
            return;
        }
    }
    if (out_len != 0) {
        *out_len = (size_t)length;
    }
    if (out_lump != 0) {
        *out_lump = lump;
    }
}

/* Los lumps de musica de un IWAD son MUS; algunos WADs traen MIDI. Los dos
 * caminos terminan en un tema de libsxmidi, con el banco OPL2 del WAD cuando
 * esta. */
static void *DG_Music_RegisterSong(void *data, int len) {
    struct sx_midi_song *song = 0;
    const unsigned char *genmidi = 0;
    size_t genmidi_len = 0;
    int genmidi_lump = -1;

    if (data == 0 || len <= 0) {
        return 0;
    }
    /* Sin la libreria en el volumen el programa arranca igual: el cargador deja
     * las referencias sin reubicar y llamar a sx_midi_* seria un salto a
     * cualquier lado. Se avisa una vez y la musica se ignora. */
    if (ldso_missing() != 0) {
        if (!g_music_missing_reported) {
            eprintf("doomgeneric: sin %s; la musica se ignora\n", ldso_missing());
            g_music_missing_reported = 1;
        }
        return 0;
    }

    dg_music_bank(&genmidi, &genmidi_len, &genmidi_lump);

    if (len >= 4 && memcmp(data, "MUS\x1a", 4) == 0) {
        MEMFILE *mus = mem_fopen_read(data, (size_t)len);
        MEMFILE *out = mem_fopen_write();
        int midi_status = -1;

        /* mus2mid devuelve distinto de cero al FALLAR (convencion de upstream),
         * no al exito. */
        if (mus != 0 && out != 0) {
            void *midi = 0;
            size_t midi_len = 0;

            midi_status = mus2mid(mus, out);
            if (midi_status == 0) {
                mem_get_buf(out, &midi, &midi_len);
                song = sx_midi_song_create_with_bank(midi, midi_len,
                                                     genmidi, genmidi_len);
            }
            if (song == 0) {
                eprintf("doomgeneric: musica: MUS no sirvio (len=%d mus2mid=%d midi=%zu)\n",
                        len, midi_status, midi_len);
            }
        } else {
            eprintf("doomgeneric: musica: sin memoria para memio (mus=%d out=%d)\n",
                    mus != 0, out != 0);
        }
        if (mus != 0) {
            mem_fclose(mus);
        }
        if (out != 0) {
            mem_fclose(out);
        }
    } else if (len >= 4 && memcmp(data, "MThd", 4) == 0) {
        song = sx_midi_song_create_with_bank(data, (size_t)len,
                                             genmidi, genmidi_len);
    } else if (len >= 4) {
        eprintf("doomgeneric: musica: lump desconocido len=%d magic=%02x%02x%02x%02x\n",
                len, ((const unsigned char *)data)[0], ((const unsigned char *)data)[1],
                ((const unsigned char *)data)[2], ((const unsigned char *)data)[3]);
    }

    if (genmidi != 0 && genmidi_lump >= 0) {
        W_ReleaseLumpNum(genmidi_lump);
    }

    if (song == 0) {
        eprintf("doomgeneric: no se pudo preparar la musica del WAD\n");
        return 0;
    }
    if (g_music_song != 0) {
        sx_midi_song_destroy(g_music_song);
    }
    g_music_song = song;
    return song;
}

static void DG_Music_UnRegisterSong(void *handle) {
    (void)sx_audio_mixer_stop_voice(&g_audio_mixer, g_music_voice);
    g_music_playing = 0;
    dg_music_release_pcm();
    if (g_music_song != 0 && (void *)g_music_song == handle) {
        sx_midi_song_destroy(g_music_song);
        g_music_song = 0;
    }
}

static void DG_Music_PlaySong(void *handle, boolean looping) {
    struct sx_midi_song *song = (struct sx_midi_song *)handle;
    uint32_t duration_ms;
    unsigned long render_start;
    unsigned long render_ms;

    g_music_playing = 0;
    if (song == 0 || !sx_audio_mixer_active(&g_audio_mixer)) {
        return;
    }
    dg_music_release_pcm();

    /* El buffer es del tamano del tema y arranca en silencio: la voz lo recorre
     * entera en loop mientras el resto se va sintetizando por frame. */
    duration_ms = sx_midi_song_duration_ms(song);
    g_music_total_frames =
        (uint32_t)(((uint64_t)duration_ms * DG_MUSIC_RATE + 999u) / 1000u);
    if (g_music_total_frames == 0u) {
        g_music_total_frames = 1u;
    }
    g_music_pcm = (unsigned char *)malloc(g_music_total_frames);
    if (g_music_pcm == 0) {
        eprintf("doomgeneric: sin memoria para renderizar la musica\n");
        return;
    }
    g_music_pcm_frames = g_music_total_frames;
    memset(g_music_pcm, 128, g_music_total_frames);
    g_music_rendered = 0;
    g_music_streaming = 1;

    render_start = uptime_ms();
    dg_music_fill(DG_MUSIC_LEAD_FRAMES);
    render_ms = uptime_ms() - render_start;

    (void)sx_audio_mixer_stop_voice(&g_audio_mixer, g_music_voice);
    if (sx_audio_mixer_start_voice(&g_audio_mixer, g_music_voice, g_music_pcm,
                                   g_music_pcm_frames, DG_MUSIC_RATE,
                                   SX_AUDIO_PITCH_NORMAL_Q16,
                                   g_music_paused ? 0 : g_music_volume,
                                   DG_MUSIC_CENTRE) < 0) {
        return;
    }
    (void)sx_audio_mixer_set_voice_loop(&g_audio_mixer, g_music_voice, looping ? 1 : 0);
    g_music_playing = 1;
    printf("doomgeneric: musica sonando: %u frames a %u Hz%s (%s) (arranque %lu ms)\n",
           g_music_pcm_frames, (unsigned)DG_MUSIC_RATE,
           looping ? " (loop)" : "",
           sx_midi_song_uses_genmidi(song) ? "FM" : "familias", render_ms);
}

static void DG_Music_StopSong(void) {
    (void)sx_audio_mixer_stop_voice(&g_audio_mixer, g_music_voice);
    g_music_playing = 0;
}

static boolean DG_Music_IsPlaying(void) {
    return g_music_playing != 0 &&
        sx_audio_mixer_voice_playing(&g_audio_mixer, g_music_voice) ? true : false;
}

static void DG_Music_Poll(void) {
    /* Un trozo por frame alcanza de sobra: a ~780x tiempo real, un cuarto de
     * segundo de musica por frame llena el tema mucho antes de que la voz
     * llegue al final del buffer. */
    if (g_music_playing) {
        dg_music_fill(DG_MUSIC_CHUNK_FRAMES);
    }
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
