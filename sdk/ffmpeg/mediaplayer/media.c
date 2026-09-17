#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* El framebuffer de SavanXP es XRGB8888 en un uint32 con el byte alto en cero
 * (ver gfx_rgb): B,G,R,0 en memoria, que es exactamente AV_PIX_FMT_BGR0. BGRA
 * pondria 0xff en ese byte. */
#define MEDIA_PIXEL_FORMAT AV_PIX_FMT_BGR0

/* Tope de paquetes por cola. Solo se llega si una de las dos colas no se
 * consume durante minutos; antes de crecer sin limite se descarta. */
#define MEDIA_QUEUE_LIMIT 8192

#define MEDIA_US_PER_SECOND 1000000LL

/* ---- cola de paquetes ---------------------------------------------------- */

static int queue_push(struct media_packet_queue* queue, AVPacket* source) {
    AVPacket* packet = NULL;

    if (queue->count == queue->capacity) {
        int capacity = queue->capacity == 0 ? 64 : queue->capacity * 2;
        AVPacket** items = NULL;
        int index = 0;

        if (capacity > MEDIA_QUEUE_LIMIT) {
            queue->dropped += 1;
            av_packet_unref(source);
            return 0;
        }
        items = (AVPacket**)malloc((size_t)capacity * sizeof(AVPacket*));
        if (items == NULL) {
            queue->dropped += 1;
            av_packet_unref(source);
            return 0;
        }
        /* Se desenrolla el anillo al copiarlo: la cabeza queda en 0. */
        for (index = 0; index < queue->count; ++index) {
            items[index] = queue->items[(queue->head + index) % queue->capacity];
        }
        free(queue->items);
        queue->items = items;
        queue->capacity = capacity;
        queue->head = 0;
    }

    packet = av_packet_alloc();
    if (packet == NULL) {
        queue->dropped += 1;
        av_packet_unref(source);
        return 0;
    }
    av_packet_move_ref(packet, source);
    queue->items[(queue->head + queue->count) % queue->capacity] = packet;
    queue->count += 1;
    return 1;
}

static AVPacket* queue_pop(struct media_packet_queue* queue) {
    AVPacket* packet = NULL;

    if (queue->count == 0) {
        return NULL;
    }
    packet = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count -= 1;
    return packet;
}

static void queue_clear(struct media_packet_queue* queue) {
    AVPacket* packet = NULL;
    while ((packet = queue_pop(queue)) != NULL) {
        av_packet_free(&packet);
    }
    queue->head = 0;
}

static void queue_free(struct media_packet_queue* queue) {
    queue_clear(queue);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

/* ---- tiempos ------------------------------------------------------------- */

static int64_t stream_time_us(const struct media* media, int stream_index, int64_t timestamp) {
    if (timestamp == AV_NOPTS_VALUE || stream_index < 0) {
        return MEDIA_NO_TIME;
    }
    return av_rescale_q(timestamp, media->format->streams[stream_index]->time_base, AV_TIME_BASE_Q) -
           media->start_time_us;
}

/* ---- apertura ------------------------------------------------------------ */

static void copy_error(char* error, size_t capacity, const char* text) {
    if (error != NULL && capacity != 0) {
        snprintf(error, capacity, "%s", text);
    }
}

static void stream_reset(struct media_stream* stream) {
    memset(stream, 0, sizeof(*stream));
    stream->index = -1;
    stream->next_time_us = MEDIA_NO_TIME;
}

static int open_stream(struct media* media, struct media_stream* stream, enum AVMediaType type) {
    const AVCodec* codec = NULL;
    AVStream* av_stream = NULL;
    int index = av_find_best_stream(media->format, type, -1, -1, &codec, 0);

    if (index < 0 || codec == NULL) {
        return 0;
    }
    av_stream = media->format->streams[index];
    /* La tapa de un disco en un MP3 llega como un stream de video de un solo
     * cuadro. Mostrarla es una mejora futura; reproducirla como video no. */
    if (type == AVMEDIA_TYPE_VIDEO && (av_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
        return 0;
    }

    stream->decoder = avcodec_alloc_context3(codec);
    if (stream->decoder == NULL) {
        return 0;
    }
    if (avcodec_parameters_to_context(stream->decoder, av_stream->codecpar) < 0) {
        avcodec_free_context(&stream->decoder);
        return 0;
    }
    stream->decoder->pkt_timebase = av_stream->time_base;
    /* Sin hilos en el kernel: un decoder que intente crearlos no arranca. */
    stream->decoder->thread_count = 1;
    if (avcodec_open2(stream->decoder, codec, NULL) < 0) {
        avcodec_free_context(&stream->decoder);
        return 0;
    }
    stream->index = index;
    return 1;
}

int media_open(struct media* media, const char* path, const struct media_audio_format* audio_out,
               char* error, size_t error_capacity) {
    memset(media, 0, sizeof(*media));
    stream_reset(&media->video);
    stream_reset(&media->audio);
    media->video_skip_until_us = MEDIA_NO_TIME;
    media->audio_skip_until_us = MEDIA_NO_TIME;
    media->video_time_us = MEDIA_NO_TIME;
    media->shown_time_us = MEDIA_NO_TIME;

    if (avformat_open_input(&media->format, path, NULL, NULL) < 0) {
        copy_error(error, error_capacity, "cannot open the file");
        return 0;
    }
    if (avformat_find_stream_info(media->format, NULL) < 0) {
        copy_error(error, error_capacity, "cannot read the stream information");
        media_close(media);
        return 0;
    }

    media->start_time_us = media->format->start_time != AV_NOPTS_VALUE ? media->format->start_time : 0;
    media->duration_us = media->format->duration != AV_NOPTS_VALUE ? media->format->duration : 0;
    media->seekable = media->format->pb != NULL && (media->format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0 &&
                      media->duration_us > 0;

    (void)open_stream(media, &media->video, AVMEDIA_TYPE_VIDEO);
    if (audio_out != NULL && audio_out->sample_rate > 0 && audio_out->channels > 0) {
        media->audio_out = *audio_out;
        (void)open_stream(media, &media->audio, AVMEDIA_TYPE_AUDIO);
    }

    if (media->video.index < 0 && media->audio.index < 0) {
        copy_error(error, error_capacity, "no stream with a supported codec");
        media_close(media);
        return 0;
    }

    media->packet = av_packet_alloc();
    media->video_frame = av_frame_alloc();
    media->shown_frame = av_frame_alloc();
    media->audio_frame = av_frame_alloc();
    if (media->packet == NULL || media->video_frame == NULL || media->shown_frame == NULL ||
        media->audio_frame == NULL) {
        copy_error(error, error_capacity, "out of memory");
        media_close(media);
        return 0;
    }
    return 1;
}

void media_close(struct media* media) {
    queue_free(&media->video.packets);
    queue_free(&media->audio.packets);
    avcodec_free_context(&media->video.decoder);
    avcodec_free_context(&media->audio.decoder);
    if (media->scaler != NULL) {
        sws_freeContext(media->scaler);
        media->scaler = NULL;
    }
    swr_free(&media->resampler);
    av_freep(&media->scaled);
    free(media->audio_buffer);
    media->audio_buffer = NULL;
    av_frame_free(&media->video_frame);
    av_frame_free(&media->shown_frame);
    av_frame_free(&media->audio_frame);
    av_packet_free(&media->packet);
    if (media->format != NULL) {
        avformat_close_input(&media->format);
    }
}

int media_has_video(const struct media* media) {
    return media->video.index >= 0;
}

int media_has_audio(const struct media* media) {
    return media->audio.index >= 0;
}

void media_display_size(const struct media* media, int* width, int* height) {
    AVStream* stream = NULL;
    AVRational aspect;
    int w = 0;
    int h = 0;

    if (media->video.index >= 0) {
        stream = media->format->streams[media->video.index];
        w = stream->codecpar->width;
        h = stream->codecpar->height;
        aspect = av_guess_sample_aspect_ratio(media->format, stream, NULL);
        if (aspect.num > 0 && aspect.den > 0 && aspect.num != aspect.den) {
            w = (int)av_rescale(w, aspect.num, aspect.den);
        }
    }
    *width = w;
    *height = h;
}

int64_t media_frame_duration_us(const struct media* media) {
    if (media->video.index >= 0) {
        AVRational rate = av_guess_frame_rate(media->format, media->format->streams[media->video.index], NULL);
        if (rate.num > 0 && rate.den > 0) {
            return av_rescale(MEDIA_US_PER_SECOND, rate.den, rate.num);
        }
    }
    return 40000;
}

const char* media_container_name(const struct media* media) {
    return media->format != NULL && media->format->iformat != NULL ? media->format->iformat->name : "?";
}

const char* media_video_codec_name(const struct media* media) {
    return media->video.decoder != NULL ? media->video.decoder->codec->name : "none";
}

const char* media_audio_codec_name(const struct media* media) {
    return media->audio.decoder != NULL ? media->audio.decoder->codec->name : "none";
}

/* ---- demultiplexado y decodificacion -------------------------------------- */

/* Lee UN paquete y lo deja en la cola de su stream. 0 = no hay mas. */
static int demux_one(struct media* media) {
    int status = 0;

    if (media->demux_finished) {
        return 0;
    }
    status = av_read_frame(media->format, media->packet);
    if (status == AVERROR(EAGAIN)) {
        return 1;
    }
    if (status < 0) {
        media->demux_finished = 1;
        return 0;
    }
    if (media->packet->stream_index == media->video.index && media->video.index >= 0) {
        (void)queue_push(&media->video.packets, media->packet);
    } else if (media->packet->stream_index == media->audio.index && media->audio.index >= 0) {
        (void)queue_push(&media->audio.packets, media->packet);
    } else {
        av_packet_unref(media->packet);
    }
    return 1;
}

/* Saca el proximo cuadro del decoder de un stream, alimentandolo desde su cola
 * y demultiplexando cuando la cola esta vacia. 1 = cuadro, 0 = fin. */
static int stream_receive(struct media* media, struct media_stream* stream, AVFrame* frame) {
    for (;;) {
        AVPacket* packet = NULL;
        int status = 0;

        if (stream->finished) {
            return 0;
        }
        status = avcodec_receive_frame(stream->decoder, frame);
        if (status == 0) {
            return 1;
        }
        if (status != AVERROR(EAGAIN)) {
            stream->finished = 1;
            return 0;
        }

        packet = queue_pop(&stream->packets);
        if (packet != NULL) {
            /* Un paquete corrupto devuelve error y se saltea: el decoder se
             * resincroniza solo en el proximo keyframe. */
            (void)avcodec_send_packet(stream->decoder, packet);
            av_packet_free(&packet);
            continue;
        }
        if (demux_one(media)) {
            continue;
        }
        if (!stream->flushed) {
            (void)avcodec_send_packet(stream->decoder, NULL);
            stream->flushed = 1;
            continue;
        }
        stream->finished = 1;
        return 0;
    }
}

int media_next_video_frame(struct media* media) {
    const int64_t frame_duration = media_frame_duration_us(media);

    if (media->video.index < 0) {
        return 0;
    }
    for (;;) {
        int64_t time_us = MEDIA_NO_TIME;

        if (!stream_receive(media, &media->video, media->video_frame)) {
            return 0;
        }
        time_us = stream_time_us(media, media->video.index, media->video_frame->best_effort_timestamp);
        if (time_us == MEDIA_NO_TIME) {
            time_us = media->video.next_time_us != MEDIA_NO_TIME ? media->video.next_time_us : 0;
        }
        media->video.next_time_us = time_us + frame_duration;

        /* Seek preciso: el cuadro que se ve en target es el que empieza antes y
         * todavia no termino. Los anteriores se decodifican -- hacen falta como
         * referencia -- pero no se entregan. */
        if (media->video_skip_until_us != MEDIA_NO_TIME) {
            if (time_us + frame_duration <= media->video_skip_until_us) {
                continue;
            }
            media->video_skip_until_us = MEDIA_NO_TIME;
        }
        media->video_time_us = time_us;
        return 1;
    }
}

void media_show_video_frame(struct media* media) {
    av_frame_unref(media->shown_frame);
    av_frame_move_ref(media->shown_frame, media->video_frame);
    media->shown_time_us = media->video_time_us;
}

int media_has_shown_frame(const struct media* media) {
    return media->shown_frame != NULL && media->shown_frame->data[0] != NULL;
}

const char* media_metadata(const struct media* media, const char* key) {
    const AVDictionaryEntry* entry = NULL;
    if (media->format == NULL) {
        return NULL;
    }
    entry = av_dict_get(media->format->metadata, key, NULL, 0);
    return entry != NULL && entry->value != NULL && entry->value[0] != '\0' ? entry->value : NULL;
}

const uint32_t* media_scale_video(struct media* media, int width, int height) {
    AVFrame* frame = media->shown_frame;
    uint8_t* planes[4] = {NULL, NULL, NULL, NULL};
    int strides[4] = {0, 0, 0, 0};

    if (frame == NULL || frame->data[0] == NULL || width <= 0 || height <= 0) {
        return NULL;
    }
    if (width != media->scaled_width || height != media->scaled_height || media->scaled == NULL) {
        av_freep(&media->scaled);
        /* Una fila de mas: swscale puede leer/escribir un poco pasado el final
         * del ultimo renglon en algunos caminos. */
        media->scaled = (uint8_t*)av_malloc((size_t)width * (size_t)(height + 1) * 4u);
        if (media->scaled == NULL) {
            return NULL;
        }
        media->scaled_width = width;
        media->scaled_height = height;
    }

    /* FAST_BILINEAR y no BILINEAR: sin asm la diferencia de costo es grande y en
     * video en movimiento la de calidad casi no se ve. */
    media->scaler = sws_getCachedContext(media->scaler, frame->width, frame->height,
                                         (enum AVPixelFormat)frame->format, width, height,
                                         MEDIA_PIXEL_FORMAT, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (media->scaler == NULL) {
        return NULL;
    }
    planes[0] = media->scaled;
    strides[0] = width * 4;
    sws_scale(media->scaler, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, planes,
              strides);
    return (const uint32_t*)(void*)media->scaled;
}

/* ---- audio --------------------------------------------------------------- */

static int ensure_resampler(struct media* media, const AVFrame* frame) {
    AVChannelLayout out_layout;
    int status = 0;

    if (media->resampler != NULL && media->resampler_rate == frame->sample_rate &&
        media->resampler_format == frame->format && media->resampler_channels == frame->ch_layout.nb_channels) {
        return 1;
    }
    swr_free(&media->resampler);
    av_channel_layout_default(&out_layout, media->audio_out.channels);
    status = swr_alloc_set_opts2(&media->resampler, &out_layout, AV_SAMPLE_FMT_S16, media->audio_out.sample_rate,
                                 &frame->ch_layout, (enum AVSampleFormat)frame->format, frame->sample_rate, 0,
                                 NULL);
    av_channel_layout_uninit(&out_layout);
    if (status < 0 || swr_init(media->resampler) < 0) {
        swr_free(&media->resampler);
        return 0;
    }
    media->resampler_rate = frame->sample_rate;
    media->resampler_format = frame->format;
    media->resampler_channels = frame->ch_layout.nb_channels;
    return 1;
}

static int ensure_audio_capacity(struct media* media, int frames) {
    int16_t* buffer = NULL;

    if (frames <= media->audio_buffer_capacity) {
        return 1;
    }
    buffer = (int16_t*)realloc(media->audio_buffer,
                               (size_t)frames * (size_t)media->audio_out.channels * sizeof(int16_t));
    if (buffer == NULL) {
        return 0;
    }
    media->audio_buffer = buffer;
    media->audio_buffer_capacity = frames;
    return 1;
}

/* Decodifica y convierte el proximo bloque de audio al buffer interno, que tiene
 * que estar vacio. 1 = hay muestras nuevas, 0 = se termino. */
static int refill_audio(struct media* media) {
    for (;;) {
        AVFrame* frame = media->audio_frame;
        int64_t time_us = MEDIA_NO_TIME;
        int capacity = 0;
        int converted = 0;
        uint8_t* planes[1];

        media->audio_buffer_frames = 0;
        media->audio_buffer_offset = 0;

        if (!stream_receive(media, &media->audio, frame)) {
            /* Lo que el resampler todavia tiene adentro por su retardo. */
            if (media->resampler != NULL && ensure_audio_capacity(media, 4096)) {
                planes[0] = (uint8_t*)media->audio_buffer;
                converted = swr_convert(media->resampler, planes, media->audio_buffer_capacity, NULL, 0);
                swr_free(&media->resampler);
                if (converted > 0) {
                    media->audio_buffer_frames = converted;
                    media->audio_buffer_time_us = media->audio.next_time_us != MEDIA_NO_TIME
                                                      ? media->audio.next_time_us
                                                      : 0;
                    return 1;
                }
            }
            return 0;
        }
        if (!ensure_resampler(media, frame)) {
            continue;
        }

        time_us = stream_time_us(media, media->audio.index, frame->best_effort_timestamp);
        if (time_us == MEDIA_NO_TIME) {
            time_us = media->audio.next_time_us != MEDIA_NO_TIME ? media->audio.next_time_us : 0;
        }

        capacity = swr_get_out_samples(media->resampler, frame->nb_samples);
        if (capacity <= 0 || !ensure_audio_capacity(media, capacity)) {
            continue;
        }
        planes[0] = (uint8_t*)media->audio_buffer;
        converted = swr_convert(media->resampler, planes, capacity, (const uint8_t**)frame->extended_data,
                                frame->nb_samples);
        av_frame_unref(frame);
        if (converted <= 0) {
            continue;
        }
        media->audio_buffer_frames = converted;
        media->audio_buffer_time_us = time_us;
        media->audio.next_time_us = time_us + av_rescale(converted, MEDIA_US_PER_SECOND,
                                                         media->audio_out.sample_rate);
        return 1;
    }
}

static int64_t audio_offset_time(const struct media* media, int offset) {
    return media->audio_buffer_time_us + av_rescale(offset, MEDIA_US_PER_SECOND, media->audio_out.sample_rate);
}

int media_read_audio(struct media* media, int16_t* out, int frames, int64_t* first_time_us) {
    const int channels = media->audio_out.channels;
    int written = 0;

    if (first_time_us != NULL) {
        *first_time_us = MEDIA_NO_TIME;
    }
    if (media->audio.index < 0) {
        return 0;
    }

    while (written < frames) {
        int available = media->audio_buffer_frames - media->audio_buffer_offset;
        int count = 0;

        if (available <= 0) {
            if (!refill_audio(media)) {
                break;
            }
            continue;
        }

        /* Seek preciso del lado del audio: se descartan las muestras anteriores
         * al instante pedido, contando de a muestra y no de a bloque. */
        if (media->audio_skip_until_us != MEDIA_NO_TIME) {
            const int64_t now_us = audio_offset_time(media, media->audio_buffer_offset);
            if (now_us < media->audio_skip_until_us) {
                int64_t skip = av_rescale(media->audio_skip_until_us - now_us, media->audio_out.sample_rate,
                                          MEDIA_US_PER_SECOND);
                if (skip <= 0) {
                    skip = 1;
                }
                if (skip > available) {
                    skip = available;
                }
                media->audio_buffer_offset += (int)skip;
                continue;
            }
            media->audio_skip_until_us = MEDIA_NO_TIME;
        }

        count = available < (frames - written) ? available : (frames - written);
        if (written == 0 && first_time_us != NULL) {
            *first_time_us = audio_offset_time(media, media->audio_buffer_offset);
        }
        memcpy(out + ((size_t)written * (size_t)channels),
               media->audio_buffer + ((size_t)media->audio_buffer_offset * (size_t)channels),
               (size_t)count * (size_t)channels * sizeof(int16_t));
        media->audio_buffer_offset += count;
        written += count;
    }
    return written;
}

/* ---- seek ---------------------------------------------------------------- */

static void stream_flush(struct media_stream* stream) {
    queue_clear(&stream->packets);
    if (stream->decoder != NULL) {
        avcodec_flush_buffers(stream->decoder);
    }
    stream->flushed = 0;
    stream->finished = 0;
    stream->next_time_us = MEDIA_NO_TIME;
}

int media_seek(struct media* media, int64_t target_us) {
    int64_t timestamp = 0;

    if (media->format == NULL) {
        return 0;
    }
    if (target_us < 0) {
        target_us = 0;
    }
    if (media->duration_us > 0 && target_us > media->duration_us) {
        target_us = media->duration_us;
    }

    /* max_ts = target: el demuxer cae en el keyframe de ANTES, y la precision
     * la pone el descarte de *_skip_until_us. */
    timestamp = target_us + media->start_time_us;
    if (avformat_seek_file(media->format, -1, INT64_MIN, timestamp, timestamp, 0) < 0) {
        return 0;
    }

    media->demux_finished = 0;
    stream_flush(&media->video);
    stream_flush(&media->audio);
    swr_free(&media->resampler);
    media->audio_buffer_frames = 0;
    media->audio_buffer_offset = 0;
    media->video_time_us = MEDIA_NO_TIME;
    media->video_skip_until_us = target_us;
    media->audio_skip_until_us = target_us;
    return 1;
}
