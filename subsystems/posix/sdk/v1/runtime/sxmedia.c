/* SxMedia: the media engine and the backend registry.
 *
 * See savanxp/sxmedia.h for the contract. Three properties of this file are
 * worth stating before anything else, because together they are why the
 * registry is testable on a host with no kernel behind it:
 *
 *   - It opens no file. `sx_media_open` hands the source to a provider and the
 *     provider does the I/O, so this half never calls a syscall and a host test
 *     needs no `savanxp_*` stubs at all.
 *   - It reads no samples and no pixels. A frame's `data` is opaque and goes
 *     straight back to the provider that produced it.
 *   - It owns the packet bytes, one allocation per packet.
 *
 * The third one is not a detail. A packet read from the source is borrowed, so
 * the queue needs its own copy for a stream that is not being read right now --
 * asking for video must never lose audio. That copy is freed the moment it has
 * been handed to a decoder, because the documented contract is that a packet is
 * valid for the duration of `send_packet` only. An arena with `realloc` would
 * have been the cheaper answer and the wrong one: a later growth moves the
 * arena and every live pointer into it dangles.
 *
 * The fourth property is what makes the fallback relationship work, and it is
 * why the routing lives here and nowhere else: the engine is the only code that
 * knows both which provider demuxed a file and which provider owns each stream's
 * decoder. Everything else in the system sees one API.
 */

#include "savanxp/sxmedia.h"

#include <stdlib.h>
#include <string.h>

/* The three copies this file needs are copies, not formatting. Doing them here
 * keeps <stdio.h> out, which is not a style preference: a module that calls
 * snprintf drags the formatter into every binary that links the engine, and the
 * engine is linked by a program whose whole job is playing sound effects. */
static void copy_text(char* destination, size_t capacity, const char* source) {
    size_t index = 0;
    if (destination == NULL || capacity == 0) {
        return;
    }
    if (source != NULL) {
        while (source[index] != '\0' && index + 1u < capacity) {
            destination[index] = source[index];
            index += 1u;
        }
    }
    destination[index] = '\0';
}

#define SX_MEDIA_US_PER_SECOND 1000000LL
/* What a container that does not declare a frame rate is assumed to be. */
#define SX_MEDIA_DEFAULT_FRAME_US 40000
/* Drain slack: a resampler asked to flush may still hold a filter delay. */
#define SX_MEDIA_RESAMPLE_TAIL 4096

/* ---- registry ------------------------------------------------------------ */

/* Sorted by descending priority, stable inside a priority by registration
 * order. A fixed table with no removal: the SDK has no dynamic linker, so
 * registration is a link-time fact and a registry that could be emptied would
 * have nothing to offer. */
static struct sx_media_backend g_backends[SX_MEDIA_MAX_BACKENDS];
static int g_backend_count;

int sx_media_register_backend(const struct sx_media_backend* backend) {
    int index;
    int position;

    if (backend == NULL || backend->name == NULL || backend->ops == NULL) {
        return -22; /* EINVAL */
    }
    if (backend->abi_version != SX_MEDIA_BACKEND_ABI) {
        return -22;
    }
    if (g_backend_count >= SX_MEDIA_MAX_BACKENDS) {
        return -28; /* ENOSPC */
    }
    for (index = 0; index < g_backend_count; ++index) {
        if (strcmp(g_backends[index].name, backend->name) == 0) {
            return -17; /* EEXIST */
        }
    }

    /* Insert by priority, keeping registration order inside a priority, so the
     * first registered wins a tie and the order in link.sh is meaningful. */
    position = g_backend_count;
    for (index = 0; index < g_backend_count; ++index) {
        if (g_backends[index].priority < backend->priority) {
            position = index;
            break;
        }
    }
    for (index = g_backend_count; index > position; --index) {
        g_backends[index] = g_backends[index - 1];
    }
    g_backends[position] = *backend;
    g_backend_count += 1;
    return 0;
}

int sx_media_backend_count(void) {
    return g_backend_count;
}

const char* sx_media_backend_name_at(int index) {
    if (index < 0 || index >= g_backend_count) {
        return NULL;
    }
    return g_backends[index].name;
}

int sx_media_has_codec(const char* codec) {
    struct sx_media_stream_desc probe;
    int index;

    if (codec == NULL || codec[0] == '\0') {
        return 0;
    }
    memset(&probe, 0, sizeof(probe));
    probe.index = -1;
    probe.kind = SX_MEDIA_KIND_ANY;
    copy_text(probe.codec, sizeof(probe.codec), codec);
    probe.sar_num = 1;
    probe.sar_den = 1;

    for (index = 0; index < g_backend_count; ++index) {
        const struct sx_media_backend_ops* ops = g_backends[index].ops;
        if (ops->claim_stream != NULL && ops->claim_stream(g_backends[index].user, &probe)) {
            return 1;
        }
    }
    return 0;
}

/* The provider that owns a stream, by priority. NULL when nobody claims it. */
static const struct sx_media_backend* find_decoder(const struct sx_media_stream_desc* stream) {
    int index;
    for (index = 0; index < g_backend_count; ++index) {
        const struct sx_media_backend_ops* ops = g_backends[index].ops;
        if (ops->claim_stream != NULL && ops->claim_stream(g_backends[index].user, stream)) {
            return &g_backends[index];
        }
    }
    return NULL;
}

/* The provider that parses this source. NULL when nobody will. */
static const struct sx_media_backend* find_source(const struct sx_media_source* source) {
    int index;
    for (index = 0; index < g_backend_count; ++index) {
        const struct sx_media_backend_ops* ops = g_backends[index].ops;
        if (ops->claim_source != NULL && ops->claim_source(g_backends[index].user, source)) {
            return &g_backends[index];
        }
    }
    return NULL;
}

const char* sx_media_status_string(enum sx_media_status status) {
    switch (status) {
        case SX_MEDIA_OK:
            return "ok";
        case SX_MEDIA_ERR_NO_BACKEND:
            return "this build has no media backend";
        case SX_MEDIA_ERR_NO_DEMUXER:
            return "SavanXP cannot read this container format";
        case SX_MEDIA_ERR_NO_CODEC:
            return "this system has no decoder for that codec";
        case SX_MEDIA_ERR_UNREADABLE:
            return "this file could not be read";
        case SX_MEDIA_ERR_UNSUPPORTED:
            return "this file has nothing playable in it";
        case SX_MEDIA_ERR_NOMEM:
            return "out of memory";
    }
    return "unknown error";
}

/* ---- per-stream packet queue -------------------------------------------- */

/* One allocation per packet, owned by the queue, released by packet_release
 * after the decoder has had it. */
struct sx_queue {
    struct sx_media_packet* items;
    int capacity;
    int head;
    int count;
    int dropped;
};

static void queue_reset(struct sx_queue* queue) {
    int index;
    for (index = 0; index < queue->count; ++index) {
        struct sx_media_packet* item = &queue->items[(queue->head + index) % queue->capacity];
        free((void*)item->data);
    }
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/* Doubles until SX_MEDIA_QUEUE_PACKET_LIMIT, then drops and counts: a file only
 * reaches that if one stream goes unread for minutes, and growing without bound
 * on a fixed arena is worse than a hole in the audio. */
static int queue_push(struct sx_queue* queue, const struct sx_media_packet* packet) {
    struct sx_media_packet* item;
    uint8_t* copy = NULL;
    int position;

    if (queue->count == queue->capacity) {
        int capacity = queue->capacity == 0 ? 64 : queue->capacity * 2;
        struct sx_media_packet* items;
        int index;

        if (capacity > SX_MEDIA_QUEUE_PACKET_LIMIT) {
            queue->dropped += 1;
            return 0;
        }
        items = (struct sx_media_packet*)realloc(queue->items, (size_t)capacity * sizeof(*items));
        if (items == NULL) {
            queue->dropped += 1;
            return 0;
        }
        /* Unrolled so the head lands back on 0. */
        for (index = 0; index < queue->count; ++index) {
            items[index] = queue->items[(queue->head + index) % queue->capacity];
        }
        free(queue->items);
        queue->items = items;
        queue->capacity = capacity;
        queue->head = 0;
    }

    if (packet->size > 0 && packet->data != NULL) {
        copy = (uint8_t*)malloc(packet->size);
        if (copy == NULL) {
            queue->dropped += 1;
            return 0;
        }
        memcpy(copy, packet->data, packet->size);
    }

    position = (queue->head + queue->count) % queue->capacity;
    item = &queue->items[position];
    item->stream_index = packet->stream_index;
    item->time_us = packet->time_us;
    item->key = packet->key;
    item->data = copy;
    item->size = packet->size > 0 && copy != NULL ? packet->size : 0;
    queue->count += 1;
    return 1;
}

/* Hands the bytes to the caller, who must packet_release them. */
static int queue_pop(struct sx_queue* queue, struct sx_media_packet* out) {
    if (queue->count == 0) {
        return 0;
    }
    *out = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count -= 1;
    return 1;
}

static void packet_release(struct sx_media_packet* packet) {
    free((void*)packet->data);
    packet->data = NULL;
    packet->size = 0;
}

/* ---- engine -------------------------------------------------------------- */

struct sx_media_stream {
    struct sx_media_stream_desc desc;
    const struct sx_media_backend_ops* ops;  /* NULL = undecodable */
    void* user;
    void* decoder;
    /* The decoder reads the source itself instead of being fed packets, because
     * its library is a whole-file decoder. No packet is ever routed to it and it
     * has no queue. */
    int self_fed;
    struct sx_queue queue;
    int flushed;
    int finished;
    int64_t skip_until_us;
    int64_t next_time_us;  /* estimate for frames with no timestamp */
};

struct sx_media {
    const struct sx_media_backend_ops* source_ops;
    void* source_user;
    void* source;
    int source_seekable;
    int64_t duration_us;
    /* A copy of the caller's request, with `audio_out` pointed at the engine's
     * own copy of the format rather than at the caller's. A self-fed decoder is
     * handed this after the caller's struct may be gone, and the format has to
     * still be there. */
    struct sx_media_source request;

    int stream_count;
    int video_slot;  /* index into streams[], -1 if none */
    int audio_slot;
    struct sx_media_stream streams[SX_MEDIA_MAX_STREAMS];

    struct sx_media_audio_format audio_out;
    int wants_audio;

    int missing_count;
    char missing[SX_MEDIA_MAX_MISSING][SX_MEDIA_CODEC_CAPACITY];

    /* video */
    struct sx_media_frame video;
    struct sx_media_frame shown;
    int has_shown;
    int64_t video_time_us;
    void* scaler;
    int scaler_format;
    int scaler_rate;   /* unused for video; keeps the rebuild check symmetric */
    int scaler_channels;
    uint32_t* scaled;
    int scaled_width;
    int scaled_height;
    const void* scaled_source;  /* the frame `scaled` was produced from; NULL */

    /* audio */
    void* resampler;
    int resampler_format;
    int resampler_rate;
    int resampler_channels;
    int16_t* audio_buffer;
    int audio_capacity;  /* frames */
    int audio_frames;    /* valid frames */
    int audio_offset;    /* frames already handed out */
    int64_t audio_time_us;
};

int sx_media_create(struct sx_media** out) {
    struct sx_media* media;

    if (out == NULL) {
        return -22;
    }
    media = (struct sx_media*)calloc(1, sizeof(*media));
    if (media == NULL) {
        return -12;
    }
    media->video_slot = -1;
    media->audio_slot = -1;
    media->video_time_us = SX_MEDIA_NO_TIME;
    *out = media;
    return 0;
}

static void release_frame(struct sx_media_frame* frame) {
    if (frame->data != NULL && frame->release != NULL) {
        frame->release(frame->data);
    }
    memset(frame, 0, sizeof(*frame));
    frame->time_us = SX_MEDIA_NO_TIME;
}

void sx_media_close(struct sx_media* media) {
    int index;

    if (media == NULL) {
        return;
    }
    if (media->source != NULL && media->source_ops != NULL && media->source_ops->close != NULL) {
        media->source_ops->close(media->source);
    }
    for (index = 0; index < media->stream_count; ++index) {
        struct sx_media_stream* stream = &media->streams[index];
        if (stream->decoder != NULL && stream->ops != NULL && stream->ops->close_decoder != NULL) {
            stream->ops->close_decoder(stream->decoder);
        }
        stream->decoder = NULL;
        stream->ops = NULL;
        stream->user = NULL;
        queue_reset(&stream->queue);
    }
    if (media->scaler != NULL && media->source_ops != NULL && media->source_ops->scaler_close != NULL) {
        media->source_ops->scaler_close(media->scaler);
    }
    if (media->resampler != NULL && media->source_ops != NULL &&
        media->source_ops->resampler_close != NULL) {
        media->source_ops->resampler_close(media->resampler);
    }
    release_frame(&media->video);
    release_frame(&media->shown);
    free(media->scaled);
    free(media->audio_buffer);
    media->scaler = NULL;
    media->resampler = NULL;
    media->scaled = NULL;
    media->audio_buffer = NULL;
    media->source = NULL;
    media->source_ops = NULL;
    media->source_user = NULL;
    media->stream_count = 0;
    media->video_slot = -1;
    media->audio_slot = -1;
    media->wants_audio = 0;
    media->has_shown = 0;
    media->missing_count = 0;
    media->video_time_us = SX_MEDIA_NO_TIME;
    media->duration_us = 0;
    media->scaled_width = 0;
    media->scaled_height = 0;
    media->scaled_source = NULL;
    media->audio_frames = 0;
    media->audio_offset = 0;
}

void sx_media_destroy(struct sx_media* media) {
    if (media == NULL) {
        return;
    }
    sx_media_close(media);
    free(media);
}

/* ---- open ---------------------------------------------------------------- */

static void note_missing(struct sx_media* media, const char* codec) {
    if (media->missing_count < SX_MEDIA_MAX_MISSING) {
        copy_text(media->missing[media->missing_count], SX_MEDIA_CODEC_CAPACITY,
                  codec != NULL && codec[0] != '\0' ? codec : "unknown");
    }
    media->missing_count += 1;
}

int sx_media_open(struct sx_media* media, const struct sx_media_source* source,
                  enum sx_media_status* status) {
    struct sx_media_stream_desc descs[SX_MEDIA_MAX_STREAMS];
    const struct sx_media_backend* provider;
    enum sx_media_status reason = SX_MEDIA_OK;
    int count = 0;
    int decodable = 0;
    int index;

    if (status != NULL) {
        *status = SX_MEDIA_OK;
    }
    if (media == NULL || source == NULL) {
        if (status != NULL) {
            *status = SX_MEDIA_ERR_UNREADABLE;
        }
        return -22;
    }
    sx_media_close(media);

    if (g_backend_count == 0) {
        if (status != NULL) {
            *status = SX_MEDIA_ERR_NO_BACKEND;
        }
        return -19; /* ENODEV */
    }
    provider = find_source(source);
    if (provider == NULL) {
        if (status != NULL) {
            *status = SX_MEDIA_ERR_NO_DEMUXER;
        }
        return -19;
    }

    memset(descs, 0, sizeof(descs));
    for (index = 0; index < SX_MEDIA_MAX_STREAMS; ++index) {
        descs[index].index = -1;
        descs[index].sar_num = 1;
        descs[index].sar_den = 1;
        descs[index].frame_duration_us = SX_MEDIA_NO_TIME;
    }
    media->source = provider->ops->open(provider->user, source, descs, SX_MEDIA_MAX_STREAMS, &count,
                                        &media->duration_us, &media->source_seekable, &reason);
    if (media->source == NULL) {
        if (status != NULL) {
            *status = reason != SX_MEDIA_OK ? reason : SX_MEDIA_ERR_UNREADABLE;
        }
        return -5; /* EIO */
    }
    media->source_ops = provider->ops;
    media->source_user = provider->user;
    media->wants_audio = source->audio_out != NULL && source->audio_out->sample_rate > 0 &&
                         source->audio_out->channels > 0;
    if (media->wants_audio) {
        media->audio_out = *source->audio_out;
    }
    media->request.path = source->path;
    media->request.fd = source->fd;
    media->request.audio_out = media->wants_audio ? &media->audio_out : NULL;

    if (count > SX_MEDIA_MAX_STREAMS) {
        count = SX_MEDIA_MAX_STREAMS;
    }
    media->stream_count = count;

    /* Decoders are assigned before the first packet is read, so a stream nobody
     * will ever decode costs nothing beyond the answer. */
    for (index = 0; index < count; ++index) {
        struct sx_media_stream* stream = &media->streams[index];
        const struct sx_media_backend* owner;

        stream->desc = descs[index];
        stream->next_time_us = SX_MEDIA_NO_TIME;
        stream->skip_until_us = SX_MEDIA_NO_TIME;
        if (stream->desc.sar_num <= 0 || stream->desc.sar_den <= 0) {
            stream->desc.sar_num = 1;
            stream->desc.sar_den = 1;
        }

        owner = find_decoder(&stream->desc);
        if (owner != NULL && owner->ops->open_whole != NULL) {
            /* A whole-file decoder is handed the source and reads it itself. */
            stream->decoder = owner->ops->open_whole(owner->user, &media->request, &stream->desc);
            stream->self_fed = stream->decoder != NULL;
        } else if (owner != NULL && owner->ops->open_decoder != NULL) {
            stream->decoder = owner->ops->open_decoder(owner->user, &stream->desc);
        }
        if (stream->decoder == NULL) {
            /* Only playback content is reported as dropped. A text stream
             * nobody decodes is a known gap of the system, not something wrong
             * with this file, and a notice that says "this system has no
             * subtitle decoder" every time an MKV has subtitles would be
             * noise. Subtitles are listed in the docs, not shouted per file. */
            if (stream->desc.kind == SX_MEDIA_KIND_VIDEO ||
                stream->desc.kind == SX_MEDIA_KIND_AUDIO) {
                note_missing(media, stream->desc.codec);
            }
            continue;
        }
        stream->ops = owner->ops;
        stream->user = owner->user;
        decodable += 1;
        if (stream->desc.kind == SX_MEDIA_KIND_VIDEO && media->video_slot < 0) {
            media->video_slot = index;
        } else if (stream->desc.kind == SX_MEDIA_KIND_AUDIO && media->audio_slot < 0 &&
                   media->wants_audio) {
            media->audio_slot = index;
        }
    }
    media->video_time_us = SX_MEDIA_NO_TIME;

    /* Nothing decodable at all is the one case that fails: a file whose only
     * streams this build cannot decode is not "partially playable". */
    if (decodable == 0) {
        if (status != NULL) {
            *status = SX_MEDIA_ERR_NO_CODEC;
        }
        sx_media_close(media);
        return -5;
    }
    return 0;
}

const char* sx_media_container_name(const struct sx_media* media) {
    if (media == NULL || media->source == NULL || media->source_ops == NULL ||
        media->source_ops->container_name == NULL) {
        return "?";
    }
    return media->source_ops->container_name(media->source);
}

int sx_media_has_video(const struct sx_media* media) {
    return media != NULL && media->video_slot >= 0;
}

int sx_media_has_audio(const struct sx_media* media) {
    return media != NULL && media->audio_slot >= 0;
}

int sx_media_is_seekable(const struct sx_media* media) {
    return media != NULL && media->source != NULL && media->source_seekable;
}

const char* sx_media_video_codec(const struct sx_media* media) {
    if (media == NULL || media->video_slot < 0) {
        return NULL;
    }
    return media->streams[media->video_slot].desc.codec;
}

const char* sx_media_audio_codec(const struct sx_media* media) {
    if (media == NULL || media->audio_slot < 0) {
        return NULL;
    }
    return media->streams[media->audio_slot].desc.codec;
}

int sx_media_audio_source_format(const struct sx_media* media,
                                 struct sx_media_audio_format* out) {
    if (out == NULL) {
        return 0;
    }
    out->sample_rate = 0;
    out->channels = 0;
    if (media == NULL || media->audio_slot < 0) {
        return 0;
    }
    out->sample_rate = media->streams[media->audio_slot].desc.sample_rate;
    out->channels = media->streams[media->audio_slot].desc.channels;
    return out->sample_rate > 0 && out->channels > 0;
}

void sx_media_display_size(const struct sx_media* media, int* width, int* height) {
    int w = 0;
    int h = 0;

    if (media != NULL && media->video_slot >= 0) {
        const struct sx_media_stream_desc* desc = &media->streams[media->video_slot].desc;
        w = desc->width;
        h = desc->height;
        /* The size to present, corrected by the pixel aspect ratio. */
        if (desc->sar_num > 0 && desc->sar_den > 0 && desc->sar_num != desc->sar_den) {
            w = (int)(((long)w * desc->sar_num) / desc->sar_den);
        }
    }
    if (width != NULL) {
        *width = w;
    }
    if (height != NULL) {
        *height = h;
    }
}

int64_t sx_media_duration_us(const struct sx_media* media) {
    return media != NULL ? media->duration_us : 0;
}

int64_t sx_media_frame_duration_us(const struct sx_media* media) {
    if (media != NULL && media->video_slot >= 0) {
        int64_t value = media->streams[media->video_slot].desc.frame_duration_us;
        if (value > 0) {
            return value;
        }
    }
    return SX_MEDIA_DEFAULT_FRAME_US;
}

const char* sx_media_metadata(const struct sx_media* media, const char* key) {
    if (media == NULL || media->source == NULL || media->source_ops == NULL ||
        media->source_ops->metadata == NULL || key == NULL) {
        return NULL;
    }
    return media->source_ops->metadata(media->source, key);
}

int sx_media_stream_count(const struct sx_media* media) {
    return media != NULL ? media->stream_count : 0;
}

int sx_media_stream_decodable(const struct sx_media* media, int stream_index) {
    if (media == NULL || stream_index < 0 || stream_index >= media->stream_count) {
        return 0;
    }
    return media->streams[stream_index].decoder != NULL;
}

const char* sx_media_stream_codec(const struct sx_media* media, int stream_index) {
    if (media == NULL || stream_index < 0 || stream_index >= media->stream_count) {
        return NULL;
    }
    return media->streams[stream_index].desc.codec;
}

int sx_media_stream_missing_count(const struct sx_media* media) {
    return media != NULL ? media->missing_count : 0;
}

int sx_media_stream_missing_at(const struct sx_media* media, int index, char* codec,
                               size_t codec_capacity) {
    if (media == NULL || index < 0 || index >= media->missing_count ||
        index >= SX_MEDIA_MAX_MISSING) {
        return 0;
    }
    if (codec != NULL && codec_capacity > 0) {
        copy_text(codec, codec_capacity, media->missing[index]);
    }
    return 1;
}

int sx_media_packet_drops(const struct sx_media* media) {
    int total = 0;
    int index;

    if (media == NULL) {
        return 0;
    }
    for (index = 0; index < media->stream_count; ++index) {
        total += media->streams[index].queue.dropped;
    }
    return total;
}

/* ---- decode -------------------------------------------------------------- */

static int slot_of_stream_index(const struct sx_media* media, int stream_index) {
    int slot;
    for (slot = 0; slot < media->stream_count; ++slot) {
        if (media->streams[slot].desc.index == stream_index) {
            return slot;
        }
    }
    return -1;
}

/* One packet, into its stream's queue. 0 = no more packets. */
static int demux_one(struct sx_media* media) {
    struct sx_media_packet packet;
    int slot;

    if (media->source == NULL || media->source_ops->read_packet == NULL) {
        return 0;
    }
    /* Once every stream reads its own source, the payload is nobody's business
     * but the self-fed decoders'. Reading it would walk the whole file for
     * nothing, and on a large one that is the difference between a demuxer pass
     * over the headers and a second pass over the gigabytes. */
    {
        int index;
        int fed = 0;
        for (index = 0; index < media->stream_count; ++index) {
            if (media->streams[index].decoder != NULL && !media->streams[index].self_fed) {
                fed = 1;
                break;
            }
        }
        if (!fed) {
            return 0;
        }
    }
    memset(&packet, 0, sizeof(packet));
    if (media->source_ops->read_packet(media->source, &packet) <= 0) {
        return 0;
    }
    slot = slot_of_stream_index(media, packet.stream_index);
    if (slot < 0 || media->streams[slot].decoder == NULL) {
        return 1;  /* a stream nobody decodes: consumed and dropped */
    }
    (void)queue_push(&media->streams[slot].queue, &packet);
    return 1;
}

/* Drains `slot` into `frame`. 1 = frame, 0 = the stream is finished.
 *
 * The send/receive split is not decoration: a decoder that has been fed can
 * hold several frames, and the engine has to be able to take one out without
 * feeding more. A corrupt packet returns an error and is skipped, and the
 * decoder resynchronizes on the next keyframe by itself. */
static int stream_receive(struct sx_media* media, int slot, struct sx_media_frame* frame) {
    struct sx_media_stream* stream = &media->streams[slot];

    for (;;) {
        struct sx_media_packet packet;
        int status;

        if (stream->finished) {
            return 0;
        }
        status = stream->ops->receive_frame(stream->decoder, frame);
        if (status > 0) {
            return 1;
        }
        if (status < 0) {
            stream->finished = 1;
            return 0;
        }
        if (stream->self_fed) {
            /* Nothing to feed and nothing to wait for: the decoder either has a
             * frame or it is at the end. Spinning here would be a busy loop. */
            return 0;
        }
        if (queue_pop(&stream->queue, &packet)) {
            (void)stream->ops->send_packet(stream->decoder, &packet);
            packet_release(&packet);
            continue;
        }
        if (demux_one(media)) {
            continue;
        }
        if (!stream->flushed) {
            /* The end-of-stream packet, which is what makes a decoder emit what
             * is still inside it. */
            (void)stream->ops->send_packet(stream->decoder, NULL);
            stream->flushed = 1;
            continue;
        }
        stream->finished = 1;
        return 0;
    }
}

int sx_media_next_video_frame(struct sx_media* media) {
    int slot;
    int64_t frame_duration;

    if (media == NULL || media->video_slot < 0) {
        return 0;
    }
    slot = media->video_slot;
    frame_duration = sx_media_frame_duration_us(media);

    for (;;) {
        struct sx_media_stream* stream = &media->streams[slot];
        int64_t time_us;

        release_frame(&media->video);
        if (!stream_receive(media, slot, &media->video)) {
            return 0;
        }
        time_us = media->video.time_us;
        if (time_us == SX_MEDIA_NO_TIME) {
            time_us = stream->next_time_us != SX_MEDIA_NO_TIME ? stream->next_time_us : 0;
        }
        stream->next_time_us = time_us + frame_duration;

        /* The frame on screen at the target is the one that starts before it and
         * has not finished. Earlier ones are decoded -- later frames reference
         * them -- but not delivered. */
        if (stream->skip_until_us != SX_MEDIA_NO_TIME) {
            if (time_us + frame_duration <= stream->skip_until_us) {
                release_frame(&media->video);
                continue;
            }
            stream->skip_until_us = SX_MEDIA_NO_TIME;
        }
        media->video_time_us = time_us;
        return 1;
    }
}

void sx_media_show_video_frame(struct sx_media* media) {
    if (media == NULL) {
        return;
    }
    release_frame(&media->shown);
    media->shown = media->video;
    memset(&media->video, 0, sizeof(media->video));
    media->video.time_us = SX_MEDIA_NO_TIME;
    media->has_shown = 1;
}

int sx_media_has_shown_frame(const struct sx_media* media) {
    return media != NULL && media->has_shown;
}

int64_t sx_media_video_time_us(const struct sx_media* media) {
    return media != NULL ? media->video_time_us : SX_MEDIA_NO_TIME;
}

const uint32_t* sx_media_scale_video(struct sx_media* media, int width, int height) {
    size_t pixels;
    const struct sx_media_frame* shown;
    int cache_valid;

    if (media == NULL || media->video_slot < 0 || !media->has_shown) {
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    shown = &media->shown;
    if (media->source_ops == NULL || media->source_ops->scale == NULL ||
        media->source_ops->scaler_open == NULL) {
        return NULL;
    }

    /* A converter is built for one input format and one output size, and its
     * output is a pure function of the frame it was handed. So the result is
     * reusable exactly when the same frame is still shown at the same size --
     * which is the case that matters: a paused window being resized back and
     * forth, and a player that asks for the same size every pump. The windowed
     * case (a resize) is deliberately not cached, because a same-size pump is
     * the hot path and a stale frame there is a visible bug, while a rebuild
     * costs one scaler. */
    cache_valid = media->scaled != NULL && media->scaled_width == width &&
                  media->scaled_height == height && media->scaled_source == shown->data &&
                  media->scaler != NULL && media->scaler_format == shown->format &&
                  media->scaler_rate == shown->sample_rate &&
                  media->scaler_channels == shown->channels;
    if (cache_valid) {
        return media->scaled;
    }

    if (media->scaler != NULL &&
        (media->scaled == NULL || media->scaled_width != width || media->scaled_height != height ||
         media->scaler_format != shown->format || media->scaler_rate != shown->sample_rate ||
         media->scaler_channels != shown->channels)) {
        media->source_ops->scaler_close(media->scaler);
        media->scaler = NULL;
    }
    if (media->scaler == NULL) {
        media->scaler = media->source_ops->scaler_open(media->source_user, shown, width, height);
        if (media->scaler == NULL) {
            return NULL;
        }
        media->scaler_format = shown->format;
        media->scaler_rate = shown->sample_rate;
        media->scaler_channels = shown->channels;
    }

    pixels = (size_t)width * (size_t)height;
    if (media->scaled == NULL || media->scaled_width != width || media->scaled_height != height) {
        /* One spare row. swscale can read and write a little past the end of the
         * last row on some paths, and the alternative to the slack is a
         * converter that scribbles one row past the buffer. */
        uint32_t* grown = (uint32_t*)realloc(media->scaled, pixels * sizeof(uint32_t) + 4u);
        if (grown == NULL) {
            media->source_ops->scaler_close(media->scaler);
            media->scaler = NULL;
            return NULL;
        }
        media->scaled = grown;
        media->scaled_width = width;
        media->scaled_height = height;
    }
    if (media->source_ops->scale(media->scaler, shown, media->scaled, width, height, width) < 0) {
        return NULL;
    }
    media->scaled_source = shown->data;
    return media->scaled;
}

/* Rebuilds the resampler when the frame's shape changes. A stream whose sample
 * format changes mid-flight is real, and a resampler built for the container's
 * declared format is wrong for every frame after the change. */
static int ensure_resampler(struct sx_media* media, const struct sx_media_frame* frame) {
    if (media->resampler != NULL && media->resampler_format == frame->format &&
        media->resampler_rate == frame->sample_rate && media->resampler_channels == frame->channels) {
        return 1;
    }
    if (media->resampler != NULL && media->source_ops->resampler_close != NULL) {
        media->source_ops->resampler_close(media->resampler);
    }
    media->resampler = media->source_ops->resampler_open(media->source_user, frame, &media->audio_out);
    if (media->resampler == NULL) {
        return 0;
    }
    media->resampler_format = frame->format;
    media->resampler_rate = frame->sample_rate;
    media->resampler_channels = frame->channels;
    return 1;
}

static int ensure_audio_capacity(struct sx_media* media, int frames) {
    int16_t* grown;
    size_t samples;

    if (frames <= media->audio_capacity) {
        return 1;
    }
    samples = (size_t)frames * (size_t)media->audio_out.channels;
    grown = (int16_t*)realloc(media->audio_buffer, samples * sizeof(int16_t));
    if (grown == NULL) {
        return 0;
    }
    media->audio_buffer = grown;
    media->audio_capacity = frames;
    return 1;
}

/* Decodes and converts the next block of audio into the internal buffer, which
 * must be drained first. 1 = new samples, 0 = finished. */
static int refill_audio(struct sx_media* media) {
    int channels = media->audio_out.channels;

    for (;;) {
        struct sx_media_frame frame;
        struct sx_media_stream* stream = &media->streams[media->audio_slot];
        int64_t time_us;
        int converted;
        int capacity;

        media->audio_frames = 0;
        media->audio_offset = 0;

        memset(&frame, 0, sizeof(frame));
        frame.time_us = SX_MEDIA_NO_TIME;
        if (!stream_receive(media, media->audio_slot, &frame)) {
            /* What the resampler still holds from its own delay, so the tail of
             * the file is not lost. Drained once and then dropped. */
            if (media->resampler != NULL && media->source_ops->resample_flush != NULL &&
                ensure_audio_capacity(media, SX_MEDIA_RESAMPLE_TAIL)) {
                converted = media->source_ops->resample_flush(media->resampler, media->audio_buffer,
                                                             media->audio_capacity);
                media->source_ops->resampler_close(media->resampler);
                media->resampler = NULL;
                if (converted > 0) {
                    media->audio_frames = converted;
                    media->audio_time_us = stream->next_time_us != SX_MEDIA_NO_TIME
                                               ? stream->next_time_us
                                               : 0;
                    return 1;
                }
            }
            return 0;
        }
        if (media->source_ops->resample == NULL || !ensure_resampler(media, &frame)) {
            release_frame(&frame);
            continue;
        }

        time_us = frame.time_us;
        if (time_us == SX_MEDIA_NO_TIME) {
            time_us = stream->next_time_us != SX_MEDIA_NO_TIME ? stream->next_time_us : 0;
        }
        /* Room for the converted block, not a fixed guess. A converter produces
         * more frames than it consumes whenever the sink's rate is above the
         * source's, and the ratio decides how many, so the old fixed 2048 was
         * quietly too small for a 48 kHz sink fed by a 44.1 kHz source: the
         * converter would be handed less than it wanted and the engine would
         * drop the tail, which is a hole in the audio with nothing reporting it.
         *
         * A backend with nothing to convert still writes at most `frame->count`,
         * so this is an upper bound rather than a requirement. */
        capacity = 2048;
        if (frame.count > 0 && frame.sample_rate > 0) {
            const int64_t needed = (int64_t)frame.count * media->audio_out.sample_rate / frame.sample_rate;
            if (needed + 64 > capacity) {
                capacity = (int)(needed + 64);
            }
        }
        if (!ensure_audio_capacity(media, capacity)) {
            release_frame(&frame);
            continue;
        }
        converted = media->source_ops->resample(media->resampler, &frame, media->audio_buffer,
                                                media->audio_capacity);
        release_frame(&frame);
        if (converted <= 0) {
            continue;
        }
        media->audio_frames = converted;
        media->audio_time_us = time_us;
        stream->next_time_us = time_us +
                               (int64_t)converted * SX_MEDIA_US_PER_SECOND / media->audio_out.sample_rate;
        (void)channels;
        return 1;
    }
}

static int64_t audio_offset_time(const struct sx_media* media, int offset) {
    return media->audio_time_us +
           (int64_t)offset * SX_MEDIA_US_PER_SECOND / media->audio_out.sample_rate;
}

int sx_media_read_audio(struct sx_media* media, int16_t* out, int frames, int64_t* first_time_us) {
    int channels;
    int written = 0;

    if (first_time_us != NULL) {
        *first_time_us = SX_MEDIA_NO_TIME;
    }
    if (media == NULL || media->audio_slot < 0 || out == NULL || frames <= 0) {
        return 0;
    }
    channels = media->audio_out.channels;

    while (written < frames) {
        int available = media->audio_frames - media->audio_offset;
        int count;

        if (available <= 0) {
            if (!refill_audio(media)) {
                break;
            }
            continue;
        }
        count = frames - written < available ? frames - written : available;

        /* The seek trim counts samples, not blocks: the first sample delivered is
         * the one at the requested instant. */
        if (media->streams[media->audio_slot].skip_until_us != SX_MEDIA_NO_TIME) {
            int64_t now_us = audio_offset_time(media, media->audio_offset);
            if (now_us < media->streams[media->audio_slot].skip_until_us) {
                int64_t skip_us = media->streams[media->audio_slot].skip_until_us - now_us;
                int64_t skip_frames = skip_us * media->audio_out.sample_rate / SX_MEDIA_US_PER_SECOND;
                if (skip_frames > 0) {
                    if (skip_frames >= available) {
                        media->audio_offset += available;
                        continue;
                    }
                    count -= (int)skip_frames;
                    media->audio_offset += (int)skip_frames;
                }
            }
            media->streams[media->audio_slot].skip_until_us = SX_MEDIA_NO_TIME;
        }
        if (count <= 0) {
            continue;
        }
        if (first_time_us != NULL && *first_time_us == SX_MEDIA_NO_TIME) {
            *first_time_us = audio_offset_time(media, media->audio_offset);
        }
        memcpy(out + (size_t)written * (size_t)channels,
               media->audio_buffer + (size_t)media->audio_offset * (size_t)channels,
               (size_t)count * (size_t)channels * sizeof(int16_t));
        media->audio_offset += count;
        written += count;
    }
    if (media->audio_frames > 0 && media->audio_offset >= media->audio_frames) {
        media->audio_frames = 0;
        media->audio_offset = 0;
    }
    return written;
}

int sx_media_seek(struct sx_media* media, int64_t target_us) {
    int index;

    if (media == NULL || media->source == NULL || !media->source_seekable) {
        return 0;
    }
    /* A self-fed stream repositions itself; the source provider only knows about
     * the streams it feeds. Both happen: a file can have a self-fed audio stream
     * and packet-fed video, and seeking has to move the second as well. */
    for (index = 0; index < media->stream_count; ++index) {
        struct sx_media_stream* stream = &media->streams[index];
        if (stream->self_fed && stream->ops->seek_stream != NULL &&
            !stream->ops->seek_stream(stream->decoder, target_us)) {
            return 0;
        }
    }
    if (media->source_ops->seek == NULL || !media->source_ops->seek(media->source, target_us)) {
        return 0;
    }
    if (media->resampler != NULL && media->source_ops->resampler_close != NULL) {
        media->source_ops->resampler_close(media->resampler);
        media->resampler = NULL;
    }
    for (index = 0; index < media->stream_count; ++index) {
        struct sx_media_stream* stream = &media->streams[index];
        queue_reset(&stream->queue);
        /* The decoder has to let go of what it is holding: reference frames from
         * before the seek make the picture after it wrong, not merely slow. */
        if (stream->decoder != NULL && stream->ops != NULL && stream->ops->flush != NULL) {
            stream->ops->flush(stream->decoder);
        }
        stream->flushed = 0;
        stream->finished = 0;
        stream->next_time_us = SX_MEDIA_NO_TIME;
        stream->skip_until_us = target_us;
    }
    release_frame(&media->video);
    release_frame(&media->shown);
    media->has_shown = 0;
    media->video_time_us = SX_MEDIA_NO_TIME;
    media->audio_frames = 0;
    media->audio_offset = 0;
    return 1;
}

void sx_media_skip_until(struct sx_media* media, int64_t time_us) {
    int index;

    if (media == NULL) {
        return;
    }
    for (index = 0; index < media->stream_count; ++index) {
        if (media->streams[index].decoder != NULL) {
            media->streams[index].skip_until_us = time_us;
        }
    }
}
