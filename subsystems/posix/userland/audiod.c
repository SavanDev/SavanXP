#include "libc.h"
#include "savanxp/audio_server.h"
#include "sxn/sxn.h"

#include <stdio.h>
#include <string.h>

/*
 * audiod is the single PCM writer and mixes each application's stereo stream.
 * SUN clients use the native audiod port; existing POSIX clients use UDP
 * loopback when networking is available. The native port reserves the service
 * name before acquiring the device and makes duplicate startup harmless even
 * on a machine without a network driver. Master gain remains in the kernel.
 * init starts the service before windowd and restarts it after a serving crash.
 * `audiod --selftest` checks exact mixed samples and a real device write.
 */

#define AUDIOD_MAX_STREAMS SAVANXP_AUDIOD_MAX_STREAMS
#define AUDIOD_FIFO_FRAMES 4800u /* 100 ms a 48 kHz */
/* Dos tiempos distintos que antes eran uno solo. La expiracion es de la ENTRADA
 * del stream: callado un rato largo, se libera el slot. El PLC es otra cosa:
 * cubre un instante de jitter repitiendo el ultimo frame. Estirarlo hasta la
 * expiracion entera dejaba medio segundo de meseta despues de que el cliente se
 * fuera -- con el cliente muerto, nadie pidio ese audio. */
#define AUDIOD_STREAM_EXPIRY_MS 500u
#define AUDIOD_PLC_MS 50u
#define AUDIOD_RENDER_PERIOD_MS 10u
#define AUDIOD_MAX_RENDER_FRAMES 4800u
#define AUDIOD_ACQUIRE_RETRY_MS 500u
#define AUDIOD_STATS_PERIOD_MS 30000u

struct audiod_stream {
    int used;
    uint16_t port;
    uint32_t rate_hz;
    uint8_t volume; /* 0..100, initially 100 */
    uint8_t muted;
    uint32_t id;
    uint16_t peak_left, peak_right;
    uint64_t peak_ms;
    char name[SAVANXP_AUDIOD_NAME_BYTES];
    uint32_t last_sequence;
    int has_sequence;
    uint64_t last_ms; /* ultima vez que se supo del stream (audio o DECLARE) */
    uint64_t last_audio_ms; /* ultimo datagrama con muestras: manda el PLC */
    /* Anillo de stereo s16: head consume, tail produce. Sin memmoves: bajo
     * tcg un corrimiento por frame quemaria la CPU para nada. */
    int16_t fifo[AUDIOD_FIFO_FRAMES * 2u];
    size_t fifo_head;
    size_t fifo_frames;
    /* Last raw frame for PLC. Apply current gain/mute when concealing too,
     * so a control change takes effect without waiting for another packet.
     * has_last stays clear until the first frame is played. */
    int16_t last_left;
    int16_t last_right;
    int has_last;
    uint64_t received;
    uint64_t lost;
    uint64_t underruns;
};

static struct audiod_stream g_streams[AUDIOD_MAX_STREAMS];

/* SUN clients have no sockets: they connect to the native port
 * SAVANXP_AUDIOD_PORT_NAME and write fixed-size records on a pipe
 * (audio/audiod_protocol.h). Each connection is a stream numbered by its
 * slot, below every port UDP hands out. */
struct audiod_native_client {
    int used;
    long read_handle; /* client -> daemon */
    long write_handle; /* daemon -> client: census replies */
};

static struct audiod_native_client g_native[SAVANXP_AUDIOD_NATIVE_SLOTS];
static long g_native_port = -1;
static uint32_t g_next_stream_id = 1;
static uint16_t g_peak_left, g_peak_right;
static uint64_t g_peak_ms;
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
            stream->volume = 100;
            stream->id = g_next_stream_id++;
            if (!g_next_stream_id) g_next_stream_id = 1;
            stream->last_ms = now_ms;
            stream->last_audio_ms = now_ms;
            eprintf("audiod: cliente en puerto %u (%u Hz)\n", (unsigned int)port, rate_hz);
            return stream;
        }
    }
    return 0;
}

static void audiod_copy_name(char* out, const char* in)
{
    size_t index = 0;

    memset(out, 0, SAVANXP_AUDIOD_NAME_BYTES);
    if (in == 0)
    {
        return;
    }
    while (index + 1u < SAVANXP_AUDIOD_NAME_BYTES && in[index] != '\0')
    {
        out[index] = in[index];
        ++index;
    }
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

static void audiod_handle_audio(
    const unsigned char* data,
    size_t length,
    uint16_t source_port,
    uint64_t now_ms);
static void audiod_handle_declare(
    const unsigned char* data,
    size_t length,
    uint16_t source_port,
    uint64_t now_ms);
static void audiod_handle_set_volume(
    const unsigned char* data,
    size_t length,
    uint64_t now_ms);
typedef void (*audiod_reply_sink)(const struct savanxp_audiod_list_reply* reply, void* context);
static void audiod_handle_list(audiod_reply_sink sink, void* context);

/* Where a census goes when it was asked for over UDP. */
struct audiod_udp_reply {
    int sock_fd;
    const struct savanxp_sockaddr_in* source;
};

static void audiod_send_udp_reply(const struct savanxp_audiod_list_reply* reply, void* context)
{
    const struct audiod_udp_reply* to = (const struct audiod_udp_reply*)context;
    (void)savanxp_sendto(to->sock_fd, reply, sizeof(*reply), to->source);
}

static void audiod_handle_datagram(
    int sock_fd,
    const unsigned char* data,
    size_t length,
    const struct savanxp_sockaddr_in* source,
    uint64_t now_ms)
{
    uint32_t magic = 0;

    if (length >= sizeof(magic))
    {
        memcpy(&magic, data, sizeof(magic));
    }
    if (magic == SAVANXP_AUDIOD_CONTROL_MAGIC)
    {
        uint16_t kind = 0;
        if (length >= sizeof(magic) + 2u + 2u)
        {
            memcpy(&kind, data + sizeof(magic) + 2u, sizeof(kind));
        }
        if (kind == SAVANXP_AUDIOD_DECLARE)
        {
            audiod_handle_declare(data, length, source->port, now_ms);
        }
        else if (kind == SAVANXP_AUDIOD_SET_VOLUME)
        {
            audiod_handle_set_volume(data, length, now_ms);
        }
        else if (kind == SAVANXP_AUDIOD_LIST)
        {
            struct audiod_udp_reply reply_to;
            reply_to.sock_fd = sock_fd;
            reply_to.source = source;
            audiod_handle_list(audiod_send_udp_reply, &reply_to);
        }
        else
        {
            ++g_mismatch_packets;
        }
        return;
    }
    audiod_handle_audio(data, length, source->port, now_ms);
}

static void audiod_handle_audio(
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
    stream->last_audio_ms = now_ms;
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

static void audiod_handle_declare(
    const unsigned char* data,
    size_t length,
    uint16_t source_port,
    uint64_t now_ms)
{
    struct savanxp_audiod_declare declare;
    struct audiod_stream* stream;

    if (length < sizeof(declare))
    {
        ++g_dropped_packets;
        return;
    }
    memcpy(&declare, data, sizeof(declare));
    if (declare.version != SAVANXP_AUDIOD_CONTROL_VERSION ||
        declare.kind != SAVANXP_AUDIOD_DECLARE)
    {
        ++g_mismatch_packets;
        return;
    }
    declare.name[sizeof(declare.name) - 1u] = '\0';
    stream = audiod_find_stream(source_port);
    if (stream == 0)
    {
        stream = audiod_register_stream(source_port, g_device_rate_hz, now_ms);
        if (stream == 0)
        {
            ++g_dropped_packets;
            return;
        }
    }
    audiod_copy_name(stream->name, declare.name);
    stream->last_ms = now_ms;
}

static void audiod_handle_set_volume(
    const unsigned char* data,
    size_t length,
    uint64_t now_ms)
{
    struct savanxp_audiod_set_volume request;
    struct audiod_stream* stream;
    size_t index;

    (void)now_ms;
    if (length < sizeof(request))
    {
        ++g_dropped_packets;
        return;
    }
    memcpy(&request, data, sizeof(request));
    if (request.version != SAVANXP_AUDIOD_CONTROL_VERSION ||
        request.kind != SAVANXP_AUDIOD_SET_VOLUME || request.volume > 100)
    {
        ++g_mismatch_packets;
        return;
    }
    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (g_streams[index].used && g_streams[index].port == request.target_port)
        {
            stream = &g_streams[index];
            stream->volume = request.volume;
            return;
        }
    }
    ++g_dropped_packets;
}

/* The census: one REPLY per live stream, or a single one with count 0 so
 * that an empty census still answers. `sink` delivers each reply the way the
 * question came in. */
static void audiod_handle_list(audiod_reply_sink sink, void* context)
{
    struct savanxp_audiod_list_reply reply;
    struct audiod_stream* snapshot[AUDIOD_MAX_STREAMS];
    size_t count = 0;
    size_t index;

    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index)
    {
        if (g_streams[index].used)
        {
            snapshot[count++] = &g_streams[index];
        }
    }
    if (count == 0)
    {
        memset(&reply, 0, sizeof(reply));
        reply.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        reply.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        reply.kind = SAVANXP_AUDIOD_REPLY;
        sink(&reply, context);
        return;
    }
    for (index = 0; index < count; ++index)
    {
        memset(&reply, 0, sizeof(reply));
        reply.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        reply.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        reply.kind = SAVANXP_AUDIOD_REPLY;
        reply.count = (uint8_t)count;
        reply.index = (uint8_t)index;
        reply.port = snapshot[index]->port;
        reply.volume = snapshot[index]->volume;
        audiod_copy_name(reply.name, snapshot[index]->name);
        sink(&reply, context);
    }
}

/* --- Native clients ------------------------------------------------------ */

static void audiod_send_native_reply(const struct savanxp_audiod_list_reply* reply, void* context)
{
    const struct audiod_native_client* client = (const struct audiod_native_client*)context;
    struct savanxp_audiod_record record;

    memset(&record, 0, sizeof(record));
    record.magic = SAVANXP_AUDIOD_RECORD_MAGIC;
    record.length = (uint32_t)sizeof(*reply);
    memcpy(record.payload, reply, sizeof(*reply));
    /* Non-blocking and whole: a client that does not read its replies loses
     * them, it does not stall the mixer. */
    (void)sxn_write(client->write_handle, &record, sizeof(record));
}

static void audiod_close_native(struct audiod_native_client* client)
{
    (void)sxn_close(client->read_handle);
    (void)sxn_close(client->write_handle);
    memset(client, 0, sizeof(*client));
}

/* Takes every pending connection. A ninth client is refused by closing its
 * pipes, which it reads as the daemon going away. */
static void audiod_accept_native(uint64_t now_ms)
{
    if (g_native_port < 0)
    {
        return;
    }
    while (sxn_wait_one(g_native_port, 0) == 0)
    {
        struct sxn_port_connection connection;
        size_t slot;
        uint16_t port;
        struct audiod_stream* stale;

        if (sxn_port_accept(g_native_port, &connection) < 0)
        {
            return;
        }
        for (slot = 0; slot < SAVANXP_AUDIOD_NATIVE_SLOTS && g_native[slot].used; ++slot)
        {
        }
        if (slot == SAVANXP_AUDIOD_NATIVE_SLOTS)
        {
            (void)sxn_close(connection.read_handle);
            (void)sxn_close(connection.write_handle);
            ++g_dropped_packets;
            continue;
        }
        (void)sxn_set_nonblocking(connection.read_handle, 1);
        (void)sxn_set_nonblocking(connection.write_handle, 1);
        g_native[slot].used = 1;
        g_native[slot].read_handle = connection.read_handle;
        g_native[slot].write_handle = connection.write_handle;
        port = (uint16_t)(SAVANXP_AUDIOD_NATIVE_FIRST_PORT + slot);
        /* A new client in an old slot starts a new stream, not the tail of
         * the last one's. */
        stale = audiod_find_stream(port);
        if (stale != 0)
        {
            memset(stale, 0, sizeof(*stale));
        }
        (void)now_ms;
        eprintf("audiod: native client pid %u on stream %u\n", connection.client_pid, (unsigned int)port);
    }
}

/* One immutable census per native query; control-only connections do not
 * allocate entries in g_streams. Peaks hold for 100 ms independently of reads. */
static void audiod_handle_snapshot(struct audiod_native_client* client,
    const unsigned char* data, size_t length)
{
    struct savanxp_audiod_snapshot_query query;
    struct savanxp_audiod_snapshot snapshot;
    struct savanxp_audiod_record record;
    size_t i;
    if (length != sizeof(query)) return;
    memcpy(&query, data, sizeof(query));
    if (query.version != SAVANXP_AUDIOD_CONTROL_VERSION) return;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
    snapshot.version = SAVANXP_AUDIOD_CONTROL_VERSION;
    snapshot.kind = SAVANXP_AUDIOD_SNAPSHOT_REPLY;
    snapshot.request_id = query.request_id;
    snapshot.peak_left = g_peak_left; snapshot.peak_right = g_peak_right;
    for (i = 0; i < AUDIOD_MAX_STREAMS; ++i) {
        const struct audiod_stream* stream = &g_streams[i];
        struct savanxp_audiod_stream_state* state;
        if (!stream->used) continue;
        state = &snapshot.streams[snapshot.count++];
        state->id = stream->id; state->port = stream->port;
        state->volume = stream->volume; state->muted = stream->muted;
        state->peak_left = stream->peak_left; state->peak_right = stream->peak_right;
        audiod_copy_name(state->name, stream->name);
    }
    memset(&record, 0, sizeof(record));
    record.magic = SAVANXP_AUDIOD_RECORD_MAGIC;
    record.length = sizeof(snapshot);
    memcpy(record.payload, &snapshot, sizeof(snapshot));
    (void)sxn_write(client->write_handle, &record, sizeof(record));
}

static void audiod_handle_stream_control(const unsigned char* data, size_t length)
{
    struct savanxp_audiod_stream_control control;
    struct audiod_stream* stream;
    if (length != sizeof(control)) return;
    memcpy(&control, data, sizeof(control));
    if (control.version != SAVANXP_AUDIOD_CONTROL_VERSION ||
        (control.kind != SAVANXP_AUDIOD_STREAM_VOLUME && control.kind != SAVANXP_AUDIOD_STREAM_MUTED) ||
        control.value > (control.kind == SAVANXP_AUDIOD_STREAM_VOLUME ? 100 : 1)) return;
    stream = audiod_find_stream(control.port);
    if (!stream || stream->id != control.id) return;
    if (control.kind == SAVANXP_AUDIOD_STREAM_VOLUME) stream->volume = control.value;
    else stream->muted = control.value;
    stream->peak_left = stream->peak_right = 0;
}

static void audiod_handle_native_datagram(
    struct audiod_native_client* client,
    const unsigned char* data,
    size_t length,
    uint16_t port,
    uint64_t now_ms)
{
    uint32_t magic = 0;
    uint16_t kind = 0;

    if (length >= sizeof(magic))
    {
        memcpy(&magic, data, sizeof(magic));
    }
    if (magic != SAVANXP_AUDIOD_CONTROL_MAGIC)
    {
        audiod_handle_audio(data, length, port, now_ms);
        return;
    }
    if (length >= sizeof(magic) + 2u + 2u)
    {
        memcpy(&kind, data + sizeof(magic) + 2u, sizeof(kind));
    }
    if (kind == SAVANXP_AUDIOD_DECLARE)
    {
        audiod_handle_declare(data, length, port, now_ms);
    }
    else if (kind == SAVANXP_AUDIOD_SET_VOLUME)
    {
        audiod_handle_set_volume(data, length, now_ms);
    }
    else if (kind == SAVANXP_AUDIOD_LIST)
    {
        audiod_handle_list(audiod_send_native_reply, client);
    }
    else if (kind == SAVANXP_AUDIOD_SNAPSHOT)
    {
        audiod_handle_snapshot(client, data, length);
    }
    else if (kind == SAVANXP_AUDIOD_STREAM_VOLUME || kind == SAVANXP_AUDIOD_STREAM_MUTED)
    {
        audiod_handle_stream_control(data, length);
    }
    else
    {
        ++g_mismatch_packets;
    }
}

/* Reads every whole record waiting on every native connection. End of
 * stream, an error, or a record that is not one closes the connection: the
 * stream itself expires with its silence, like a UDP one. */
static void audiod_drain_native(uint64_t now_ms)
{
    size_t slot;

    for (slot = 0; slot < SAVANXP_AUDIOD_NATIVE_SLOTS; ++slot)
    {
        struct audiod_native_client* client = &g_native[slot];
        uint16_t port = (uint16_t)(SAVANXP_AUDIOD_NATIVE_FIRST_PORT + slot);

        while (client->used)
        {
            struct savanxp_audiod_record record;
            long got = sxn_read(client->read_handle, &record, sizeof(record));

            if (got == -SAVANXP_EAGAIN)
            {
                break;
            }
            if (got != (long)sizeof(record) || record.magic != SAVANXP_AUDIOD_RECORD_MAGIC ||
                record.length > SAVANXP_AUDIOD_RECORD_PAYLOAD)
            {
                if (got > 0)
                {
                    ++g_mismatch_packets;
                }
                eprintf("audiod: native stream %u closed\n", (unsigned int)port);
                audiod_close_native(client);
                break;
            }
            audiod_handle_native_datagram(client, record.payload, record.length, port, now_ms);
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

static void audiod_peak(uint16_t* peak, int32_t sample)
{
    uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
    if (magnitude > *peak) *peak = (uint16_t)magnitude;
}

static size_t audiod_render(int16_t* out, size_t frame_count, uint64_t now_ms)
{
    size_t frame, index;
    if (now_ms - g_peak_ms >= 100) {
        g_peak_left = g_peak_right = 0; g_peak_ms = now_ms;
    }
    for (index = 0; index < AUDIOD_MAX_STREAMS; ++index) {
        struct audiod_stream* stream = &g_streams[index];
        if (now_ms - stream->peak_ms >= 100) {
            stream->peak_left = stream->peak_right = 0; stream->peak_ms = now_ms;
        }
    }
    for (frame = 0; frame < frame_count; ++frame) {
        int32_t left = 0, right = 0;
        for (index = 0; index < AUDIOD_MAX_STREAMS; ++index) {
            struct audiod_stream* stream = &g_streams[index];
            int32_t sample_left = 0, sample_right = 0;
            if (!stream->used) continue;
            if (stream->fifo_frames == 0) {
                /* Conceal briefly from the raw last frame, under current gain. */
                ++stream->underruns;
                if (stream->has_last && now_ms - stream->last_audio_ms <= AUDIOD_PLC_MS) {
                    sample_left = stream->last_left; sample_right = stream->last_right;
                }
            } else {
                sample_left = stream->fifo[stream->fifo_head * 2u];
                sample_right = stream->fifo[stream->fifo_head * 2u + 1u];
                stream->last_left = (int16_t)sample_left; stream->last_right = (int16_t)sample_right;
                stream->has_last = 1;
                stream->fifo_head = (stream->fifo_head + 1u) % AUDIOD_FIFO_FRAMES;
                --stream->fifo_frames;
            }
            sample_left = stream->muted ? 0 : sample_left * stream->volume / 100;
            sample_right = stream->muted ? 0 : sample_right * stream->volume / 100;
            audiod_peak(&stream->peak_left, sample_left); audiod_peak(&stream->peak_right, sample_right);
            left += sample_left; right += sample_right;
        }
        out[frame * 2u] = (int16_t)audiod_soft_knee(left);
        out[frame * 2u + 1u] = (int16_t)audiod_soft_knee(right);
        audiod_peak(&g_peak_left, out[frame * 2u]); audiod_peak(&g_peak_right, out[frame * 2u + 1u]);
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
    /* Nonblocking: drain available packets and pace rendering by the clock. */
    if (savanxp_fcntl((int)fd, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0)
    {
        savanxp_close((int)fd);
        return -SAVANXP_EIO;
    }
    return (int)fd;
}

/* UDP is optional. Verify its loopback before advertising that transport. */
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

        while (sock_fd >= 0 && (got = savanxp_recvfrom(
                    sock_fd, datagram, sizeof(datagram), &source, 0)) > 0)
        {
            audiod_handle_datagram(sock_fd, datagram, (size_t)got, &source, now_ms);
        }
        audiod_accept_native(now_ms);
        audiod_drain_native(now_ms);
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
        (void)audiod_render(mixed, frames, now_ms);
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

    {
        /* PLC: el render no puede inyectar ceros si un stream habla de
         * a momentos; ante jitter repite su ultimo frame (post fondo de
         * escala, pre volumen maestro). */
        struct audiod_stream* ai = &g_streams[0];
        uint64_t plc_now = uptime_ms();
        int16_t out0[2] = {0,0};
        int16_t out1[2] = {0,0};
        int16_t out2[2] = {0,0};
        memset(g_streams, 0, sizeof(g_streams));
        memset(ai, 0, sizeof(*ai));
        ai->used = 1;
        ai->volume = 100;
        ai->fifo_head = 0;
        ai->fifo_frames = 1;
        ai->fifo[0] = 8000;
        ai->fifo[1] = -8000;
        ai->last_audio_ms = plc_now;
        (void)audiod_render(out0, 1, plc_now);
        ai->fifo_frames = 0;
        (void)audiod_render(out1, 1, plc_now);
        if (out0[0] != 8000 || out0[1] != -8000 || out1[0] != 8000 || out1[1] != -8000)
        {
            eprintf("AUDIOD SELFTEST FAIL PLC (%d,%d -> %d,%d)\n",
                    out0[0], out0[1], out1[0], out1[1]);
            goto fail;
        }
        /* Y el PLC tiene que ACABARSE: pasado el margen de jitter, un stream
         * callado sale en silencio en vez de repetir su ultimo frame. Era el
         * residuo que seguia sonando despues de cerrar una app. */
        (void)audiod_render(out2, 1, plc_now + AUDIOD_PLC_MS + 50u);
        if (out2[0] != 0 || out2[1] != 0)
        {
            eprintf("AUDIOD SELFTEST FAIL PLC largo (%d,%d)\n", out2[0], out2[1]);
            goto fail;
        }
        memset(g_streams, 0, sizeof(g_streams));
    }

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
                audiod_handle_datagram(daemon_fd, datagram, (size_t)got, &source, uptime_ms());
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
        audiod_handle_datagram(daemon_fd, datagram, (size_t)got, &source, now_ms);
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
    (void)audiod_render(mixed, AUDIOD_SELFTEST_FRAMES, uptime_ms());
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
    /* Nombres, volumen por app y censo: declarar ambos, fijar A a 50 por su
     * puerto (aprendido del censo) y comprobar la mezcla escalada. */
    {
        struct savanxp_audiod_declare declare_a;
        struct savanxp_audiod_declare declare_b;
        struct savanxp_sockaddr_in control;
        struct sx_audio_server_entry census[8];
        long census_count;
        uint16_t port_a = 0;
        size_t i;
        size_t sent;

        memset(&declare_a, 0, sizeof(declare_a));
        declare_a.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        declare_a.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        declare_a.kind = SAVANXP_AUDIOD_DECLARE;
        memcpy(declare_a.name, "test-a", 7);
        memset(&declare_b, 0, sizeof(declare_b));
        declare_b.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        declare_b.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        declare_b.kind = SAVANXP_AUDIOD_DECLARE;
        memcpy(declare_b.name, "test-b", 7);
        control.ipv4 = SAVANXP_AUDIOD_HOST_IPV4;
        control.port = SAVANXP_AUDIOD_PORT;
        control.reserved0 = 0;
        if (savanxp_sendto((int)sock_a, &declare_a, sizeof(declare_a), &control) !=
                (long)sizeof(declare_a) ||
            savanxp_sendto((int)sock_b, &declare_b, sizeof(declare_b), &control) !=
                (long)sizeof(declare_b))
        {
            eprintf("AUDIOD SELFTEST FAIL no se pudo declarar\n");
            goto fail;
        }
        /* Drenar para que el demonio procese los DECLARE antes del censo:
         * en servicio lo hace el loop, aca hay que bombearlo a mano. */
        for (;;)
        {
            long got = savanxp_recvfrom(
                (int)daemon_fd, datagram, sizeof(datagram), &source, 0);
            if (got <= 0)
            {
                break;
            }
            audiod_handle_datagram(daemon_fd, datagram, (size_t)got, &source, uptime_ms());
        }
        /* Censo a mano (no vale sx_audio_server_list: nadie bombearia al
         * demonio mientras espera las respuestas). */
        {
            struct savanxp_audiod_list_query query;
            long collected = 0;
            int rounds = 0;
            memset(&query, 0, sizeof(query));
            query.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
            query.version = SAVANXP_AUDIOD_CONTROL_VERSION;
            query.kind = SAVANXP_AUDIOD_LIST;
            if (savanxp_sendto((int)sock_a, &query, sizeof(query), &control) !=
                (long)sizeof(query))
            {
                eprintf("AUDIOD SELFTEST FAIL no se pudo pedir censo\n");
                goto fail;
            }
            census_count = -1;
            while (rounds < 20 && collected < 2)
            {
                long got = savanxp_recvfrom(
                    (int)daemon_fd, datagram, sizeof(datagram), &source, 0);
                uint64_t now_ms = uptime_ms();
                long answer;
                if (got > 0)
                {
                    audiod_handle_datagram(
                        daemon_fd, datagram, (size_t)got, &source, now_ms);
                }
                answer = savanxp_recvfrom(
                    (int)sock_a, datagram, sizeof(datagram), &source, 20);
                if (answer == (long)sizeof(struct savanxp_audiod_list_reply))
                {
                    struct savanxp_audiod_list_reply reply;
                    memcpy(&reply, datagram, sizeof(reply));
                    if (reply.magic == SAVANXP_AUDIOD_CONTROL_MAGIC &&
                        reply.version == SAVANXP_AUDIOD_CONTROL_VERSION &&
                        reply.kind == SAVANXP_AUDIOD_REPLY && reply.count == 2 &&
                        reply.index < 2u)
                    {
                        census[reply.index].port = reply.port;
                        census[reply.index].volume = reply.volume;
                        memcpy(census[reply.index].name, reply.name,
                               sizeof(census[reply.index].name));
                        census[reply.index].name[sizeof(census[reply.index].name) - 1u] = '\0';
                        ++collected;
                    }
                }
                ++rounds;
            }
            if (collected == 2)
            {
                census_count = 2;
            }
        }
        if (census_count != 2)
        {
            eprintf("AUDIOD SELFTEST FAIL censo=%d\n", (int)census_count);
            goto fail;
        }
        for (i = 0; i < 2u; ++i)
        {
            if (strcmp(census[i].name, "test-a") == 0)
            {
                port_a = census[i].port;
            }
            if (census[i].volume != 100)
            {
                eprintf("AUDIOD SELFTEST FAIL volumen inicial %u\n",
                        (unsigned int)census[i].volume);
                goto fail;
            }
        }
        if (port_a == 0)
        {
            eprintf("AUDIOD SELFTEST FAIL test-a sin puerto\n");
            goto fail;
        }
        if (sx_audio_server_set_volume((int)sock_a, port_a, 50) < 0)
        {
            eprintf("AUDIOD SELFTEST FAIL no se pudo fijar volumen\n");
            goto fail;
        }
        for (sent = 0; sent < 480u;)
        {
            size_t chunk = 480u - sent;
            long got;
            if (chunk > SAVANXP_AUDIOD_MAX_FRAMES)
            {
                chunk = SAVANXP_AUDIOD_MAX_FRAMES;
            }
            if (audiod_selftest_send_one((int)sock_a, 0, &sequence_a, 4800u + sent, chunk) < 0 ||
                audiod_selftest_send_one((int)sock_b, 1, &sequence_b, 4800u + sent, chunk) < 0)
            {
                eprintf("AUDIOD SELFTEST FAIL no se pudo reenviar\n");
                goto fail;
            }
            sent += chunk;
            while ((got = savanxp_recvfrom(
                        (int)daemon_fd, datagram, sizeof(datagram), &source, 0)) > 0)
            {
                audiod_handle_datagram(daemon_fd, datagram, (size_t)got, &source, uptime_ms());
            }
        }
        (void)audiod_render(mixed, 480u, uptime_ms());
        ok = 1;
        for (frame = 0; frame < 480u; ++frame)
        {
            int16_t a = (int16_t)(audiod_selftest_tone(0, 4800u + frame) * 50 / 100);
            int16_t expect = (int16_t)(a + audiod_selftest_tone(1, 4800u + frame));
            if (mixed[frame * 2u] != expect || mixed[frame * 2u + 1u] != expect)
            {
                ok = 0;
                break;
            }
        }
        if (!ok)
        {
            eprintf("AUDIOD SELFTEST FAIL mezcla con volumen inexacta en frame %u\n",
                    (unsigned int)frame);
            goto fail;
        }
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
    /* Assert actual rendered samples, not only control metadata: muting A
     * leaves B intact; mute also covers PLC and restoring gain preserves it.
     * Reusing a port must reject the previous stream's delayed command. */
    {
        struct audiod_stream* a;
        struct audiod_stream* b;
        struct savanxp_audiod_stream_control control;
        int16_t rendered[2];
        int16_t frame_a[2] = {12000, -12000};
        int16_t frame_b[2] = {4000, -4000};
        uint32_t old_id;
        memset(g_streams,0,sizeof(g_streams));
        a = audiod_register_stream(50000,g_device_rate_hz,1000);
        b = audiod_register_stream(50001,g_device_rate_hz,1000);
        if (!a || !b) goto fail;
        memset(&control,0,sizeof(control));
        control.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        control.version = SAVANXP_AUDIOD_CONTROL_VERSION;
        control.kind = SAVANXP_AUDIOD_STREAM_VOLUME;
        control.id = a->id; control.port = a->port; control.value = 50;
        audiod_handle_stream_control((const unsigned char*)&control,sizeof(control));
        (void)audiod_push_frames(a,frame_a,1); (void)audiod_push_frames(b,frame_b,1);
        (void)audiod_render(rendered,1,1000);
        if (rendered[0] != 10000 || rendered[1] != -10000 || a->peak_left != 6000 || b->peak_left != 4000) {
            eprintf("AUDIOD SELFTEST FAIL stream gain/peak samples\n"); goto fail;
        }
        control.kind = SAVANXP_AUDIOD_STREAM_MUTED; control.value = 1;
        audiod_handle_stream_control((const unsigned char*)&control,sizeof(control));
        (void)audiod_push_frames(a,frame_a,1); (void)audiod_push_frames(b,frame_b,1);
        (void)audiod_render(rendered,1,1000);
        if (rendered[0] != 4000 || rendered[1] != -4000 || a->peak_left != 0) {
            eprintf("AUDIOD SELFTEST FAIL per-stream mute affects another client\n"); goto fail;
        }
        a->last_audio_ms = b->last_audio_ms = 1000;
        (void)audiod_render(rendered,1,1010);
        if (rendered[0] != 4000 || rendered[1] != -4000) {
            eprintf("AUDIOD SELFTEST FAIL mute ignored by PLC\n"); goto fail;
        }
        control.value = 0;
        audiod_handle_stream_control((const unsigned char*)&control,sizeof(control));
        (void)audiod_render(rendered,1,1020);
        if (rendered[0] != 10000 || a->volume != 50 || a->muted) {
            eprintf("AUDIOD SELFTEST FAIL unmute did not preserve gain\n"); goto fail;
        }
        old_id = a->id;
        memset(a,0,sizeof(*a));
        a = audiod_register_stream(50000,g_device_rate_hz,1100);
        control.id = old_id; control.value = 1;
        audiod_handle_stream_control((const unsigned char*)&control,sizeof(control));
        if (!a || a->muted || a->id == old_id) {
            eprintf("AUDIOD SELFTEST FAIL stale stream control accepted\n"); goto fail;
        }
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
        eprintf("usage: audiod [--selftest]\n");
        return 1;
    }

    /* Reserve the native name first, before waiting for the PCM writer. */
    g_native_port = sxn_port_create(SAVANXP_AUDIOD_PORT_NAME);
    if (g_native_port == -SAVANXP_EEXIST) {
        eprintf("audiod: service already running\n");
        return 0;
    }
    if (g_native_port < 0) {
        eprintf("audiod: cannot create native port (%s)\n", result_error_string(g_native_port));
        return 1;
    }
    sock_fd = audiod_open_socket();
    if (sock_fd < 0 || audiod_check_loopback((int)sock_fd) < 0) {
        if (sock_fd >= 0) savanxp_close((int)sock_fd);
        sock_fd = -1;
        eprintf("audiod: UDP unavailable; serving native clients\n");
    }

    audio_fd = (long)audio_open();
    if (audio_fd < 0)
    {
        eprintf("audiod: /dev/audio0 unavailable (%s)\n", result_error_string(audio_fd));
        savanxp_close((int)sock_fd);
        return 1;
    }
    if (audio_get_info((int)audio_fd, &info) < 0 || info.sample_rate_hz == 0 ||
        info.channels != 2 || info.bits_per_sample != 16 || info.frame_bytes != 4)
    {
        eprintf("audiod: unexpected audio format\n");
        savanxp_close((int)sock_fd);
        savanxp_close((int)audio_fd);
        return 1;
    }
    g_device_rate_hz = info.sample_rate_hz;
    if (audiod_acquire_device((int)audio_fd, &info) < 0)
    {
        eprintf("audiod: cannot write /dev/audio0\n");
        savanxp_close((int)sock_fd);
        savanxp_close((int)audio_fd);
        return 1;
    }
    eprintf("audiod: mixing at %u Hz\n", g_device_rate_hz);
    return audiod_serve((int)sock_fd, (int)audio_fd);
}
