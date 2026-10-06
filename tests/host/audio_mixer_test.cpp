#include <stdint.h>

extern "C" {
#include <stdio.h>
#include <string.h>
}

#include "savanxp/audio.h"

namespace {

int g_failures = 0;
int g_checks = 0;
uint32_t g_now_ms = 1000;
int g_write_fails = 0;
int g_write_busy = 0;
int g_sendto_fails = 0;
long g_sendto_calls = 0;
long g_declare_count = 0;
unsigned char g_last_datagram[1024];
size_t g_last_datagram_bytes = 0;
size_t g_last_write_bytes = 0;
size_t g_last_write_frames = 0;
const int16_t* g_last_write_samples = nullptr;
int g_close_count = 0;

bool check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    fflush(0);
    return ok;
}

} // namespace

extern "C" {

long audio_open(void) {
    return 41;
}

long audio_get_info(int fd, struct savanxp_audio_info* info) {
    if (fd != 41 || info == nullptr) {
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->sample_rate_hz = 44100;
    info->channels = 2;
    info->bits_per_sample = 16;
    info->frame_bytes = 4;
    info->period_bytes = 1764;
    info->buffer_bytes = 17640;
    return 0;
}

long savanxp_close(int fd) {
    if (fd == 41) {
        ++g_close_count;
    }
    return 0;
}

long savanxp_write(int fd, const void* buffer, size_t count) {
    if (fd != 41 || buffer == nullptr || (count % 4u) != 0) {
        return -1;
    }
    if (g_write_busy) {
        return -SAVANXP_EBUSY;
    }
    if (g_write_fails) {
        return -5;
    }
    g_last_write_bytes = count;
    g_last_write_frames = count / 4u;
    g_last_write_samples = static_cast<const int16_t*>(buffer);
    return static_cast<long>(count);
}

long savanxp_socket(unsigned long domain, unsigned long type, unsigned long protocol) {
    (void)domain;
    (void)type;
    (void)protocol;
    return 42;
}

long savanxp_sendto(
    int fd,
    const void* buffer,
    size_t count,
    const struct savanxp_sockaddr_in* address)
{
    (void)address;
    if (fd != 42 || buffer == nullptr || count == 0 || count > 1024) {
        return -1;
    }
    if (g_sendto_fails) {
        return -SAVANXP_EIO;
    }
    ++g_sendto_calls;
    g_last_datagram_bytes = count;
    memcpy(g_last_datagram, buffer, count);
    if (count >= 4) {
        uint32_t magic = 0;
        memcpy(&magic, buffer, 4);
        if (magic == SAVANXP_AUDIOD_CONTROL_MAGIC) {
            ++g_declare_count;
        }
    }
    return static_cast<long>(count);
}

static unsigned char g_recv_script[8][64];
static size_t g_recv_script_lengths[8];
static size_t g_recv_script_count = 0;
static size_t g_recv_script_index = 0;

long savanxp_recvfrom(
    int fd,
    void* buffer,
    size_t count,
    struct savanxp_sockaddr_in* address,
    unsigned long timeout_ms)
{
    (void)timeout_ms;
    if (fd != 42 || buffer == nullptr) {
        return -1;
    }
    if (g_recv_script_index >= g_recv_script_count) {
        return -SAVANXP_ETIMEDOUT;
    }
    size_t take = g_recv_script_lengths[g_recv_script_index];
    if (take > count) {
        take = count;
    }
    memcpy(buffer, g_recv_script[g_recv_script_index], take);
    if (address != 0) {
        address->ipv4 = 0;
        address->port = 0;
        address->reserved0 = 0;
    }
    ++g_recv_script_index;
    return static_cast<long>(take);
}

unsigned long uptime_ms(void) {
    return g_now_ms;
}

} // extern "C"

int main() {
    printf("AUDIO MIXER TEST START\n");
    struct sx_audio_mixer mixer = {};
    /* Por debajo de la rodilla (0.75 FS) la mezcla es exacta: la conversion
     * unsigned-8 a S16 y el paneo se afirman con muestras que no la tocan. */
    const unsigned char samples[] = {128, 200, 64};
    long result;

    check(sx_audio_mixer_init(&mixer, 2, 25) == 0, "el mixer abre el device PCM");
    check(sx_audio_mixer_active(&mixer) && mixer.voice_count == 2,
          "inicializa las voces solicitadas");
    check(sx_audio_mixer_start_voice(&mixer, 0, samples, sizeof(samples), 44100,
                                     SX_AUDIO_PITCH_NORMAL_Q16, 127, 0) == 0,
          "una voz unsigned-8 mono arranca");
    check(mixer.voices[0].step_fixed == SX_AUDIO_PITCH_NORMAL_Q16,
          "el pitch normal conserva el ratio de sample rate");
    check(sx_audio_mixer_set_pan(&mixer, 0, 127, 0) == 0,
          "el paneo se puede actualizar sin reiniciar la voz");

    g_now_ms += 10;
    result = sx_audio_mixer_update(&mixer);
    check(result == 1 && g_last_write_frames == 441 && g_last_write_bytes == 1764,
          "calcula los frames por reloj y escribe PCM stereo");
    check(g_last_write_samples[0] == 0 && g_last_write_samples[1] == 0 &&
              g_last_write_samples[2] == 18432 && g_last_write_samples[3] == 0 &&
              g_last_write_samples[4] == -16384 && g_last_write_samples[5] == 0,
          "convierte unsigned-8 a S16 y aplica el paneo izquierdo");
    check(!sx_audio_mixer_voice_playing(&mixer, 0),
          "la voz termina al consumir sus samples");

    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "conserva el resto fraccionario de 1 ms");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "acumula el reloj sin perder frames");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "acumula tres milisegundos sin redondear prematuramente");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "conserva la fraccion durante cuatro milisegundos");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el quinto milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el sexto milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el septimo milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el octavo milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 44,
          "mantiene la fraccion en el noveno milisegundo");
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 45,
          "redondea el siguiente frame al completar la fraccion");

    g_now_ms += 1000;
    check(sx_audio_mixer_update(&mixer) == 1 && g_last_write_frames == 1102,
          "limita el catch-up a la ventana maxima configurada");

    g_write_fails = 1;
    g_now_ms += 1;
    check(sx_audio_mixer_update(&mixer) < 0 && !sx_audio_mixer_active(&mixer),
          "un write fallido desactiva el mixer");
    check(!sx_audio_mixer_voice_playing(&mixer, 0),
          "desactivar el mixer detiene las voces");

    sx_audio_mixer_destroy(&mixer);
    check(g_close_count == 1 && mixer.fd == -1 && mixer.voices == nullptr,
          "destroy cierra el device y libera el estado");

    {
        /* Cinco cuadradas a fondo de escala solapadas, como el gameplay de
         * Celeste (8 voces a 127 al centro, crest ~1): con clamp duro
         * sumaban 80640 y cuadraban al riel, y ningun volumen maestro
         * arregla una forma ya cuadrada. Con la rodilla comprimen a 31584
         * sin tocar el riel. */
        struct sx_audio_mixer loud = {};
        unsigned char square[512];
        size_t voice;
        size_t frame;
        int peak_positive = 0;
        int peak_negative = 0;
        for (frame = 0; frame < sizeof(square); ++frame) {
            square[frame] = (frame & 1u) != 0u ? (unsigned char)0 : (unsigned char)255;
        }
        check(sx_audio_mixer_init(&loud, 8, 25) == 0, "el mixer de saturacion abre");
        g_write_fails = 0;
        for (voice = 0; voice < 5; ++voice) {
            check(sx_audio_mixer_start_voice(&loud, voice, square, sizeof(square), 44100,
                                             SX_AUDIO_PITCH_NORMAL_Q16, 127, 127) == 0,
                  "arranca una voz cuadrada a fondo de escala");
        }
        g_now_ms += 10;
        check(sx_audio_mixer_update(&loud) == 1 && g_last_write_frames == 441,
              "mezcla diez milisegundos de solapamiento");
        for (frame = 0; frame < (size_t)g_last_write_frames * 2u; ++frame) {
            int sample = (int)g_last_write_samples[frame];
            if (sample > peak_positive) {
                peak_positive = sample;
            }
            if (sample < peak_negative) {
                peak_negative = sample;
            }
        }
        check(peak_positive == 31584, "cinco voces no llegan al riel positivo");
        check(peak_negative == -31662, "cinco voces no llegan al riel negativo");
        sx_audio_mixer_destroy(&loud);
        check(g_close_count == 2, "el segundo mixer tambien cierra el device");
    }

    {
        /* Loop: la voz repite en vez de terminar. Cuatro muestras a 1:1 con
         * loop dan dos vueltas exactas en 8 frames. */
        struct sx_audio_mixer loop = {};
        const unsigned char cycle[] = {0, 255, 128, 64};
        size_t frame;
        const int expected[] = {-16254, 16128, 0, -8127, -16254, 16128, 0, -8127};
        int ok = 1;
        check(sx_audio_mixer_init(&loop, 1, 25) == 0, "el mixer con loop abre");
        check(sx_audio_mixer_start_voice(&loop, 0, cycle, sizeof(cycle), 44100,
                                         SX_AUDIO_PITCH_NORMAL_Q16, 127, 127) == 0,
              "arranca la voz del loop");
        check(sx_audio_mixer_set_voice_loop(&loop, 0, 1) == 0, "activa el loop");
        check(sx_audio_mixer_set_voice_loop(&loop, 9, 1) != 0, "rechaza voz inexistente");
        g_now_ms += 1;
        check(sx_audio_mixer_update(&loop) == 1 && g_last_write_frames == 44,
              "mezcla mas alla del final de la muestra");
        check(sx_audio_mixer_voice_playing(&loop, 0), "la voz sigue activa tras el borde");
        for (frame = 0; frame < 8; ++frame) {
            if (g_last_write_samples[frame * 2] != expected[frame]) {
                ok = 0;
            }
        }
        check(ok, "los primeros 8 frames repiten el ciclo");
        sx_audio_mixer_destroy(&loop);
    }

    {
        /* Remoto: el primer write con EBUSY pasa al demonio; el header lleva
         * magia, version, canales, rate y secuencia creciente; si el envio
         * falla, la proxima vuelta prueba directo de nuevo. */
        struct sx_audio_mixer remote = {};
        unsigned char square[512];
        uint32_t first_seq = 0;
        uint32_t last_seq = 0;
        size_t frame;
        for (frame = 0; frame < sizeof(square); ++frame) {
            square[frame] = (frame & 1u) != 0u ? (unsigned char)0 : (unsigned char)255;
        }
        check(sx_audio_mixer_init(&remote, 1, 25) == 0, "el mixer remoto abre");
        check(sx_audio_mixer_start_voice(&remote, 0, square, sizeof(square), 44100,
                                         SX_AUDIO_PITCH_NORMAL_Q16, 127, 127) == 0,
              "arranca la voz remota");
        sx_audio_mixer_set_client_name(&remote, "test");
        g_write_busy = 1;
        g_now_ms += 10;
        check(sx_audio_mixer_update(&remote) == 1, "con EBUSY no se desactiva");
        check(remote.server_link.mode == 2, "el EBUSY lo pasa a remoto");
        check(g_declare_count >= 1, "al pasar a remoto se presenta");
        check(g_sendto_calls > 0 && g_last_datagram_bytes <= 1024, "manda datagramas");
        {
            uint32_t magic = 0;
            uint16_t version = 0;
            uint16_t channels = 0;
            uint32_t rate = 0;
            memcpy(&magic, g_last_datagram + 0, 4);
            memcpy(&version, g_last_datagram + 4, 2);
            memcpy(&channels, g_last_datagram + 6, 2);
            memcpy(&rate, g_last_datagram + 8, 4);
            memcpy(&first_seq, g_last_datagram + 12, 4);
            check(magic == 0x53415544u && version == 1 && channels == 2 && rate == 44100,
                "el header lleva magia, version, canales y rate");
        }
        g_now_ms += 10;
        check(sx_audio_mixer_update(&remote) == 1, "en remoto sigue mezclando");
        memcpy(&last_seq, g_last_datagram + 12, 4);
        check(last_seq != first_seq, "la secuencia avanza por datagrama");
        g_sendto_fails = 1;
        g_write_busy = 0;
        g_now_ms += 10;
        check(sx_audio_mixer_update(&remote) == 1, "sin demonio no se desactiva");
        check(remote.server_link.mode == 1, "sin demonio vuelve a directo");
        g_sendto_fails = 0;
        {
            long declares = g_declare_count;
            g_now_ms += 15000u;
            /* La voz termino hace rato; reactivarla para que haya que mezclar. */
            check(sx_audio_mixer_start_voice(&remote, 0, square, sizeof(square), 44100,
                                             SX_AUDIO_PITCH_NORMAL_Q16, 127, 127) == 0,
                  "rearranca la voz para el heartbeat");
            g_write_busy = 1;
            check(sx_audio_mixer_update(&remote) == 1, "sigue remoto tras el hueco");
            check(g_declare_count > declares, "el heartbeat re-anuncia el nombre");
            g_write_busy = 0;
        }
        sx_audio_mixer_destroy(&remote);
    }

    {
        /* Censo: dos respuestas enlatadas con nombres y niveles, y el caso
         * vacio. El demonio real lo cubre el selftest en QEMU; aca va el
         * parseo del SDK, que sin esto no lo ejercita nadie. */
        struct sx_audio_server_entry census[8];
        struct savanxp_audiod_list_reply answers[2];
        long total;
        memset(answers, 0, sizeof(answers));
        answers[0].magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        answers[0].version = SAVANXP_AUDIOD_CONTROL_VERSION;
        answers[0].kind = SAVANXP_AUDIOD_REPLY;
        answers[0].count = 2;
        answers[0].index = 0;
        answers[0].port = 100;
        answers[0].volume = 50;
        memcpy(answers[0].name, "test-a", 7);
        answers[1].magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
        answers[1].version = SAVANXP_AUDIOD_CONTROL_VERSION;
        answers[1].kind = SAVANXP_AUDIOD_REPLY;
        answers[1].count = 2;
        answers[1].index = 1;
        answers[1].port = 200;
        answers[1].volume = 100;
        memcpy(answers[1].name, "test-b", 7);
        memcpy(g_recv_script[0], &answers[0], sizeof(answers[0]));
        g_recv_script_lengths[0] = sizeof(answers[0]);
        memcpy(g_recv_script[1], &answers[1], sizeof(answers[1]));
        g_recv_script_lengths[1] = sizeof(answers[1]);
        g_recv_script_count = 2;
        g_recv_script_index = 0;
        memset(census, 0xAA, sizeof(census));
        total = sx_audio_server_list(42, census, 8);
        check(total == 2, "el censo dice cuantos hay");
        check(census[0].port == 100 && census[0].volume == 50 &&
                  strcmp(census[0].name, "test-a") == 0,
            "la primera entrada trae puerto, nivel y nombre");
        check(census[1].port == 200 && census[1].volume == 100 &&
                  strcmp(census[1].name, "test-b") == 0,
            "la segunda entrada trae puerto, nivel y nombre");
        g_recv_script_count = 0;
        g_recv_script_index = 0;
        total = sx_audio_server_list(42, census, 8);
        check(total == -SAVANXP_ETIMEDOUT, "sin respuestas hay timeout, no censo vacio");
        {
            struct savanxp_audiod_list_reply empty;
            memset(&empty, 0, sizeof(empty));
            empty.magic = SAVANXP_AUDIOD_CONTROL_MAGIC;
            empty.version = SAVANXP_AUDIOD_CONTROL_VERSION;
            empty.kind = SAVANXP_AUDIOD_REPLY;
            empty.count = 0;
            empty.index = 0;
            memcpy(g_recv_script[0], &empty, sizeof(empty));
            g_recv_script_lengths[0] = sizeof(empty);
            g_recv_script_count = 1;
            g_recv_script_index = 0;
            total = sx_audio_server_list(42, census, 8);
            check(total == 0, "el censo vacio vuelve con su respuesta");
        }
    }

    printf(g_failures == 0 ? "AUDIO MIXER TEST PASS\n" : "AUDIO MIXER TEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
