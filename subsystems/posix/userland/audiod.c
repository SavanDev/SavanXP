#include "libc.h"
#include "savanxp/audio_server.h"

#include <stdio.h>
#include <string.h>

/*
 * audiod: el que mezcla cuando suenan varios a la vez.
 *
 * /dev/audio0 admite un solo escritor, y el mixer del SDK vive uno por
 * proceso: dos programas nunca podian sonar juntos (el segundo recibia
 * EBUSY). Este demonio es el unico escritor del device y suma los submixes
 * que le llegan por UDP loopback; cada cliente sigue mezclando sus voces en
 * su proceso y manda stereo ya mezclado. El volumen maestro sigue en el
 * kernel (el popup no se entero), y sin demonio todo sigue directo como
 * antes: el cliente lo detecta con el primer write (EBUSY = remoto).
 *
 * Sin red no hay demonio (el loopback necesita la pila, que pide NIC): sale
 * 0 y el sistema sigue por turnos. Sin audio tampoco (exit 1). Si otro
 * audiod ya escucha, el bind da EBUSY y sale 0: singleton por construccion.
 * init lo lanza una vez antes de windowd y no lo supervisa; ante un error
 * fatal de arranque sale, ante uno en servicio sigue (el device se recupera
 * solo cuando el dueño anterior cierra).
 *
 * `audiod --selftest` ejercita el camino completo en el invitado: dos
 * clientes, un datagrama roto, mezcla exacta y escritura real al device.
 */

#define AUDIOD_MAX_STREAMS 8u
#define AUDIOD_FIFO_FRAMES 4800u /* 100 ms a 48 kHz */
#define AUDIOD_STREAM_EXPIRY_MS 500u
#define AUDIOD_RENDER_PERIOD_MS 10u
#define AUDIOD_MAX_RENDER_FRAMES 4800u
#define AUDIOD_RECV_BYTES 2048u
#define AUDIOD_ACQUIRE_RETRY_MS 500u
#define AUDIOD_STATS_PERIOD_MS 30000u

struct audiod_stream {
    int used;
    uint16_t port;
    uint32_t rate_hz;
    uint32_t last_sequence;
    int has_sequence;
    uint64_t last_ms;
    /* Anillo de stereo s16: head consume, tail produce. Sin memmoves: bajo
     * tcg un corrimiento por frame quemaria la CPU para nada. */
    int16_t fifo[AUDIOD_FIFO_FRAMES * 2u];
    size_t fifo_head;
    size_t fifo_frames;
    uint64_t received;
    uint64_t lost;
    uint64_t underruns;
};

static struct audiod_stream g_streams[AUDIOD_MAX_STREAMS];
static uint32_t g_device_rate_hz;
static uint64_t g_dropped_packets;
static uint64_t g_mismatch_packets;
static uint64_t g_last_stats_ms;

static void audiod_log_stats(uint64_t now_ms, int force)
{
    size_t index;
    int active = 0;

    if (!force && now_ms - g_last_stats_ms < AUDIOD_STATS_PERIOD_MS)
    {
        return;
    }
    g_last_stats_ms = now_ms;
    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (g_streams[index].used)
        {
            ++active;
        }
    }
    if (active == 0 && g_dropped_packets == 0 && g_mismatch_packets == 0 && !force)
    {
        return;
    }
    eprintf("audiod: streams=%d dropped=%llu mismatch=%llu\n",
            active,
            (unsigned long long)g_dropped_packets,
            (unsigned long long)g_mismatch_packets);
    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (g_streams[index].used)
        {
            eprintf("audiod: port=%u lost=%llu underruns=%llu\n",
                    (unsigned int)g_streams[index].port,
                    (unsigned long long)g_streams[index].lost,
                    (unsigned long long)g_streams[index].underruns);
        }
    }
}

static struct audiod_stream* audiod_find_stream(uint16_t port)
{
    size_t index;

    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (g_streams[index].used && g_streams[index].port == port)
        {
            return &g_streams[index];
        }
    }
    return 0;
}

static struct audiod_stream* audiod_register_stream(uint16_t port, uint32_t rate_hz, uint64_t now_ms)
{
    size_t index;

    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (!g_streams[index].used)
        {
            struct audiod_stream* stream = &g_streams[index];
            memset(stream, 0, sizeof(*stream));
            stream->used = 1;
            stream->port = port;
            stream->rate_hz = rate_hz;
            stream->last_ms = now_ms;
            eprintf("audiod: cliente en puerto %u (%u Hz)\n", (unsigned int)port, rate_hz);
            return stream;
        }
    }
    return 0;
}

static int audiod_push_frames(
    struct audiod_stream* stream,
    const int16_t* frames,
    size_t frame_count)
{
    size_t room;
    size_t slot;

    if (frame_count == 0)
    {
        return 0;
    }
    if (frame_count > AUDIOD_FIFO_FRAMES)
    {
        /* Mas que la cola entera: quedarse con la cola mas fresca descarta
         * lo viejo, que es lo que menos se extraña en vivo. */
        frames += (frame_count - AUDIOD_FIFO_FRAMES) * 2u;
        frame_count = AUDIOD_FIFO_FRAMES;
        ++stream->underruns;
    }
    room = AUDIOD_FIFO_FRAMES - stream->fifo_frames;
    if (frame_count > room)
    {
        /* Cola llena: avanzar el head descarta lo viejo sin mover memoria. */
        size_t drop = frame_count - room;
        stream->fifo_head = (stream->fifo_head + drop) % AUDIOD_FIFO_FRAMES;
        stream->fifo_frames -= drop;
        ++stream->underruns;
    }
    slot = (stream->fifo_head + stream->fifo_frames) % AUDIOD_FIFO_FRAMES;
    if (slot + frame_count <= AUDIOD_FIFO_FRAMES)
    {
        memcpy(
            stream->fifo + slot * 2u,
            frames,
            frame_count * 2u * sizeof(int16_t));
    }
    else
    {
        size_t first = AUDIOD_FIFO_FRAMES - slot;
        memcpy(stream->fifo + slot * 2u, frames, first * 2u * sizeof(int16_t));
        memcpy(stream->fifo, frames + first * 2u, (frame_count - first) * 2u * sizeof(int16_t));
    }
    stream->fifo_frames += frame_count;
    return 0;
}

static void audiod_handle_datagram(
    const unsigned char* data,
    size_t length,
    uint16_t source_port,
    uint64_t now_ms)
{
    const struct savanxp_audiod_header* header;
    size_t frame_count;
    struct audiod_stream* stream;
    uint32_t gap;

    if (length < sizeof(*header))
    {
        ++g_dropped_packets;
        return;
    }
    header = (const struct savanxp_audiod_header*)data;
    frame_count = (length - sizeof(*header)) / 4u;
    if (header->magic != SAVANXP_AUDIOD_MAGIC || header->version != SAVANXP_AUDIOD_VERSION ||
        header->channels != 2 || header->sample_rate_hz != g_device_rate_hz || frame_count == 0)
    {
        ++g_mismatch_packets;
        return;
    }
    stream = audiod_find_stream(source_port);
    if (stream == 0)
    {
        stream = audiod_register_stream(source_port, header->sample_rate_hz, now_ms);
        if (stream == 0)
        {
            ++g_dropped_packets;
            return;
        }
    }
    if (stream->has_sequence)
    {
        gap = header->sequence - stream->last_sequence - 1u;
        if (gap != 0)
        {
            stream->lost += gap;
        }
    }
    stream->has_sequence = 1;
    stream->last_sequence = header->sequence;
    stream->last_ms = now_ms;
    stream->received += 1;
    (void)audiod_push_frames(stream, (const int16_t*)(data + sizeof(*header)), frame_count);
}

static void audiod_expire_streams(uint64_t now_ms)
{
    size_t index;

    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (g_streams[index].used && now_ms - g_streams[index].last_ms > AUDIOD_STREAM_EXPIRY_MS)
        {
            eprintf("audiod: cliente en puerto %u expiro\n", (unsigned int)g_streams[index].port);
            memset(&g_streams[index], 0, sizeof(g_streams[index]));
        }
    }
}

static int32_t audiod_soft_knee(int32_t mixed)
{
    /* La misma rodilla del mixer del SDK (0.75 FS, 8:1): la suma de submixes
     * a fondo de escala cuadra igual que la suma de voces. */
    const int32_t knee = 24576;
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
    return mixed;
}

static size_t audiod_render(int16_t* out, size_t frame_count)
{
    size_t frame;

    for (frame = 0; frame < frame_count; ++frame)
    {
        int32_t left = 0;
        int32_t right = 0;
        size_t index;

        for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
        {
            struct audiod_stream* stream = &g_streams[index];
            if (!stream->used)
            {
                continue;
            }
            if (stream->fifo_frames == 0)
            {
                ++stream->underruns;
                continue;
            }
            left += stream->fifo[stream->fifo_head * 2u];
            right += stream->fifo[stream->fifo_head * 2u + 1u];
            stream->fifo_head = (stream->fifo_head + 1u) % AUDIOD_FIFO_FRAMES;
            stream->fifo_frames -= 1u;
        }
        out[frame * 2u] = (int16_t)audiod_soft_knee(left);
        out[frame * 2u + 1u] = (int16_t)audiod_soft_knee(right);
    }
    return frame_count;
}

static int audiod_open_socket(void)
{
    struct savanxp_sockaddr_in address;
    long fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (fd < 0)
    {
        return (int)fd;
    }
    address.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
    address.port = SAVANXP_AUDIOD_PORT;
    address.reserved0 = 0;
    {
        long bound = savanxp_bind((int)fd, &address);
        if (bound < 0)
        {
            savanxp_close((int)fd);
            return (int)bound;
        }
    }
    /* Sin bloqueo: el loop drena lo que haya y duerme por reloj. */
    if (savanxp_fcntl((int)fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
    {
        savanxp_close((int)fd);
        return -SAVANXP_EIO;
    }
    return (int)fd;
}

/* El loopback tiene que funcionar antes de tocar el device: sin red el
 * demonio es sordo y el sistema sigue por turnos sin el. */
static int audiod_check_loopback(int fd)
{
    struct savanxp_sockaddr_in address;
    struct savanxp_sockaddr_in source;
    unsigned char probe[16];
    unsigned char reply[16];
    long sent;
    long got;

    address.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
    address.port = SAVANXP_AUDIOD_PORT;
    address.reserved0 = 0;
    memset(probe, 0xA5, sizeof(probe));
    sent = savanxp_sendto(fd, probe, sizeof(probe), &address);
    if (sent != (long)sizeof(probe))
    {
        return -1;
    }
    got = savanxp_recvfrom(fd, reply, sizeof(reply), &source, 500);
    if (got != (long)sizeof(reply) || memcmp(probe, reply, sizeof(probe)) != 0)
    {
        return -1;
    }
    return 0;
}

static int audiod_acquire_device(int fd, const struct savanxp_audio_info* info)
{
    unsigned char silence[4096];
    unsigned long waited = 0;

    memset(silence, 0, sizeof(silence));
    for (;;)
    {
        long written = savanxp_write(fd, silence, info->period_bytes);
        if (written == (long)info->period_bytes)
        {
            return 0;
        }
        if (written != -(long)SAVANXP_EBUSY)
        {
            return -1;
        }
        if (waited == 0)
        {
            eprintf("audiod: /dev/audio0 ocupado, esperando\n");
        }
        sleep_ms(AUDIOD_ACQUIRE_RETRY_MS);
        waited += AUDIOD_ACQUIRE_RETRY_MS;
        if ((waited % 30000u) == 0)
        {
            eprintf("audiod: /dev/audio0 sigue ocupado\n");
        }
    }
}

static int audiod_serve(int sock_fd, int audio_fd)
{
    unsigned char datagram[SAVANXP_AUDIOD_MAX_DATAGRAM];
    struct savanxp_sockaddr_in source;
    int16_t mixed[AUDIOD_MAX_RENDER_FRAMES * 2u];
    uint64_t last_render_ms = uptime_ms();
    uint64_t next_tick_ms = last_render_ms;

    for (;;)
    {
        long got;
        uint64_t now_ms = uptime_ms();
        uint64_t due_ms;
        size_t frames;
        size_t bytes;
        long written;

        while ((got = savanxp_recvfrom(
                    sock_fd, datagram, sizeof(datagram), &source, 0)) > 0)
        {
            audiod_handle_datagram(datagram, (size_t)got, source.port, now_ms);
        }
        audiod_expire_streams(now_ms);

        due_ms = now_ms > last_render_ms ? now_ms - last_render_ms : 0;
        frames = (size_t)((due_ms * (uint64_t)g_device_rate_hz) / 1000u);
        if (frames > AUDIOD_MAX_RENDER_FRAMES)
        {
            frames = AUDIOD_MAX_RENDER_FRAMES;
        }
        if (frames == 0)
        {
            if (next_tick_ms > now_ms)
            {
                sleep_ms((unsigned long)(next_tick_ms - now_ms));
            }
            else
            {
                sleep_ms(1);
            }
            next_tick_ms = uptime_ms() + AUDIOD_RENDER_PERIOD_MS;
            continue;
        }
        last_render_ms = now_ms;
        next_tick_ms = now_ms + AUDIOD_RENDER_PERIOD_MS;
        (void)audiod_render(mixed, frames);
        bytes = frames * 4u;
        written = savanxp_write(audio_fd, mixed, bytes);
        if (written != (long)bytes)
        {
            eprintf("audiod: escritura al device fallo (%s)\n", result_error_string(written));
        }
        audiod_log_stats(now_ms, 0);
    }
}

/* --- Selftest ------------------------------------------------------------ */

/* Dos clientes en el mismo proceso (dos sockets = dos puertos = dos
 * streams), tonos cuadrados de fase continua, un datagrama roto en el
 * medio, mezcla exacta y escritura real al device. Sin device o con otro
 * audiod vivo se omite: el entorno no da para probarlo. */
#define AUDIOD_SELFTEST_FRAMES 4800u
#define AUDIOD_SELFTEST_TONE_A_PERIOD 120u
#define AUDIOD_SELFTEST_TONE_B_PERIOD 80u

static int16_t audiod_selftest_tone(int which, size_t frame)
{
    unsigned int period =
        which == 0 ? AUDIOD_SELFTEST_TONE_A_PERIOD : AUDIOD_SELFTEST_TONE_B_PERIOD;
    int16_t level = which == 0 ? 8000 : 6000;
    return (frame % period) < period / 2u ? level : (int16_t)-level;
}

static int audiod_selftest_send_one(
    int fd,
    int which,
    uint32_t* sequence,
    size_t base_frame,
    size_t frame_count)
{
    unsigned char datagram[SAVANXP_AUDIOD_MAX_DATAGRAM];
    struct savanxp_audiod_header* header = (struct savanxp_audiod_header*)datagram;
    struct savanxp_sockaddr_in address;
    size_t bytes;
    int16_t* out;
    size_t j;

    if (frame_count == 0 || frame_count > SAVANXP_AUDIOD_MAX_FRAMES)
    {
        return -1;
    }
    address.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
    address.port = SAVANXP_AUDIOD_PORT;
    address.reserved0 = 0;
    header->magic = SAVANXP_AUDIOD_MAGIC;
    header->version = SAVANXP_AUDIOD_VERSION;
    header->channels = 2;
    header->sample_rate_hz = g_device_rate_hz;
    header->sequence = (*sequence)++;
    out = (int16_t*)(datagram + sizeof(*header));
    for (j = 0; j < frame_count; ++j)
    {
        int16_t sample = audiod_selftest_tone(which, base_frame + j);
        out[j * 2u] = sample;
        out[j * 2u + 1u] = sample;
    }
    bytes = sizeof(*header) + frame_count * 4u;
    if (savanxp_sendto(fd, datagram, bytes, &address) != (long)bytes)
    {
        return -1;
    }
    return 0;
}

static int audiod_selftest(void)
{
    struct savanxp_audio_info info = {0};
    struct savanxp_sockaddr_in address;
    long audio_fd;
    long sock_a = -1;
    long sock_b = -1;
    long daemon_fd = -1;
    int16_t mixed[AUDIOD_SELFTEST_FRAMES * 2u];
    unsigned char datagram[SAVANXP_AUDIOD_MAX_DATAGRAM];
    struct savanxp_sockaddr_in source;
    uint32_t sequence_a = 0;
    uint32_t sequence_b = 0;
    size_t frame;
    int ok = 1;

    audio_fd = (long)audio_open();
    if (audio_fd < 0)
    {
        eprintf("AUDIOD SELFTEST SKIP sin device\n");
        return 0;
    }
    if (audio_get_info((int)audio_fd, &info) < 0 || info.sample_rate_hz == 0 ||
        info.channels != 2 || info.bits_per_sample != 16 || info.frame_bytes != 4)
    {
        eprintf("AUDIOD SELFTEST SKIP formato inesperado\n");
        savanxp_close((int)audio_fd);
        return 0;
    }
    g_device_rate_hz = info.sample_rate_hz;

    daemon_fd = audiod_open_socket();
    if (daemon_fd == -SAVANXP_EBUSY)
    {
        eprintf("AUDIOD SELFTEST SKIP demonio corriendo\n");
        savanxp_close((int)audio_fd);
        return 0;
    }
    if (daemon_fd < 0)
    {
        eprintf("AUDIOD SELFTEST FAIL no se pudo escuchar\n");
        savanxp_close((int)audio_fd);
        return 1;
    }
    sock_a = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    sock_b = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (sock_a < 0 || sock_b < 0)
    {
        eprintf("AUDIOD SELFTEST FAIL sin sockets\n");
        goto fail;
    }
    /* Entrelazado por cliente y con drenaje: 20 datagramas de golpe rebalsan
     * la cola de 16 del socket, y en produccion el drenaje cada 10 ms evita
     * ese patron. Aca se manda de a un datagrama por lado y se drena. */
    {
        size_t sent_a = 0;
        size_t sent_b = 0;
        int failed = 0;
        while ((sent_a < AUDIOD_SELFTEST_FRAMES || sent_b < AUDIOD_SELFTEST_FRAMES) && !failed)
        {
            long got;
            if (sent_a < AUDIOD_SELFTEST_FRAMES)
            {
                size_t chunk = AUDIOD_SELFTEST_FRAMES - sent_a;
                if (chunk > SAVANXP_AUDIOD_MAX_FRAMES)
                {
                    chunk = SAVANXP_AUDIOD_MAX_FRAMES;
                }
                if (audiod_selftest_send_one(
                        (int)sock_a, 0, &sequence_a, sent_a, chunk) < 0)
                {
                    failed = 1;
                }
                sent_a += chunk;
            }
            if (!failed && sent_b < AUDIOD_SELFTEST_FRAMES)
            {
                size_t chunk = AUDIOD_SELFTEST_FRAMES - sent_b;
                if (chunk > SAVANXP_AUDIOD_MAX_FRAMES)
                {
                    chunk = SAVANXP_AUDIOD_MAX_FRAMES;
                }
                if (audiod_selftest_send_one(
                        (int)sock_b, 1, &sequence_b, sent_b, chunk) < 0)
                {
                    failed = 1;
                }
                sent_b += chunk;
            }
            while ((got = savanxp_recvfrom(
                        (int)daemon_fd, datagram, sizeof(datagram), &source, 0)) > 0)
            {
                audiod_handle_datagram(datagram, (size_t)got, source.port, uptime_ms());
            }
        }
        if (failed)
        {
            eprintf("AUDIOD SELFTEST FAIL no se pudo enviar\n");
            goto fail;
        }
    }
    /* Un datagrama roto no puede voltear nada: se cuenta y se sigue. */
    address.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
    address.port = SAVANXP_AUDIOD_PORT;
    address.reserved0 = 0;
    memset(datagram, 0x5A, 32);
    if (savanxp_sendto((int)sock_a, datagram, 32, &address) != 32)
    {
        eprintf("AUDIOD SELFTEST FAIL send roto fallo\n");
        goto fail;
    }
    for (;;)
    {
        long got = savanxp_recvfrom(
            (int)daemon_fd, datagram, sizeof(datagram), &source, 100);
        uint64_t now_ms;
        if (got == -SAVANXP_ETIMEDOUT || got == -SAVANXP_EAGAIN)
        {
            break;
        }
        if (got < 0)
        {
            eprintf("AUDIOD SELFTEST FAIL recv (%s)\n", result_error_string(got));
            goto fail;
        }
        now_ms = uptime_ms();
        audiod_handle_datagram(datagram, (size_t)got, source.port, now_ms);
    }
    {
        size_t index;
        int active = 0;
        for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
        {
            active += g_streams[index].used ? 1 : 0;
        }
        if (active != 2)
        {
            eprintf("AUDIOD SELFTEST FAIL streams=%d, se esperaban 2\n", active);
            goto fail;
        }
    }
    (void)audiod_render(mixed, AUDIOD_SELFTEST_FRAMES);
    for (frame = 0; frame < AUDIOD_SELFTEST_FRAMES; ++frame)
    {
        int16_t expect =
            (int16_t)(audiod_selftest_tone(0, frame) + audiod_selftest_tone(1, frame));
        if (mixed[frame * 2u] != expect || mixed[frame * 2u + 1u] != expect)
        {
            ok = 0;
            break;
        }
    }
    if (!ok)
    {
        eprintf("AUDIOD SELFTEST FAIL mezcla inexacta en frame %u\n", (unsigned int)frame);
        goto fail;
    }
    if (savanxp_write((int)audio_fd, mixed, sizeof(mixed)) != (long)sizeof(mixed))
    {
        eprintf("AUDIOD SELFTEST FAIL no se pudo escribir al device\n");
        goto fail;
    }
    /* Sostener 2.5 s como audiostream: un burst de 100 ms no llega al WAV
     * que graba el host (el close frena el motor antes de que QEMU consuma
     * nada), y la mezcla continua tambien merece oirse. Paceado a reloj de
     * pared para que el device drene a ritmo real. */
    for (frame = 0; frame < 25u; ++frame)
    {
        if (savanxp_write((int)audio_fd, mixed, sizeof(mixed)) != (long)sizeof(mixed))
        {
            eprintf("AUDIOD SELFTEST FAIL no se pudo sostener\n");
            goto fail;
        }
        sleep_ms(100);
    }
    eprintf("AUDIOD SELFTEST PASS\n");
    savanxp_close((int)sock_a);
    savanxp_close((int)sock_b);
    savanxp_close((int)daemon_fd);
    savanxp_close((int)audio_fd);
    return 0;

fail:
    if (sock_a >= 0)
    {
        savanxp_close((int)sock_a);
    }
    if (sock_b >= 0)
    {
        savanxp_close((int)sock_b);
    }
    if (daemon_fd >= 0)
    {
        savanxp_close((int)daemon_fd);
    }
    savanxp_close((int)audio_fd);
    return 1;
}

int main(int argc, char** argv)
{
    struct savanxp_audio_info info = {0};
    long audio_fd;
    long sock_fd;

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
    {
        return audiod_selftest();
    }
    if (argc >= 2)
    {
        eprintf("uso: audiod [--selftest]\n");
        return 1;
    }

    sock_fd = audiod_open_socket();
    if (sock_fd == -SAVANXP_EBUSY)
    {
        eprintf("audiod: otro demonio ya escucha\n");
        return 0;
    }
    if (sock_fd < 0)
    {
        eprintf("audiod: no se pudo escuchar (%s)\n", result_error_string(sock_fd));
        return 1;
    }
    if (audiod_check_loopback((int)sock_fd) < 0)
    {
        eprintf("audiod: sin loopback (sin red); sale sin tomar el device\n");
        savanxp_close((int)sock_fd);
        return 0;
    }

    audio_fd = (long)audio_open();
    if (audio_fd < 0)
    {
        eprintf("audiod: /dev/audio0 no disponible (%s)\n", result_error_string(audio_fd));
        savanxp_close((int)sock_fd);
        return 1;
    }
    if (audio_get_info((int)audio_fd, &info) < 0 || info.sample_rate_hz == 0 ||
        info.channels != 2 || info.bits_per_sample != 16 || info.frame_bytes != 4)
    {
        eprintf("audiod: formato de audio inesperado\n");
        savanxp_close((int)sock_fd);
        savanxp_close((int)audio_fd);
        return 1;
    }
    g_device_rate_hz = info.sample_rate_hz;
    if (audiod_acquire_device((int)audio_fd, &info) < 0)
    {
        eprintf("audiod: /dev/audio0 no se deja escribir\n");
        savanxp_close((int)sock_fd);
        savanxp_close((int)audio_fd);
        return 1;
    }
    eprintf("audiod: mezclando a %u Hz\n", g_device_rate_hz);
    return audiod_serve((int)sock_fd, (int)audio_fd);
}
