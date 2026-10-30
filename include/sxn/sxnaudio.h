#pragma once

/* sxnaudio: SUN sound (docs/userland/SUN.md), a stream of PCM to the speaker.
 *
 * A SUN program has no sockets, so it cannot reach audiod over UDP the way a
 * POSIX program does. It connects to the daemon's native port instead
 * (audio/audiod_protocol.h) and sends the same datagrams wrapped in
 * fixed-size records. When there is no daemon -- no network, or a session
 * that never started one -- the stream writes to /dev/audio0 directly, which
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
    uint64_t frames_sent; /* delivered, or skipped when the program fell behind */
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
static inline long sxnaudio_send_record(struct sxnaudio_stream* stream, const void* datagram, uint32_t length) {
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
    written = sxn_write(stream->to_daemon, &record, sizeof(record));
    if (written == (long)sizeof(record)) {
        return 0;
    }
    return written < 0 ? written : -SAVANXP_EIO;
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

    /* Opening the device never contends; only writing claims it. */
    stream->device = sxn_open_device("audio0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
    if (stream->device < 0) {
        result = stream->device;
        stream->device = -1;
        return result;
    }
    result = sxn_device_control(stream->device, AUDIO_IOC_GET_INFO, (unsigned long)(uintptr_t)&info);
    if (result < 0 || info.sample_rate_hz == 0 || info.channels != 2 || info.bits_per_sample != 16) {
        (void)sxn_close(stream->device);
        stream->device = -1;
        return result < 0 ? result : -SAVANXP_EINVAL;
    }
    stream->rate_hz = info.sample_rate_hz;

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
        long written = sxn_write(stream->device, frames, (unsigned long)count * 4ul);
        if (written < 0) {
            return written;
        }
        stream->frames_sent += (uint64_t)written / 4u;
        return written / 4;
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
        (void)sxn_close(stream->device);
    }
    stream->to_daemon = -1;
    stream->from_daemon = -1;
    stream->device = -1;
    stream->mode = SXNAUDIO_MODE_CLOSED;
}
