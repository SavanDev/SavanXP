#include "playback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/libc.h"

/* Cuanto por delante del reloj se le escribe al dispositivo. Tiene que entrar,
 * junto con el colchon que precarga el driver, en lo que el driver acepta en
 * vuelo antes de empezar a DESCARTAR (8 periodos en AC97, ~170 ms): con 4
 * periodos de colchon quedan ~4 libres, y un bloque de lectura puede pasarse
 * uno. */
#define PLAYBACK_AUDIO_LEAD_US 40000
/* Periodos de silencio que precargan ac97.cpp y virtio_sound.cpp al abrir el
 * stream: es la latencia entre escribir una muestra y oirla. */
#define PLAYBACK_DRIVER_PRIME_PERIODS 4
/* Un cuadro que llega mas tarde que esto se descarta sin mostrarlo... */
#define PLAYBACK_LATE_US 100000
/* ...salvo que la pantalla lleve este tiempo sin cambiar: con un decoder mas
 * lento que el video, descartar todo congelaria la imagen. */
#define PLAYBACK_MAX_FREEZE_NS 250000000ULL
#define PLAYBACK_MAX_DROPS_PER_PUMP 8
#define PLAYBACK_MAX_WAIT_MS 10UL

unsigned long long playback_now_ns(void) {
    const unsigned long long now = monotonic_ns();
    /* monotonic_ns devuelve 0 si el TSC no se calibro; el tick del kernel es
     * grueso pero sirve para no quedar con el reloj parado. */
    return now != 0 ? now : (unsigned long long)uptime_ms() * 1000000ULL;
}

void playback_init(struct playback* playback) {
    /* El engine es opaco y sobrevive a los archivos, porque abrir uno es una
     * operacion del engine y no del player. playback_open vuelve a llamar a
     * init, asi que el puntero tiene que sobrevivir al memset. */
    struct sx_media* engine = playback->media;

    memset(playback, 0, sizeof(*playback));
    playback->state = PLAYBACK_EMPTY;
    playback->audio_fd = -1;
    playback->volume = 100;
    if (engine == NULL) {
        if (sx_media_create(&playback->media) < 0) {
            playback->media = NULL;
        }
    } else {
        playback->media = engine;
    }
}

void playback_destroy(struct playback* playback) {
    if (playback == NULL) {
        return;
    }
    playback_close(playback);
    sx_media_destroy(playback->media);
    playback->media = NULL;
}

static int has_video(const struct playback* playback) {
    return playback->state != PLAYBACK_EMPTY && sx_media_has_video(playback->media);
}

/* ---- audio --------------------------------------------------------------- */

/* Mira que hay en /dev/audio0 y lo suelta: el dispositivo tiene un solo dueno,
 * y un reproductor abierto en pausa no deberia quitarselo a nadie. */
static void probe_audio_device(struct playback* playback) {
    struct savanxp_audio_info info;
    long fd = audio_open();

    playback->audio_device_ok = 0;
    if (fd < 0) {
        return;
    }
    memset(&info, 0, sizeof(info));
    if (audio_get_info((int)fd, &info) == 0 && info.bits_per_sample == 16 && info.channels > 0 &&
        info.sample_rate_hz > 0 && info.frame_bytes == info.channels * 2u && info.period_bytes >= info.frame_bytes) {
        playback->audio_device_ok = 1;
        playback->audio_format.sample_rate = (int)info.sample_rate_hz;
        playback->audio_format.channels = (int)info.channels;
        playback->audio_frame_bytes = info.frame_bytes;
        playback->audio_period_frames = (int)(info.period_bytes / info.frame_bytes);
        playback->audio_latency_us = (int64_t)PLAYBACK_DRIVER_PRIME_PERIODS * playback->audio_period_frames *
                                     1000000LL / playback->audio_format.sample_rate;
    }
    savanxp_close((int)fd);
}

static void audio_stop(struct playback* playback) {
    if (playback->audio_fd >= 0) {
        savanxp_close(playback->audio_fd);
        playback->audio_fd = -1;
    }
}

/* Abre el dispositivo para empezar a sonar desde anchor_us. Cerrar y volver a
 * abrir es lo que vacia la cola del driver y vuelve a precargar su colchon, asi
 * que despues de esto la latencia vuelve a ser audio_latency_us. */
static void audio_start(struct playback* playback, unsigned long long now_ns) {
    long fd = -1;

    audio_stop(playback);
    if (!playback->audio_device_ok || !sx_media_has_audio(playback->media) || playback->audio_finished) {
        return;
    }
    fd = audio_open();
    if (fd < 0) {
        return;
    }
    playback->audio_fd = (int)fd;
    playback->audio_written_until_us = playback->anchor_us;
    playback->audio_last_feed_ns = now_ns;
}

static void apply_volume(const struct playback* playback, int16_t* samples, int count) {
    int index = 0;
    if (playback->volume >= 100) {
        return;
    }
    for (index = 0; index < count; ++index) {
        samples[index] = (int16_t)((int)samples[index] * playback->volume / 100);
    }
}

static int64_t frames_to_us(const struct playback* playback, int64_t frames) {
    return frames * 1000000LL / playback->audio_format.sample_rate;
}

/* Deja de usar el dispositivo para el resto del archivo -- otro proceso lo
 * tiene, o fallo -- y sigue en silencio con el reloj de pared solo. */
static void audio_give_up(struct playback* playback, unsigned long long now_ns) {
    const int64_t position = playback_position_us(playback, now_ns);
    audio_stop(playback);
    playback->audio_finished = 1;
    playback->anchor_us = position;
    playback->anchor_ns = now_ns;
}

static int audio_write(struct playback* playback, const int16_t* samples, int frames, unsigned long long now_ns) {
    const size_t bytes = (size_t)frames * playback->audio_frame_bytes;
    if (savanxp_write(playback->audio_fd, samples, bytes) != (long)bytes) {
        audio_give_up(playback, now_ns);
        return 0;
    }
    playback->audio_written_until_us += frames_to_us(playback, frames);
    return 1;
}

static void feed_audio(struct playback* playback, unsigned long long now_ns) {
    int64_t elapsed_us = 0;
    int64_t target_us = 0;
    int guard = 32;

    if (playback->audio_fd < 0 || playback->audio_finished) {
        return;
    }

    /* Si pasaron mas de latencia + adelanto sin escribir, el driver ya se quedo
     * sin nada y al volver va a precargar OTRO colchon: la latencia real crecio
     * y el video quedaria adelantado. Se reinicia el stream como en un seek:
     * lo ya escrito se oyo entero, asi que el reloj retoma desde ahi, o desde
     * donde iba el video si este siguio de largo durante el atasco. */
    if (now_ns - playback->audio_last_feed_ns >
        (unsigned long long)(playback->audio_latency_us + PLAYBACK_AUDIO_LEAD_US) * 1000ULL) {
        const int64_t position = playback_position_us(playback, now_ns);
        int64_t resume_us = playback->audio_written_until_us;
        if (position > resume_us) {
            sx_media_skip_until(playback->media, position);
            resume_us = position;
        }
        playback->audio_restarts += 1;
        playback->anchor_us = resume_us;
        playback->anchor_ns = now_ns;
        audio_start(playback, now_ns);
        if (playback->audio_fd < 0) {
            return;
        }
    }

    elapsed_us = (int64_t)((now_ns - playback->anchor_ns) / 1000ULL);
    target_us = playback->anchor_us + elapsed_us + PLAYBACK_AUDIO_LEAD_US;

    while (playback->audio_written_until_us < target_us && guard-- > 0) {
        int64_t first_us = SX_MEDIA_NO_TIME;
        const int frames = sx_media_read_audio(playback->media, playback->audio_chunk,
                                            playback->audio_period_frames, &first_us);
        if (frames <= 0) {
            playback->audio_finished = 1;
            break;
        }

        /* Un hueco en el audio (el stream empieza despues que el video, o le
         * faltan paquetes) se rellena con silencio para no correr el reloj. */
        if (first_us != SX_MEDIA_NO_TIME && first_us > playback->audio_written_until_us + 20000) {
            int64_t gap = (first_us - playback->audio_written_until_us) *
                          playback->audio_format.sample_rate / 1000000LL;
            int16_t* silence = (int16_t*)calloc((size_t)playback->audio_period_frames *
                                                    (size_t)playback->audio_format.channels,
                                                sizeof(int16_t));
            if (gap > (int64_t)playback->audio_format.sample_rate * 2) {
                gap = playback->audio_format.sample_rate * 2;
            }
            while (silence != NULL && gap > 0 && playback->audio_fd >= 0) {
                const int count = gap > playback->audio_period_frames ? playback->audio_period_frames : (int)gap;
                if (!audio_write(playback, silence, count, now_ns)) {
                    break;
                }
                gap -= count;
            }
            free(silence);
            if (playback->audio_fd < 0) {
                return;
            }
        }

        apply_volume(playback, playback->audio_chunk, frames * playback->audio_format.channels);
        if (!audio_write(playback, playback->audio_chunk, frames, now_ns)) {
            return;
        }
    }
    playback->audio_last_feed_ns = now_ns;
}

/* ---- apertura y control -------------------------------------------------- */

void playback_close(struct playback* playback) {
    audio_stop(playback);
    /* Idempotente: sx_media_open ya se limpia solo cuando falla a medias, y un
     * close sin source abierto no hace nada. */
    sx_media_close(playback->media);
    free(playback->audio_chunk);
    playback->audio_chunk = NULL;
    playback->state = PLAYBACK_EMPTY;
    playback->video_pending = 0;
    playback->video_finished = 0;
    playback->audio_finished = 0;
}

int playback_open(struct playback* playback, const char* path) {
    int volume = playback->volume;
    struct sx_media_source source;
    enum sx_media_status status = SX_MEDIA_OK;

    playback_close(playback);
    playback_init(playback);
    playback->volume = volume;
    snprintf(playback->path, sizeof(playback->path), "%s", path);

    probe_audio_device(playback);
    memset(&source, 0, sizeof(source));
    source.path = path;
    source.fd = -1;
    source.audio_out = playback->audio_device_ok ? &playback->audio_format : NULL;
    if (sx_media_open(playback->media, &source, &status) < 0) {
        /* El motivo viene tipado, no como una frase: "no hay demuxer para este
         * contenedor" y "no hay decoder" son cosas distintas y el usuario solo
         * puede actuar sobre una. Ver sx_media_status_string. */
        snprintf(playback->error, sizeof(playback->error), "%s", sx_media_status_string(status));
        return 0;
    }
    /* Un stream que este build no puede decodificar no es un fallo de apertura:
     * el archivo se reproduce sin el, y se dice cual. */
    if (sx_media_stream_missing_count(playback->media) > 0) {
        char codec[SX_MEDIA_CODEC_CAPACITY];
        char detail[SX_MEDIA_CODEC_CAPACITY + 8];
        if (sx_media_stream_missing_at(playback->media, 0, codec, sizeof(codec))) {
            snprintf(detail, sizeof(detail), " without %s", codec);
            snprintf(playback->error, sizeof(playback->error), "plays%s", detail);
        }
    }
    if (playback->audio_device_ok) {
        playback->audio_chunk = (int16_t*)malloc((size_t)playback->audio_period_frames *
                                                 (size_t)playback->audio_format.channels * sizeof(int16_t));
        if (playback->audio_chunk == NULL) {
            playback->audio_device_ok = 0;
        }
    }

    playback->state = PLAYBACK_PAUSED;
    playback->paused_us = 0;
    playback->fresh = 1;
    playback->show_next_now = 1;
    if (playback->video_width == 0) {
        /* Nadie fijo un tamano todavia: se muestra al tamano del archivo, con el
         * aspecto de pixel ya corregido por el engine. */
        sx_media_display_size(playback->media, &playback->video_width, &playback->video_height);
    }
    return 1;
}

int64_t playback_duration_us(const struct playback* playback) {
    return playback->state != PLAYBACK_EMPTY ? sx_media_duration_us(playback->media) : 0;
}

int64_t playback_position_us(const struct playback* playback, unsigned long long now_ns) {
    int64_t position = 0;

    if (playback->state != PLAYBACK_PLAYING) {
        return playback->paused_us;
    }
    position = (int64_t)((now_ns - playback->anchor_ns) / 1000ULL);
    /* Con audio, la posicion es la de lo que SE OYE: lo recien escrito tarda
     * la latencia del driver en salir. */
    if (playback->audio_fd >= 0) {
        position -= playback->audio_latency_us;
        if (position < 0) {
            position = 0;
        }
    }
    position += playback->anchor_us;
    if (sx_media_duration_us(playback->media) > 0 && position > sx_media_duration_us(playback->media)) {
        position = sx_media_duration_us(playback->media);
    }
    return position;
}

static void reset_streams_after_seek(struct playback* playback) {
    playback->video_pending = 0;
    playback->video_finished = 0;
    playback->audio_finished = 0;
    playback->show_next_now = 1;
}

void playback_play(struct playback* playback) {
    const unsigned long long now = playback_now_ns();

    if (playback->state == PLAYBACK_EMPTY || playback->state == PLAYBACK_PLAYING) {
        return;
    }
    if (playback->state == PLAYBACK_ENDED) {
        playback->paused_us = 0;
        if (sx_media_seek(playback->media, 0)) {
            reset_streams_after_seek(playback);
        } else {
            /* Un archivo sin seek (un MJPEG crudo) se vuelve a abrir. */
            char path[sizeof(playback->path)];
            snprintf(path, sizeof(path), "%s", playback->path);
            if (!playback_open(playback, path)) {
                return;
            }
        }
        playback->fresh = 1;
    }

    /* Reanudar despues de una pausa: el motor quedo ADELANTADO respecto de lo
     * que se oyo (lo que estaba en la cola del driver se perdio al cerrarlo), asi
     * que se reposiciona exactamente donde se pauso. */
    if (!playback->fresh && sx_media_seek(playback->media, playback->paused_us)) {
        reset_streams_after_seek(playback);
    }
    playback->fresh = 0;

    playback->anchor_ns = now;
    playback->anchor_us = playback->paused_us;
    playback->state = PLAYBACK_PLAYING;
    playback->last_present_ns = now;
    audio_start(playback, now);
}

void playback_pause(struct playback* playback) {
    if (playback->state != PLAYBACK_PLAYING) {
        return;
    }
    playback->paused_us = playback_position_us(playback, playback_now_ns());
    audio_stop(playback);
    playback->state = PLAYBACK_PAUSED;
}

void playback_toggle(struct playback* playback) {
    if (playback->state == PLAYBACK_PLAYING) {
        playback_pause(playback);
    } else {
        playback_play(playback);
    }
}

void playback_stop(struct playback* playback) {
    if (playback->state == PLAYBACK_EMPTY) {
        return;
    }
    playback_pause(playback);
    (void)playback_seek(playback, 0);
}

int playback_seek(struct playback* playback, int64_t target_us) {
    const unsigned long long now = playback_now_ns();

    if (playback->state == PLAYBACK_EMPTY || !sx_media_seek(playback->media, target_us)) {
        return 0;
    }
    if (target_us < 0) {
        target_us = 0;
    }
    if (sx_media_duration_us(playback->media) > 0 && target_us > sx_media_duration_us(playback->media)) {
        target_us = sx_media_duration_us(playback->media);
    }
    reset_streams_after_seek(playback);
    playback->paused_us = target_us;

    if (playback->state == PLAYBACK_PLAYING) {
        playback->anchor_us = target_us;
        playback->anchor_ns = now;
        playback->last_present_ns = now;
        audio_start(playback, now);
    } else {
        /* En pausa el motor queda parado justo en target: al dar play no hace
         * falta otro seek. */
        playback->state = PLAYBACK_PAUSED;
        playback->fresh = 1;
    }
    return 1;
}

void playback_set_volume(struct playback* playback, int volume) {
    playback->volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
}

void playback_set_video_size(struct playback* playback, int width, int height) {
    /* Lo decide quien tiene la ventana; el player solo lo pasa al conversor. */
    if (width > 0 && height > 0) {
        playback->video_width = width;
        playback->video_height = height;
    }
}

/* ---- bomba --------------------------------------------------------------- */

static int decode_ahead(struct playback* playback) {
    if (!playback->video_pending && !playback->video_finished) {
        if (sx_media_next_video_frame(playback->media) > 0) {
            playback->video_pending = 1;
        } else {
            playback->video_finished = 1;
        }
    }
    return playback->video_pending;
}

/* Escala al tamano pedido y se lo pasa al front end ya en pixeles. El callback
 * recibia `struct media*` antes, con lo que la ventana tenia que conocer el
 * tipo del engine; ahora recibe lo que va a pintar y nada mas. */
static void present_pending(struct playback* playback, playback_present_fn present, void* user,
                            unsigned long long now_ns) {
    const uint32_t* pixels;

    sx_media_show_video_frame(playback->media);
    playback->video_pending = 0;
    playback->show_next_now = 0;
    playback->last_present_ns = now_ns;
    playback->frames_presented += 1;
    if (present == NULL) {
        return;
    }
    pixels = sx_media_scale_video(playback->media, playback->video_width, playback->video_height);
    if (pixels != NULL) {
        present(user, pixels, playback->video_width, playback->video_height);
    }
}

unsigned playback_pump(struct playback* playback, unsigned long long now_ns, playback_present_fn present,
                       void* user, unsigned long* wait_ms) {
    unsigned flags = 0;
    int64_t position = 0;
    int drops = 0;

    *wait_ms = 50;
    if (playback->state == PLAYBACK_EMPTY) {
        return 0;
    }

    if (playback->state != PLAYBACK_PLAYING) {
        /* En pausa solo se muestra el cuadro de un seek o de la apertura, y NO
         * se decodifica por adelantado: el motor queda parado en la posicion. */
        if (playback->show_next_now && has_video(playback) && decode_ahead(playback)) {
            present_pending(playback, present, user, now_ns);
            flags |= PLAYBACK_PUMP_PRESENTED;
        }
        return flags;
    }

    feed_audio(playback, now_ns);
    position = playback_position_us(playback, now_ns);

    while (has_video(playback) && decode_ahead(playback)) {
        const int64_t late_us = position - sx_media_video_time_us(playback->media);
        if (!playback->show_next_now && late_us < 0) {
            break;
        }
        if (!playback->show_next_now && late_us > PLAYBACK_LATE_US && drops < PLAYBACK_MAX_DROPS_PER_PUMP &&
            now_ns - playback->last_present_ns < PLAYBACK_MAX_FREEZE_NS) {
            playback->video_pending = 0;
            playback->frames_dropped += 1;
            drops += 1;
            now_ns = playback_now_ns();
            feed_audio(playback, now_ns);
            position = playback_position_us(playback, now_ns);
            continue;
        }
        present_pending(playback, present, user, now_ns);
        flags |= PLAYBACK_PUMP_PRESENTED;
        now_ns = playback_now_ns();
        feed_audio(playback, now_ns);
        position = playback_position_us(playback, now_ns);
    }

    /* Fin: no queda video por mostrar y todo el audio escrito ya se oyo. */
    if ((!has_video(playback) || (playback->video_finished && !playback->video_pending)) &&
        (playback->audio_fd < 0 || playback->audio_finished) &&
        (playback->audio_fd < 0 || position >= playback->audio_written_until_us)) {
        playback->paused_us = position;
        audio_stop(playback);
        playback->state = PLAYBACK_ENDED;
        return flags | PLAYBACK_PUMP_STATE_CHANGED;
    }

    if (playback->video_pending) {
        const int64_t until_us = sx_media_video_time_us(playback->media) - position;
        *wait_ms = until_us <= 0 ? 0 : (unsigned long)(until_us / 1000);
    } else {
        *wait_ms = PLAYBACK_MAX_WAIT_MS;
    }
    if (*wait_ms > PLAYBACK_MAX_WAIT_MS) {
        *wait_ms = PLAYBACK_MAX_WAIT_MS;
    }
    return flags;
}
