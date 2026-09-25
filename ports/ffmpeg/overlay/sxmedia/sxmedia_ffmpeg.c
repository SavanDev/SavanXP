/* The FFmpeg backend for SxMedia.
 *
 * This is `media.c` reshaped. Every line of decode, demux, resample, scale and
 * seek behaviour is the same code it was, rearranged so that the parts the
 * engine now owns -- the packet routing, the per-stream decoder choice, the
 * queue, the time base -- are gone, and the parts a backend owns -- this file --
 * fill a vtable.
 *
 * It fills all three roles, which is what makes FFmpeg the fallback rather than
 * a competitor: as a source provider it takes every container the port enables,
 * as a decoder provider it claims every stream it can open, and as the converter
 * it is swscale and swresample. A single-codec library would fill one role and
 * leave the other two null, and the engine would route around it.
 *
 * The two places where this is not a mechanical move, both documented at the
 * point they happen:
 *
 *   - `send_packet` hands the engine's bytes to libavcodec with an
 *     `av_buffer_create` whose free function does nothing. libavcodec takes its
 *     own reference when it needs the packet after the call returns, and the
 *     engine frees its copy afterwards; the refcount is what makes that safe,
 *     and it is why the packet is not copied a second time here.
 *   - `receive_frame` hands out `av_frame_clone`, not the decoder's own frame.
 *     The engine keeps a decoded frame and a shown frame at the same time, so
 *     they cannot be the same allocation. A clone is a reference, not a copy of
 *     the pixels, which is what makes the shown frame independent for free.
 */

#include "savanxp/sxmedia.h"

#include "sxmedia_ffmpeg.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <stdlib.h>
#include <string.h>

/* The SavanXP framebuffer is XRGB8888 in a uint32 with the top byte zero (see
 * gfx_rgb): B,G,R,0 in memory, which is exactly AV_PIX_FMT_BGR0. BGRA would put
 * 0xff in that byte. */
#define SX_FFMPEG_PIXEL_FORMAT AV_PIX_FMT_BGR0

#define SX_FFMPEG_US_PER_SECOND AV_TIME_BASE

/* ---- source role --------------------------------------------------------- */

/* What the source provider publishes per stream, and what the decoder role
 * reads. It is FFmpeg's own type on purpose: decoder configuration is demuxer
 * data, and the only honest way to carry it across a library boundary is as
 * something the boundary's owner recognises. A decoder that needs nothing but a
 * codec name and the extradata -- stb_vorbis, for instance -- would leave
 * `setup` NULL and work with any source provider, which is the case that keeps
 * the seam worth having. */
typedef struct {
    AVCodecParameters* parameters;
    /* The stream's time base. It is here rather than in the descriptor because
     * AVCodecParameters does not carry one, and because leaving it out is silent:
     * the decoder still produces frames and the timestamps come out in the
     * default base, so video lands 1000x late and only the --sync test sees it.
     * The engine's contract is microseconds, so the conversion has to happen
     * somewhere and this is where both directions of it live. */
    AVRational time_base;
    int64_t start_time_us;
} SxFFmpegStreamSetup;

typedef struct {
    AVFormatContext* format;
    AVPacket* packet;
    int64_t start_time_us;
    SxFFmpegStreamSetup* setups;
    int setup_count;
} SxFFmpegSource;

static void ffmpeg_close(void* opaque);

static int64_t stream_time_us(const SxFFmpegSource* source, int stream_index, int64_t timestamp) {
    if (timestamp == AV_NOPTS_VALUE || stream_index < 0 || stream_index >= (int)source->format->nb_streams) {
        return SX_MEDIA_NO_TIME;
    }
    return av_rescale_q(timestamp, source->format->streams[stream_index]->time_base, AV_TIME_BASE_Q) -
           source->start_time_us;
}

/* Every container the port enables. FFmpeg is the catch-all provider, so it
 * claims everything and lets `open` be the thing that fails: a provider that
 * ships no demuxer leaves this NULL instead, and a future one that does would
 * be asked first because it registers at a higher priority. */
static int ffmpeg_claim_source(void* user, const struct sx_media_source* source) {
    (void)user;
    if (source == NULL) {
        return 0;
    }
    /* A path is all the configured protocol supports; --enable-protocol=file
     * means there is no other. The fd field exists so that adding one is not a
     * signature change, and FFmpeg has no way to take it today. */
    return source->path != NULL && source->path[0] != '\0';
}

static void* ffmpeg_open(void* user, const struct sx_media_source* source,
                         struct sx_media_stream_desc* streams, int stream_capacity,
                         int* stream_count, int64_t* duration_us, int* seekable,
                         enum sx_media_status* status) {
    SxFFmpegSource* handle;
    const AVCodec* codec = NULL;
    int count = 0;
    unsigned index;

    (void)user;
    handle = (SxFFmpegSource*)calloc(1, sizeof(*handle));
    if (handle == NULL) {
        *status = SX_MEDIA_ERR_NOMEM;
        return NULL;
    }
    if (avformat_open_input(&handle->format, source->path, NULL, NULL) < 0) {
        /* FFmpeg gives one error code for "no demuxer matched" and for "matched
         * and then failed on it", so this cannot be told apart here. It is
         * reported as UNREADABLE because that is the answer the user can act on
         * with either, and because a claim of NO_DEMUXER would send a program
         * looking for a backend that is not the problem. */
        avformat_close_input(&handle->format);
        free(handle);
        *status = SX_MEDIA_ERR_UNREADABLE;
        return NULL;
    }
    if (avformat_find_stream_info(handle->format, NULL) < 0) {
        avformat_close_input(&handle->format);
        free(handle);
        *status = SX_MEDIA_ERR_UNREADABLE;
        return NULL;
    }

    handle->start_time_us = handle->format->start_time != AV_NOPTS_VALUE ? handle->format->start_time : 0;
    handle->packet = av_packet_alloc();
    handle->setups = (SxFFmpegStreamSetup*)calloc(stream_capacity > 0 ? (size_t)stream_capacity : 1u,
                                                  sizeof(SxFFmpegStreamSetup));
    if (handle->packet == NULL || handle->setups == NULL) {
        av_packet_free(&handle->packet);
        free(handle->setups);
        avformat_close_input(&handle->format);
        free(handle);
        *status = SX_MEDIA_ERR_NOMEM;
        return NULL;
    }

    /* Every stream, not the best video and the best audio: the engine decides
     * which ones to decode, and that decision is what lets a second library
     * take some of them. */
    for (index = 0; index < handle->format->nb_streams && count < stream_capacity; ++index) {
        AVStream* stream = handle->format->streams[index];
        struct sx_media_stream_desc* desc = &streams[count];

        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            /* The cover of an album is a one-frame "video" stream. Showing it
             * is a feature; playing it as the video is not. */
            continue;
        }
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
        memset(desc, 0, sizeof(*desc));
        desc->index = (int)index;
        desc->codec[0] = '\0';
        if (codec != NULL && codec->name != NULL) {
            strncpy(desc->codec, codec->name, SX_MEDIA_CODEC_CAPACITY - 1);
        }
        desc->width = stream->codecpar->width;
        desc->height = stream->codecpar->height;
        desc->sample_rate = stream->codecpar->sample_rate;
        desc->channels = stream->codecpar->ch_layout.nb_channels;
        desc->sar_num = stream->codecpar->sample_aspect_ratio.num;
        desc->sar_den = stream->codecpar->sample_aspect_ratio.den;
        desc->duration_us = stream->duration != AV_NOPTS_VALUE && stream->time_base.den > 0
                                ? av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q)
                                : 0;
        desc->frame_duration_us = SX_MEDIA_NO_TIME;
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVRational rate = av_guess_frame_rate(handle->format, stream, NULL);
            if (rate.num > 0 && rate.den > 0) {
                desc->frame_duration_us = av_rescale_q(1, rate, AV_TIME_BASE_Q);
            }
            desc->kind = SX_MEDIA_KIND_VIDEO;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            desc->kind = SX_MEDIA_KIND_AUDIO;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            desc->kind = SX_MEDIA_KIND_TEXT;
        } else {
            desc->kind = SX_MEDIA_KIND_ANY;
        }
        /* The parameter sets and the AudioSpecificConfig, handed to whichever
         * library ends up owning the stream. */
        desc->extradata = stream->codecpar->extradata;
        desc->extradata_size = stream->codecpar->extradata_size;
        /* And the library's own setup blob, for a decoder that needs more than
         * the descriptor carries. It has to be a copy: the descriptor outlives
         * nothing, but the source handle does, and this is freed with it. */
        desc->setup = NULL;
        if (count < stream_capacity) {
            SxFFmpegStreamSetup* setup = (SxFFmpegStreamSetup*)calloc(1, sizeof(*setup));
            if (setup != NULL) {
                setup->parameters = avcodec_parameters_alloc();
                if (setup->parameters != NULL &&
                    avcodec_parameters_copy(setup->parameters, stream->codecpar) < 0) {
                    avcodec_parameters_free(&setup->parameters);
                }
                setup->time_base = stream->time_base;
                setup->start_time_us = handle->start_time_us;
                handle->setups[handle->setup_count++] = *setup;
                desc->setup = &handle->setups[handle->setup_count - 1];
            }
        }
        count += 1;
    }

    *stream_count = count;
    *duration_us = handle->format->duration != AV_NOPTS_VALUE ? handle->format->duration : 0;
    *seekable = handle->format->pb != NULL &&
               (handle->format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0 && *duration_us > 0;
    *status = count > 0 ? SX_MEDIA_OK : SX_MEDIA_ERR_UNSUPPORTED;
    if (count == 0) {
        ffmpeg_close(handle);
        return NULL;
    }
    return handle;
}

static void ffmpeg_close(void* opaque) {
    SxFFmpegSource* source = (SxFFmpegSource*)opaque;
    if (source == NULL) {
        return;
    }
    av_packet_free(&source->packet);
    if (source->setups != NULL) {
        int index = 0;
        for (index = 0; index < source->setup_count; ++index) {
            SxFFmpegStreamSetup* setup = &source->setups[index];
            if (setup->parameters != NULL) {
                avcodec_parameters_free(&setup->parameters);
            }
        }
        free(source->setups);
    }
    avformat_close_input(&source->format);
    free(source);
}

static const char* ffmpeg_container_name(void* opaque) {
    SxFFmpegSource* source = (SxFFmpegSource*)opaque;
    if (source == NULL || source->format == NULL || source->format->iformat == NULL) {
        return "?";
    }
    return source->format->iformat->name;
}

static const char* ffmpeg_metadata(void* opaque, const char* key) {
    SxFFmpegSource* source = (SxFFmpegSource*)opaque;
    const AVDictionaryEntry* entry;

    if (source == NULL || source->format == NULL || key == NULL) {
        return NULL;
    }
    entry = av_dict_get(source->format->metadata, key, NULL, 0);
    return entry != NULL && entry->value != NULL && entry->value[0] != '\0' ? entry->value : NULL;
}

static int ffmpeg_seek(void* opaque, int64_t target_us) {
    SxFFmpegSource* source = (SxFFmpegSource*)opaque;
    int64_t timestamp;

    if (source == NULL || source->format == NULL) {
        return 0;
    }
    if (target_us < 0) {
        target_us = 0;
    }
    /* max_ts = target: the demuxer lands on the keyframe at or before it, and
     * the precision comes from the engine discarding up to the target. */
    timestamp = target_us + source->start_time_us;
    if (avformat_seek_file(source->format, -1, INT64_MIN, timestamp, timestamp, 0) < 0) {
        return 0;
    }
    return 1;
}

static int ffmpeg_read_packet(void* opaque, struct sx_media_packet* packet) {
    SxFFmpegSource* source = (SxFFmpegSource*)opaque;
    int status;

    if (source == NULL || source->format == NULL) {
        return 0;
    }
    status = av_read_frame(source->format, source->packet);
    if (status < 0) {
        return 0;
    }
    /* Borrowed until the next call: the engine copies what it keeps, which is
     * the price of a queue that does not know what an AVPacket is. */
    packet->stream_index = source->packet->stream_index;
    packet->time_us = stream_time_us(source, source->packet->stream_index, source->packet->pts);
    packet->key = (source->packet->flags & AV_PKT_FLAG_KEY) != 0 ? 1 : 0;
    packet->data = source->packet->data;
    packet->size = (size_t)source->packet->size;
    return 1;
}

/* ---- decoder role -------------------------------------------------------- */

typedef struct {
    AVCodecContext* decoder;
    AVFrame* scratch;  /* the decoder's reusable output frame */
    /* The stream's time base and the container's start_time, so the engine's
     * microseconds can be turned into the stream's units on the way in and out.
     * A decoder that loses the base mis-times B-frame reordering, which is
     * silent: frames come out, in the wrong order. */
    AVRational time_base;
    int64_t start_time_us;
} SxFFmpegDecoder;

/* The codec behind a descriptor, from the setup the source provider published.
 * When there is none -- the capability query has no source at all -- the name is
 * looked up across the whole library instead, which is how a codec name is
 * resolved with nothing but a name. */
static AVCodec* decoder_for_desc(const struct sx_media_stream_desc* stream) {
    const SxFFmpegStreamSetup* setup = (const SxFFmpegStreamSetup*)stream->setup;
    const AVCodecParameters* parameters = setup != NULL ? setup->parameters : NULL;
    const AVCodec* codec = NULL;
    void* iterator = NULL;
    const AVCodec* next = NULL;

    if (parameters != NULL) {
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && stream->kind != SX_MEDIA_KIND_VIDEO) {
            return NULL;
        }
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO && stream->kind != SX_MEDIA_KIND_AUDIO) {
            return NULL;
        }
        return (AVCodec*)avcodec_find_decoder(parameters->codec_id);
    }
    while ((next = av_codec_iterate(&iterator)) != NULL) {
        if (next->name != NULL && strcmp(next->name, stream->codec) == 0) {
            codec = next;
            break;
        }
    }
    return (AVCodec*)codec;
}

static int ffmpeg_claim_stream(void* user, const struct sx_media_stream_desc* stream) {
    (void)user;
    if (stream == NULL) {
        return 0;
    }
    return decoder_for_desc(stream) != NULL;
}

static void ffmpeg_frame_release(void* data) {
    AVFrame* frame = (AVFrame*)data;
    av_frame_free(&frame);
}

static void* ffmpeg_open_decoder(void* user, const struct sx_media_stream_desc* stream) {
    const SxFFmpegStreamSetup* setup = (const SxFFmpegStreamSetup*)stream->setup;
    const AVCodecParameters* parameters = setup != NULL ? setup->parameters : NULL;
    SxFFmpegDecoder* handle;
    AVCodec* codec;

    (void)user;
    codec = decoder_for_desc(stream);
    if (codec == NULL) {
        return NULL;
    }
    handle = (SxFFmpegDecoder*)calloc(1, sizeof(*handle));
    if (handle == NULL) {
        return NULL;
    }
    handle->decoder = avcodec_alloc_context3(codec);
    if (handle->decoder == NULL) {
        free(handle);
        return NULL;
    }
    if (parameters != NULL) {
        /* Everything the decoder needs, including the extradata: a decoder
         * without the parameter sets does not fail to open, it produces
         * silence. */
        if (avcodec_parameters_to_context(handle->decoder, parameters) < 0) {
            avcodec_free_context(&handle->decoder);
            free(handle);
            return NULL;
        }
        handle->time_base = setup->time_base;
        if (handle->time_base.den <= 0) {
            handle->time_base = av_inv_q(AV_TIME_BASE_Q);
        }
        /* pkt_timebase has to agree with the packets the engine will hand over,
         * or libavcodec reads the timestamps in the wrong unit. */
        handle->decoder->pkt_timebase = handle->time_base;
    } else {
        handle->time_base = av_inv_q(AV_TIME_BASE_Q);
    }
    /* The kernel has no threads: a decoder that tries to create them does not
     * start. */
    handle->decoder->thread_count = 1;
    if (avcodec_open2(handle->decoder, codec, NULL) < 0) {
        avcodec_free_context(&handle->decoder);
        free(handle);
        return NULL;
    }
    handle->start_time_us = setup != NULL ? setup->start_time_us : 0;
    handle->scratch = av_frame_alloc();
    if (handle->scratch == NULL) {
        avcodec_free_context(&handle->decoder);
        free(handle);
        return NULL;
    }
    return handle;
}

static void ffmpeg_flush(void* opaque) {
    SxFFmpegDecoder* handle = (SxFFmpegDecoder*)opaque;
    if (handle != NULL && handle->decoder != NULL) {
        avcodec_flush_buffers(handle->decoder);
    }
}

static void ffmpeg_close_decoder(void* opaque) {
    SxFFmpegDecoder* handle = (SxFFmpegDecoder*)opaque;
    if (handle == NULL) {
        return;
    }
    av_frame_free(&handle->scratch);
    avcodec_free_context(&handle->decoder);
    free(handle);
}

/* The engine's packet is engine-owned memory it frees when this returns. A
 * buffer whose free function does nothing turns libavcodec's reference into a
 * refcount rather than a copy, so the decoder can hold the packet after the call
 * and the engine can still release its own. */
static void ffmpeg_noop_free(void* opaque, uint8_t* data) {
    (void)opaque;
    (void)data;
}

static int ffmpeg_send_packet(void* opaque, const struct sx_media_packet* packet) {
    SxFFmpegDecoder* handle = (SxFFmpegDecoder*)opaque;
    AVPacket* wrapped;
    int status;

    if (handle == NULL || handle->decoder == NULL) {
        return 0;
    }
    if (packet == NULL) {
        /* The end-of-stream flush: what makes the decoder emit what is inside. */
        return avcodec_send_packet(handle->decoder, NULL) >= 0 ? 1 : 0;
    }
    wrapped = av_packet_alloc();
    if (wrapped == NULL) {
        return 0;
    }
    wrapped->data = (uint8_t*)packet->data;
    wrapped->size = (int)packet->size;
    if (packet->size > 0) {
        wrapped->buf = av_buffer_create(wrapped->data, (size_t)packet->size, ffmpeg_noop_free, NULL, 0);
    }
    /* The engine's times are microseconds from the start of the file, and the
     * decoder's pkt_timebase is the stream's. Putting one into the other
     * without rescaling would leave every timestamp wrong by a factor of the
     * time base, which for B-frame reordering shows up as frames out of order
     * rather than as an error. */
    if (packet->time_us == SX_MEDIA_NO_TIME) {
        wrapped->pts = AV_NOPTS_VALUE;
    } else {
        wrapped->pts = av_rescale_q(packet->time_us + handle->start_time_us, AV_TIME_BASE_Q,
                                    handle->time_base);
    }
    wrapped->dts = wrapped->pts;
    wrapped->flags = packet->key != 0 ? AV_PKT_FLAG_KEY : 0;
    /* pkt_timebase is the stream's, and the engine's times are microseconds from
     * the start of the file, so they are rescaled on the way back out. */
    status = avcodec_send_packet(handle->decoder, wrapped);
    /* Unref drops libavcodec's reference. The no-op free means the engine's
     * copy is the only thing that ever releases the bytes. */
    av_packet_unref(wrapped);
    av_packet_free(&wrapped);
    return status < 0 ? -1 : 1;
}

static int ffmpeg_receive_frame(void* opaque, struct sx_media_frame* out) {
    SxFFmpegDecoder* handle = (SxFFmpegDecoder*)opaque;
    AVFrame* frame;
    int status;

    if (handle == NULL || handle->decoder == NULL) {
        return 0;
    }
    status = avcodec_receive_frame(handle->decoder, handle->scratch);
    if (status == AVERROR(EAGAIN)) {
        return 0;
    }
    if (status < 0) {
        return -1;  /* end of stream, or a decoder that gave up */
    }
    /* A clone, not the scratch frame: the engine holds a decoded frame and a
     * shown frame at once, so they cannot be the same allocation. It is a
     * reference, not a copy of the pixels. */
    frame = av_frame_clone(handle->scratch);
    if (frame == NULL) {
        return 0;
    }
    out->time_us = SX_MEDIA_NO_TIME;
    if (handle->scratch->best_effort_timestamp != AV_NOPTS_VALUE) {
        out->time_us = av_rescale_q(handle->scratch->best_effort_timestamp, handle->time_base, AV_TIME_BASE_Q) -
                       handle->start_time_us;
    }
    out->width = handle->scratch->width;
    out->height = handle->scratch->height;
    out->sample_rate = handle->scratch->sample_rate;
    out->channels = handle->scratch->ch_layout.nb_channels;
    out->format = handle->scratch->format;
    out->key = (handle->scratch->flags & AV_FRAME_FLAG_KEY) != 0;
    out->data = frame;
    out->release = ffmpeg_frame_release;
    return 1;
}

/* ---- converter role ------------------------------------------------------ */

typedef struct {
    struct SwsContext* context;
} SxFFmpegScaler;

typedef struct {
    SwrContext* context;
} SxFFmpegResampler;

static void* ffmpeg_scaler_open(void* user, const struct sx_media_frame* prototype, int width,
                                int height) {
    const AVFrame* frame = (const AVFrame*)prototype->data;
    SxFFmpegScaler* scaler;
    struct SwsContext* context;

    (void)user;
    if (frame == NULL || width <= 0 || height <= 0) {
        av_log(NULL, AV_LOG_ERROR, "sxmedia: scaler prototype is empty\n");
        return NULL;
    }
    if (frame->width == 0 || frame->height == 0 || frame->format < 0) {
        av_log(NULL, AV_LOG_ERROR, "sxmedia: scaler prototype is degenerate: %dx%d format %d\n",
               frame->width, frame->height, frame->format);
        return NULL;
    }
    scaler = (SxFFmpegScaler*)calloc(1, sizeof(*scaler));
    if (scaler == NULL) {
        return NULL;
    }
    /* FAST_BILINEAR and not BILINEAR: without assembly the cost difference is
     * large and on moving video the quality difference is not. */
    context = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format, width, height,
                             SX_FFMPEG_PIXEL_FORMAT, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (context == NULL) {
        av_log(NULL, AV_LOG_ERROR, "sxmedia: sws_getContext(%d,%d,fmt %d -> %d,%d) failed\n",
               frame->width, frame->height, frame->format, width, height);
        free(scaler);
        return NULL;
    }
    scaler->context = context;
    return scaler;
}

static void ffmpeg_scaler_close(void* opaque) {
    SxFFmpegScaler* scaler = (SxFFmpegScaler*)opaque;
    if (scaler == NULL) {
        return;
    }
    sws_freeContext(scaler->context);
    free(scaler);
}

static int ffmpeg_scale(void* opaque, const struct sx_media_frame* frame, uint32_t* out, int width,
                        int height, int stride) {
    SxFFmpegScaler* scaler = (SxFFmpegScaler*)opaque;
    const AVFrame* av_frame = (const AVFrame*)frame->data;
    uint8_t* planes[4] = {NULL, NULL, NULL, NULL};
    int strides[4] = {0, 0, 0, 0};

    if (scaler == NULL || av_frame == NULL || out == NULL) {
        av_log(NULL, AV_LOG_ERROR, "sxmedia: scale called with an empty argument\n");
        return -1;
    }
    if (av_frame->width == 0 || av_frame->height == 0) {
        av_log(NULL, AV_LOG_ERROR, "sxmedia: scale got a degenerate frame %dx%d\n", av_frame->width,
               av_frame->height);
        return -1;
    }
    planes[0] = (uint8_t*)out;
    strides[0] = stride * 4;
    {
        const int written = sws_scale(scaler->context, (const uint8_t* const*)av_frame->data, av_frame->linesize,
                                      0, av_frame->height, planes, strides);
        if (written != height) {
            av_log(NULL, AV_LOG_ERROR, "sxmedia: sws_scale wrote %d rows, wanted %d\n", written, height);
            return -1;
        }
    }
    return 0;
}

static void* ffmpeg_resampler_open(void* user, const struct sx_media_frame* prototype,
                                   const struct sx_media_audio_format* dst) {
    const AVFrame* frame = (const AVFrame*)prototype->data;
    SxFFmpegResampler* resampler;
    AVChannelLayout out_layout;
    int status;

    (void)user;
    if (frame == NULL || dst == NULL || dst->sample_rate <= 0 || dst->channels <= 0) {
        return NULL;
    }
    resampler = (SxFFmpegResampler*)calloc(1, sizeof(*resampler));
    if (resampler == NULL) {
        return NULL;
    }
    av_channel_layout_default(&out_layout, dst->channels);
    status = swr_alloc_set_opts2(&resampler->context, &out_layout, AV_SAMPLE_FMT_S16, dst->sample_rate,
                                 &frame->ch_layout, (enum AVSampleFormat)frame->format, frame->sample_rate, 0,
                                 NULL);
    av_channel_layout_uninit(&out_layout);
    if (status < 0 || resampler->context == NULL || swr_init(resampler->context) < 0) {
        swr_free(&resampler->context);
        free(resampler);
        return NULL;
    }
    return resampler;
}

static void ffmpeg_resampler_close(void* opaque) {
    SxFFmpegResampler* resampler = (SxFFmpegResampler*)opaque;
    if (resampler == NULL) {
        return;
    }
    swr_free(&resampler->context);
    free(resampler);
}

static int ffmpeg_resample(void* opaque, const struct sx_media_frame* frame, int16_t* out,
                           int out_capacity) {
    SxFFmpegResampler* resampler = (SxFFmpegResampler*)opaque;
    const AVFrame* av_frame = (const AVFrame*)frame->data;
    uint8_t* planes[1];

    if (resampler == NULL || av_frame == NULL || out == NULL) {
        return -1;
    }
    planes[0] = (uint8_t*)out;
    return swr_convert(resampler->context, planes, out_capacity, (const uint8_t**)av_frame->extended_data,
                       av_frame->nb_samples);
}

static int ffmpeg_resample_flush(void* opaque, int16_t* out, int out_capacity) {
    SxFFmpegResampler* resampler = (SxFFmpegResampler*)opaque;
    uint8_t* planes[1];

    if (resampler == NULL || out == NULL) {
        return -1;
    }
    planes[0] = (uint8_t*)out;
    return swr_convert(resampler->context, planes, out_capacity, NULL, 0);
}

/* ---- the backend --------------------------------------------------------- */

static const struct sx_media_backend_ops kFFmpegOps = {
    ffmpeg_claim_source, ffmpeg_open, ffmpeg_close, ffmpeg_seek, ffmpeg_read_packet,
    ffmpeg_container_name, ffmpeg_metadata,
    ffmpeg_claim_stream, ffmpeg_open_decoder, ffmpeg_close_decoder, ffmpeg_flush,
    ffmpeg_send_packet, ffmpeg_receive_frame,
    /* The whole-source decoder shape, absent: libavcodec is fed packets and
     * wants nothing else. The two entries have to be here anyway -- they sit in
     * the middle of the table, and leaving them out shifts every converter
     * after them. */
    NULL, NULL,
    ffmpeg_scaler_open, ffmpeg_scaler_close, ffmpeg_scale,
    ffmpeg_resampler_open, ffmpeg_resampler_close, ffmpeg_resample, ffmpeg_resample_flush,
};

static struct sx_media_backend g_ffmpeg_backend = {
    "ffmpeg", SX_MEDIA_BACKEND_ABI, 0, &kFFmpegOps, NULL,
};

/* The av_log calls on the converter's failure paths are deliberate. A scaler
 * that returns NULL without a word is indistinguishable from a window that
 * forgot to paint, and finding which one it was took two smoke runs. They are
 * diagnostics on stderr, not a notice: a program that shows a message to a
 * person still names a codec and never a library.
 *
 * The one symbol the program needs from this file.
 *
 * The log level is set here rather than in the program on purpose: it was
 * `av_log_set_level(AV_LOG_ERROR)` in the player's main, which meant the window
 * had a libavutil include and a library call for a decision that belongs to the
 * library. A program that registers this backend has no way to set it, and does
 * not need to. */
int sxmedia_ffmpeg_register(void) {
    /* FFmpeg's warnings go to stdout, which nobody reads on a desktop and which
     * pollutes the harness log. */
    av_log_set_level(AV_LOG_ERROR);
    return sx_media_register_backend(&g_ffmpeg_backend);
}
