#include "savanxp/audio.h"

#include "savanxp/audio_server.h"
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
    if (mixer->server_link.udp_fd >= 0)
    {
        (void)savanxp_close(mixer->server_link.udp_fd);
        mixer->server_link.udp_fd = -1;
    }
    mixer->server_link.mode = 0;
    mixer->server_link.sequence = 0;
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
    mixer->server_link.udp_fd = -1;
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
    mixer->server_link.udp_fd = -1;
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
    voice->loop = 0;
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

int sx_audio_mixer_set_voice_loop(
    struct sx_audio_mixer* mixer,
    size_t voice_index,
    int loop)
{
    if (!sx_audio_voice_index_valid(mixer, voice_index))
    {
        return -SAVANXP_EINVAL;
    }
    mixer->voices[voice_index].loop = loop != 0;
    return 0;
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

static int16_t sx_audio_soft_clip(int mixed)
{
    /* Rodilla suave a 0.75 del fondo de escala, 8:1 por encima. Las SFX de
     * los juegos son cuadradas a fondo de escala por naturaleza (crest ~1),
     * y con 8 voces a 127 tres solapadas ya cuadran un clamp duro -- que es
     * lo que se escuchaba saturado por mas que se bajara el volumen maestro
     * (la forma cuadrada ya venia rota de la mezcla). Por debajo de la
     * rodilla no se toca ni un bit, asi que una mezcla que no clipeaba es
     * identica; por encima se comprime en vez de cuadrar, y solo un extremo
     * absurdo llega al clamp final. */
    const int knee = 24576;
    if (mixed > knee)
    {
        mixed = knee + (mixed - knee) / 8;
    }
    else if (mixed < -knee)
    {
        mixed = -knee + (mixed + knee) / 8;
    }
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
                if (voice->loop && voice->sample_count != 0)
                {
                    voice->position_fixed %= (uint64_t)voice->sample_count << 16;
                    sample_index_wide = voice->position_fixed >> 16;
                }
                else
                {
                    voice->active = 0;
                    continue;
                }
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
                if (voice->loop && voice->sample_count != 0)
                {
                    voice->position_fixed %= (uint64_t)voice->sample_count << 16;
                }
                else
                {
                    voice->active = 0;
                }
            }
        }

        mixer->mix_buffer[(frame * mixer->info.channels) + 0] =
            sx_audio_soft_clip(mixed_left);
        mixer->mix_buffer[(frame * mixer->info.channels) + 1] =
            sx_audio_soft_clip(mixed_right);
    }
}

void sx_audio_server_link_init(struct sx_audio_server_link* link)
{
    if (link == 0)
    {
        return;
    }
    link->mode = 0;
    link->udp_fd = -1;
    link->sequence = 0;
}

void sx_audio_server_link_close(struct sx_audio_server_link* link)
{
    if (link == 0)
    {
        return;
    }
    if (link->udp_fd >= 0)
    {
        savanxp_close(link->udp_fd);
    }
    link->mode = 0;
    link->udp_fd = -1;
    link->sequence = 0;
}

long sx_audio_server_send(
    int socket_fd,
    uint32_t* sequence,
    const int16_t* frames,
    size_t frame_count,
    uint32_t rate_hz)
{
    struct savanxp_sockaddr_in address;
    size_t offset = 0;

    if (socket_fd < 0 || sequence == 0 || (frames == 0 && frame_count != 0) || rate_hz == 0)
    {
        return -SAVANXP_EINVAL;
    }
    address.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
    address.port = SAVANXP_AUDIOD_PORT;
    address.reserved0 = 0;

    while (offset < frame_count)
    {
        unsigned char datagram[SAVANXP_AUDIOD_MAX_DATAGRAM];
        struct savanxp_audiod_header* header = (struct savanxp_audiod_header*)datagram;
        size_t chunk = frame_count - offset;
        size_t bytes;
        long sent;

        if (chunk > SAVANXP_AUDIOD_MAX_FRAMES)
        {
            chunk = SAVANXP_AUDIOD_MAX_FRAMES;
        }
        bytes = sizeof(*header) + chunk * 4u;
        header->magic = SAVANXP_AUDIOD_MAGIC;
        header->version = SAVANXP_AUDIOD_VERSION;
        header->channels = 2;
        header->sample_rate_hz = rate_hz;
        header->sequence = (*sequence)++;
        memcpy(datagram + sizeof(*header), frames + offset * 2u, chunk * 4u);
        sent = savanxp_sendto(socket_fd, datagram, bytes, &address);
        if (sent != (long)bytes)
        {
            return sent < 0 ? sent : -SAVANXP_EIO;
        }
        offset += chunk;
    }
    return 0;
}

long sx_audio_server_output(
    struct sx_audio_server_link* link,
    int audio_fd,
    const int16_t* frames,
    size_t bytes,
    uint32_t rate_hz)
{
    long written;

    if (link == 0 || frames == 0 || bytes == 0 || (bytes % 4u) != 0 || rate_hz == 0)
    {
        return -SAVANXP_EINVAL;
    }
    if (link->mode == 2)
    {
        if (sx_audio_server_send(
                link->udp_fd, &link->sequence, frames, bytes / 4u, rate_hz) == 0)
        {
            return 1;
        }
        link->mode = 0;
        if (link->udp_fd >= 0)
        {
            savanxp_close(link->udp_fd);
            link->udp_fd = -1;
        }
    }
    /* Directo o indeciso: el write decide. Exito = directo de ahora en mas;
     * EBUSY = otro tiene el device, se intenta remoto abajo; cualquier otro
     * error es fatal. */
    written = savanxp_write(audio_fd, frames, bytes);
    if (written == (long)bytes)
    {
        link->mode = 1;
        return 1;
    }
    if (written != -(long)SAVANXP_EBUSY)
    {
        return written < 0 ? written : -SAVANXP_EIO;
    }
    if (link->udp_fd < 0)
    {
        long sock = savanxp_socket(
            SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
        if (sock < 0)
        {
            return sock;
        }
        link->udp_fd = (int)sock;
    }
    if (sx_audio_server_send(
            link->udp_fd, &link->sequence, frames, bytes / 4u, rate_hz) == 0)
    {
        link->mode = 2;
        return 1;
    }
    return 0;
}

long sx_audio_mixer_update(struct sx_audio_mixer* mixer)
{
    size_t frames;
    size_t bytes;

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
    {
        /* Por el demonio si esta, o directo al device si no: el enlace
         * aprende solo con el primer write (EBUSY = remoto). */
        long sent = sx_audio_server_output(
            &mixer->server_link,
            mixer->fd,
            mixer->mix_buffer,
            bytes,
            mixer->info.sample_rate_hz);
        if (sent < 0)
        {
            sx_audio_mixer_disable(mixer);
            return sent;
        }
        return sent;
    }
}
