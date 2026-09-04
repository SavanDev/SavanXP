/* Player de video sobre FFmpeg para SavanXP.
 *
 * Decodifica con libavcodec, convierte YUV->RGB con swscale y presenta por
 * /dev/gpu0, el mismo camino que usa gputest.
 *
 * Dos modos:
 *
 *   player <archivo>              toma la pantalla y lo muestra
 *   player --selftest <archivo>   decodifica y convierte todo sin pantalla,
 *                                 verifica lo que se puede verificar sin ojos y
 *                                 reporta. Es el que corre en el smoke.
 *   player --hold <ms> <archivo>  muestra y despues DEJA el ultimo cuadro en
 *                                 pantalla ese tiempo, anunciandolo por serial.
 *                                 Es lo que hace posible una captura: sin eso
 *                                 el clip termina en un segundo y no hay
 *                                 momento determinista para sacarla.
 *
 * El selftest existe porque "se ve bien" no es algo que un harness pueda
 * comprobar, pero "todos los cuadros decodificaron, con el tamano declarado, y
 * la conversion produjo pixeles que no son todos iguales" si.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/libc.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* El framebuffer de SavanXP es XRGB8888 en un uint32 (ver gfx_rgb), o sea
 * B,G,R,X en memoria: eso es exactamente AV_PIX_FMT_BGRA. */
#define PLAYER_PIXEL_FORMAT AV_PIX_FMT_BGRA

struct player {
    AVFormatContext* format;
    AVCodecContext* decoder;
    struct SwsContext* scaler;
    AVFrame* frame;
    AVFrame* rgb;
    AVPacket* packet;
    uint8_t* rgb_buffer;
    int stream_index;
    int width;
    int height;
};

static void player_close(struct player* state) {
    if (state->scaler != NULL) {
        sws_freeContext(state->scaler);
        state->scaler = NULL;
    }
    av_freep(&state->rgb_buffer);
    av_frame_free(&state->rgb);
    av_frame_free(&state->frame);
    av_packet_free(&state->packet);
    avcodec_free_context(&state->decoder);
    if (state->format != NULL) {
        avformat_close_input(&state->format);
    }
}

static int player_open(struct player* state, const char* path, int target_width, int target_height) {
    const AVCodec* codec = NULL;
    int status = 0;
    int index = 0;

    memset(state, 0, sizeof(*state));
    state->stream_index = -1;

    status = avformat_open_input(&state->format, path, NULL, NULL);
    if (status < 0) {
        printf("player: no se pudo abrir '%s' (%d)\n", path, status);
        return 0;
    }
    if (avformat_find_stream_info(state->format, NULL) < 0) {
        printf("player: sin informacion de streams\n");
        player_close(state);
        return 0;
    }

    for (index = 0; index < (int)state->format->nb_streams; ++index) {
        if (state->format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            state->stream_index = index;
            break;
        }
    }
    if (state->stream_index < 0) {
        printf("player: el archivo no tiene video\n");
        player_close(state);
        return 0;
    }

    codec = avcodec_find_decoder(
        state->format->streams[state->stream_index]->codecpar->codec_id);
    if (codec == NULL) {
        printf("player: no hay decoder para el codec\n");
        player_close(state);
        return 0;
    }

    state->decoder = avcodec_alloc_context3(codec);
    if (state->decoder == NULL ||
        avcodec_parameters_to_context(
            state->decoder, state->format->streams[state->stream_index]->codecpar) < 0 ||
        avcodec_open2(state->decoder, codec, NULL) < 0) {
        printf("player: no se pudo abrir el decoder\n");
        player_close(state);
        return 0;
    }

    state->width = target_width > 0 ? target_width : state->decoder->width;
    state->height = target_height > 0 ? target_height : state->decoder->height;

    state->frame = av_frame_alloc();
    state->rgb = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (state->frame == NULL || state->rgb == NULL || state->packet == NULL) {
        printf("player: sin memoria\n");
        player_close(state);
        return 0;
    }

    {
        const int bytes = av_image_get_buffer_size(PLAYER_PIXEL_FORMAT, state->width, state->height, 1);
        if (bytes <= 0) {
            printf("player: tamano de imagen invalido\n");
            player_close(state);
            return 0;
        }
        state->rgb_buffer = (uint8_t*)av_malloc((size_t)bytes);
        if (state->rgb_buffer == NULL) {
            printf("player: sin memoria para el buffer RGB\n");
            player_close(state);
            return 0;
        }
        av_image_fill_arrays(state->rgb->data, state->rgb->linesize, state->rgb_buffer,
                             PLAYER_PIXEL_FORMAT, state->width, state->height, 1);
    }

    printf("player: '%s' %s %dx%d -> %dx%d\n", path, codec->name,
           state->decoder->width, state->decoder->height, state->width, state->height);
    return 1;
}

/* Crea el escalador cuando ya se conoce el formato real del primer cuadro: el
 * pix_fmt del AVCodecContext puede no estar resuelto hasta ese momento. */
static int player_ensure_scaler(struct player* state) {
    if (state->scaler != NULL) {
        return 1;
    }
    state->scaler = sws_getContext(
        state->frame->width, state->frame->height, (enum AVPixelFormat)state->frame->format,
        state->width, state->height, PLAYER_PIXEL_FORMAT,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (state->scaler == NULL) {
        printf("player: sws_getContext fallo (formato %d)\n", state->frame->format);
        return 0;
    }
    return 1;
}

static int player_convert(struct player* state) {
    if (!player_ensure_scaler(state)) {
        return 0;
    }
    sws_scale(state->scaler, (const uint8_t* const*)state->frame->data, state->frame->linesize,
              0, state->frame->height, state->rgb->data, state->rgb->linesize);
    return 1;
}

/* Cuenta cuantos colores distintos hay en una muestra del cuadro. Un cuadro que
 * no se decodifico -- o que quedo todo negro -- da 1. */
static int distinct_sample_colors(const uint32_t* pixels, int width, int height, int stride_pixels) {
    uint32_t seen[16];
    int count = 0;
    int y = 0;

    for (y = 0; y < height; y += (height / 8) > 0 ? (height / 8) : 1) {
        int x = 0;
        for (x = 0; x < width; x += (width / 8) > 0 ? (width / 8) : 1) {
            const uint32_t value = pixels[(y * stride_pixels) + x] & 0x00ffffffu;
            int index = 0;
            int found = 0;
            for (index = 0; index < count; ++index) {
                if (seen[index] == value) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (count == (int)(sizeof(seen) / sizeof(seen[0]))) {
                    return count;
                }
                seen[count++] = value;
            }
        }
    }
    return count;
}

static int run_selftest(const char* path) {
    struct player state;
    long frames = 0;
    long converted = 0;
    int failures = 0;
    int first_width = 0;
    int first_height = 0;

    if (!player_open(&state, path, 0, 0)) {
        return 1;
    }

    while (av_read_frame(state.format, state.packet) >= 0) {
        if (state.packet->stream_index == state.stream_index &&
            avcodec_send_packet(state.decoder, state.packet) >= 0) {
            while (avcodec_receive_frame(state.decoder, state.frame) >= 0) {
                frames += 1;
                if (first_width == 0) {
                    first_width = state.frame->width;
                    first_height = state.frame->height;
                }
                if (state.frame->width != first_width || state.frame->height != first_height) {
                    printf("player: el cuadro %ld cambio de tamano (%dx%d)\n",
                           frames, state.frame->width, state.frame->height);
                    failures += 1;
                }
                if (player_convert(&state)) {
                    const int colors = distinct_sample_colors(
                        (const uint32_t*)state.rgb->data[0], state.width, state.height,
                        state.rgb->linesize[0] / 4);
                    if (colors < 2) {
                        printf("player: el cuadro %ld convertido tiene %d color(es)\n",
                               frames, colors);
                        failures += 1;
                    }
                    converted += 1;
                }
            }
        }
        av_packet_unref(state.packet);
    }

    printf("player: %ld cuadros decodificados, %ld convertidos, %dx%d\n",
           frames, converted, first_width, first_height);

    if (frames == 0 || converted != frames || failures != 0) {
        printf("PLAYER FAIL\n");
        player_close(&state);
        return 1;
    }
    printf("PLAYER PASS\n");
    player_close(&state);
    return 0;
}

/* El display va por /dev/gpu0 directo (gpu_open/gpu_acquire/gpu_present), no
 * por gfx_open: ese camino es el de un cliente del WM, que mapea un fd heredado
 * de windowd. Un proceso lanzado por init -- que es como corre esto en el
 * harness -- no lo tiene, y gfx_open devuelve ENODEV. Es el mismo camino que
 * usa gputest. */
/* Centra el cuadro en el framebuffer. No escala a pantalla completa a
 * proposito: lo que se quiere ver es el cuadro tal cual salio del decoder. */
static void blit_centered(const struct savanxp_fb_info* info, uint32_t* target,
                          const uint32_t* source, int width, int height, int source_stride) {
    const uint32_t fb_stride = gfx_stride_pixels(info);
    const int offset_x = ((int)info->width - width) / 2;
    const int offset_y = ((int)info->height - height) / 2;
    int y = 0;

    for (y = 0; y < height; ++y) {
        const int destination_y = offset_y + y;
        if (destination_y < 0 || destination_y >= (int)info->height) {
            continue;
        }
        memcpy(&target[((uint32_t)destination_y * fb_stride) + (uint32_t)(offset_x > 0 ? offset_x : 0)],
               &source[(size_t)y * (size_t)source_stride],
               (size_t)width * sizeof(uint32_t));
    }
}

static int run_display(const char* path, unsigned long hold_ms) {
    struct savanxp_gpu_info gpu_info = {};
    struct savanxp_fb_info fb_info = {};
    struct player state;
    uint32_t* framebuffer = NULL;
    long gpu_fd = -1;
    long frames = 0;

    gpu_fd = gpu_open();
    if (gpu_fd < 0) {
        printf("player: /dev/gpu0 no disponible\n");
        return 1;
    }
    if (gpu_get_info((int)gpu_fd, &gpu_info) < 0) {
        printf("player: GPU_IOC_GET_INFO fallo\n");
        savanxp_close((int)gpu_fd);
        return 1;
    }
    /* El backbuffer se pide del tamano real de la pantalla y no como un arreglo
     * estatico: la BSS se mapea entera al exec, asi que dimensionarlo para el
     * peor caso serian megabytes residentes en cada corrida. */
    framebuffer = (uint32_t*)malloc(gpu_info.buffer_size);
    if (framebuffer == NULL) {
        printf("player: sin memoria para el backbuffer (%u bytes)\n", gpu_info.buffer_size);
        savanxp_close((int)gpu_fd);
        return 1;
    }
    if (gpu_acquire((int)gpu_fd) < 0) {
        printf("player: GPU_IOC_ACQUIRE fallo\n");
        savanxp_close((int)gpu_fd);
        return 1;
    }

    fb_info.width = gpu_info.width;
    fb_info.height = gpu_info.height;
    fb_info.pitch = gpu_info.pitch;
    fb_info.bpp = gpu_info.bpp;
    fb_info.buffer_size = gpu_info.buffer_size;

    if (!player_open(&state, path, 0, 0)) {
        free(framebuffer);
        gpu_release((int)gpu_fd);
        savanxp_close((int)gpu_fd);
        return 1;
    }

    gfx_clear(framebuffer, &fb_info, gfx_rgb(0, 0, 0));

    while (av_read_frame(state.format, state.packet) >= 0) {
        if (state.packet->stream_index == state.stream_index &&
            avcodec_send_packet(state.decoder, state.packet) >= 0) {
            while (avcodec_receive_frame(state.decoder, state.frame) >= 0) {
                if (!player_convert(&state)) {
                    continue;
                }
                blit_centered(&fb_info, framebuffer, (const uint32_t*)state.rgb->data[0],
                              state.width, state.height, state.rgb->linesize[0] / 4);
                if (gpu_present((int)gpu_fd, framebuffer) < 0) {
                    printf("player: GPU_IOC_PRESENT fallo\n");
                    break;
                }
                frames += 1;
                sleep_ms(40); /* ~25 cuadros por segundo */
            }
        }
        av_packet_unref(state.packet);
    }

    printf("player: %ld cuadros presentados\n", frames);
    if (hold_ms != 0) {
        /* El ultimo cuadro ya esta presentado: recien aca hay algo estable en
         * pantalla para capturar. */
        puts_out("PLAYER DISPLAY READY\n");
        sleep_ms(hold_ms);
    }
    player_close(&state);
    free(framebuffer);
    gpu_release((int)gpu_fd);
    savanxp_close((int)gpu_fd);
    return frames > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* path = "/disk/media/clip.mjpeg";
    unsigned long hold_ms = 0;
    int selftest = 0;
    int index = 1;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--selftest") == 0) {
            selftest = 1;
        } else if (strcmp(argv[index], "--hold") == 0 && (index + 1) < argc) {
            hold_ms = (unsigned long)strtol(argv[++index], 0, 10);
        } else {
            path = argv[index];
        }
    }

    printf("player: FFmpeg %s sobre SavanXP\n", av_version_info());
    if (selftest) {
        return run_selftest(path);
    }
    {
        const int status = run_display(path, hold_ms);
        if (status == 0 && hold_ms != 0) {
            puts_out("PLAYER PASS\n");
        }
        return status;
    }
}
