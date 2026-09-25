#pragma once

/* Motor de decodificacion del reproductor: todo lo que es FFmpeg y nada que sea
 * pantalla, parlante o reloj.
 *
 * Es de un solo hilo y a demanda. Quien lo usa PIDE el proximo cuadro de video
 * o las proximas muestras de audio, y el motor demultiplexa lo que haga falta
 * para conseguirlos. Los paquetes del otro stream que aparecen en el camino
 * quedan en su cola, asi que pedir video no pierde audio ni al reves. Ver
 * docs/MEDIA_PLAYER.md.
 *
 * Los tiempos salen en microsegundos desde el comienzo del archivo (el
 * start_time del contenedor ya descontado), que es la unica unidad que cruza
 * esta interfaz: los time_base de cada stream no salen de media.c. */

#include <stddef.h>
#include <stdint.h>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

#define MEDIA_NO_TIME INT64_MIN

/* Formato de salida del audio: siempre PCM s16 intercalado, que es lo unico
 * que acepta /dev/audio0. El motor resamplea y remezcla a esto. */
struct media_audio_format {
    int sample_rate;
    int channels;
};

struct media_packet_queue {
    struct AVPacket** items;
    int capacity;
    int head;
    int count;
    int dropped;
};

struct media_stream {
    int index;                        /* -1 = el archivo no tiene este stream */
    struct AVCodecContext* decoder;
    struct media_packet_queue packets;
    int flushed;                      /* ya se le mando el paquete NULL de fin */
    int finished;                     /* el decoder no va a entregar mas nada */
    int64_t next_time_us;             /* estimado para cuadros sin pts */
};

struct media {
    struct AVFormatContext* format;
    struct AVPacket* packet;
    int demux_finished;
    int64_t start_time_us;
    int64_t duration_us;              /* 0 si el contenedor no lo sabe */
    int seekable;

    struct media_stream video;
    struct AVFrame* video_frame;      /* el ultimo cuadro decodificado */
    int64_t video_time_us;
    struct AVFrame* shown_frame;      /* el que se esta mostrando */
    int64_t shown_time_us;
    struct SwsContext* scaler;
    uint8_t* scaled;                  /* BGR0 del tamano pedido, alineado */
    int scaled_width;
    int scaled_height;

    struct media_stream audio;
    struct AVFrame* audio_frame;
    struct SwrContext* resampler;
    int resampler_rate;
    int resampler_format;
    int resampler_channels;
    struct media_audio_format audio_out;
    int16_t* audio_buffer;            /* muestras ya convertidas y sin entregar */
    int audio_buffer_capacity;        /* en cuadros de muestras */
    int audio_buffer_frames;
    int audio_buffer_offset;
    int64_t audio_buffer_time_us;     /* tiempo de la muestra en el offset 0 */

    /* Seek preciso: lo anterior a esto se decodifica pero no se entrega. Uno
     * por stream, y se limpia con lo primero que se entrega: despues del seek
     * un tiempo que retrocede (un wrap de MPEG-TS) no tiene que descartar nada. */
    int64_t video_skip_until_us;
    int64_t audio_skip_until_us;
};

/* Abre el archivo y los decoders del mejor stream de video y de audio. Con
 * audio_out en NULL el audio se ignora. Devuelve 1 si hay al menos un stream
 * que se pueda reproducir; si no, 0 con el motivo en error. */
int media_open(struct media* media, const char* path, const struct media_audio_format* audio_out,
               char* error, size_t error_capacity);
void media_close(struct media* media);

int media_has_video(const struct media* media);
int media_has_audio(const struct media* media);

/* Tamano con el que se deberia MOSTRAR el video: el de los cuadros corregido
 * por el aspect ratio de pixel. */
void media_display_size(const struct media* media, int* width, int* height);
/* Duracion de un cuadro segun el frame rate declarado, o 40 ms si no hay. */
int64_t media_frame_duration_us(const struct media* media);

const char* media_container_name(const struct media* media);
const char* media_video_codec_name(const struct media* media);
const char* media_audio_codec_name(const struct media* media);

/* Decodifica el proximo cuadro de video: queda en media->video_frame con su
 * tiempo en media->video_time_us. 1 = hay cuadro, 0 = se termino, <0 = error. */
int media_next_video_frame(struct media* media);

/* Pasa el ultimo cuadro decodificado a ser el que se muestra. Se separan para
 * poder decodificar por adelantado sin perder lo que esta en pantalla: una
 * ventana que cambia de tamano en pausa tiene que poder re-escalarlo. */
void media_show_video_frame(struct media* media);
int media_has_shown_frame(const struct media* media);

/* Escala el cuadro que se muestra a width x height en BGR0 (el orden de bytes
 * del framebuffer de SavanXP) y devuelve el buffer, que es del motor y vale
 * hasta la proxima llamada. NULL si fallo. */
const uint32_t* media_scale_video(struct media* media, int width, int height);

/* Metadato del contenedor ("title", "artist"...), o NULL. */
const char* media_metadata(const struct media* media, const char* key);

/* Llena out con hasta `frames` cuadros de muestras. Devuelve cuantos escribio
 * (0 = se termino el audio). En first_time_us, si no es NULL, deja el tiempo de
 * la primera muestra escrita. */
int media_read_audio(struct media* media, int16_t* out, int frames, int64_t* first_time_us);

/* Reposiciona en target_us. Despues del seek el primer cuadro y la primera
 * muestra que se entregan son los del instante pedido, no los del keyframe
 * anterior. 1 = ok, 0 = el archivo no se puede reposicionar. */
int media_seek(struct media* media, int64_t target_us);
