/* Modos sin ventana del reproductor: las herramientas de prueba del port.
 *
 * Viven adentro del mismo binario porque cada ejecutable que registra un
 * backend de decodificacion se lleva todos los decoders habilitados (ver
 * link.sh): un wavinfo y un player aparte eran megas repetidos en una imagen de
 * 64 MiB.
 *
 *   --probe <archivo>                 lo que hay adentro, y decodificarlo entero
 *   --selftest [--sync] <archivo>...  decodifica, convierte, reposiciona y
 *                                     verifica; con --sync ademas mide la
 *                                     sincronia contra el clip de make-avclip.py
 *   --gpu-hold <ms> <archivo>         muestra el video por /dev/gpu0 y deja el
 *                                     ultimo cuadro fijo para la captura
 *
 * El selftest existe porque "se ve y se oye bien" no es algo que un harness
 * pueda asertar, pero "todos los cuadros decodificaron, el audio dura lo que
 * el archivo, el seek cae donde se pidio y el destello coincide con el pitido"
 * si. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/libc.h"

#include "playback.h"
#include "selftest.h"

#define SELFTEST_RATE 48000
#define SELFTEST_CHANNELS 2
#define SELFTEST_CHUNK 1024
#define SELFTEST_THUMB_W 64
#define SELFTEST_THUMB_H 48
#define SELFTEST_MAX_EVENTS 64
/* Umbrales del clip de make-avclip.py: destello blanco sobre fondo oscuro y
 * tono de amplitud ~0.5 sobre silencio. */
#define SELFTEST_FLASH_LUMA 180
#define SELFTEST_TONE_LEVEL 4000
#define SELFTEST_SYNC_TOLERANCE_US 20000

static long long abs_ll(long long value) {
    return value < 0 ? -value : value;
}

static void print_time(const char* label, int64_t us) {
    if (us == SX_MEDIA_NO_TIME) {
        printf("%s=?", label);
        return;
    }
    printf("%s=%lld.%03llds", label, (long long)(us / 1000000), (long long)(abs_ll(us) / 1000 % 1000));
}

/* Colores distintos en una grilla de muestras. Un cuadro sin decodificar, o
 * todo negro, da 1. */
static int distinct_colors(const uint32_t* pixels, int width, int height) {
    uint32_t seen[16];
    int count = 0;
    int y = 0;

    for (y = 0; y < height; y += 6) {
        int x = 0;
        for (x = 0; x < width; x += 8) {
            const uint32_t value = pixels[(y * width) + x] & 0x00ffffffu;
            int index = 0;
            for (index = 0; index < count && seen[index] != value; ++index) {
            }
            if (index == count) {
                if (count == (int)(sizeof(seen) / sizeof(seen[0]))) {
                    return count;
                }
                seen[count++] = value;
            }
        }
    }
    return count;
}

static int mean_luma(const uint32_t* pixels, int width, int height) {
    long long total = 0;
    int index = 0;
    for (index = 0; index < width * height; ++index) {
        const uint32_t value = pixels[index];
        total += (((value >> 16) & 0xff) * 3 + ((value >> 8) & 0xff) * 6 + (value & 0xff)) / 10;
    }
    return (int)(total / (width * height));
}

struct selftest_run {
    long video_frames;
    long audio_frames;
    int failures;
    int64_t last_video_us;
    int64_t audio_end_us;

    /* --sync: donde empieza cada destello y cada pitido */
    int64_t flashes[SELFTEST_MAX_EVENTS];
    int flash_count;
    int flash_on;
    int64_t tones[SELFTEST_MAX_EVENTS];
    int tone_count;
    long quiet_samples;
};

static void fail(struct selftest_run* run, const char* what) {
    printf("mediaplayer: FAIL %s\n", what);
    run->failures += 1;
}

static void check_video_frame(struct sx_media* media, struct selftest_run* run, int sync) {
    const uint32_t* thumb = NULL;
    const int64_t time_us = sx_media_video_time_us(media);

    sx_media_show_video_frame(media);
    run->video_frames += 1;
    if (run->last_video_us != SX_MEDIA_NO_TIME && time_us < run->last_video_us) {
        printf("mediaplayer: el cuadro %ld retrocede en el tiempo\n", run->video_frames);
        fail(run, "timestamps de video no monotonos");
    }
    run->last_video_us = time_us;

    thumb = sx_media_scale_video(media, SELFTEST_THUMB_W, SELFTEST_THUMB_H);
    if (thumb == NULL) {
        fail(run, "swscale no convirtio un cuadro");
        return;
    }
    if (distinct_colors(thumb, SELFTEST_THUMB_W, SELFTEST_THUMB_H) < 2) {
        printf("mediaplayer: el cuadro %ld tiene un solo color\n", run->video_frames);
        fail(run, "cuadro convertido plano");
    }
    if (sync) {
        const int flash = mean_luma(thumb, SELFTEST_THUMB_W, SELFTEST_THUMB_H) >= SELFTEST_FLASH_LUMA;
        if (flash && !run->flash_on && run->flash_count < SELFTEST_MAX_EVENTS) {
            run->flashes[run->flash_count++] = time_us;
        }
        run->flash_on = flash;
    }
}

static void check_audio_chunk(struct sx_media* media, struct selftest_run* run, const int16_t* samples, int frames,
                              int64_t first_us, int sync) {
    int index = 0;

    run->audio_frames += frames;
    if (first_us != SX_MEDIA_NO_TIME) {
        run->audio_end_us = first_us + (int64_t)frames * 1000000LL / SELFTEST_RATE;
    }
    if (!sync || first_us == SX_MEDIA_NO_TIME) {
        return;
    }
    for (index = 0; index < frames; ++index) {
        const int value = samples[index * SELFTEST_CHANNELS];
        if (value > SELFTEST_TONE_LEVEL || value < -SELFTEST_TONE_LEVEL) {
            /* Un tono empieza despues de al menos 50 ms de silencio. */
            if (run->quiet_samples >= SELFTEST_RATE / 20 && run->tone_count < SELFTEST_MAX_EVENTS) {
                run->tones[run->tone_count++] = first_us + (int64_t)index * 1000000LL / SELFTEST_RATE;
            }
            run->quiet_samples = 0;
        } else {
            run->quiet_samples += 1;
        }
    }
    (void)media;
}

/* Decodifica el archivo entero intercalando video y audio como lo hace la
 * reproduccion: el audio se pide hasta alcanzar al video. */
static void decode_everything(struct sx_media* media, struct selftest_run* run, int sync) {
    int16_t samples[SELFTEST_CHUNK * SELFTEST_CHANNELS];
    int video_done = !sx_media_has_video(media);
    int audio_done = !sx_media_has_audio(media);

    run->last_video_us = SX_MEDIA_NO_TIME;
    run->audio_end_us = 0;
    run->quiet_samples = SELFTEST_RATE;

    while (!video_done || !audio_done) {
        if (!video_done && (audio_done || sx_media_video_time_us(media) == SX_MEDIA_NO_TIME ||
                            sx_media_video_time_us(media) <= run->audio_end_us)) {
            if (sx_media_next_video_frame(media) > 0) {
                check_video_frame(media, run, sync);
            } else {
                video_done = 1;
            }
            continue;
        }
        {
            int64_t first_us = SX_MEDIA_NO_TIME;
            const int frames = sx_media_read_audio(media, samples, SELFTEST_CHUNK, &first_us);
            if (frames <= 0) {
                audio_done = 1;
            } else {
                check_audio_chunk(media, run, samples, frames, first_us, sync);
            }
        }
    }
}

static void check_sync(struct selftest_run* run, int64_t duration_us) {
    int expected = (int)(duration_us / 1000000);
    int index = 0;

    printf("mediaplayer: sync: %d destellos, %d pitidos (se esperan %d)\n", run->flash_count, run->tone_count,
           expected);
    if (run->flash_count != expected || run->tone_count != expected) {
        fail(run, "la cantidad de destellos/pitidos no es la del clip");
        return;
    }
    for (index = 0; index < expected; ++index) {
        const long long delta = (long long)(run->tones[index] - run->flashes[index]);
        if (abs_ll(delta) > SELFTEST_SYNC_TOLERANCE_US) {
            printf("mediaplayer: sync: evento %d: video ", index);
            print_time("t", run->flashes[index]);
            printf(" audio ");
            print_time("t", run->tones[index]);
            printf(" (%lld ms)\n", delta / 1000);
            fail(run, "audio y video desfasados");
        }
    }
}

/* Reposiciona a la mitad (redondeada a segundo, que en el clip de sync es un
 * destello con su pitido) y comprueba donde cae cada stream. */
static void check_seek(struct sx_media* media, struct selftest_run* run, int sync) {
    int16_t samples[SELFTEST_CHUNK * SELFTEST_CHANNELS];
    const int64_t duration_us = sx_media_duration_us(media);
    const int64_t frame_us = sx_media_frame_duration_us(media);
    int64_t target;

    if (!sx_media_is_seekable(media) || duration_us < 2000000) {
        printf("mediaplayer: seek: no aplica a este archivo\n");
        return;
    }
    target = (duration_us / 2) / 1000000 * 1000000;
    if (target == 0) {
        target = 1000000;
    }
    if (!sx_media_seek(media, target)) {
        fail(run, "sx_media_seek fallo");
        return;
    }
    if (sx_media_has_video(media)) {
        if (sx_media_next_video_frame(media) <= 0) {
            fail(run, "no hay video despues del seek");
        } else {
            const int64_t delta = sx_media_video_time_us(media) - target;
            printf("mediaplayer: seek a %llds: video ", (long long)(target / 1000000));
            print_time("t", sx_media_video_time_us(media));
            printf("\n");
            if (delta > frame_us / 2 || -delta >= frame_us) {
                fail(run, "el seek de video no cae en el cuadro pedido");
            }
            if (sync) {
                const uint32_t* thumb = NULL;
                sx_media_show_video_frame(media);
                thumb = sx_media_scale_video(media, SELFTEST_THUMB_W, SELFTEST_THUMB_H);
                if (thumb == NULL || mean_luma(thumb, SELFTEST_THUMB_W, SELFTEST_THUMB_H) < SELFTEST_FLASH_LUMA) {
                    fail(run, "el cuadro despues del seek no es el destello");
                }
            }
        }
    }
    if (sx_media_has_audio(media)) {
        int64_t first_us = SX_MEDIA_NO_TIME;
        const int frames = sx_media_read_audio(media, samples, SELFTEST_CHUNK, &first_us);
        printf("mediaplayer: seek a %llds: audio ", (long long)(target / 1000000));
        print_time("t", first_us);
        printf("\n");
        if (frames <= 0 || first_us == SX_MEDIA_NO_TIME || abs_ll(first_us - target) > 2000) {
            fail(run, "el seek de audio no cae en la muestra pedida");
        } else if (sync) {
            int index = 0;
            int loud = 0;
            /* El pitido arranca en el instante exacto: en los primeros 10 ms ya
             * tiene que haber senal. */
            for (index = 0; index < SELFTEST_RATE / 100 && index < frames; ++index) {
                const int value = samples[index * SELFTEST_CHANNELS];
                loud |= value > SELFTEST_TONE_LEVEL || value < -SELFTEST_TONE_LEVEL;
            }
            if (!loud) {
                fail(run, "el audio despues del seek no es el pitido");
            }
        }
    }
}

static int selftest_file(const char* path, int sync) {
    const struct sx_media_audio_format format = {SELFTEST_RATE, SELFTEST_CHANNELS};
    struct selftest_run run;
    struct sx_media* media = NULL;
    struct sx_media_source source;
    enum sx_media_status status = SX_MEDIA_OK;
    int64_t duration_us = 0;

    memset(&run, 0, sizeof(run));
    if (sx_media_create(&media) < 0) {
        printf("mediaplayer: FAIL %s: out of memory\n", path);
        return 1;
    }
    memset(&source, 0, sizeof(source));
    source.path = path;
    source.fd = -1;
    source.audio_out = &format;
    if (sx_media_open(media, &source, &status) < 0) {
        /* El motivo sale del engine y ya esta en su propia frase: el selftest
         * no vuelve a formatear el error. */
        printf("mediaplayer: FAIL %s: %s\n", path, sx_media_status_string(status));
        sx_media_destroy(media);
        return 1;
    }
    duration_us = sx_media_duration_us(media);
    printf("mediaplayer: %s: %s, video %s, audio %s, ", path, sx_media_container_name(media),
           sx_media_video_codec(media), sx_media_audio_codec(media));
    print_time("duracion", duration_us);
    printf("\n");
    if (sx_media_stream_missing_count(media) > 0) {
        char codec[SX_MEDIA_CODEC_CAPACITY];
        if (sx_media_stream_missing_at(media, 0, codec, sizeof(codec))) {
            printf("mediaplayer:   sin stream %s en este build\n", codec);
        }
    }

    decode_everything(media, &run, sync);
    printf("mediaplayer: %ld cuadros hasta ", run.video_frames);
    print_time("t", run.last_video_us);
    printf(", %ld muestras hasta ", run.audio_frames);
    print_time("t", run.audio_end_us);
    printf("\n");

    if (sx_media_has_video(media) && run.video_frames == 0) {
        fail(&run, "el stream de video no entrego cuadros");
    }
    if (sx_media_has_audio(media)) {
        const long long tolerance = 100000 + duration_us / 50;
        if (run.audio_frames == 0) {
            fail(&run, "el stream de audio no entrego muestras");
        } else if (duration_us > 0 && abs_ll(run.audio_end_us - duration_us) > tolerance) {
            fail(&run, "el audio no dura lo que el archivo");
        }
    }
    if (sx_media_packet_drops(media) != 0) {
        fail(&run, "se descartaron paquetes por cola llena");
    }
    if (sync) {
        check_sync(&run, duration_us);
    }
    check_seek(media, &run, sync);

    sx_media_close(media);
    sx_media_destroy(media);
    return run.failures;
}

int selftest_main(int argc, char** argv) {
    int failures = 0;
    int files = 0;
    int sync = 0;
    int index = 0;

    for (index = 0; index < argc; ++index) {
        if (strcmp(argv[index], "--sync") == 0) {
            sync = 1;
            continue;
        }
        failures += selftest_file(argv[index], sync);
        files += 1;
        sync = 0;
    }
    if (files == 0) {
        printf("mediaplayer: --selftest necesita al menos un archivo\n");
        failures += 1;
    }
    printf(failures == 0 ? "MEDIAPLAYER PASS\n" : "MEDIAPLAYER FAIL\n");
    return failures == 0 ? 0 : 1;
}

/* ---- --probe ------------------------------------------------------------- */

int probe_main(const char* path) {
    const struct sx_media_audio_format format = {SELFTEST_RATE, SELFTEST_CHANNELS};
    struct selftest_run run;
    struct sx_media* media = NULL;
    struct sx_media_source source;
    struct sx_media_audio_format source_format;
    enum sx_media_status status = SX_MEDIA_OK;
    int width = 0;
    int height = 0;

    memset(&run, 0, sizeof(run));
    if (sx_media_create(&media) < 0) {
        printf("mediaplayer: %s: out of memory\n", path);
        return 1;
    }
    memset(&source, 0, sizeof(source));
    source.path = path;
    source.fd = -1;
    source.audio_out = &format;
    if (sx_media_open(media, &source, &status) < 0) {
        printf("mediaplayer: %s: %s\n", path, sx_media_status_string(status));
        sx_media_destroy(media);
        return 1;
    }
    sx_media_display_size(media, &width, &height);
    printf("%s\n  container %s, ", path, sx_media_container_name(media));
    print_time("duration", sx_media_duration_us(media));
    printf(", %s\n", sx_media_is_seekable(media) ? "seekable" : "not seekable");
    if (sx_media_has_video(media)) {
        printf("  video %s, %dx%d display, frame %lld us\n", sx_media_video_codec(media), width, height,
               (long long)sx_media_frame_duration_us(media));
    }
    if (sx_media_has_audio(media)) {
        if (sx_media_audio_source_format(media, &source_format)) {
            printf("  audio %s, %d Hz, %d channel(s)\n", sx_media_audio_codec(media), source_format.sample_rate,
                   source_format.channels);
        } else {
            printf("  audio %s\n", sx_media_audio_codec(media));
        }
    }
    if (sx_media_stream_missing_count(media) > 0) {
        char codec[SX_MEDIA_CODEC_CAPACITY];
        int index = 0;
        while (sx_media_stream_missing_at(media, index++, codec, sizeof(codec))) {
            printf("  no decoder for %s in this build\n", codec);
        }
    }
    /* --probe es el camino de diagnostico y el unico donde el nombre de una
     * implementacion tiene que aparecer: el aviso que ve el usuario nunca lo
     * nombra, este si. */
    {
        const char* backend = NULL;
        int index = 0;
        printf("  backends:");
        while ((backend = sx_media_backend_name_at(index++)) != NULL) {
            printf(" %s", backend);
        }
        printf("\n");
    }
    decode_everything(media, &run, 0);
    printf("  decoded %ld video frames, %ld audio samples at %d Hz\n", run.video_frames, run.audio_frames,
           SELFTEST_RATE);
    sx_media_close(media);
    sx_media_destroy(media);
    return 0;
}

/* ---- --gpu-hold ---------------------------------------------------------- */

/* Presenta por /dev/gpu0 directo y no como cliente del WM: un proceso lanzado
 * por init -- que es como corre en el harness -- no tiene la sesion de windowd.
 * Es el mismo camino que usa gputest. */
int gpu_hold_main(unsigned long hold_ms, const char* path) {
    struct savanxp_gpu_info gpu_info;
    struct savanxp_fb_info fb_info;
    struct sx_media* media = NULL;
    struct sx_media_source source;
    enum sx_media_status status = SX_MEDIA_OK;
    uint32_t* framebuffer = NULL;
    unsigned long long start_ns = 0;
    long gpu_fd = -1;
    long frames = 0;
    int width = 0;
    int height = 0;
    int target_w = 0;
    int target_h = 0;

    memset(&gpu_info, 0, sizeof(gpu_info));
    memset(&fb_info, 0, sizeof(fb_info));
    gpu_fd = gpu_open();
    if (gpu_fd < 0 || gpu_get_info((int)gpu_fd, &gpu_info) < 0) {
        printf("mediaplayer: /dev/gpu0 no disponible\n");
        return 1;
    }
    /* Del tamano real de la pantalla y no un arreglo estatico: la BSS se mapea
     * entera al exec. */
    framebuffer = (uint32_t*)malloc(gpu_info.buffer_size);
    if (framebuffer == NULL || gpu_acquire((int)gpu_fd) < 0) {
        printf("mediaplayer: no se pudo tomar la pantalla\n");
        free(framebuffer);
        savanxp_close((int)gpu_fd);
        return 1;
    }
    fb_info.width = gpu_info.width;
    fb_info.height = gpu_info.height;
    fb_info.pitch = gpu_info.pitch;
    fb_info.bpp = gpu_info.bpp;
    fb_info.buffer_size = gpu_info.buffer_size;

    /* audio_out en NULL: este modo solo quiere el video en la pantalla, y no
     * tiene por que tocar /dev/audio0 para conseguirlo. */
    if (sx_media_create(&media) < 0) {
        printf("mediaplayer: %s: out of memory\n", path);
        gpu_release((int)gpu_fd);
        savanxp_close((int)gpu_fd);
        free(framebuffer);
        return 1;
    }
    memset(&source, 0, sizeof(source));
    source.path = path;
    source.fd = -1;
    source.audio_out = NULL;
    if (sx_media_open(media, &source, &status) < 0) {
        printf("mediaplayer: %s: %s\n", path, sx_media_status_string(status));
        sx_media_destroy(media);
        gpu_release((int)gpu_fd);
        savanxp_close((int)gpu_fd);
        free(framebuffer);
        return 1;
    }
    if (!sx_media_has_video(media)) {
        printf("mediaplayer: %s: sin video\n", path);
        sx_media_close(media);
        sx_media_destroy(media);
        gpu_release((int)gpu_fd);
        savanxp_close((int)gpu_fd);
        free(framebuffer);
        return 1;
    }

    /* Al doble si entra, para que la captura se lea; si no, tal cual. */
    sx_media_display_size(media, &width, &height);
    target_w = width * 2 <= (int)fb_info.width && height * 2 <= (int)fb_info.height ? width * 2 : width;
    target_h = target_w == width * 2 ? height * 2 : height;

    gfx_clear(framebuffer, &fb_info, gfx_rgb(0, 0, 0));
    start_ns = playback_now_ns();
    while (sx_media_next_video_frame(media) > 0) {
        const int64_t due_us = sx_media_video_time_us(media);
        const uint32_t* pixels = NULL;
        const uint32_t stride = gfx_stride_pixels(&fb_info);
        const int x0 = ((int)fb_info.width - target_w) / 2;
        const int y0 = ((int)fb_info.height - target_h) / 2;
        int64_t now_us = (int64_t)((playback_now_ns() - start_ns) / 1000ULL);
        int row = 0;

        if (due_us > now_us) {
            sleep_ms((unsigned long)((due_us - now_us) / 1000));
        }
        sx_media_show_video_frame(media);
        pixels = sx_media_scale_video(media, target_w, target_h);
        if (pixels == NULL) {
            continue;
        }
        for (row = 0; row < target_h; ++row) {
            memcpy(&framebuffer[(uint32_t)(y0 + row) * stride + (uint32_t)x0], &pixels[row * target_w],
                   (size_t)target_w * sizeof(uint32_t));
        }
        if (gpu_present((int)gpu_fd, framebuffer) < 0) {
            printf("mediaplayer: GPU_IOC_PRESENT fallo\n");
            break;
        }
        frames += 1;
    }

    printf("mediaplayer: %ld cuadros presentados\n", frames);
    if (frames > 0 && hold_ms != 0) {
        /* Recien aca hay algo estable en pantalla para capturar. */
        puts_out("MEDIAPLAYER DISPLAY READY\n");
        sleep_ms(hold_ms);
    }
    sx_media_close(media);
    gpu_release((int)gpu_fd);
    savanxp_close((int)gpu_fd);
    free(framebuffer);
    printf(frames > 0 ? "MEDIAPLAYER PASS\n" : "MEDIAPLAYER FAIL\n");
    return frames > 0 ? 0 : 1;
}
