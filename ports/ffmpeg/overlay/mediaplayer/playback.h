#pragma once

/* Reproduccion: el reloj, el audio al parlante y la decision de que cuadro va
 * a pantalla y cuando. No sabe nada de ventanas: la presentacion la hace quien
 * la llama, con el callback de playback_pump.
 *
 * El reloj es el de pared (monotonic_ns) y no el del dispositivo de audio,
 * porque /dev/audio0 no informa cuanto reprodujo. Ver docs/MEDIA_PLAYER.md,
 * "The clock", para lo que eso implica y lo que falta para cambiarlo. */

#include <stdint.h>

#include "media.h"

enum playback_state {
    PLAYBACK_EMPTY = 0,
    PLAYBACK_PAUSED,
    PLAYBACK_PLAYING,
    PLAYBACK_ENDED
};

/* Bits que devuelve playback_pump. */
#define PLAYBACK_PUMP_PRESENTED     (1u << 0)
#define PLAYBACK_PUMP_STATE_CHANGED (1u << 1)

typedef void (*playback_present_fn)(void* user, struct media* media);

struct playback {
    struct media media;
    enum playback_state state;
    char path[256];
    char error[128];

    /* audio */
    int audio_fd;                   /* -1 = cerrado (en pausa, o sin dispositivo) */
    int audio_device_ok;            /* hubo /dev/audio0 al abrir el archivo */
    struct media_audio_format audio_format;
    uint32_t audio_frame_bytes;
    int audio_period_frames;
    int16_t* audio_chunk;
    int audio_finished;
    int64_t audio_written_until_us; /* hasta donde se le escribio al dispositivo */
    int64_t audio_latency_us;       /* colchon de silencio que precarga el driver */
    unsigned long long audio_last_feed_ns;
    int volume;                     /* 0..100 */

    /* reloj: la posicion anchor_us corresponde al instante anchor_ns */
    unsigned long long anchor_ns;
    int64_t anchor_us;
    int64_t paused_us;
    int fresh;                      /* recien abierto: nada consumido todavia */

    /* video */
    int video_pending;              /* media.video_frame tiene un cuadro sin mostrar */
    int video_finished;
    int show_next_now;              /* mostrar el proximo sin esperar su tiempo */
    unsigned long long last_present_ns;

    unsigned long frames_presented;
    unsigned long frames_dropped;
    unsigned long audio_restarts;
};

unsigned long long playback_now_ns(void);

void playback_init(struct playback* playback);
/* Abre el archivo y deja el primer cuadro listo para mostrar, en pausa. */
int playback_open(struct playback* playback, const char* path);
void playback_close(struct playback* playback);

void playback_play(struct playback* playback);
void playback_pause(struct playback* playback);
void playback_toggle(struct playback* playback);
void playback_stop(struct playback* playback);
int playback_seek(struct playback* playback, int64_t target_us);
void playback_set_volume(struct playback* playback, int volume);

int64_t playback_position_us(const struct playback* playback, unsigned long long now_ns);
int64_t playback_duration_us(const struct playback* playback);

/* Hace avanzar la reproduccion: alimenta el audio y presenta el cuadro que
 * corresponda. Hay que llamarla seguido -- el colchon de audio es de decenas de
 * milisegundos --; en wait_ms deja cuanto se puede dormir hasta la proxima. */
unsigned playback_pump(struct playback* playback, unsigned long long now_ns, playback_present_fn present,
                       void* user, unsigned long* wait_ms);
