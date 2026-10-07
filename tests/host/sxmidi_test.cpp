/* Prueba de host del sintetizador (libsxmidi): arma SMF en memoria y afirma lo
 * que sale del render, sin QEMU de por medio. Es la unica verificacion que
 * mira el sonido: el resto del sistema solo prueba que la libreria carga. */
#include <stdint.h>

extern "C" {
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

#include <vector>

#include "savanxp/midi.h"

namespace {

int g_failures = 0;

bool check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        printf("  FAIL %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
    fflush(0);
    return ok;
}

/* Un constructor minimo de SMF tipo 0: una pista con delta-tiempos explicitos. */
struct Smf {
    std::vector<unsigned char> track;

    void byte(unsigned char value) { track.push_back(value); }

    void varlen(uint32_t value) {
        unsigned char tmp[5];
        int count = 0;
        do {
            tmp[count++] = (unsigned char)(value & 0x7fu);
            value >>= 7;
        } while (value != 0);
        for (int i = count - 1; i >= 0; --i) {
            unsigned char b = tmp[i];
            if (i != 0) {
                b |= 0x80u;
            }
            track.push_back(b);
        }
    }

    void meta(uint32_t delta, unsigned char type, const unsigned char* data, uint32_t len) {
        varlen(delta);
        byte(0xff);
        byte(type);
        varlen(len);
        for (uint32_t i = 0; i < len; ++i) {
            byte(data[i]);
        }
    }

    void tempo(uint32_t delta, uint32_t us) {
        unsigned char data[3] = {
            (unsigned char)((us >> 16) & 0xffu),
            (unsigned char)((us >> 8) & 0xffu),
            (unsigned char)(us & 0xffu)};
        meta(delta, 0x51, data, 3);
    }

    void program(uint32_t delta, unsigned char channel, unsigned char value) {
        varlen(delta);
        byte((unsigned char)(0xc0u | (channel & 0x0fu)));
        byte(value);
    }

    void note_on(uint32_t delta, unsigned char channel, unsigned char note, unsigned char velocity) {
        varlen(delta);
        byte((unsigned char)(0x90u | (channel & 0x0fu)));
        byte(note);
        byte(velocity);
    }

    void note_off(uint32_t delta, unsigned char channel, unsigned char note) {
        varlen(delta);
        byte((unsigned char)(0x80u | (channel & 0x0fu)));
        byte(note);
        byte(0);
    }

    void bend(uint32_t delta, unsigned char channel, uint16_t value) {
        varlen(delta);
        byte((unsigned char)(0xe0u | (channel & 0x0fu)));
        byte((unsigned char)(value & 0x7fu));
        byte((unsigned char)((value >> 7) & 0x7fu));
    }

    void end(uint32_t delta) { meta(delta, 0x2f, nullptr, 0); }

    std::vector<unsigned char> build(uint16_t division) {
        std::vector<unsigned char> file;
        const unsigned char header[14] = {
            'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
            (unsigned char)(division >> 8), (unsigned char)(division & 0xffu)};
        for (unsigned char b : header) {
            file.push_back(b);
        }
        const uint32_t length = (uint32_t)track.size();
        const unsigned char track_header[8] = {
            'M', 'T', 'r', 'k',
            (unsigned char)(length >> 24), (unsigned char)(length >> 16),
            (unsigned char)(length >> 8), (unsigned char)(length & 0xffu)};
        for (unsigned char b : track_header) {
            file.push_back(b);
        }
        for (unsigned char b : track) {
            file.push_back(b);
        }
        return file;
    }
};

/* Cuenta cruces por cero de la componente alterna: un diente de sierra cruza
 * dos veces por ciclo, asi que la frecuencia estimada es cruces*rate/(2n). */
double estimate_frequency(const unsigned char* pcm, size_t count, uint32_t rate) {
    long crossings = 0;
    int previous = 0;

    for (size_t i = 0; i < count; ++i) {
        int value = (int)pcm[i] - 128;
        int sign = value > 0 ? 1 : (value < 0 ? -1 : 0);
        if (sign != 0) {
            if (previous != 0 && sign != previous) {
                ++crossings;
            }
            previous = sign;
        }
    }
    return (double)crossings * (double)rate / (2.0 * (double)count);
}

size_t count_non_silent(const unsigned char* pcm, size_t count) {
    size_t non_silent = 0;
    for (size_t i = 0; i < count; ++i) {
        if (pcm[i] != 128) {
            ++non_silent;
        }
    }
    return non_silent;
}

double tail_deviation(const unsigned char* pcm, size_t count) {
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) {
        int value = (int)pcm[i] - 128;
        total += value < 0 ? -value : value;
    }
    return count != 0 ? total / (double)count : 0.0;
}

/* Frecuencia de C4; la usan los chequeos de periodicidad, que necesitan un
 * retardo fraccionario exacto en samples. */
const double kC4_Hz = 261.6255653;

/* Correlacion normalizada de la senal con una copia retrasada `lag` samples
 * (fraccionario, interpolado lineal). Vale ~1 cuando la senal es periodica con
 * ese periodo y menos cuando no lo es. Es la forma de ver dos cosas que los
 * cruces por cero no ven: un vibrato (la fase se corre y deja de cerrar) y un
 * parcial de octava (a medio periodo la correlacion deja de ser -1). */
double periodic_correlation(
    const unsigned char* pcm, size_t begin, size_t end, double lag) {
    const size_t whole = (size_t)lag;
    const double fraction = lag - (double)whole;
    double numerator = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;

    for (size_t n = begin; n + whole + 1u < end; ++n) {
        double a = (double)pcm[n] - 128.0;
        double b = ((double)pcm[n + whole] - 128.0) * (1.0 - fraction) +
                   ((double)pcm[n + whole + 1u] - 128.0) * fraction;
        numerator += a * b;
        energy_a += a * a;
        energy_b += b * b;
    }
    if (energy_a <= 0.0 || energy_b <= 0.0) {
        return 0.0;
    }
    return numerator / (sqrt(energy_a) * sqrt(energy_b));
}

/* Periodo real de la senal, en samples, buscando el retardo que maximiza la
 * correlacion. El test no puede asumir el periodo teorico de la nota: el motor
 * lo trunca al calcular el incremento de fase, y unos centesimos de sample por
 * ciclo alcanzan para que la correlacion larga se caiga sola y mida otra cosa
 * que el vibrato. */
/* Un tema de una nota larga (1 s) sobre el que medir el vibrato: dos ventanas
 * de medio ciclo de LFO, una en el pico y otra en el valle. */
std::vector<unsigned char> long_note_song(unsigned char program_value) {
    Smf song;
    song.tempo(0, 500000);
    song.program(0, 0, program_value);
    song.note_on(0, 0, 96, 100);
    song.note_off(192, 0, 96);
    song.end(0);
    return song.build(96);
}

/* Un tema de una nota: C4 (60) con el programa dado. Nota 0..96, fin en 192. */
std::vector<unsigned char> one_note_song(unsigned char program_value, uint32_t tempo_us) {
    Smf song;
    song.tempo(0, tempo_us);
    song.program(0, 0, program_value);
    song.note_on(0, 0, 60, 100);
    song.note_off(96, 0, 60);
    song.end(0);
    return song.build(96);
}

/* GENMIDI sintetico: 8 de cabecera + 175 registros de 36. Todos los registros
 * iguales, para que el test controle el timbre sin depender de un WAD. */
std::vector<unsigned char> make_genmidi_record(
    uint16_t flags, uint8_t fine, uint8_t fixed_note,
    int16_t base0, int16_t base1,
    uint8_t mod_level, uint8_t car_wave0, uint8_t car_wave1) {
    std::vector<unsigned char> record(36, 0);
    unsigned char voice0[16] = {
        0x21, 0xf5, 0x27, 0x00, 0x00, 0x00, 0x00, 0x21,
        0xf5, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    unsigned char voice1[16] = {
        0x21, 0xf5, 0x27, 0x00, 0x00, 0x00, 0x00, 0x21,
        0xf5, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    voice0[5] = mod_level;
    voice0[10] = car_wave0;
    voice1[10] = car_wave1;
    record[0] = (unsigned char)(flags & 0xffu);
    record[1] = (unsigned char)((flags >> 8) & 0xffu);
    record[2] = fine;
    record[3] = fixed_note;
    for (int i = 0; i < 16; ++i) {
        record[4 + i] = voice0[i];
        record[20 + i] = voice1[i];
    }
    record[4 + 14] = (unsigned char)(base0 & 0xff);
    record[4 + 15] = (unsigned char)((base0 >> 8) & 0xff);
    record[20 + 14] = (unsigned char)(base1 & 0xff);
    record[20 + 15] = (unsigned char)((base1 >> 8) & 0xff);
    return record;
}

std::vector<unsigned char> make_genmidi_bank(
    uint16_t flags, uint8_t fine, uint8_t fixed_note,
    int16_t base0, int16_t base1,
    uint8_t mod_level, uint8_t car_wave0, uint8_t car_wave1) {
    std::vector<unsigned char> bank;
    const char header[8] = {'#', 'O', 'P', 'L', '_', 'I', 'I', '#'};
    for (char c : header) {
        bank.push_back((unsigned char)c);
    }
    std::vector<unsigned char> record =
        make_genmidi_record(flags, fine, fixed_note, base0, base1,
                            mod_level, car_wave0, car_wave1);
    for (int i = 0; i < 175; ++i) {
        for (unsigned char b : record) {
            bank.push_back(b);
        }
    }
    return bank;
}

/* Banco FM minimo y valido: una voz, seno en los dos operadores, modulador
 * casi mudo (nivel 63) para que la portadora mande y la afinacion se mida. */
std::vector<unsigned char> simple_fm_bank() {
    return make_genmidi_bank(0, 128, 0, 0, 0, 63, 0, 0);
}

} // namespace

int main() {
    printf("SXMIDI TEST START\n");
    const uint32_t rate = SX_MIDI_DEFAULT_RATE;

    /* Un tema de 500 ms exactos a 120 BPM: 96 ticks por negra, 500000 us. */
    {
        std::vector<unsigned char> file = one_note_song(0, 500000);
        struct sx_midi_song* song = sx_midi_song_create(file.data(), file.size());
        check(song != nullptr, "un SMF valido se parsea");
        if (song != nullptr) {
            check(sx_midi_song_duration_ms(song) == 500,
                  "la duracion sale del mapa de tempo (500 ms)");

            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            check(sx_midi_song_render_all(song, rate, &pcm, &frames) == 0 && pcm != nullptr,
                  "render_all reserva y renderiza");
            check(frames == (uint32_t)((rate * 500u + 999u) / 1000u),
                  "renderiza tantos frames como dura el tema");
            if (pcm != nullptr) {
                check(count_non_silent(pcm, frames) > frames / 2,
                      "el tema suena: mas de la mitad de las muestras no son silencio");
                double frequency = estimate_frequency(pcm, rate / 5u, rate);
                check(frequency > 261.6 * 0.94 && frequency < 261.6 * 1.06,
                      "la afinacion de C4 (261.6 Hz) es correcta");
                free(pcm);
            }
            sx_midi_song_destroy(song);
        }
    }

    /* La mitad de tempo significa la mitad de frames. */
    {
        std::vector<unsigned char> file = one_note_song(0, 250000);
        struct sx_midi_song* song = sx_midi_song_create(file.data(), file.size());
        check(song != nullptr && sx_midi_song_duration_ms(song) == 250,
              "otro tempo cambia la duracion (250 ms)");
        sx_midi_song_destroy(song);
    }

    /* Regresion: con un numero impar de pasadas el merge ordenaba en un buffer
     * auxiliar y liberaba el arreglo del tema, corrompiendo un tick. Seis
     * eventos dan tres pasadas. */
    {
        Smf song;
        song.tempo(0, 500000);
        song.program(0, 0, 0);
        song.note_on(0, 0, 60, 100);
        song.note_off(48, 0, 60);
        song.note_on(0, 0, 64, 100);
        song.note_off(48, 0, 64);
        song.end(0);
        std::vector<unsigned char> file = song.build(96);
        struct sx_midi_song* parsed = sx_midi_song_create(file.data(), file.size());
        check(parsed != nullptr && sx_midi_song_duration_ms(parsed) == 500,
              "el orden por tick sobrevive a un numero impar de pasadas");
        if (parsed != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            bool ok = sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 &&
                      frames == (uint32_t)((rate * 500u + 999u) / 1000u) &&
                      count_non_silent(pcm, frames) > frames / 2;
            check(ok, "y el tema de seis eventos renderiza entero");
            free(pcm);
            sx_midi_song_destroy(parsed);
        }
    }

    /* Cambiar de familia cambia el timbre: el buffer no puede salir igual. */
    {
        std::vector<unsigned char> piano = one_note_song(0, 500000);
        std::vector<unsigned char> strings = one_note_song(40, 500000);
        struct sx_midi_song* a = sx_midi_song_create(piano.data(), piano.size());
        struct sx_midi_song* b = sx_midi_song_create(strings.data(), strings.size());
        check(a != nullptr && b != nullptr, "los dos programas se parsean");
        if (a != nullptr && b != nullptr) {
            unsigned char* pa = nullptr;
            unsigned char* pb = nullptr;
            uint32_t fa = 0;
            uint32_t fb = 0;
            if (sx_midi_song_render_all(a, rate, &pa, &fa) == 0 &&
                sx_midi_song_render_all(b, rate, &pb, &fb) == 0) {
                check(fa == fb && pa != nullptr && pb != nullptr,
                      "los dos programas rinden la misma duracion");
                check(memcmp(pa, pb, fa < fb ? fa : fb) != 0,
                      "el programa 40 suena distinto del programa 0");
            }
            free(pa);
            free(pb);
        }
        sx_midi_song_destroy(a);
        sx_midi_song_destroy(b);
    }

    /* La nota se apaga: tras la liberacion el resto es silencio. */
    {
        Smf song;
        song.tempo(0, 500000);
        song.program(0, 0, 0);
        song.note_on(0, 0, 60, 100);
        song.note_off(96, 0, 60); /* 500 ms */
        song.end(96);             /* 1000 ms */
        std::vector<unsigned char> file = song.build(96);
        struct sx_midi_song* parsed = sx_midi_song_create(file.data(), file.size());
        check(parsed != nullptr, "el tema con cola se parsea");
        if (parsed != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            if (sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 && pcm != nullptr) {
                size_t tail_start = (size_t)rate * 9u / 10u; /* ultimos 100 ms */
                check(frames > tail_start, "el buffer cubre la cola");
                if (frames > tail_start) {
                    check(tail_deviation(pcm + tail_start, frames - tail_start) < 2.0,
                          "tras la liberacion la cola queda en silencio");
                }
                free(pcm);
            }
            sx_midi_song_destroy(parsed);
        }
    }

    /* Con loop el render vuelve al principio y llena siempre. */
    {
        std::vector<unsigned char> file = one_note_song(0, 500000);
        struct sx_midi_song* song = sx_midi_song_create(file.data(), file.size());
        check(song != nullptr, "el tema para loop se parsea");
        if (song != nullptr) {
            const size_t pass = (size_t)((rate * 500u + 999u) / 1000u);
            unsigned char* first = (unsigned char*)malloc(pass);
            unsigned char* second = (unsigned char*)malloc(pass);
            check(first != nullptr && second != nullptr, "reserva los dos bloques del loop");
            if (first != nullptr && second != nullptr) {
                check(sx_midi_song_render(song, first, pass, rate, 1) == pass,
                      "con loop el primer pase se llena");
                check(sx_midi_song_render(song, second, pass, rate, 1) == pass,
                      "con loop el segundo pase tambien");
                check(memcmp(first, second, pass) == 0,
                      "el loop reinicia de forma determinista");
            }
            free(first);
            free(second);
            sx_midi_song_destroy(song);
        }
    }

    /* Percusion: canal 9, bombo. */
    {
        Smf song;
        song.tempo(0, 500000);
        song.note_on(0, 9, 36, 120);
        song.note_off(48, 9, 36);
        song.end(96);
        std::vector<unsigned char> file = song.build(96);
        struct sx_midi_song* parsed = sx_midi_song_create(file.data(), file.size());
        check(parsed != nullptr, "el tema de percusion se parsea");
        if (parsed != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            size_t window = rate / 5u; /* primeros 200 ms */
            if (sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 && pcm != nullptr) {
                check(frames > window && count_non_silent(pcm, window) > window / 4,
                      "el bombo del canal 9 suena");
                free(pcm);
            }
            sx_midi_song_destroy(parsed);
        }
    }

    /* El bombo barre el pitch: arranca muy arriba y cae a la nota. Un seno
     * fijo, que era lo que habia antes, da la misma frecuencia en las dos
     * ventanas. */
    {
        Smf song;
        song.tempo(0, 500000);
        song.note_on(0, 9, 36, 127);
        song.note_off(96, 9, 36);
        song.end(0);
        std::vector<unsigned char> file = song.build(96);
        struct sx_midi_song* parsed = sx_midi_song_create(file.data(), file.size());
        unsigned char* pcm = nullptr;
        uint32_t frames = 0;
        check(parsed != nullptr, "el tema de bombo para el barrido se parsea");
        if (parsed != nullptr && sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 &&
            pcm != nullptr) {
            double early = estimate_frequency(pcm, rate * 45u / 1000u, rate);
            double late = estimate_frequency(pcm + rate * 50u / 1000u, rate * 100u / 1000u, rate);
            check(late > 20.0 && early > late * 1.5,
                  "el bombo barre: el ataque suena al menos 1.5x mas agudo que la cola");
        }
        free(pcm);
        sx_midi_song_destroy(parsed);
    }

    /* Vibrato: el LFO sube y baja el pitch, asi que la misma ventana de tiempo
     * suena mas aguda en el pico del LFO que en el valle. Se mide en un timbre
     * con vibrato (pad, 4 Hz: pico y valle a 125 ms de distancia) contra uno
     * sin el (piano): el del piano cuenta los mismos cruces en las dos. */
    {
        struct {
            unsigned char program;
            const char* what;
        } timbres[2] = {
            {0, "sin vibrato las dos ventanas del LFO cuentan igual"},
            {88, "el LFO del pad hace la ventana del pico mas aguda que la del valle"},
        };
        long difference[2] = {0, 0};
        for (int index = 0; index < 2; ++index) {
            std::vector<unsigned char> file = long_note_song(timbres[index].program);
            struct sx_midi_song* song = sx_midi_song_create(file.data(), file.size());
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            check(song != nullptr, "el tema largo para el vibrato se parsea");
            if (song != nullptr && sx_midi_song_render_all(song, rate, &pcm, &frames) == 0 &&
                pcm != nullptr && frames >= rate) {
                double peak = estimate_frequency(pcm + rate / 2u, rate / 8u, rate);
                double valley = estimate_frequency(pcm + rate * 5u / 8u, rate / 8u, rate);
                difference[index] = (long)((peak - valley) * (rate / 8u) * 2.0 / (double)rate);
            }
            free(pcm);
            sx_midi_song_destroy(song);
        }
        /* El piso de ruido de la medida es la fase del borde de ventana: con un
         * periodo de 10.5 samples, dos ventanas ciegas difieren en +-2 cruces.
         * Un vibrato real mueve dos ordenes de magnitud mas. */
        printf("  --   vibrato (cruces de diferencia): programa 0 = %ld, programa 88 = %ld\n",
               difference[0], difference[1]);
        check(difference[0] <= 3 && difference[0] >= -3, timbres[0].what);
        check(difference[1] >= 20, timbres[1].what);
    }

    /* Parcial de octava: a medio periodo la correlacion vale -1 en un seno puro
     * y bastante mas en un timbre con octava encima, que es lo que hace a las
     * campanas. Y sigue siendo periodico al periodo completo. */
    {
        std::vector<unsigned char> file = one_note_song(8, 500000);
        struct sx_midi_song* song = sx_midi_song_create(file.data(), file.size());
        unsigned char* pcm = nullptr;
        uint32_t frames = 0;
        check(song != nullptr, "la campana se parsea");
        if (song != nullptr && sx_midi_song_render_all(song, rate, &pcm, &frames) == 0 &&
            pcm != nullptr && frames >= rate / 2u) {
            /* La ventana es corta a proposito: el parcial 2 va desafinado (asi
             * son las campanas), y con mas de un par de decimas de segundo su
             * propia deriva se come la correlacion de la nota entera. */
            double whole = periodic_correlation(
                pcm, rate / 20u, rate / 4u, (double)rate / kC4_Hz);
            double half = periodic_correlation(
                pcm, rate / 20u, rate / 4u, (double)rate / (2.0 * kC4_Hz));
            check(whole > 0.3, "la campana cierra consigo misma a la nota");
            check(half > -0.8, "la campana lleva la octava encima (no es un seno puro)");
        }
        free(pcm);
        sx_midi_song_destroy(song);
    }

    /* Camino FM: con un banco valido el tema usa GENMIDI, suena distinto que
     * las familias y afina. Sin banco o con uno ilegible cae en las familias. */
    {
        std::vector<unsigned char> file = one_note_song(0, 500000);
        std::vector<unsigned char> bank = simple_fm_bank();
        struct sx_midi_song* fm =
            sx_midi_song_create_with_bank(file.data(), file.size(),
                                          bank.data(), bank.size());
        check(fm != nullptr && sx_midi_song_uses_genmidi(fm) == 1,
              "un GENMIDI valido activa el camino FM");
        if (fm != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            if (sx_midi_song_render_all(fm, rate, &pcm, &frames) == 0 && pcm != nullptr) {
                check(count_non_silent(pcm, frames) > frames / 4,
                      "el FM suena: la envolvente lenta no se apaga en 6 ms");
                double frequency = estimate_frequency(pcm, rate / 5u, rate);
                check(frequency > 261.6 * 0.94 && frequency < 261.6 * 1.06,
                      "el FM afina C4 (261.6 Hz) con seno");
                free(pcm);
            }
            sx_midi_song_destroy(fm);
        }
    }

    /* El FM suena distinto que las familias: mismo MIDI, distinto buffer. */
    {
        std::vector<unsigned char> file = one_note_song(0, 500000);
        std::vector<unsigned char> bank = simple_fm_bank();
        struct sx_midi_song* fm =
            sx_midi_song_create_with_bank(file.data(), file.size(),
                                          bank.data(), bank.size());
        struct sx_midi_song* plain = sx_midi_song_create(file.data(), file.size());
        check(fm != nullptr && plain != nullptr, "los dos caminos se parsean");
        if (fm != nullptr && plain != nullptr) {
            unsigned char* a = nullptr;
            unsigned char* b = nullptr;
            uint32_t fa = 0;
            uint32_t fb = 0;
            if (sx_midi_song_render_all(fm, rate, &a, &fa) == 0 &&
                sx_midi_song_render_all(plain, rate, &b, &fb) == 0) {
                check(fa == fb && a != nullptr && b != nullptr,
                      "FM y familias rinden la misma duracion");
                check(memcmp(a, b, fa < fb ? fa : fb) != 0,
                      "el FM suena distinto que las familias");
            }
            free(a);
            free(b);
        }
        sx_midi_song_destroy(fm);
        sx_midi_song_destroy(plain);
    }

    /* Fallback: sin banco, corto o con cabecera rota cae en las familias y
     * suena exactamente igual que sin banco. */
    {
        std::vector<unsigned char> file = one_note_song(0, 500000);
        struct sx_midi_song* plain = sx_midi_song_create(file.data(), file.size());
        unsigned char* expect = nullptr;
        uint32_t expect_frames = 0;
        check(plain != nullptr, "la referencia sin banco se parsea");
        if (plain != nullptr) {
            sx_midi_song_render_all(plain, rate, &expect, &expect_frames);
            sx_midi_song_destroy(plain);
        }
        struct {
            const char* what;
            std::vector<unsigned char> bank;
        } cases[3] = {
            {"sin banco suena como familias", std::vector<unsigned char>()},
            {"un banco corto cae en familias",
             std::vector<unsigned char>(100, 0)},
            {"una cabecera rota cae en familias",
             [] {
                 std::vector<unsigned char> b = simple_fm_bank();
                 b[0] = 'B';
                 return b;
             }()},
        };
        for (int i = 0; i < 3; ++i) {
            const void* data = cases[i].bank.empty() ? nullptr : cases[i].bank.data();
            size_t bytes = cases[i].bank.size();
            struct sx_midi_song* song = sx_midi_song_create_with_bank(
                file.data(), file.size(), data, bytes);
            bool same = false;
            check(song != nullptr && sx_midi_song_uses_genmidi(song) == 0,
                  cases[i].what);
            if (song != nullptr) {
                unsigned char* pcm = nullptr;
                uint32_t frames = 0;
                if (sx_midi_song_render_all(song, rate, &pcm, &frames) == 0 &&
                    pcm != nullptr && expect != nullptr) {
                    same = frames == expect_frames &&
                           memcmp(pcm, expect, frames) == 0;
                }
                free(pcm);
                sx_midi_song_destroy(song);
            }
            check(same, "y el fallback rinde el mismo PCM");
        }
        /* Un registro que no puede venir del chip tampoco se adivina: forma de
         * onda 0xff en el primer operador. */
        {
            std::vector<unsigned char> bank = simple_fm_bank();
            bank[8 + 4 + 3] = 0xff;
            struct sx_midi_song* song = sx_midi_song_create_with_bank(
                file.data(), file.size(), bank.data(), bank.size());
            check(song != nullptr && sx_midi_song_uses_genmidi(song) == 0,
                  "una forma imposible cae en familias");
            if (song != nullptr) {
                unsigned char* pcm = nullptr;
                uint32_t frames = 0;
                bool same = false;
                if (sx_midi_song_render_all(song, rate, &pcm, &frames) == 0 &&
                    pcm != nullptr && expect != nullptr) {
                    same = frames == expect_frames &&
                           memcmp(pcm, expect, frames) == 0;
                }
                check(same, "y la forma imposible rinde el mismo PCM");
                free(pcm);
                sx_midi_song_destroy(song);
            }
        }
        free(expect);
    }

    /* Doble voz: dos juegos de operadores distintos no pueden sonar igual que
     * uno duplicado. Si la segunda voz se ignorara, los dos bancos darian lo
     * mismo. */
    {
        std::vector<unsigned char> file = one_note_song(0, 500000);
        std::vector<unsigned char> dup = make_genmidi_bank(4, 128, 0, 0, 0, 63, 0, 0);
        std::vector<unsigned char> distinct =
            make_genmidi_bank(4, 128, 0, 0, 0, 63, 0, 2);
        struct sx_midi_song* a =
            sx_midi_song_create_with_bank(file.data(), file.size(),
                                          dup.data(), dup.size());
        struct sx_midi_song* b =
            sx_midi_song_create_with_bank(file.data(), file.size(),
                                          distinct.data(), distinct.size());
        check(a != nullptr && b != nullptr &&
                  sx_midi_song_uses_genmidi(a) == 1 &&
                  sx_midi_song_uses_genmidi(b) == 1,
              "los dos bancos dobles activan FM");
        if (a != nullptr && b != nullptr) {
            unsigned char* pa = nullptr;
            unsigned char* pb = nullptr;
            uint32_t fa = 0;
            uint32_t fb = 0;
            bool different = false;
            if (sx_midi_song_render_all(a, rate, &pa, &fa) == 0 &&
                sx_midi_song_render_all(b, rate, &pb, &fb) == 0 &&
                pa != nullptr && pb != nullptr) {
                different = fa == fb && memcmp(pa, pb, fa) != 0;
            }
            check(different, "la segunda voz OPL aporta su propio timbre");
            free(pa);
            free(pb);
        }
        sx_midi_song_destroy(a);
        sx_midi_song_destroy(b);
    }

    /* Pitch bend en FM: +-2 semitonos, no +-8. Al maximo, C4 sube a D4. */
    {
        Smf song;
        song.tempo(0, 500000);
        song.program(0, 0, 0);
        song.bend(0, 0, 16383);
        song.note_on(0, 0, 60, 100);
        song.note_off(96, 0, 60);
        song.end(0);
        std::vector<unsigned char> file = song.build(96);
        std::vector<unsigned char> bank = simple_fm_bank();
        struct sx_midi_song* parsed = sx_midi_song_create_with_bank(
            file.data(), file.size(), bank.data(), bank.size());
        check(parsed != nullptr && sx_midi_song_uses_genmidi(parsed) == 1,
              "el tema con bend FM se parsea");
        if (parsed != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            if (sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 &&
                pcm != nullptr) {
                double frequency = estimate_frequency(pcm, rate / 5u, rate);
                check(frequency > 293.7 * 0.94 && frequency < 293.7 * 1.06,
                      "el bend FM sube dos semitonos (C4->D4)");
            }
            free(pcm);
            sx_midi_song_destroy(parsed);
        }
    }

    /* Percusion FM: la tecla 36 suena y la 20 (fuera de 35..81) no abre voz. */
    {
        Smf hit;
        hit.tempo(0, 500000);
        hit.note_on(0, 9, 36, 120);
        hit.note_off(48, 9, 36);
        hit.end(96);
        std::vector<unsigned char> hit_file = hit.build(96);
        std::vector<unsigned char> bank = simple_fm_bank();
        struct sx_midi_song* parsed = sx_midi_song_create_with_bank(
            hit_file.data(), hit_file.size(), bank.data(), bank.size());
        check(parsed != nullptr && sx_midi_song_uses_genmidi(parsed) == 1,
              "la percusion FM se parsea");
        if (parsed != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            size_t window = rate / 5u;
            if (sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 &&
                pcm != nullptr) {
                check(frames > window && count_non_silent(pcm, window) > window / 8,
                      "el bombo FM del canal 9 suena");
                free(pcm);
            }
            sx_midi_song_destroy(parsed);
        }
        Smf miss;
        miss.tempo(0, 500000);
        miss.note_on(0, 9, 20, 120);
        miss.note_off(48, 9, 20);
        miss.end(96);
        std::vector<unsigned char> miss_file = miss.build(96);
        struct sx_midi_song* silent = sx_midi_song_create_with_bank(
            miss_file.data(), miss_file.size(), bank.data(), bank.size());
        check(silent != nullptr && sx_midi_song_uses_genmidi(silent) == 1,
              "la tecla fuera de rango tambien usa el banco");
        if (silent != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            if (sx_midi_song_render_all(silent, rate, &pcm, &frames) == 0 &&
                pcm != nullptr) {
                check(count_non_silent(pcm, frames) == 0,
                      "fuera de 35..81 el FM no abre voz");
                free(pcm);
            }
            sx_midi_song_destroy(silent);
        }
    }

    /* Nota fija: un instrumento fijo en 96 suena C7 aunque el evento pida 48.
     * La nota fija no se envuelve a la octava: un bombo en 90 no es el mismo
     * una octava abajo. */
    {
        Smf song;
        song.tempo(0, 500000);
        song.program(0, 0, 0);
        song.note_on(0, 0, 48, 100);
        song.note_off(96, 0, 48);
        song.end(0);
        std::vector<unsigned char> file = song.build(96);
        std::vector<unsigned char> bank = make_genmidi_bank(1, 128, 96, 0, 0, 63, 0, 0);
        struct sx_midi_song* parsed = sx_midi_song_create_with_bank(
            file.data(), file.size(), bank.data(), bank.size());
        check(parsed != nullptr && sx_midi_song_uses_genmidi(parsed) == 1,
              "el instrumento fijo se parsea");
        if (parsed != nullptr) {
            unsigned char* pcm = nullptr;
            uint32_t frames = 0;
            if (sx_midi_song_render_all(parsed, rate, &pcm, &frames) == 0 &&
                pcm != nullptr) {
                double frequency = estimate_frequency(pcm, rate / 5u, rate);
                check(frequency > 2093.0 * 0.94 && frequency < 2093.0 * 1.06,
                      "la nota fija no baila con el teclado (ni se envuelve)");
                free(pcm);
            }
            sx_midi_song_destroy(parsed);
        }
    }

    /* Datos que no son un MIDI se rechazan en vez de adivinarse. */
    {
        const unsigned char junk[32] = {'N', 'O', 'T', 'A', 'M', 'I', 'D', 'I'};
        check(sx_midi_song_create(junk, sizeof(junk)) == nullptr,
              "un blob que no es MThd se rechaza");
        std::vector<unsigned char> file = one_note_song(0, 500000);
        check(sx_midi_song_create(file.data(), 10) == nullptr,
              "un SMF truncado se rechaza");
    }

    printf(g_failures == 0 ? "SXMIDI TEST PASS\n" : "SXMIDI TEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
