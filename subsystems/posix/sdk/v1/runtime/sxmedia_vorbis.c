/* SxCodecs' first codec: Ogg Vorbis, through stb_vorbis.
 *
 * The shape of this backend is the whole reason it is here. A decoder role is
 * packet-oriented by default -- a demuxer reads packets and the engine routes
 * them to `send_packet` -- and stb_vorbis cannot be used that way at all. Its
 * `open_memory` wants the entire file, and its pushdata workflow resynchronises
 * by finding Ogg page boundaries inside the bytes it is handed. A demuxer hands
 * out packets with the Ogg framing already stripped, so the bytes stb_vorbis
 * needs and the bytes a decoder role is given are not the same bytes, and no
 * adapter bridges them.
 *
 * So this provider fills `open_whole` instead of `send_packet` and reads the
 * source itself. The engine routes it no packets, queues none, and calls only
 * `open_whole`, `receive_frame` and `seek_stream`. That costs a second read of
 * the file -- once to enumerate the streams and once to decode them -- which is
 * why the header says so out loud and why this is a fallback rather than the
 * first thing asked.
 *
 * It is asked before FFmpeg anyway, at priority 100 against FFmpeg's 0, because
 * a codec the OS owns should win over one a port happens to ship. If it cannot
 * read a file, `open_whole` returns NULL and the stream goes to the next
 * provider, which is a property of the engine's registry and not a courtesy of
 * this file.
 *
 * What crosses the boundary: the codec name from the demuxer, the path from the
 * caller, and interleaved s16 out. Nothing else. No Ogg page crosses, no Vorbis
 * setup block crosses, and no libvorbis type crosses -- `stb_vorbis.h` is not
 * installed, because a program that compiled against it would depend on the
 * implementation, which is the one thing this whole arrangement exists to
 * prevent.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "savanxp/sxmedia.h"

/* One .c library in one vendored file. The stdio entry points are dropped
 * because the SDK's stdio is not what a codec wants to read a file with, and
 * this backend opens the file itself; the integer conversion stays because
 * `get_samples_short_interleaved` is what the sink format is. */
#define STB_VORBIS_IMPLEMENTATION
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

/* Frames per `receive_frame`. Big enough that the per-call overhead of walking
 * the Ogg pages is noise, small enough that the block is a few tens of
 * kilobytes of PCM rather than a second of music. The engine sizes the
 * converter's buffer from the block and the rate ratio, so this number and the
 * sink's rate are what decide whether a block fits. */
#define SX_VORBIS_BLOCK 1024

/* stb_vorbis keeps a pointer into the bytes it was given for its whole life, so
 * the file is not read, decoded and freed -- it is read, and then owned by the
 * decoder until it closes. A whole-file decoder is holding the file, not a
 * window onto it. */
struct VorbisDecoder {
    stb_vorbis* file;
    unsigned char* bytes;
    int16_t* block;
    int sample_rate;
    int channels;
    /* Samples already handed out. This is the position, and it is counted here
     * rather than asked for, because `stb_vorbis_get_sample_offset` says plainly
     * that it does not work after a seek with the pulldata API -- which is
     * exactly when a media engine most wants to know where it is. Counting
     * decoded frames is exact for a pull API: the position advances by exactly
     * what the call returned. */
    int64_t delivered;
    int finished;
};

/* The whole file, or NULL. Read with a growing buffer because the size is not
 * known in advance and `lseek` to the end would be a second syscall for
 * something `read` already reports. */
static unsigned char* vorbis_read_file(const char* path, int* out_size) {
    unsigned char* bytes;
    size_t capacity = 1u << 16;
    size_t used = 0;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    bytes = (unsigned char*)malloc(capacity);
    if (bytes == NULL) {
        close(fd);
        return NULL;
    }
    for (;;) {
        ssize_t got;
        if (used == capacity) {
            unsigned char* bigger;
            capacity *= 2;
            bigger = (unsigned char*)realloc(bytes, capacity);
            if (bigger == NULL) {
                free(bytes);
                close(fd);
                return NULL;
            }
            bytes = bigger;
        }
        got = read(fd, bytes + used, capacity - used);
        if (got < 0) {
            free(bytes);
            close(fd);
            return NULL;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }
    close(fd);
    if (used == 0) {
        free(bytes);
        return NULL;
    }
    /* stb_vorbis takes an int length. A file that does not fit in one is not a
     * file this decoder can hold in memory, and saying so is better than
     * truncating a track in half with no error anywhere. */
    if (used > 0x7fffffffu) {
        free(bytes);
        return NULL;
    }
    *out_size = (int)used;
    return bytes;
}

/* -- decoder role ---------------------------------------------------------- */

/* By codec name and nothing else, and that is the honest answer: a whole-file
 * decoder cannot know whether it can read a file until it has tried. Which is
 * why the engine asks every claimant in turn rather than stopping at the first
 * one -- a claim is an intention, not a guarantee.
 *
 * `sx_media_has_codec` calls this with `SX_MEDIA_KIND_ANY` and no stream to
 * inspect, so the kind check has to let ANY through. */
static int vorbis_claim(void* user, const struct sx_media_stream_desc* stream) {
    (void)user;
    if (stream == NULL) {
        return 0;
    }
    if (stream->kind != SX_MEDIA_KIND_ANY && stream->kind != SX_MEDIA_KIND_AUDIO) {
        return 0;
    }
    return strcmp(stream->codec, "vorbis") == 0;
}

static void* vorbis_open(void* user, const struct sx_media_source* source,
                         const struct sx_media_stream_desc* stream) {
    struct VorbisDecoder* decoder;
    stb_vorbis_info info;
    stb_vorbis* file;
    unsigned char* bytes;
    int size = 0;
    int error = 0;
    (void)user;
    (void)stream;

    /* It has to be a path this backend can open. A source handed over as a bare
     * descriptor is not something it can hand to `open_memory`, and declining
     * is the right answer: the engine moves on, and a source given as an fd is
     * not the shape this codec was built for. */
    if (source == NULL || source->path == NULL) {
        return NULL;
    }
    bytes = vorbis_read_file(source->path, &size);
    if (bytes == NULL) {
        return NULL;
    }
    file = stb_vorbis_open_memory(bytes, size, &error, NULL);
    if (file == NULL) {
        /* Not an Ogg Vorbis stream, or not one this build can read. Either way
         * this is not the provider for it, and the next one gets the stream. */
        free(bytes);
        return NULL;
    }
    info = stb_vorbis_get_info(file);
    if (info.channels <= 0 || info.sample_rate <= 0) {
        stb_vorbis_close(file);
        free(bytes);
        return NULL;
    }
    decoder = (struct VorbisDecoder*)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        stb_vorbis_close(file);
        free(bytes);
        return NULL;
    }
    decoder->block = (int16_t*)malloc(sizeof(int16_t) * (size_t)(SX_VORBIS_BLOCK * info.channels));
    if (decoder->block == NULL) {
        free(decoder);
        stb_vorbis_close(file);
        free(bytes);
        return NULL;
    }
    decoder->file = file;
    decoder->bytes = bytes;
    decoder->sample_rate = (int)info.sample_rate;
    decoder->channels = info.channels;
    return decoder;
}

static void vorbis_close(void* handle) {
    struct VorbisDecoder* decoder = (struct VorbisDecoder*)handle;
    if (decoder == NULL) {
        return;
    }
    stb_vorbis_close(decoder->file);
    free(decoder->block);
    free(decoder->bytes);
    free(decoder);
}

static void vorbis_flush(void* handle) {
    struct VorbisDecoder* decoder = (struct VorbisDecoder*)handle;
    if (decoder == NULL) {
        return;
    }
    if (!stb_vorbis_seek(decoder->file, 0)) {
        decoder->finished = 1;
        return;
    }
    decoder->delivered = 0;
    decoder->finished = 0;
}

static int vorbis_receive(void* handle, struct sx_media_frame* out) {
    struct VorbisDecoder* decoder = (struct VorbisDecoder*)handle;
    int frames;

    if (decoder->finished) {
        return -1;
    }
    /* The interleaved entry point, because the sink format is interleaved s16
     * and a planar block would only be interleaved afterwards, in a second pass,
     * by a converter that does not exist here. `get_samples_*` takes a count of
     * SHORTS and returns FRAMES -- frames per channel -- which is the same unit
     * `count` is in, so there is no factor of `channels` to get wrong between
     * the call and the frame it fills. */
    frames = stb_vorbis_get_samples_short_interleaved(decoder->file, decoder->channels,
                                                     decoder->block,
                                                     SX_VORBIS_BLOCK * decoder->channels);
    if (frames <= 0) {
        decoder->finished = 1;
        return -1;
    }
    out->time_us = (decoder->delivered * 1000000LL) / decoder->sample_rate;
    out->width = 0;
    out->height = 0;
    out->sample_rate = decoder->sample_rate;
    out->channels = decoder->channels;
    out->format = 1;  /* interleaved s16 */
    out->count = frames;
    out->data = decoder->block;
    out->release = NULL;  /* the decoder's own buffer, reused by the next block */
    decoder->delivered += frames;
    return 1;
}

static int vorbis_seek(void* handle, int64_t target_us) {
    struct VorbisDecoder* decoder = (struct VorbisDecoder*)handle;
    int64_t sample;

    if (target_us < 0) {
        target_us = 0;
    }
    /* `stb_vorbis_seek` and not `seek_frame`: the frame variant lines the next
     * `get_frame_*` up with the sample, and this decoder pulls blocks through
     * `get_samples_*`, which is what `seek` is the one that promises to start on
     * the requested sample. */
    sample = (target_us * decoder->sample_rate) / 1000000;
    if (!stb_vorbis_seek(decoder->file, (unsigned int)sample)) {
        return 0;
    }
    decoder->delivered = sample;
    decoder->finished = 0;
    return 1;
}

/* -- converter role --------------------------------------------------------
 *
 * A frame's data belongs to the library that made it, so the converter that
 * reads it has to be this one. That is not a preference: swresample was once
 * handed a block of interleaved s16 from this file, read the first bytes of it as
 * a channel layout, and produced silence.
 *
 * What it can honestly do is hand the samples on unchanged, and only when the sink
 * already wants what the stream already produces: interleaved s16, the same rate,
 * the same channel count. Copying is the whole job there, and it is exact.
 *
 * What it cannot do yet is change the rate, and it refuses rather than
 * approximating. A linear resampler is sixty lines and would make this codec
 * "work" on a device at a different rate, and a music track resampled by
 * something that is not a resampler is a music track with an artefact in it --
 * which is exactly the sound the first two consumers of this codec are. So
 * `resampler_open` returns NULL for a rate mismatch and the stream produces no
 * audio, and the notice names `vorbis` rather than blaming the hardware.
 *
 * That is a real gap, not a placeholder dressed up. It is the next piece of
 * SxCodecs and it wants a proper windowed-sinc resampler, which is a piece of
 * work in its own right. */
static void* vorbis_resampler_open(void* user, const struct sx_media_frame* prototype,
                                   const struct sx_media_audio_format* dst) {
    (void)user;
    if (prototype == NULL || dst == NULL || prototype->format != 1) {
        return NULL;
    }
    if (dst->sample_rate != prototype->sample_rate || dst->channels != prototype->channels) {
        return NULL;  /* see above: refuse, do not approximate */
    }
    /* The frame's own pointer, cast to something the close side ignores. It is
     * never dereferenced, because nothing is converted. */
    return prototype->data;
}

static void vorbis_resampler_close(void* opaque) { (void)opaque; }

static int vorbis_resample(void* opaque, const struct sx_media_frame* frame, int16_t* out,
                           int out_capacity) {
    int frames;
    (void)opaque;
    frames = out_capacity / (frame->channels > 0 ? frame->channels : 1);
    if (frames > frame->count) {
        frames = frame->count;
    }
    memcpy(out, frame->data, (size_t)frames * (size_t)frame->channels * sizeof(int16_t));
    return frames;
}

/* -- the vtable ------------------------------------------------------------
 *
 * Source role null: this is a codec library, not a container library. It never
 * opens a container, so it cannot claim one, and the notice for an unknown
 * format comes from the providers that can demux.
 *
 * `send_packet` null and `open_whole` set: the whole-source variant, above.
 *
 * Both converter roles null: this backend does not resample and does not scale.
 * A converter is built for one input format, and this one's input format is
 * whatever a Vorbis stream happens to declare, so claiming the role would mean
 * claiming to handle every rate there is. */
static const struct sx_media_backend_ops kVorbisOps = {
    /* source role: none */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* decoder role */
    vorbis_claim, NULL, vorbis_close, vorbis_flush, NULL, vorbis_receive,
    vorbis_open, vorbis_seek,
    /* video converters: none */
    NULL, NULL, NULL,
    /* audio converters: the identity, and only the identity */
    vorbis_resampler_open, vorbis_resampler_close, vorbis_resample, NULL,
};

static struct sx_media_backend g_vorbis_backend = {
    "vorbis", SX_MEDIA_BACKEND_ABI, 100, &kVorbisOps, NULL,
};

int sxmedia_vorbis_register(void) {
    return sx_media_register_backend(&g_vorbis_backend);
}
