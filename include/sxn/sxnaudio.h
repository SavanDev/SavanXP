#pragma once

/* sxnaudio: SUN sound (docs/userland/SUN.md), PCM playback and mixer control.
 *
 * A SUN program connects to audiod's native port
 * (audio/audiod_protocol.h) and sends the same datagrams wrapped in
 * fixed-size records. When a session has no daemon, the stream writes to
 * /dev/audio0 directly, which
 * works as long as nobody else is playing: the device has one writer.
 *
 * The format is the device's: signed 16-bit stereo at stream->rate_hz. The
 * program mixes its own voices and hands over finished frames, paced by
 * sxnaudio_frames_due so that it stays a little ahead of the speaker and
 * never runs away from it.
 *
 * Header-only like sxn.h and sxnwm.h. Returns follow the kernel convention:
 * 0 or a count on success, negative -errno on failure.
 */

#include <stdint.h>

#include "abi/savanxp_abi.h"
#include "audio/audiod_protocol.h"
#include "sxn/sxn.h"

enum sxnaudio_mode {
    SXNAUDIO_MODE_CLOSED = 0,
    SXNAUDIO_MODE_DAEMON = 1, /* mixed with everyone else by audiod */
    SXNAUDIO_MODE_DIRECT = 2, /* the only writer of /dev/audio0 */
};

struct sxnaudio_stream {
    int mode; /* enum sxnaudio_mode */
    long device; /* /dev/audio0: its format, and the writes in direct mode */
    long to_daemon; /* request pipe, write end (daemon mode) */
    long from_daemon; /* event pipe, read end (daemon mode) */
    uint32_t rate_hz;
    uint32_t sequence;
    uint64_t started_ns;
    uint64_t frames_sent; /* accepted for playback, or skipped when the program fell behind */
    uint32_t period_bytes, direct_buffered;
    int direct_owned;
    int16_t direct_pcm[2048];
};

static inline void sxnaudio_copy_name(char* out, const char* name) {
    uint32_t index = 0;

    for (index = 0; index < SAVANXP_AUDIOD_NAME_BYTES; index += 1) {
        out[index] = 0;
    }
    for (index = 0; name != 0 && name[index] != '\0' && index + 1u < SAVANXP_AUDIOD_NAME_BYTES; index += 1) {
        out[index] = name[index];
    }
}

/* Sends one datagram wrapped in a record. Never blocks: 0, or -EAGAIN when
 * the daemon has not drained eight records yet. */
static inline long sxnaudio_send_record_to(long handle, const void* datagram, uint32_t length) {
    struct savanxp_audiod_record record;
    const unsigned char* in = (const unsigned char*)datagram;
    uint32_t index = 0;
    long written = 0;

    if (length > SAVANXP_AUDIOD_RECORD_PAYLOAD) {
        return -SAVANXP_EINVAL;
    }
    record.magic = SAVANXP_AUDIOD_RECORD_MAGIC;
    record.length = length;
    for (index = 0; index < length; index += 1) {
        record.payload[index] = in[index];
    }
    for (; index < SAVANXP_AUDIOD_RECORD_PAYLOAD; index += 1) {
        record.payload[index] = 0;
    }
    written = sxn_write(handle, &record, sizeof(record));
    if (written == (long)sizeof(record)) {
        return 0;
    }
    return written < 0 ? written : -SAVANXP_EIO;
}

static inline long sxnaudio_send_record(struct sxnaudio_stream* stream, const void* datagram, uint32_t length) {
    return sxnaudio_send_record_to(stream->to_daemon,datagram,length);
}

/* Names the stream in the daemon's census (the volume popup, `volume list`).
 * Only meaningful in daemon mode; a no-op otherwise. */
static inline long sxnaudio_declare(struct sxnaudio_stream* stream, const char* name) {
    struct savanxp_audiod_declare declare;

    if (stream == 0 || stream->mode != SXNAUDIO_MODE_DAEMON) {
        return 0;
    }
    declare.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
    declare.version = SAVANXP_AUDIOD_CONTROL_VERSION;
    declare.kind = SAVANXP_AUDIOD_DECLARE;
    sxnaudio_copy_name(declare.name, name);
    return sxnaudio_send_record(stream, &declare, (uint32_t)sizeof(declare));
}

/* Opens a stream named `name`: the daemon if one is listening, the device
 * directly if not. 0, or negative -errno when there is no audio device at
 * all (the program should then simply be silent). */
static inline long sxnaudio_open(struct sxnaudio_stream* stream, const char* name) {
    struct savanxp_audio_info info;
    int32_t handles[2] = {-1, -1};
    long result = 0;

    if (stream == 0) {
        return -SAVANXP_EINVAL;
    }
    stream->mode = SXNAUDIO_MODE_CLOSED;
    stream->device = -1;
    stream->to_daemon = -1;
    stream->from_daemon = -1;
    stream->sequence = 0;
    stream->frames_sent = 0;
    stream->period_bytes = stream->direct_buffered = 0; stream->direct_owned = 0;

    /* Opening the device never contends; only writing claims it. */
    stream->device = sxn_open_device("audio0", SXN_DEVICE_WRITE);
    if (stream->device < 0) {
        result = stream->device;
        stream->device = -1;
        return result;
    }
    result = sxn_device_control(stream->device, AUDIO_IOC_GET_INFO, (unsigned long)(uintptr_t)&info);
    if (result < 0 || info.sample_rate_hz == 0 || info.channels != 2 || info.bits_per_sample != 16 ||
        !info.period_bytes || info.period_bytes > sizeof(stream->direct_pcm) || info.period_bytes%4) {
        (void)sxn_close(stream->device);
        stream->device = -1;
        return result < 0 ? result : -SAVANXP_EINVAL;
    }
    stream->rate_hz = info.sample_rate_hz; stream->period_bytes = info.period_bytes;

    if (sxn_port_connect(SAVANXP_AUDIOD_PORT_NAME, handles) == 0) {
        stream->to_daemon = handles[0];
        stream->from_daemon = handles[1];
        (void)sxn_set_nonblocking(stream->to_daemon, 1);
        (void)sxn_set_nonblocking(stream->from_daemon, 1);
        stream->mode = SXNAUDIO_MODE_DAEMON;
        (void)sxnaudio_declare(stream, name);
    } else {
        stream->mode = SXNAUDIO_MODE_DIRECT;
    }
    stream->started_ns = sxn_clock_ns();
    return 0;
}

/* How many frames to hand over now to stay `lead_ms` ahead of the speaker.
 * A stream that fell behind (the program stalled) is caught up to the clock
 * rather than made to replay the gap; at most a quarter of a second at once. */
static inline uint32_t sxnaudio_frames_due(struct sxnaudio_stream* stream, uint32_t lead_ms) {
    uint64_t elapsed = 0;
    uint64_t played = 0;
    uint64_t target = 0;
    uint64_t due = 0;

    if (stream == 0 || stream->mode == SXNAUDIO_MODE_CLOSED) {
        return 0;
    }
    elapsed = sxn_clock_ns() - stream->started_ns;
    played = elapsed / 1000ull * (uint64_t)stream->rate_hz / 1000000ull;
    if (stream->frames_sent < played) {
        stream->frames_sent = played;
    }
    target = played + (uint64_t)lead_ms * (uint64_t)stream->rate_hz / 1000ull;
    due = target > stream->frames_sent ? target - stream->frames_sent : 0;
    if (due > stream->rate_hz / 4u) {
        due = stream->rate_hz / 4u;
    }
    return (uint32_t)due;
}

/* Hands over up to `count` stereo frames and returns how many were taken. In
 * daemon mode it never blocks: the pipe holds about 40 ms of sound, and once
 * it is full the write ends early -- the rest is simply due again on the next
 * sxnaudio_frames_due. In direct mode the write may block for a period, and
 * fails with -EBUSY when another program owns the device. */
static inline long sxnaudio_write(struct sxnaudio_stream* stream, const int16_t* frames, uint32_t count) {
    uint32_t done = 0;

    if (stream == 0 || frames == 0) {
        return -SAVANXP_EINVAL;
    }
    if (stream->mode == SXNAUDIO_MODE_DIRECT) {
        const uint32_t period = stream->period_bytes/4;
        uint32_t i;
        long written;
        if (!count) return 0;
        // Claim the writer with one silent frame before accepting samples.
        // Coalesce tiny producer writes into device periods so they cannot
        // exhaust a descriptor ring while carrying very little PCM.
        if (!stream->direct_owned) {
            stream->direct_pcm[0] = stream->direct_pcm[1] = 0;
            written = sxn_write(stream->device,stream->direct_pcm,4);
            if (written != 4) return written < 0 ? written : -SAVANXP_EIO;
            stream->direct_owned = 1;
        }
        while (done < count) {
            uint32_t take = period-stream->direct_buffered;
            if (take > count-done) take = count-done;
            for (i = 0; i < take*2; ++i) stream->direct_pcm[stream->direct_buffered*2+i] = frames[done*2+i];
            stream->direct_buffered += take; done += take;
            if (stream->direct_buffered == period) {
                written = sxn_write(stream->device,stream->direct_pcm,stream->period_bytes);
                if (written != (long)stream->period_bytes) {
                    stream->frames_sent += done;
                    return done ? (long)done : written < 0 ? written : -SAVANXP_EIO;
                }
                stream->direct_buffered = 0;
            }
        }
        stream->frames_sent += done;
        return done;
    }
    if (stream->mode != SXNAUDIO_MODE_DAEMON) {
        return -SAVANXP_EINVAL;
    }
    while (done < count) {
        unsigned char datagram[SAVANXP_AUDIOD_RECORD_PAYLOAD];
        struct savanxp_audiod_header* header = (struct savanxp_audiod_header*)datagram;
        uint32_t chunk = count - done;
        uint32_t index = 0;
        const unsigned char* in = (const unsigned char*)(frames + (uint64_t)done * 2u);
        long result = 0;

        if (chunk > SAVANXP_AUDIOD_RECORD_MAX_FRAMES) {
            chunk = SAVANXP_AUDIOD_RECORD_MAX_FRAMES;
        }
        header->magic = SAVANXP_AUDIOD_MAGIC;
        header->version = SAVANXP_AUDIOD_VERSION;
        header->channels = 2;
        header->sample_rate_hz = stream->rate_hz;
        header->sequence = stream->sequence;
        for (index = 0; index < chunk * 4u; index += 1) {
            datagram[SAVANXP_AUDIOD_HEADER_BYTES + index] = in[index];
        }
        result = sxnaudio_send_record(stream, datagram, SAVANXP_AUDIOD_HEADER_BYTES + chunk * 4u);
        if (result == -SAVANXP_EAGAIN) {
            break;
        }
        if (result < 0) {
            return done > 0 ? (long)done : result;
        }
        stream->sequence += 1u;
        stream->frames_sent += chunk;
        done += chunk;
    }
    return (long)done;
}

static inline void sxnaudio_close(struct sxnaudio_stream* stream) {
    if (stream == 0) {
        return;
    }
    if (stream->to_daemon >= 0) {
        (void)sxn_close(stream->to_daemon);
    }
    if (stream->from_daemon >= 0) {
        (void)sxn_close(stream->from_daemon);
    }
    if (stream->device >= 0) {
        if (stream->mode == SXNAUDIO_MODE_DIRECT && stream->direct_owned && stream->direct_buffered) {
            uint32_t i;
            for (i = stream->direct_buffered*2; i < stream->period_bytes/2; ++i) stream->direct_pcm[i] = 0;
            (void)sxn_write(stream->device,stream->direct_pcm,stream->period_bytes);
        }
        (void)sxn_close(stream->device);
    }
    stream->to_daemon = -1;
    stream->from_daemon = -1;
    stream->device = -1;
    stream->mode = SXNAUDIO_MODE_CLOSED;
}

/* Control-only connection: never declares or feeds a playback stream. The
 * caller owns retry timing; all record traffic is non-blocking. */
struct sxnaudio_control {
    long request;
    long reply;
};

static inline void sxnaudio_control_close(struct sxnaudio_control* control) {
    if (control->request >= 0) (void)sxn_close(control->request);
    if (control->reply >= 0) (void)sxn_close(control->reply);
    control->request = control->reply = -1;
}

static inline long sxnaudio_control_open(struct sxnaudio_control* control) {
    int32_t handles[2] = {-1, -1};
    long result;
    control->request = control->reply = -1;
    result = sxn_port_connect(SAVANXP_AUDIOD_PORT_NAME, handles);
    if (result < 0) return result;
    control->request = handles[0]; control->reply = handles[1];
    result = sxn_set_nonblocking(control->request, 1);
    if (result >= 0) result = sxn_set_nonblocking(control->reply, 1);
    if (result < 0) sxnaudio_control_close(control);
    return result < 0 ? result : 0;
}

static inline long sxnaudio_control_send(struct sxnaudio_control* control, const void* data, uint32_t length) {
    return sxnaudio_send_record_to(control->request,data,length);
}

static inline long sxnaudio_control_query(struct sxnaudio_control* control, uint32_t request_id) {
    struct savanxp_audiod_snapshot_query query;
    query.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
    query.version = SAVANXP_AUDIOD_CONTROL_VERSION;
    query.kind = SAVANXP_AUDIOD_SNAPSHOT;
    query.request_id = request_id;
    return sxnaudio_control_send(control, &query, sizeof(query));
}

/* Receive one native control reply: 1 snapshot, 2 endpoint acknowledgement,
 * 3 jack policy (receive_ex/full), 4 preferences (receive_full),
 * 0 when nothing is waiting, negative errno for EOF/malformed data. */
static inline long sxnaudio_control_receive_full(struct sxnaudio_control* control,
    struct savanxp_audiod_snapshot* snapshot, struct savanxp_audiod_endpoint_reply* endpoint,
    struct savanxp_audiod_jack_policy_reply* policy, struct savanxp_audiod_preference_reply* preferences) {
    struct savanxp_audiod_record record;
    const struct savanxp_audiod_list_query* header = (const struct savanxp_audiod_list_query*)record.payload;
    unsigned char* out;
    uint32_t i;
    long got = sxn_read(control->reply, &record, sizeof(record));
    if (got == -SAVANXP_EAGAIN) return 0;
    if (got < 0) return got;
    if (got != sizeof(record) || record.magic != SAVANXP_AUDIOD_RECORD_MAGIC ||
        record.length < sizeof(*header) || header->magic != SAVANXP_AUDIOD_CONTROL_MAGIC ||
        header->version != SAVANXP_AUDIOD_CONTROL_VERSION) return -SAVANXP_EIO;
    if (header->kind == SAVANXP_AUDIOD_ENDPOINT_REPLY && record.length == sizeof(*endpoint) && endpoint) {
        out = (unsigned char*)endpoint;
        for (i = 0; i < sizeof(*endpoint); ++i) out[i] = record.payload[i];
        if (endpoint->reserved || (endpoint->direction != SAVANXP_AUDIO_PLAYBACK &&
            endpoint->direction != SAVANXP_AUDIO_CAPTURE)) return -SAVANXP_EIO;
        return 2;
    }
    if (header->kind == SAVANXP_AUDIOD_JACK_POLICY_REPLY && record.length == sizeof(*policy) && policy) {
        out = (unsigned char*)policy;
        for (i = 0; i < sizeof(*policy); ++i) out[i] = record.payload[i];
        if (policy->enabled > 1 || policy->supported > 1 || policy->reserved) return -SAVANXP_EIO;
        return 3;
    }
    if (header->kind == SAVANXP_AUDIOD_PREFERENCE_REPLY && record.length == sizeof(*preferences) && preferences) {
        out = (unsigned char*)preferences;
        for (i = 0; i < sizeof(*preferences); ++i) out[i] = record.payload[i];
        if ((preferences->flags & ~15u) || preferences->result > 0 || preferences->last_error > 0 ||
            preferences->saved_generation > preferences->generation) return -SAVANXP_EIO;
        return 4;
    }
    if (header->kind != SAVANXP_AUDIOD_SNAPSHOT_REPLY || record.length != sizeof(*snapshot) || !snapshot)
        return -SAVANXP_EIO;
    out = (unsigned char*)snapshot;
    for (i = 0; i < sizeof(*snapshot); ++i) out[i] = record.payload[i];
    if (snapshot->count > SAVANXP_AUDIOD_MAX_STREAMS) return -SAVANXP_EIO;
    for (i = 0; i < snapshot->count; ++i) {
        if (!snapshot->streams[i].id || snapshot->streams[i].volume > 100 ||
            snapshot->streams[i].muted > 1) return -SAVANXP_EIO;
        snapshot->streams[i].name[SAVANXP_AUDIOD_NAME_BYTES - 1] = '\0';
    }
    return 1;
}
static inline long sxnaudio_control_receive_ex(struct sxnaudio_control* control,
    struct savanxp_audiod_snapshot* snapshot, struct savanxp_audiod_endpoint_reply* endpoint,
    struct savanxp_audiod_jack_policy_reply* policy) {
    return sxnaudio_control_receive_full(control,snapshot,endpoint,policy,0);
}
static inline long sxnaudio_preferences_request(struct sxnaudio_control* control, uint32_t id, uint16_t kind) {
    struct savanxp_audiod_preference_request request = {SAVANXP_AUDIOD_CONTROL_MAGIC,
        SAVANXP_AUDIOD_CONTROL_VERSION,kind,id,0};
    if (kind != SAVANXP_AUDIOD_PREFERENCE_QUERY && kind != SAVANXP_AUDIOD_PREFERENCE_FLUSH) return -SAVANXP_EINVAL;
    return sxnaudio_control_send(control,&request,sizeof(request));
}
static inline long sxnaudio_preferences_query(struct sxnaudio_control* control, uint32_t id) {
    return sxnaudio_preferences_request(control,id,SAVANXP_AUDIOD_PREFERENCE_QUERY);
}
static inline long sxnaudio_preferences_flush(struct sxnaudio_control* control, uint32_t id) {
    return sxnaudio_preferences_request(control,id,SAVANXP_AUDIOD_PREFERENCE_FLUSH);
}
static inline long sxnaudio_control_receive(struct sxnaudio_control* control,
    struct savanxp_audiod_snapshot* snapshot, struct savanxp_audiod_endpoint_reply* endpoint) {
    return sxnaudio_control_receive_ex(control,snapshot,endpoint,0);
}
static inline long sxnaudio_jack_policy_send(struct sxnaudio_control* control,
    uint32_t request_id, uint16_t kind, uint32_t enabled) {
    struct savanxp_audiod_jack_policy_request request;
    if (enabled > 1) return -SAVANXP_EINVAL;
    request.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
    request.version = SAVANXP_AUDIOD_CONTROL_VERSION;
    request.kind = kind; request.request_id = request_id; request.enabled = enabled;
    return sxnaudio_control_send(control,&request,sizeof(request));
}
static inline long sxnaudio_jack_policy_query(struct sxnaudio_control* control, uint32_t request_id) {
    return sxnaudio_jack_policy_send(control,request_id,SAVANXP_AUDIOD_JACK_POLICY_QUERY,0);
}
static inline long sxnaudio_jack_policy_set(struct sxnaudio_control* control, uint32_t request_id, uint32_t enabled) {
    return sxnaudio_jack_policy_send(control,request_id,SAVANXP_AUDIOD_JACK_POLICY_SET,enabled);
}

static inline long sxnaudio_control_read(struct sxnaudio_control* control,
                                        struct savanxp_audiod_snapshot* snapshot) {
    return sxnaudio_control_receive(control,snapshot,0);
}

/* Read-only hardware inventory never claims PCM. Enumeration ends at ENOENT;
 * each id describes hardware, rather than this process's handles or row order. */
static inline long sxnaudio_endpoint_get(long device, uint32_t index,
                                        struct savanxp_audio_endpoint* endpoint) {
    struct savanxp_audio_endpoint_query query;
    long result;
    query.index = index; query.reserved = 0;
    result = sxn_device_control(device,AUDIO_IOC_GET_ENDPOINT,(unsigned long)(uintptr_t)&query);
    if (result < 0) return result;
    *endpoint = query.endpoint;
    if (!endpoint->id || !endpoint->device_id) return -SAVANXP_EIO;
    endpoint->name[sizeof(endpoint->name)-1] = '\0';
    endpoint->device_name[sizeof(endpoint->device_name)-1] = '\0';
    return 0;
}
static inline long sxnaudio_endpoint_select(struct sxnaudio_control* control,
                                           uint32_t request_id, uint64_t endpoint_id, uint32_t direction) {
    struct savanxp_audiod_endpoint_request request;
    if (!endpoint_id || (direction != SAVANXP_AUDIO_PLAYBACK && direction != SAVANXP_AUDIO_CAPTURE))
        return -SAVANXP_EINVAL;
    request.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
    request.version = SAVANXP_AUDIOD_CONTROL_VERSION;
    request.kind = SAVANXP_AUDIOD_SELECT_ENDPOINT;
    request.request_id = request_id; request.direction = direction; request.endpoint_id = endpoint_id;
    return sxnaudio_control_send(control,&request,sizeof(request));
}

static inline long sxnaudio_control_change(struct sxnaudio_control* control,
    const struct savanxp_audiod_stream_state* stream, uint16_t kind, uint8_t value) {
    struct savanxp_audiod_stream_control change;
    if ((kind != SAVANXP_AUDIOD_STREAM_VOLUME && kind != SAVANXP_AUDIOD_STREAM_MUTED) ||
        value > (kind == SAVANXP_AUDIOD_STREAM_VOLUME ? 100 : 1)) return -SAVANXP_EINVAL;
    change.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
    change.version = SAVANXP_AUDIOD_CONTROL_VERSION;
    change.kind = kind;
    change.id = stream->id;
    change.port = stream->port;
    change.value = value;
    change.reserved = 0;
    return sxnaudio_control_send(control, &change, sizeof(change));
}

/* Reading/controlling master gain never claims the PCM writer. */
static inline long sxnaudio_master_get(long device, uint32_t* volume, uint32_t* muted) {
    long v = sxn_device_control(device, AUDIO_IOC_GET_VOLUME, 0);
    long m = sxn_device_control(device, AUDIO_IOC_GET_MUTED, 0);
    if (v < 0 || m < 0) return v < 0 ? v : m;
    if (v > 100 || m > 1) return -SAVANXP_EIO;
    *volume = (uint32_t)v; *muted = (uint32_t)m;
    return 0;
}
static inline long sxnaudio_master_volume(long device, uint32_t volume) {
    if (volume > 100) return -SAVANXP_EINVAL;
    return sxn_device_control(device, AUDIO_IOC_SET_VOLUME, volume);
}
static inline long sxnaudio_master_muted(long device, uint32_t muted) {
    if (muted > 1) return -SAVANXP_EINVAL;
    return sxn_device_control(device, AUDIO_IOC_SET_MUTED, muted);
}
