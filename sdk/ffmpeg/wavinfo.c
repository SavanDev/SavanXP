/* Primer consumidor de FFmpeg en SavanXP: abre un archivo, informa lo que hay
 * adentro y lo decodifica entero.
 *
 * Deliberadamente no dibuja ni suena nada. Lo que prueba es la cadena completa
 * de libav* sobre esta libc: protocolo file, demuxer, decoder, el allocator, el
 * logging y el punto flotante. Si esto corre, el port existe.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>

static const char* media_type_name(enum AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return "video";
        case AVMEDIA_TYPE_AUDIO: return "audio";
        case AVMEDIA_TYPE_SUBTITLE: return "subtitulo";
        default: return "otro";
    }
}

int main(int argc, char** argv) {
    const char* path = argc >= 2 ? argv[1] : "/disk/media/tono.wav";
    AVFormatContext* format = NULL;
    const AVCodec* codec = NULL;
    AVCodecContext* decoder = NULL;
    AVPacket* packet = NULL;
    AVFrame* frame = NULL;
    long total_samples = 0;
    long total_frames = 0;
    long total_packets = 0;
    int stream_index = -1;
    int status = 0;
    int index = 0;

    printf("wavinfo: FFmpeg %s sobre SavanXP\n", av_version_info());
    printf("wavinfo: abriendo %s\n", path);

    status = avformat_open_input(&format, path, NULL, NULL);
    if (status < 0) {
        printf("wavinfo: avformat_open_input fallo (%d)\n", status);
        return 1;
    }

    status = avformat_find_stream_info(format, NULL);
    if (status < 0) {
        printf("wavinfo: avformat_find_stream_info fallo (%d)\n", status);
        avformat_close_input(&format);
        return 1;
    }

    printf("wavinfo: formato '%s', %u stream(s), duracion %ld\n",
           format->iformat != NULL ? format->iformat->name : "?",
           format->nb_streams, (long)format->duration);

    for (index = 0; index < (int)format->nb_streams; ++index) {
        const AVCodecParameters* params = format->streams[index]->codecpar;
        printf("  stream %d: %s codec_id=%d rate=%d canales=%d bits=%d\n",
               index, media_type_name(params->codec_type), (int)params->codec_id,
               params->sample_rate, params->ch_layout.nb_channels,
               params->bits_per_coded_sample);
        if (stream_index < 0 && params->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = index;
        }
    }

    if (stream_index < 0) {
        printf("wavinfo: no hay stream de audio\n");
        avformat_close_input(&format);
        return 1;
    }

    codec = avcodec_find_decoder(format->streams[stream_index]->codecpar->codec_id);
    if (codec == NULL) {
        printf("wavinfo: no hay decoder para el codec\n");
        avformat_close_input(&format);
        return 1;
    }
    printf("wavinfo: decoder '%s' (%s)\n", codec->name,
           codec->long_name != NULL ? codec->long_name : "");

    decoder = avcodec_alloc_context3(codec);
    if (decoder == NULL) {
        printf("wavinfo: avcodec_alloc_context3 fallo\n");
        avformat_close_input(&format);
        return 1;
    }
    status = avcodec_parameters_to_context(decoder, format->streams[stream_index]->codecpar);
    if (status < 0) {
        printf("wavinfo: avcodec_parameters_to_context fallo (%d)\n", status);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 1;
    }
    status = avcodec_open2(decoder, codec, NULL);
    if (status < 0) {
        printf("wavinfo: avcodec_open2 fallo (%d)\n", status);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 1;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL) {
        printf("wavinfo: sin memoria para packet/frame\n");
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 1;
    }

    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream_index) {
            total_packets += 1;
            if (avcodec_send_packet(decoder, packet) >= 0) {
                while (avcodec_receive_frame(decoder, frame) >= 0) {
                    total_frames += 1;
                    total_samples += frame->nb_samples;
                }
            }
        }
        av_packet_unref(packet);
    }

    /* Vaciar el decoder: los ultimos frames pueden estar adentro. */
    if (avcodec_send_packet(decoder, NULL) >= 0) {
        while (avcodec_receive_frame(decoder, frame) >= 0) {
            total_frames += 1;
            total_samples += frame->nb_samples;
        }
    }

    printf("wavinfo: %ld paquetes, %ld frames, %ld samples\n",
           total_packets, total_frames, total_samples);
    if (decoder->sample_rate > 0) {
        printf("wavinfo: duracion decodificada %.3f s\n",
               (double)total_samples / (double)decoder->sample_rate);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);

    printf(total_samples > 0 ? "WAVINFO PASS\n" : "WAVINFO FAIL\n");
    return total_samples > 0 ? 0 : 1;
}
