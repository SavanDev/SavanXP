/*
 * Sintetizador MIDI de SavanXP (libsxmidi.so.0.4).
 *
 * Lee un Standard MIDI File (tipo 0 o 1) en memoria, lo ejecuta contra un banco
 * General MIDI horneado en este mismo archivo y escribe PCM mono
 * unsigned-8-bit, que es la forma en que el mixer del runtime acepta una voz
 * (savanxp/audio.h). No hay soundfont que instalar ni libm que enlazar: el motor
 * entero es punto fijo y la unica tabla que se calcula es la del seno.
 *
 * Hay dos fuentes de instrumentos: las dieciseis familias subtractivas de
 * fabrica, y el banco OPL2 de dos operadores que un WAD de Doom trae en su lump
 * `GENMIDI`, que el llamador pasa junto con el MIDI. Sin banco (o con uno
 * ilegible) el tema suena con las familias. El diseno y las decisiones (por que
 * familias y no 128 instrumentos, por que una voz por nota, por que Doom
 * pre-renderiza el tema) estan en docs/MIDI.md.
 *
 * Se compila dos veces: dentro de la libreria (PIC, target propio) y dentro del
 * test de host, con la libc de Linux. Por eso no incluye headers del runtime
 * salvo los del SDK.
 */
#include "savanxp/midi.h"

#include <stdlib.h>
#include <string.h>

#define SX_MIDI_ENV_MAX 32768
#define SX_MIDI_DEFAULT_TEMPO_US 500000u
#define SX_MIDI_SINE_BITS 10u
#define SX_MIDI_SINE_SIZE (1u << SX_MIDI_SINE_BITS)
#define SX_MIDI_OCTAVE_SEMITONES 12
#define SX_MIDI_A4_NOTE 69
#define SX_MIDI_A4_HZ_Q16 (440u << 16)
#define SX_MIDI_PERCUSSION_CHANNEL 9
#define SX_MIDI_NO_WAVE 0xffu

/* --- Banco de instrumentos ------------------------------------------------- */

enum {
    SX_MIDI_WAVE_SINE = 0,
    SX_MIDI_WAVE_TRIANGLE,
    SX_MIDI_WAVE_SAW,
    SX_MIDI_WAVE_SQUARE,
    SX_MIDI_WAVE_PULSE,
    SX_MIDI_WAVE_NOISE,
};

/* Un timbre son dos osciladores (el segundo opcional y a un intervalo elegido),
 * una mezcla hacia el seno, un filtro de un polo y una envolvente ADSR.
 *
 * Los campos que hacen el espectro, no solo la nota:
 *  - `interval2`/`level2`: a cuantos semitonos sube (o baja, negativo) el
 *    parcial 2 y con que peso frente al 1. Un timbre de un solo oscilador usa
 *    `level2` en cero; la octava arriba da campanas y organos, el un poco
 *    abajo da cuerpo.
 *  - `wave_mix`: mezcla la forma hacia un seno. Las formas ideales (sierra,
 *    cuadrada) son brillantes hasta el extremo; con esto y el filtro se elige
 *    cuanto. 0 = forma pura, 255 = seno puro.
 *  - `vibrato`/`lfo_hz`: LFO senoidal que mueve el pitch (0 = apagado).
 *    `vibrato` esta en 1/512 de semitono por unidad: 20 son ~4 centesimos de
 *    semitono, 100 medio semitono.
 *  - `sweep_semitones`/`sweep_ms`: barrido de ataque. El parcial arranca
 *    `sweep_semitones` arriba de la nota y baja hasta ella en `sweep_ms`; es lo
 *    que hace un bombo de sintetizador. 0 = sin barrido.
 *  - `key_scale`: cuanto se cierra el filtro en las notas altas (0 = nada),
 *    el equivalente pobre del key scaling de un chip FM.
 *
 * `base_note` en -1 significa "usa la nota del evento"; si no, la fija (lo usan
 * los tambores para no bailar con el teclado). `filter` es el coeficiente del
 * one-pole: 0 lo saltea, 255 esta abierto. */
struct sx_midi_instrument {
    uint8_t wave;
    uint8_t wave2;
    uint8_t level2;
    int8_t interval2;
    uint8_t detune;
    uint8_t wave_mix;
    uint8_t filter;
    uint8_t key_scale;
    uint16_t attack_ms;
    uint16_t decay_ms;
    uint8_t sustain;
    uint16_t release_ms;
    uint8_t level;
    int16_t base_note;
    uint8_t vibrato;
    uint8_t lfo_hz;
    uint16_t sweep_ms;
    uint8_t sweep_semitones;
};

/* Una familia por cada ocho numeros de programa General MIDI. 128 instrumentos
 * fieles pedirian un soundfont y un sampler; 16 familias con dos parciales, un
 * filtro y una envolvente dan el caracter de cada grupo (cuerda, metal,
 * percusion) y suenan a la pieza, que es lo que Doom necesita.
 *
 * Los valores estan escritos como intencion, no como medida: el piano
 * percusivo, la cuerda que ataca lento y respira, la lengueta con vibrato. Ese
 * es el margen de mejora que queda -- ver `docs/MIDI.md`. */
static const struct sx_midi_instrument g_families[16] = {
    /* 0  Piano: sierra con un poco de seno encima, decaimiento largo y cola
     *    corta. Sin parcial 2: la afinacion de este programa se verifica por
     *    cruces por cero en el test de host, y una octava extra ensuciaria esa
     *    cuenta sin agregar nada que el oido note mas que la mezcla. */
    { .wave = SX_MIDI_WAVE_SAW, .wave2 = SX_MIDI_NO_WAVE, .level2 = 0,
      .interval2 = 0, .detune = 0, .wave_mix = 40, .filter = 205,
      .key_scale = 10, .attack_ms = 2, .decay_ms = 600, .sustain = 20,
      .release_ms = 220, .level = 110, .base_note = -1 },
    /* 1  Percusion cromatica (campanas, xilofono): seno + octava, decaimiento
     *    rapido y sin sostenido. */
    { .wave = SX_MIDI_WAVE_SINE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 64,
      .interval2 = 12, .detune = 4, .wave_mix = 255, .filter = 235,
      .key_scale = 12, .attack_ms = 2, .decay_ms = 420, .sustain = 0,
      .release_ms = 200, .level = 100, .base_note = -1 },
    /* 2  Organo: cuadrada con la octava arriba, sostenido pleno y sin vibrato
     *    (el organo no respira). */
    { .wave = SX_MIDI_WAVE_SQUARE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 70,
      .interval2 = 12, .detune = 2, .wave_mix = 90, .filter = 120,
      .key_scale = 6, .attack_ms = 8, .decay_ms = 40, .sustain = 112,
      .release_ms = 90, .level = 92, .base_note = -1 },
    /* 3  Guitarra: pulsada, con cuerpo y quinta arriba. */
    { .wave = SX_MIDI_WAVE_SAW, .wave2 = SX_MIDI_WAVE_TRIANGLE, .level2 = 34,
      .interval2 = 7, .detune = 3, .wave_mix = 30, .filter = 200,
      .key_scale = 14, .attack_ms = 2, .decay_ms = 500, .sustain = 10,
      .release_ms = 180, .level = 104, .base_note = -1 },
    /* 4  Bajo: cuadrada con la octava arriba para que se lea en la mezcla. */
    { .wave = SX_MIDI_WAVE_SQUARE, .wave2 = SX_MIDI_WAVE_SAW, .level2 = 45,
      .interval2 = 12, .detune = 4, .wave_mix = 40, .filter = 190,
      .key_scale = 8, .attack_ms = 2, .decay_ms = 400, .sustain = 60,
      .release_ms = 120, .level = 118, .base_note = -1 },
    /* 5  Cuerdas: dos sierras desafinadas, ataque lento y vibrato. */
    { .wave = SX_MIDI_WAVE_SAW, .wave2 = SX_MIDI_WAVE_SAW, .level2 = 90,
      .interval2 = 0, .detune = 8, .wave_mix = 20, .filter = 225,
      .key_scale = 10, .attack_ms = 150, .decay_ms = 250, .sustain = 105,
      .release_ms = 350, .level = 92, .base_note = -1,
      .vibrato = 14, .lfo_hz = 5 },
    /* 6  Ensemble: lo mismo con mas desafinado y mas vibrato. */
    { .wave = SX_MIDI_WAVE_SAW, .wave2 = SX_MIDI_WAVE_SAW, .level2 = 95,
      .interval2 = 0, .detune = 14, .wave_mix = 20, .filter = 228,
      .key_scale = 10, .attack_ms = 200, .decay_ms = 240, .sustain = 108,
      .release_ms = 420, .level = 88, .base_note = -1,
      .vibrato = 18, .lfo_hz = 5 },
    /* 7  Metal (trompeta, trombon): cuadrada con la quinta, ataque corto. */
    { .wave = SX_MIDI_WAVE_SQUARE, .wave2 = SX_MIDI_WAVE_SAW, .level2 = 55,
      .interval2 = 7, .detune = 2, .wave_mix = 20, .filter = 200,
      .key_scale = 12, .attack_ms = 35, .decay_ms = 150, .sustain = 100,
      .release_ms = 180, .level = 100, .base_note = -1,
      .vibrato = 8, .lfo_hz = 5 },
    /* 8  Lengueta (saxo, oboe): pulso del 25% con la octava, vibrato marcado. */
    { .wave = SX_MIDI_WAVE_PULSE, .wave2 = SX_MIDI_WAVE_SQUARE, .level2 = 40,
      .interval2 = 12, .detune = 3, .wave_mix = 60, .filter = 190,
      .key_scale = 10, .attack_ms = 30, .decay_ms = 150, .sustain = 100,
      .release_ms = 180, .level = 100, .base_note = -1,
      .vibrato = 12, .lfo_hz = 6 },
    /* 9  Flauta: seno con la octava muy suave, aire y vibrato. */
    { .wave = SX_MIDI_WAVE_SINE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 25,
      .interval2 = 12, .detune = 4, .wave_mix = 255, .filter = 150,
      .key_scale = 8, .attack_ms = 70, .decay_ms = 160, .sustain = 108,
      .release_ms = 220, .level = 96, .base_note = -1,
      .vibrato = 10, .lfo_hz = 6 },
    /* 10 Synth lead: sierra con la octava, vibrato lento. */
    { .wave = SX_MIDI_WAVE_SAW, .wave2 = SX_MIDI_WAVE_SQUARE, .level2 = 40,
      .interval2 = 12, .detune = 3, .wave_mix = 40, .filter = 200,
      .key_scale = 12, .attack_ms = 5, .decay_ms = 250, .sustain = 95,
      .release_ms = 150, .level = 100, .base_note = -1,
      .vibrato = 8, .lfo_hz = 6 },
    /* 11 Synth pad: dos sierras muy desafinadas, ataque y cola largos. */
    { .wave = SX_MIDI_WAVE_SAW, .wave2 = SX_MIDI_WAVE_SAW, .level2 = 100,
      .interval2 = 0, .detune = 16, .wave_mix = 30, .filter = 232,
      .key_scale = 12, .attack_ms = 350, .decay_ms = 350, .sustain = 110,
      .release_ms = 600, .level = 84, .base_note = -1,
      .vibrato = 20, .lfo_hz = 4 },
    /* 12 FX: seno con ruido encima. */
    { .wave = SX_MIDI_WAVE_SINE, .wave2 = SX_MIDI_WAVE_NOISE, .level2 = 70,
      .interval2 = 0, .detune = 0, .wave_mix = 255, .filter = 170,
      .key_scale = 0, .attack_ms = 10, .decay_ms = 500, .sustain = 40,
      .release_ms = 400, .level = 80, .base_note = -1 },
    /* 13 Etnico (pizzicato, pluck): triangulo con la octava, pulsado. */
    { .wave = SX_MIDI_WAVE_TRIANGLE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 45,
      .interval2 = 12, .detune = 2, .wave_mix = 200, .filter = 210,
      .key_scale = 12, .attack_ms = 2, .decay_ms = 300, .sustain = 12,
      .release_ms = 150, .level = 106, .base_note = -1 },
    /* 14 Percusivo (marimba, campana): triangulo con la doceava, seca. */
    { .wave = SX_MIDI_WAVE_TRIANGLE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 35,
      .interval2 = 19, .detune = 2, .wave_mix = 230, .filter = 212,
      .key_scale = 14, .attack_ms = 1, .decay_ms = 190, .sustain = 0,
      .release_ms = 90, .level = 106, .base_note = -1 },
    /* 15 Efectos de sonido: ruido. */
    { .wave = SX_MIDI_WAVE_NOISE, .wave2 = SX_MIDI_NO_WAVE, .level2 = 0,
      .interval2 = 0, .detune = 0, .wave_mix = 0, .filter = 128,
      .key_scale = 0, .attack_ms = 2, .decay_ms = 220, .sustain = 0,
      .release_ms = 120, .level = 82, .base_note = -1 },
};

/* Tambores, indexados por la nota General MIDI. Siete formas alcanzan para un
 * kit: ruido para la caja y los platos, seno o triangulo para el bombo y los
 * toms. El barrido de pitch es lo que separa un bombo de un zumbido -- el seno
 * arranca dos octavas arriba y cae a la nota en 45 ms -- y la caja lleva un
 * cuerpo tonal corto debajo del ruido. */
enum {
    SX_MIDI_DRUM_KICK = 0,
    SX_MIDI_DRUM_SNARE,
    SX_MIDI_DRUM_CLAP,
    SX_MIDI_DRUM_HAT,
    SX_MIDI_DRUM_OPEN_HAT,
    SX_MIDI_DRUM_TOM,
    SX_MIDI_DRUM_CYMBAL,
    SX_MIDI_DRUM_COUNT,
};

static struct sx_midi_instrument g_drums[SX_MIDI_DRUM_COUNT];
static uint8_t g_drum_index[128];
static int16_t g_sine[SX_MIDI_SINE_SIZE];
static int g_bank_ready = 0;

/* --- Estado ---------------------------------------------------------------- */

enum {
    SX_MIDI_STAGE_ATTACK = 0,
    SX_MIDI_STAGE_DECAY,
    SX_MIDI_STAGE_SUSTAIN,
    SX_MIDI_STAGE_RELEASE,
};

enum {
    SX_MIDI_EVENT_NOTE_OFF = 0,
    SX_MIDI_EVENT_NOTE_ON,
    SX_MIDI_EVENT_CONTROL,
    SX_MIDI_EVENT_PROGRAM,
    SX_MIDI_EVENT_PITCH,
    SX_MIDI_EVENT_TEMPO,
};

/* Evento absoluto ya fusionado de todas las pistas y ordenado por tick. */
struct sx_midi_event {
    uint64_t tick;
    uint32_t tempo_us;
    uint8_t kind;
    uint8_t channel;
    uint8_t a;
    uint8_t b;
};

struct sx_midi_channel {
    uint8_t program;
    uint8_t volume;
    uint8_t pan;
    uint16_t bend; /* 0..16383, 8192 es el centro */
};

struct sx_midi_voice {
    int active;
    int stage;
    const struct sx_midi_instrument* inst;
    uint8_t channel;
    uint8_t note;
    int16_t play_note; /* nota que suena (un tambor puede fijarla) */
    uint8_t wave;
    uint8_t wave2;
    uint8_t level2;   /* peso del parcial 2 (0 = no suena) */
    uint8_t wave_mix; /* mezcla de las formas hacia el seno */
    uint16_t noise;
    uint8_t filter;   /* ya escalado por la nota */
    uint32_t phase;
    uint32_t phase2;
    /* Paso por sample de cada parcial SIN los modificadores de pitch: el
     * barrido y el vibrato se aplican encima, en `voice_pitch_factor`, para que
     * un evento de pitch bend pueda rehacer la base sin perderlos. */
    uint32_t base_step;
    uint32_t base_step2;
    uint32_t sweep_scale; /* multiplicador Q16 del barrido, baja a 1.0 */
    uint32_t sweep_step;
    uint32_t lfo_phase;
    uint32_t lfo_step;
    uint32_t vibrato_depth; /* Q16 de semitono, pico */
    int32_t env;
    int32_t sustain_level;
    int32_t attack_step;
    int32_t decay_step;
    int32_t release_step;
    int32_t gain;
    int32_t filter_state;
    /* Camino FM: el banco OPL2 que vino con el tema. `patch` en cero significa
     * que la voz usa una familia subtractiva y el resto de estos campos no se
     * toca. Los dos operadores son el modulador (0) y la portadora (1) de UNA
     * voz OPL; un instrumento doble suena con dos voces `sx_midi_voice`. */
    const struct sx_midi_fm_patch* patch;
    uint32_t fm_mul[2];      /* multiplicador de frecuencia en Q16 */
    uint32_t fm_mod_base;    /* paso Q32 de la nota, sin el multiplicador */
    uint32_t fm_mod_step;
    uint32_t fm_car_step;
    uint32_t fm_mod_phase;
    uint32_t fm_car_phase;
    uint32_t fm_am_phase;
    uint32_t fm_am_step;
    uint32_t fm_vib_phase;
    uint32_t fm_vib_step;
    uint8_t fm_sub;        /* sub-voz del instrumento: 0 o 1 */
    uint8_t fm_additive;   /* conexion del chip: 1 = suma, 0 = FM */
    uint8_t fm_feedback;   /* 0..7 sobre el modulador */
    uint8_t fm_am;         /* algun operador pide tremolo */
    uint8_t fm_vib;        /* algun operador pide vibrato */
    uint8_t fm_level[2];   /* atenuacion fija: nivel del operador + key scaling */
    uint8_t fm_sustain[2]; /* tipo de EG de cada operador */
    int32_t fm_att[2];     /* atenuacion de la envolvente, en Q16 (ver ATT_Q) */
    int32_t fm_attack_step[2]; /* pasos Q16 por sample */
    int32_t fm_decay_step[2];
    int32_t fm_release_step[2];
    int32_t fm_sl[2];      /* nivel de sostenido de cada operador, en Q16 */
    int32_t fm_fb1;        /* las dos ultimas salidas del modulador, ya con nivel */
    int32_t fm_fb2;
};

struct sx_midi_song {
    uint16_t division;
    struct sx_midi_event* events;
    size_t event_count;
    size_t event_capacity;
    uint32_t end_tick;
    uint32_t declared_end_tick;
    uint64_t duration_us;
    struct sx_midi_channel channel[16];
    struct sx_midi_voice voices[SX_MIDI_MAX_VOICES];
    uint32_t volume;
    /* Estado de reproduccion. `rate` en cero significa "sin empezar". */
    uint32_t rate;
    uint32_t tempo_us;
    uint32_t tick;
    /* El avance de tick es exacto: se acumula `division * 1e6` por sample y
     * cada vez que se cruza `rate * tempo_us` avanza un tick. Con un step
     * truncado en Q16 el error se acumulaba (~0.1% de deriva) y el loop no
     * volvia al mismo punto. */
    uint64_t tick_accum;
    uint64_t tick_threshold;
    size_t next_event;
    int finished;
    /* Banco OPL2 del tema (el lump `GENMIDI` del WAD), o cero si el tema se
     * quedo con las familias. Es del tema y no del proceso: dos temas del mismo
     * programa pueden sonar con bancos distintos, y sin banco no hay estado
     * global que limpiar. */
    struct sx_midi_fm_patch* fm_bank;
};

/* --- Tabla del seno -------------------------------------------------------- */

/* Un seno en punto fijo sin libm: Taylor de quinto orden sobre [0, pi/2] en
 * Q16 y simetria para el resto del circulo. El error es menor que el redondeo a
 * int16 que se guarda. */
static void sx_midi_build_sine(void) {
    const int quarter = (int)(SX_MIDI_SINE_SIZE / 4u);
    const int half = (int)(SX_MIDI_SINE_SIZE / 2u);
    const int three_quarters = (int)(3u * SX_MIDI_SINE_SIZE / 4u);
    int k;

    for (k = 0; k <= quarter; ++k) {
        int64_t x = ((int64_t)k * 102944) / quarter; /* pi/2 * 2^16 */
        int64_t x2 = (x * x) >> 16;
        int64_t term = x;
        int64_t value = x;

        term = (term * x2) >> 16;
        value -= term / 6;
        term = (term * x2) >> 16;
        value += term / 120;
        term = (term * x2) >> 16;
        value -= term / 5040;

        g_sine[k] = (int16_t)(value >> 1);
    }
    for (k = quarter + 1; k < half; ++k) {
        g_sine[k] = g_sine[half - k];
    }
    for (k = half; k < three_quarters; ++k) {
        g_sine[k] = (int16_t)-g_sine[k - half];
    }
    for (k = three_quarters; k < (int)SX_MIDI_SINE_SIZE; ++k) {
        g_sine[k] = (int16_t)-g_sine[(int)SX_MIDI_SINE_SIZE - k];
    }
}

static void sx_midi_build_drums(void) {
    int note;

    g_drums[SX_MIDI_DRUM_KICK] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_SINE, .filter = 215, .attack_ms = 1,
        .decay_ms = 130, .release_ms = 70, .level = 122, .base_note = 34,
        .sweep_ms = 45, .sweep_semitones = 24 };
    /* Caja: ruido con un cuerpo de seno a la nota de la caja debajo. */
    g_drums[SX_MIDI_DRUM_SNARE] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_NOISE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 40,
        .interval2 = 0, .filter = 205, .attack_ms = 1, .decay_ms = 120,
        .release_ms = 70, .level = 112, .base_note = 60 };
    g_drums[SX_MIDI_DRUM_CLAP] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_NOISE, .filter = 230, .attack_ms = 1,
        .decay_ms = 110, .release_ms = 60, .level = 96, .base_note = 72 };
    g_drums[SX_MIDI_DRUM_HAT] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_NOISE, .filter = 245, .attack_ms = 1,
        .decay_ms = 45, .release_ms = 25, .level = 78, .base_note = 90 };
    g_drums[SX_MIDI_DRUM_OPEN_HAT] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_NOISE, .filter = 245, .attack_ms = 1,
        .decay_ms = 320, .release_ms = 160, .level = 74, .base_note = 90 };
    g_drums[SX_MIDI_DRUM_TOM] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_TRIANGLE, .wave2 = SX_MIDI_WAVE_SINE, .level2 = 50,
        .interval2 = 0, .detune = 3, .filter = 210, .attack_ms = 1,
        .decay_ms = 160, .release_ms = 90, .level = 104, .base_note = -1,
        .sweep_ms = 80, .sweep_semitones = 12 };
    g_drums[SX_MIDI_DRUM_CYMBAL] = (struct sx_midi_instrument){
        .wave = SX_MIDI_WAVE_NOISE, .filter = 250, .attack_ms = 1,
        .decay_ms = 700, .release_ms = 320, .level = 66, .base_note = 90 };

    for (note = 0; note < 128; ++note) {
        g_drum_index[note] = (uint8_t)SX_MIDI_DRUM_SNARE;
    }
    g_drum_index[35] = (uint8_t)SX_MIDI_DRUM_KICK;
    g_drum_index[36] = (uint8_t)SX_MIDI_DRUM_KICK;
    g_drum_index[38] = (uint8_t)SX_MIDI_DRUM_SNARE;
    g_drum_index[40] = (uint8_t)SX_MIDI_DRUM_SNARE;
    g_drum_index[39] = (uint8_t)SX_MIDI_DRUM_CLAP;
    g_drum_index[42] = (uint8_t)SX_MIDI_DRUM_HAT;
    g_drum_index[44] = (uint8_t)SX_MIDI_DRUM_HAT;
    g_drum_index[46] = (uint8_t)SX_MIDI_DRUM_OPEN_HAT;
    for (note = 41; note <= 50; note += 2) {
        g_drum_index[note] = (uint8_t)SX_MIDI_DRUM_TOM;
    }
    g_drum_index[49] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
    g_drum_index[51] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
    g_drum_index[52] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
    g_drum_index[53] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
    g_drum_index[55] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
    g_drum_index[57] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
    g_drum_index[59] = (uint8_t)SX_MIDI_DRUM_CYMBAL;
}

/* El banco OPL2 se decodifica mas abajo, en su propia seccion; el tema guarda
 * un puntero a sus instrumentos y las dos tablas se arman con el resto. */
struct sx_midi_fm_patch;
static void sx_midi_build_fm_gain(void);
static void sx_midi_build_fm_rates(void);

static void sx_midi_bank_init(void) {
    if (g_bank_ready) {
        return;
    }
    sx_midi_build_sine();
    sx_midi_build_drums();
    sx_midi_build_fm_gain();
    sx_midi_build_fm_rates();
    g_bank_ready = 1;
}

/* --- Banco OPL2 (GENMIDI) -------------------------------------------------- */

/* Un WAD de Doom trae el banco de instrumentos que DMX le programaba al chip FM
 * en el lump `GENMIDI`: 128 instrumentos melodicos y 47 de percusion, cada uno
 * con dos voces OPL de dos operadores. Esto es ese registro ya decodificado,
 * y es lo que el camino FM usa en lugar de las dieciseis familias.
 *
 * Cada operador tiene una de las cuatro formas de OPL2, un multiplicador de
 * frecuencia, un nivel fijo, key scaling y su propia envolvente ADSR; el
 * modulador ademas puede realimentarse. La conexion decide si el modulador
 * mueve la fase de la portadora (FM) o si las dos salidas se suman, y cada voz
 * trae su propio `base_note_offset`; `fine_index` desafina solo la segunda voz
 * de un instrumento doble.
 *
 * El formato es el del chip, no el de un archivo de musica: los campos son los
 * valores de registro de OPL2 tal cual, por eso las tasas van de 0 a 15 y el
 * nivel de 0 a 63. */
struct sx_midi_fm_op {
    uint8_t waveform;   /* 0..3: seno, media onda, rectificado, pulso */
    uint8_t multiplier; /* 0..15: 0 = x0.5, 1 = x1, 2 = x2 ... 15 = x15 */
    uint8_t level;      /* atenuacion fija 0..63, en pasos de 0.75 dB */
    uint8_t ksl;        /* key scale level: 1.5, 3 o 6 dB por octava */
    uint8_t attack;     /* las cuatro tasas 0 (lento) .. 15 (rapido) */
    uint8_t decay;
    uint8_t sustain;    /* nivel de sostenido, en pasos de 3 dB; 15 = silencio */
    uint8_t release;
    uint8_t am;         /* tremolo del chip */
    uint8_t vibrato;    /* vibrato del chip */
    uint8_t ksr;        /* la envolvente corre mas rapido en las notas altas */
    uint8_t sustaining; /* 0 = percusivo: sigue cayendo en el sostenido */
};

struct sx_midi_fm_patch {
    /* Dos voces OPL por instrumento, como las trae el lump: cada una con su
     * modulador, su portadora, su feedback y su corrimiento de nota. Si el
     * instrumento no pide doble voz solo suena la primera; si la pide, la
     * segunda va desafinada por `fine_index`. */
    struct sx_midi_fm_op mod[2];
    struct sx_midi_fm_op car[2];
    int16_t base_note_offset[2]; /* semitonos; no se aplica a una nota fija */
    int8_t fine_index;           /* afinacion fina de la segunda voz, en 1/32 */
    uint8_t fixed;               /* la nota la fija `fixed_note` */
    uint8_t fixed_note;
    uint8_t two_voice;           /* pide la segunda voz OPL, desafinada */
    uint8_t feedback[2];         /* 0..7 sobre el modulador, por voz */
    uint8_t additive[2];         /* conexion: 1 = suma en vez de FM, por voz */
};

#define SX_MIDI_GENMIDI_HEADER "#OPL_II#"
#define SX_MIDI_GENMIDI_HEADER_LENGTH 8u
#define SX_MIDI_GENMIDI_MELODIC 128
#define SX_MIDI_GENMIDI_PERCUSSION 47
#define SX_MIDI_GENMIDI_PERCUSSION_FIRST 35
#define SX_MIDI_GENMIDI_RECORD 36u
#define SX_MIDI_GENMIDI_RECORDS \
    (SX_MIDI_GENMIDI_MELODIC + SX_MIDI_GENMIDI_PERCUSSION)
#define SX_MIDI_GENMIDI_BYTES \
    (SX_MIDI_GENMIDI_HEADER_LENGTH + \
     (SX_MIDI_GENMIDI_RECORDS * SX_MIDI_GENMIDI_RECORD))

/* La envolvente se lleva en atenuacion y no en amplitud: una unidad son
 * 0.75 dB y 128 son los 96 dB del chip. La tabla de ganancia se indexa con el
 * nivel fijo del operador mas su atenuacion de envolvente, de ahi el tamano.
 * Dentro de la voz la atenuacion va en Q16 (una unidad = 65536): con un rango
 * de solo 128 unidades un paso entero por sample colapsaba las colas lentas a
 * 128 samples -- una tasa 0 de 5.5 s sonaba a 6 ms. */
#define SX_MIDI_FM_ATT_MAX 128
#define SX_MIDI_FM_ATT_LEVELS 256
#define SX_MIDI_FM_ATT_Q 16u
#define SX_MIDI_FM_ATT_ONE (1 << SX_MIDI_FM_ATT_Q)
#define SX_MIDI_FM_ATT_FULL (SX_MIDI_FM_ATT_MAX << SX_MIDI_FM_ATT_Q)

/* Multiplicador de frecuencia del operador en Q16: 0 es medio, 1 es uno y de
 * ahi en adelante el numero tal cual. */
static const uint32_t g_fm_multiplier[16] = {
    32768, 65536, 131072, 196608, 262144, 327680, 393216, 458752,
    524288, 589824, 655360, 720896, 786432, 851968, 917504, 983040,
};

static uint16_t g_fm_att_gain[SX_MIDI_FM_ATT_LEVELS];
static uint32_t g_fm_rate_ms[16];

/* Cada paso de atenuacion resta 0.75 dB (10^(-0.0375) = 0.91728), la ley del
 * chip. Se arma multiplicando en vez de con una exponencial, y a partir de las
 * 128 unidades el valor ya es cero para el oido. */
static void sx_midi_build_fm_gain(void) {
    uint32_t gain = 32768u;
    int index;

    g_fm_att_gain[0] = 32768u;
    for (index = 1; index < SX_MIDI_FM_ATT_LEVELS; ++index) {
        gain = (gain * 60116u) >> 16;
        g_fm_att_gain[index] = (uint16_t)gain;
    }
}

/* Tiempo de la envolvente por tasa: el chip recorre los 96 dB en ~5.5 s con la
 * tasa 0 y en ~1.4 ms con la 15, multiplicando el tiempo por 1.732 (2^(1/3)) en
 * cada paso. Es esa ley escrita como producto, no una tabla copiada. */
static void sx_midi_build_fm_rates(void) {
    uint32_t milliseconds = 5461u;
    int index;

    for (index = 0; index < 16; ++index) {
        g_fm_rate_ms[index] = milliseconds;
        milliseconds = (milliseconds * 37837u) >> 16;
        if (milliseconds == 0u) {
            milliseconds = 1u;
        }
    }
}

/* Cuanto baja la atenuacion por sample en un tramo de la envolvente, en Q16.
 * `attack` recorre menos dB que una caida, asi que va cuatro veces mas rapido. */
static int32_t sx_midi_fm_att_step(
    uint32_t rate,
    uint8_t nibble,
    int note,
    uint8_t ksr,
    int attack)
{
    int scaled = (int)nibble;
    uint32_t frames;
    uint64_t total;
    int32_t step;

    /* Key scale rate: arriba de C2 la envolvente del chip corre mas rapido, un
     * paso de tasa por octava. Aproximacion del KSR, que suma la nota al indice
     * de tasa en lugar de dividir la octava. */
    if (ksr && note > 36) {
        scaled += (note - 36) / SX_MIDI_OCTAVE_SEMITONES;
    }
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > 15) {
        scaled = 15;
    }
    frames = (uint32_t)(((uint64_t)rate * g_fm_rate_ms[scaled] + 999u) / 1000u);
    if (attack) {
        frames /= 4u;
    }
    if (frames < 1u) {
        frames = 1u;
    }
    total = (uint64_t)SX_MIDI_FM_ATT_MAX << SX_MIDI_FM_ATT_Q;
    step = (int32_t)(total / frames);
    if (step < 1) {
        step = 1;
    }
    return step;
}

/* Key scale level: atenuacion extra en las notas altas, en las mismas unidades
 * de 0.75 dB. Los valores del chip son 3, 1.5 y 6 dB por octava. */
static uint8_t sx_midi_fm_ksl(uint8_t ksl, int note) {
    int units;

    if (ksl == 0u || note <= 24) {
        return 0u;
    }
    switch (ksl & 3u) {
        case 1: /* 3 dB por octava: un cuarto de unidad por semitono */
            units = ((note - 24) * 1) / 3;
            break;
        case 2: /* 1.5 dB por octava */
            units = ((note - 24) * 1) / 6;
            break;
        default: /* 6 dB por octava */
            units = ((note - 24) * 2) / 3;
            break;
    }
    if (units > 60) {
        units = 60;
    }
    return (uint8_t)units;
}

/* Volumen MIDI (0..127) al nivel de OPL (0..127). Como en el chip la escala no
 * es lineal: la caida va como el cuadrado de lo que falta para el tope. */
static int sx_midi_volume_level(int volume) {
    int rest;

    if (volume < 0) {
        volume = 0;
    } else if (volume > 127) {
        volume = 127;
    }
    rest = 127 - volume;
    return 127 - (127 * rest * rest) / (127 * 127);
}

/* Los seis bytes de un operador: tremolo/vibrato/tipo de EG/KSR/multiplicador,
 * ataque y caida, sostenido y liberacion, forma de onda, key scaling y nivel. */
static void sx_midi_fm_op_parse(struct sx_midi_fm_op* op, const unsigned char* data) {
    uint8_t tremolo = data[0];

    op->am = (tremolo & 0x80u) != 0u;
    op->vibrato = (tremolo & 0x40u) != 0u;
    op->sustaining = (tremolo & 0x20u) != 0u;
    op->ksr = (tremolo & 0x10u) != 0u;
    op->multiplier = (uint8_t)(tremolo & 0x0fu);
    op->attack = (uint8_t)(data[1] >> 4);
    op->decay = (uint8_t)(data[1] & 0x0fu);
    op->sustain = (uint8_t)(data[2] >> 4);
    op->release = (uint8_t)(data[2] & 0x0fu);
    op->waveform = (uint8_t)(data[3] & 0x03u);
    /* El byte de nivel del chip lleva el key scale level en los dos bits altos
     * y la atenuacion fija en los seis bajos. */
    op->ksl = (uint8_t)((data[4] >> 6) & 0x03u);
    op->level = (uint8_t)(data[5] & 0x3fu);
}

/* Un registro de 36 bytes: flags, fine tuning, nota fija y dos voces de
 * 16 bytes, cada una con su modulador, el registro de feedback, su portadora
 * y su corrimiento de nota. Devuelve 0 si un campo no puede venir del chip:
 * un banco con basura no se adivina. */
static int sx_midi_fm_patch_parse(
    struct sx_midi_fm_patch* patch,
    const unsigned char* record)
{
    uint16_t flags = (uint16_t)((uint16_t)record[0] | ((uint16_t)record[1] << 8));
    int voice;

    if ((flags & (uint16_t)~0x0007u) != 0u) {
        return 0;
    }
    if (record[3] > 127u) {
        return 0;
    }
    patch->fixed = (flags & 0x0001u) != 0u;
    patch->two_voice = (flags & 0x0004u) != 0u;
    patch->fixed_note = record[3];
    /* La afinacion fina solo desafina la segunda voz de un instrumento doble:
     * el valor 128 es el centro y cada unidad es 1/32 de semitono, la misma
     * escala con la que el pitch bend de +-2 semitonos mueve +-64 pasos. */
    patch->fine_index = (int8_t)((int)record[2] - 128);
    for (voice = 0; voice < 2; ++voice) {
        const unsigned char* data = record + 4 + (size_t)voice * 16u;
        int16_t offset = (int16_t)((uint16_t)data[14] | ((uint16_t)data[15] << 8));

        /* Los bytes que el chip no usa vienen a cero en un GENMIDI real: forma
         * de onda 0..3, KSL con los seis bajos a cero, nivel 0..63, feedback
         * 0..15 y el hueco a cero. Cualquiera que se salga es basura. */
        if (data[3] > 3u || data[10] > 3u) {
            return 0;
        }
        if ((data[4] & 0x3fu) != 0u || (data[11] & 0x3fu) != 0u) {
            return 0;
        }
        if (data[5] > 63u || data[12] > 63u) {
            return 0;
        }
        if (data[6] > 15u || data[13] != 0u) {
            return 0;
        }
        patch->feedback[voice] = (uint8_t)((data[6] >> 1) & 0x07u);
        patch->additive[voice] = (data[6] & 0x01u) != 0u;
        patch->base_note_offset[voice] = offset;
        sx_midi_fm_op_parse(&patch->mod[voice], data);
        sx_midi_fm_op_parse(&patch->car[voice], data + 7);
    }
    return 1;
}

/* Decodifica el banco entero. Devuelve 0 (y el llamador se queda con las
 * familias) si el blob no es un GENMIDI o si algun registro no se puede leer. */
static struct sx_midi_fm_patch* sx_midi_bank_parse(const void* data, size_t bytes) {
    const unsigned char* source = (const unsigned char*)data;
    struct sx_midi_fm_patch* bank;
    int index;

    if (data == 0 || bytes < (size_t)SX_MIDI_GENMIDI_BYTES) {
        return 0;
    }
    if (memcmp(source, SX_MIDI_GENMIDI_HEADER, SX_MIDI_GENMIDI_HEADER_LENGTH) != 0) {
        return 0;
    }
    bank = (struct sx_midi_fm_patch*)calloc(SX_MIDI_GENMIDI_RECORDS, sizeof(*bank));
    if (bank == 0) {
        return 0;
    }
    for (index = 0; index < SX_MIDI_GENMIDI_RECORDS; ++index) {
        const unsigned char* record =
            source + SX_MIDI_GENMIDI_HEADER_LENGTH +
            (size_t)index * SX_MIDI_GENMIDI_RECORD;

        if (!sx_midi_fm_patch_parse(&bank[index], record)) {
            free(bank);
            return 0;
        }
    }
    return bank;
}

/* --- Lectura del SMF ------------------------------------------------------- */

static uint32_t sx_midi_read_u32be(const unsigned char* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint32_t sx_midi_read_u16be(const unsigned char* data) {
    return ((uint32_t)data[0] << 8) | (uint32_t)data[1];
}

static int sx_midi_read_varlen(
    const unsigned char* data,
    size_t size,
    size_t* position,
    uint32_t* value)
{
    uint32_t result = 0;
    int index;

    for (index = 0; index < 4; ++index) {
        unsigned char byte;

        if (*position >= size) {
            return -1;
        }
        byte = data[(*position)++];
        result = (result << 7) | (uint32_t)(byte & 0x7fu);
        if ((byte & 0x80u) == 0) {
            *value = result;
            return 0;
        }
    }
    return -1;
}

static int sx_midi_push_event(
    struct sx_midi_song* song,
    uint64_t tick,
    uint8_t kind,
    uint8_t channel,
    uint8_t a,
    uint8_t b,
    uint32_t tempo_us)
{
    struct sx_midi_event* grown;

    if (song->event_count == song->event_capacity) {
        size_t capacity = song->event_capacity != 0 ? song->event_capacity * 2u : 256u;
        grown = (struct sx_midi_event*)realloc(
            song->events, capacity * sizeof(struct sx_midi_event));
        if (grown == 0) {
            return -1;
        }
        song->events = grown;
        song->event_capacity = capacity;
    }

    song->events[song->event_count].tick = tick;
    song->events[song->event_count].kind = kind;
    song->events[song->event_count].channel = channel;
    song->events[song->event_count].a = a;
    song->events[song->event_count].b = b;
    song->events[song->event_count].tempo_us = tempo_us;
    song->event_count += 1;
    return 0;
}

/* Lee una pista completa y agrega sus eventos al tema. */
static int sx_midi_parse_track(
    struct sx_midi_song* song,
    const unsigned char* data,
    size_t size)
{
    size_t position = 0;
    uint64_t tick = 0;
    unsigned char running = 0;

    while (position < size) {
        uint32_t delta = 0;
        unsigned char status;
        unsigned char kind;

        if (sx_midi_read_varlen(data, size, &position, &delta) < 0) {
            return -1;
        }
        tick += delta;
        if (position >= size) {
            return -1;
        }

        status = data[position];
        if ((status & 0x80u) != 0) {
            position += 1;
            if (status < 0xf0u) {
                running = status;
            }
        } else {
            if (running == 0) {
                return -1;
            }
            status = running;
        }

        if (status == 0xffu) {
            unsigned char meta_type;
            uint32_t meta_length;

            if (position >= size) {
                return -1;
            }
            meta_type = data[position++];
            if (sx_midi_read_varlen(data, size, &position, &meta_length) < 0) {
                return -1;
            }
            if (position + meta_length > size) {
                return -1;
            }
            if (meta_type == 0x2fu) {
                if (tick > song->declared_end_tick) {
                    song->declared_end_tick = (uint32_t)tick;
                }
                return 0; /* fin de pista */
            }
            if (meta_type == 0x51u && meta_length >= 3u) {
                uint32_t tempo = ((uint32_t)data[position] << 16) |
                                 ((uint32_t)data[position + 1] << 8) |
                                 (uint32_t)data[position + 2];
                if (tempo != 0) {
                    if (sx_midi_push_event(song, tick, SX_MIDI_EVENT_TEMPO, 0, 0, 0,
                                           tempo) < 0) {
                        return -1;
                    }
                }
            }
            position += meta_length;
            continue;
        }

        if (status == 0xf0u || status == 0xf7u) {
            uint32_t sysex_length;

            if (sx_midi_read_varlen(data, size, &position, &sysex_length) < 0) {
                return -1;
            }
            if (position + sysex_length > size) {
                return -1;
            }
            position += sysex_length;
            continue;
        }

        kind = (unsigned char)(status & 0xf0u);
        {
            unsigned char channel = (unsigned char)(status & 0x0fu);

            if (kind == 0x80u || kind == 0x90u) {
                unsigned char note;
                unsigned char velocity;

                if (position + 2u > size) {
                    return -1;
                }
                note = data[position];
                velocity = data[position + 1];
                position += 2;
                if (kind == 0x90u && velocity != 0) {
                    if (sx_midi_push_event(song, tick, SX_MIDI_EVENT_NOTE_ON, channel,
                                           note, velocity, 0) < 0) {
                        return -1;
                    }
                } else if (sx_midi_push_event(song, tick, SX_MIDI_EVENT_NOTE_OFF,
                                              channel, note, 0, 0) < 0) {
                    return -1;
                }
            } else if (kind == 0xa0u || kind == 0xb0u || kind == 0xe0u) {
                unsigned char a;
                unsigned char b;

                if (position + 2u > size) {
                    return -1;
                }
                a = data[position];
                b = data[position + 1];
                position += 2;
                if (kind == 0xb0u) {
                    if (sx_midi_push_event(song, tick, SX_MIDI_EVENT_CONTROL, channel,
                                           a, b, 0) < 0) {
                        return -1;
                    }
                } else if (kind == 0xe0u) {
                    if (sx_midi_push_event(song, tick, SX_MIDI_EVENT_PITCH, channel,
                                           a, b, 0) < 0) {
                        return -1;
                    }
                }
                /* 0xa0 (aftertouch) se descarta: no cambia un timbre de dos
                 * osciladores sin vibrato. */
            } else if (kind == 0xc0u || kind == 0xd0u) {
                unsigned char a;

                if (position + 1u > size) {
                    return -1;
                }
                a = data[position++];
                if (kind == 0xc0u) {
                    if (sx_midi_push_event(song, tick, SX_MIDI_EVENT_PROGRAM, channel,
                                           a, 0, 0) < 0) {
                        return -1;
                    }
                }
            } else {
                return -1; /* status desconocido */
            }
        }
    }

    return 0;
}

/* Orden estable por tick: dos eventos del mismo tick conservan su orden. */
static void sx_midi_sort_events(struct sx_midi_song* song) {
    size_t count = song->event_count;
    struct sx_midi_event* buffer;
    struct sx_midi_event* source;
    struct sx_midi_event* target;
    size_t width;
    size_t index;
    struct sx_midi_event* swap;

    if (count < 2) {
        return;
    }
    /* `buffer` es SIEMPRE el buffer auxiliar, nunca el arreglo del tema: los
     * dos punteros se intercambian cada pasada, asi que liberar "el que no se
     * esta leyendo" terminaba liberando song->events en las pasadas impares y
     * el memcpy posterior escribia sobre memoria liberada. El sintoma era un
     * tick corrompido y una duracion absurda. */
    buffer = (struct sx_midi_event*)malloc(count * sizeof(struct sx_midi_event));
    if (buffer == 0) {
        return; /* sin memoria se deja el orden de pistas, que casi siempre ya
                 * esta ordenado porque cada pista es secuencial */
    }

    source = song->events;
    target = buffer;
    for (width = 1; width < count; width *= 2) {
        for (index = 0; index < count; index += width * 2u) {
            size_t left = index;
            size_t left_end = index + width < count ? index + width : count;
            size_t right = left_end;
            size_t right_end = index + width * 2u < count ? index + width * 2u : count;
            size_t out = index;

            while (left < left_end && right < right_end) {
                if (source[right].tick < source[left].tick) {
                    target[out++] = source[right++];
                } else {
                    target[out++] = source[left++];
                }
            }
            while (left < left_end) {
                target[out++] = source[left++];
            }
            while (right < right_end) {
                target[out++] = source[right++];
            }
        }
        swap = source;
        source = target;
        target = swap;
    }

    if (source != song->events) {
        memcpy(song->events, source, count * sizeof(struct sx_midi_event));
    }
    free(buffer);
}

static void sx_midi_compute_timing(struct sx_midi_song* song) {
    uint64_t us = 0;
    uint64_t last_tick = 0;
    uint64_t end_tick = song->declared_end_tick;
    uint32_t tempo = SX_MIDI_DEFAULT_TEMPO_US;
    size_t index;

    for (index = 0; index < song->event_count; ++index) {
        const struct sx_midi_event* event = &song->events[index];

        if (event->tick > last_tick) {
            us += ((event->tick - last_tick) * tempo) / song->division;
            last_tick = event->tick;
        }
        if (event->kind == SX_MIDI_EVENT_TEMPO) {
            tempo = event->tempo_us;
        }
    }
    /* El fin de pista puede caer despues del ultimo evento (un silencio final
     * a proposito); ese tramo tambien dura y el loop tiene que respetarlo. */
    if (end_tick > last_tick) {
        us += ((end_tick - last_tick) * tempo) / song->division;
        last_tick = end_tick;
    }
    song->duration_us = us;
    song->end_tick = (uint32_t)last_tick;
}

/* --- Motor de sintesis ----------------------------------------------------- */

/* Cada sample acumula `division * 1e6` y un tick se consume al cruzar
 * `rate * tempo_us`. Un tema de 120 BPM con 96 ticks por negra da 114.84
 * samples por tick, y a los 11025 samples (500 ms) el contador cae justo. */
static void sx_midi_update_threshold(struct sx_midi_song* song) {
    song->tick_threshold = (uint64_t)song->rate * (uint64_t)song->tempo_us;
    if (song->tick_threshold == 0) {
        song->tick_threshold = 1; /* sin rate todavia: el primer render lo fija */
    }
}

/* 2^(k/12) en Q16 para los doce semitonos de una octava. */
static const uint32_t g_semitone_ratio[12] = {
    65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715,
};

/* Ratio de `semitones` semitonos en Q16. Lo usa el barrido de ataque, que
 * arranca `sweep_semitones` arriba de la nota y baja hasta ella. */
static uint32_t sx_midi_semitone_scale(int semitones) {
    int octave;

    if (semitones <= 0) {
        return 65536u;
    }
    if (semitones > 36) {
        semitones = 36;
    }
    octave = semitones / SX_MIDI_OCTAVE_SEMITONES;
    return g_semitone_ratio[semitones % SX_MIDI_OCTAVE_SEMITONES] << octave;
}

/* Incremento de fase Q32 por sample para una nota MIDI a `rate` Hz, con una
 * parte fraccionaria de semitono en Q16. El banco OPL2 afina en 1/32 de semitono
 * (key scaling, fine tuning), y esa resolucion se interpola entre las dos
 * entradas de la tabla. */
static uint32_t sx_midi_note_increment_ex(
    int note,
    int32_t fine_q16,
    uint32_t rate)
{
    int n = note - SX_MIDI_A4_NOTE;
    int octave = n >= 0 ? n / SX_MIDI_OCTAVE_SEMITONES
                        : (n - (SX_MIDI_OCTAVE_SEMITONES - 1)) / SX_MIDI_OCTAVE_SEMITONES;
    int semitone = n - octave * SX_MIDI_OCTAVE_SEMITONES;
    uint32_t ratio = g_semitone_ratio[semitone];
    uint64_t frequency_q16;
    uint64_t increment;

    if (fine_q16 > 0) {
        uint32_t next = semitone + 1 < SX_MIDI_OCTAVE_SEMITONES
            ? g_semitone_ratio[semitone + 1]
            : (g_semitone_ratio[0] << 1);

        if (fine_q16 > 65535) {
            fine_q16 = 65535;
        }
        ratio += (uint32_t)(((uint64_t)(next - ratio) * (uint32_t)fine_q16) >> 16);
    }
    frequency_q16 = (uint64_t)SX_MIDI_A4_HZ_Q16 * ratio;
    frequency_q16 >>= 16;
    if (octave > 8) {
        octave = 8;
    }
    if (octave < -4) {
        octave = -4;
    }
    if (octave >= 0) {
        frequency_q16 <<= octave;
    } else {
        frequency_q16 >>= -octave;
    }
    increment = (frequency_q16 << 16) / rate;
    if (increment == 0) {
        increment = 1;
    }
    return (uint32_t)increment;
}

static uint32_t sx_midi_note_increment(int note, uint32_t rate) {
    return sx_midi_note_increment_ex(note, 0, rate);
}

/* Pitch bend a escala Q16: +-2 semitonos, aproximacion lineal del ratio (el
 * error en los extremos es menor a medio por ciento). */
static uint32_t sx_midi_bend_scale(uint16_t bend) {
    int32_t delta = (int32_t)bend - 8192;
    return (uint32_t)(65536 + (delta * 9242) / 10000);
}

static int32_t sx_midi_wave(uint32_t phase, uint8_t wave) {
    uint32_t position = phase >> 16;

    switch (wave) {
        case SX_MIDI_WAVE_SINE:
            return g_sine[(phase >> 22) & (SX_MIDI_SINE_SIZE - 1u)];
        case SX_MIDI_WAVE_SAW:
            return (int32_t)position - 32768;
        case SX_MIDI_WAVE_TRIANGLE:
            return position < 32768u
                ? ((int32_t)position * 2 - 32768)
                : (32767 - (int32_t)(position - 32768u) * 2);
        case SX_MIDI_WAVE_SQUARE:
            return position < 32768u ? 32767 : -32768;
        case SX_MIDI_WAVE_PULSE:
            return position < 16384u ? 32767 : -32768;
        default:
            return 0;
    }
}

/* Oscilador: el ruido avanza el LFSR de la voz y devuelve el bit crudo (no
 * depende de la fase: es ancho de banda pleno, y lo que lo domestica es el
 * filtro y la envolvente). Las formas se mezclan hacia el seno con `mix`, que
 * es lo que deja usar una sierra sin que suene a filo. */
static int32_t sx_midi_oscillator(
    struct sx_midi_voice* voice,
    uint8_t wave,
    uint32_t phase,
    uint8_t mix)
{
    int32_t shape;

    if (wave == SX_MIDI_WAVE_NOISE) {
        uint16_t lfsr = voice->noise != 0 ? voice->noise : 1u;
        uint16_t bit = (uint16_t)(((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u);

        lfsr = (uint16_t)((lfsr >> 1) | (uint16_t)(bit << 15));
        voice->noise = lfsr;
        return (int32_t)(int16_t)lfsr;
    }

    shape = sx_midi_wave(phase, wave);
    if (mix != 0u && wave != SX_MIDI_WAVE_SINE) {
        int32_t sine = g_sine[(phase >> 22) & (SX_MIDI_SINE_SIZE - 1u)];
        shape = sine + (int32_t)(((int64_t)(shape - sine) * (int32_t)(255u - mix)) >> 8);
    }
    return shape;
}

/* samples necesarios para recorrer el rango de la envolvente en `ms`. */
static int32_t sx_midi_envelope_step(uint32_t rate, uint16_t milliseconds) {
    uint64_t frames = ((uint64_t)rate * milliseconds) / 1000u;
    int32_t step;

    if (frames < 1u) {
        frames = 1u;
    }
    step = (int32_t)(SX_MIDI_ENV_MAX / (int32_t)frames);
    if (step < 1) {
        step = 1;
    }
    return step;
}

/* Paso de la nota en el camino FM. El chip afina en pasos de 1/32 de semitono:
 * el bend mueve hasta +-64 de esos pasos (dos semitonos) y la afinacion fina
 * del instrumento mueve `fine_index`, pero solo en la segunda sub-voz, que
 * es el desafinado con el que un instrumento de dos voces suena ancho. Cada
 * operador multiplica despues ese paso por el suyo. */
static void sx_midi_fm_voice_set_pitch(
    struct sx_midi_song* song,
    struct sx_midi_voice* voice)
{
    int32_t position = (int32_t)voice->play_note * 65536;
    int32_t fraction;
    int note;

    if (voice->fm_sub != 0u) {
        position += (int32_t)voice->patch->fine_index * 2048;
    }
    if (voice->channel != SX_MIDI_PERCUSSION_CHANNEL) {
        position += ((int32_t)song->channel[voice->channel].bend - 8192) * 16;
    }
    note = position / 65536;
    fraction = position - note * 65536;
    if (fraction < 0) {
        note -= 1;
        fraction += 65536;
    }
    voice->fm_mod_base = sx_midi_note_increment_ex(note, fraction, song->rate);
    voice->fm_mod_step = (uint32_t)(((uint64_t)voice->fm_mod_base * voice->fm_mul[0]) >> 16);
    voice->fm_car_step = (uint32_t)(((uint64_t)voice->fm_mod_base * voice->fm_mul[1]) >> 16);
    if (voice->fm_mod_step == 0u) {
        voice->fm_mod_step = 1u;
    }
    if (voice->fm_car_step == 0u) {
        voice->fm_car_step = 1u;
    }
}

/* Paso base de cada parcial. El barrido y el vibrato NO viven aca: se aplican
 * encima en el render, para que un pitch bend pueda rehacer la base sin
 * pisarlos. */
static void sx_midi_voice_set_pitch(struct sx_midi_song* song, struct sx_midi_voice* voice) {
    const struct sx_midi_instrument* inst = voice->inst;
    uint32_t bend = 65536u;
    uint32_t increment;

    if (voice->patch != 0) {
        sx_midi_fm_voice_set_pitch(song, voice);
        return;
    }
    if (voice->channel != SX_MIDI_PERCUSSION_CHANNEL) {
        bend = sx_midi_bend_scale(song->channel[voice->channel].bend);
    }
    increment = sx_midi_note_increment(voice->play_note, song->rate);
    increment = (uint32_t)(((uint64_t)increment * bend) >> 16);
    voice->base_step = increment != 0u ? increment : 1u;

    if (voice->wave2 != SX_MIDI_NO_WAVE && voice->level2 != 0u) {
        uint32_t second = sx_midi_note_increment(
            voice->play_note + (int)inst->interval2, song->rate);
        second = (uint32_t)(((uint64_t)second * bend) >> 16);
        second = (uint32_t)(((uint64_t)second *
                             (65536u + (uint32_t)inst->detune * 32u)) >> 16);
        voice->base_step2 = second != 0u ? second : 1u;
    } else {
        voice->base_step2 = 0u;
    }
}

/* Devuelve una voz libre y, si no hay ninguna, la que suena mas bajo: un tema
 * denso con instrumentos de dos voces puede pasar de las 32. */
static struct sx_midi_voice* sx_midi_voice_alloc(struct sx_midi_song* song) {
    struct sx_midi_voice* voice = 0;
    int32_t quietest = 0x7fffffff;
    size_t index;

    for (index = 0; index < SX_MIDI_MAX_VOICES; ++index) {
        if (!song->voices[index].active) {
            return &song->voices[index];
        }
    }
    for (index = 0; index < SX_MIDI_MAX_VOICES; ++index) {
        if (song->voices[index].env < quietest) {
            quietest = song->voices[index].env;
            voice = &song->voices[index];
        }
    }
    return voice;
}

/* Arranca una voz FM: un modulador, una portadora y una envolvente por
 * operador. Se llama una vez por voz OPL del instrumento (`sub` 0 o 1); los
 * que piden dos voces traen parametros distintos en cada una y la segunda va
 * ademas desafinada por `fine_index`. */
static void sx_midi_fm_voice_start(
    struct sx_midi_song* song,
    int channel,
    int key,
    int velocity,
    const struct sx_midi_fm_patch* patch,
    int sub)
{
    struct sx_midi_voice* voice = sx_midi_voice_alloc(song);
    uint32_t am_rate = song->rate != 0u ? song->rate : 1u;
    const struct sx_midi_fm_op* mod;
    const struct sx_midi_fm_op* car;
    int voice_index = sub != 0 ? 1 : 0;
    int full;
    int play;
    int index;

    if (voice == 0) {
        return;
    }
    mod = &patch->mod[voice_index];
    car = &patch->car[voice_index];
    /* Doom fija la nota si el instrumento lo pide --los tambores no bailan con
     * el teclado-- y corre el resto por su `base_note_offset` propio. La nota
     * fija no se envuelve: es el pitch que el instrumento pide (un bombo en 90
     * no es el mismo una octava abajo). La melodica con offset si, para que no
     * se salga de la tabla. */
    if (patch->fixed != 0u) {
        play = (int)patch->fixed_note;
    } else {
        play = key + (int)patch->base_note_offset[voice_index];
        while (play < 0) {
            play += SX_MIDI_OCTAVE_SEMITONES;
        }
        while (play > 95) {
            play -= SX_MIDI_OCTAVE_SEMITONES;
        }
    }

    /* La portadora del chip arranca al maximo de atenuacion y el volumen la
     * abre; el modulador conserva su nivel, que es la profundidad de la FM. */
    full = (sx_midi_volume_level(velocity) *
            (2 * (sx_midi_volume_level(song->channel[channel].volume) + 1))) >> 9;
    if (full > 63) {
        full = 63;
    }

    memset(voice, 0, sizeof(*voice));
    voice->active = 1;
    voice->stage = SX_MIDI_STAGE_ATTACK;
    voice->patch = patch;
    voice->fm_sub = (uint8_t)(sub != 0 ? 1 : 0);
    voice->channel = (uint8_t)channel;
    voice->note = (uint8_t)key;
    voice->play_note = (int16_t)play;
    voice->fm_additive = patch->additive[voice_index];
    voice->fm_feedback = patch->feedback[voice_index];
    voice->fm_mul[0] = g_fm_multiplier[mod->multiplier & 15u];
    voice->fm_mul[1] = g_fm_multiplier[car->multiplier & 15u];
    voice->fm_level[0] = (uint8_t)(mod->level + sx_midi_fm_ksl(mod->ksl, play));
    voice->fm_level[1] = (uint8_t)((63 - full) + sx_midi_fm_ksl(car->ksl, play));
    for (index = 0; index < 2; ++index) {
        const struct sx_midi_fm_op* op = index == 0 ? mod : car;

        voice->fm_att[index] = SX_MIDI_FM_ATT_FULL;
        voice->fm_sl[index] = (op->sustain == 15u
            ? SX_MIDI_FM_ATT_MAX
            : (int32_t)op->sustain * 4) << SX_MIDI_FM_ATT_Q;
        voice->fm_sustain[index] = op->sustaining;
        voice->fm_attack_step[index] =
            sx_midi_fm_att_step(song->rate, op->attack, play, op->ksr, 1);
        voice->fm_decay_step[index] =
            sx_midi_fm_att_step(song->rate, op->decay, play, op->ksr, 0);
        voice->fm_release_step[index] =
            sx_midi_fm_att_step(song->rate, op->release, play, op->ksr, 0);
        if (op->am) {
            voice->fm_am = 1;
        }
        if (op->vibrato) {
            voice->fm_vib = 1;
        }
    }
    /* Tremolo y vibrato del chip, compartidos por los dos operadores: 3.7 y
     * 6.1 Hz. */
    voice->fm_am_step = (uint32_t)(((uint64_t)37u << 32) / (am_rate * 10u));
    voice->fm_vib_step = (uint32_t)(((uint64_t)61u << 32) / (am_rate * 10u));
    sx_midi_fm_voice_set_pitch(song, voice);
}

static void sx_midi_voice_start(
    struct sx_midi_song* song,
    int channel,
    int note,
    int velocity)
{
    struct sx_midi_voice* voice;
    const struct sx_midi_instrument* inst;
    int32_t gain;

    /* Con banco del WAD el instrumento viene del GENMIDI y no de las familias.
     * La percusion del chip solo existe para las teclas 35..81. La nota que
     * guarda la voz es la del evento (para que el note-off la encuentre); la
     * que suena la decide el instrumento (fija o con su offset). */
    if (song->fm_bank != 0) {
        const struct sx_midi_fm_patch* patch;

        if (channel == SX_MIDI_PERCUSSION_CHANNEL) {
            if (note < SX_MIDI_GENMIDI_PERCUSSION_FIRST ||
                note >= SX_MIDI_GENMIDI_PERCUSSION_FIRST + SX_MIDI_GENMIDI_PERCUSSION) {
                return;
            }
            patch = &song->fm_bank[SX_MIDI_GENMIDI_MELODIC +
                                   (note - SX_MIDI_GENMIDI_PERCUSSION_FIRST)];
        } else {
            patch = &song->fm_bank[song->channel[channel].program & 127u];
        }
        sx_midi_fm_voice_start(song, channel, note, velocity, patch, 0);
        if (patch->two_voice != 0u) {
            sx_midi_fm_voice_start(song, channel, note, velocity, patch, 1);
        }
        return;
    }

    if (channel == SX_MIDI_PERCUSSION_CHANNEL) {
        inst = &g_drums[g_drum_index[(unsigned)note & 127u]];
    } else {
        inst = &g_families[(unsigned)(song->channel[channel].program / 8u) % 16u];
    }

    voice = sx_midi_voice_alloc(song);
    if (voice == 0) {
        return;
    }

    gain = ((int32_t)velocity * song->channel[channel].volume) / 127;
    gain = (gain * inst->level) / 127;
    gain = gain * 170; /* pico por voz ~ 60% de fondo de escala, deja aire */

    memset(voice, 0, sizeof(*voice));
    voice->active = 1;
    voice->stage = SX_MIDI_STAGE_ATTACK;
    voice->inst = inst;
    voice->channel = (uint8_t)channel;
    voice->note = (uint8_t)note;
    voice->play_note = (int16_t)(inst->base_note >= 0 ? inst->base_note : note);
    voice->wave = inst->wave;
    voice->wave2 = inst->wave2;
    voice->level2 = inst->level2;
    voice->wave_mix = inst->wave_mix;
    voice->noise = (uint16_t)(0x2a3u + (unsigned)note * 265u);
    /* Key scaling: las notas agudas cierran el filtro, como el KSL de un chip
     * FM. Se resuelve una vez por voz porque la nota no cambia mientras suena. */
    {
        int scaled = (int)inst->filter -
                     (((int)voice->play_note - 60) * (int)inst->key_scale) / 8;
        if (scaled < 16) {
            scaled = 16;
        }
        if (scaled > 250) {
            scaled = 250;
        }
        voice->filter = (uint8_t)scaled;
    }
    /* Barrido de ataque: el parcial arranca `sweep_semitones` arriba y cae a la
     * nota en `sweep_ms`. Es lo que hace un bombo de sintetizador. */
    if (inst->sweep_ms != 0u && inst->sweep_semitones != 0u) {
        uint32_t frames = (uint32_t)(((uint64_t)song->rate * inst->sweep_ms) / 1000u);
        voice->sweep_scale = sx_midi_semitone_scale((int)inst->sweep_semitones);
        voice->sweep_step = frames != 0u
            ? (voice->sweep_scale - 65536u) / frames
            : 0u;
        if (voice->sweep_step == 0u) {
            voice->sweep_step = 1u; /* un barrido corto igual tiene que bajar */
        }
    }
    /* Vibrato: LFO senoidal de `lfo_hz` Hz sobre el pitch. */
    if (inst->vibrato != 0u && inst->lfo_hz != 0u) {
        voice->vibrato_depth = (uint32_t)inst->vibrato * 512u;
        voice->lfo_step = (uint32_t)(((uint64_t)inst->lfo_hz << 32) / song->rate);
    }
    voice->env = 0;
    voice->sustain_level = (SX_MIDI_ENV_MAX * (int32_t)inst->sustain) / 127;
    voice->attack_step = sx_midi_envelope_step(song->rate, inst->attack_ms);
    voice->decay_step = sx_midi_envelope_step(song->rate, inst->decay_ms);
    voice->release_step = sx_midi_envelope_step(song->rate, inst->release_ms);
    voice->gain = gain;
    voice->filter_state = 0;
    sx_midi_voice_set_pitch(song, voice);
}

static void sx_midi_voice_release(struct sx_midi_song* song, int channel, int note) {
    size_t index;

    for (index = 0; index < SX_MIDI_MAX_VOICES; ++index) {
        struct sx_midi_voice* voice = &song->voices[index];

        if (voice->active && voice->channel == channel && voice->note == note &&
            voice->stage != SX_MIDI_STAGE_RELEASE) {
            voice->stage = SX_MIDI_STAGE_RELEASE;
        }
    }
}

static void sx_midi_voice_release_channel(struct sx_midi_song* song, int channel) {
    size_t index;

    for (index = 0; index < SX_MIDI_MAX_VOICES; ++index) {
        struct sx_midi_voice* voice = &song->voices[index];

        if (voice->active && voice->channel == channel) {
            voice->stage = SX_MIDI_STAGE_RELEASE;
        }
    }
}

static void sx_midi_apply_event(struct sx_midi_song* song, const struct sx_midi_event* event) {
    int channel = (int)event->channel;

    switch (event->kind) {
        case SX_MIDI_EVENT_NOTE_ON:
            sx_midi_voice_start(song, channel, event->a, event->b);
            break;
        case SX_MIDI_EVENT_NOTE_OFF:
            sx_midi_voice_release(song, channel, event->a);
            break;
        case SX_MIDI_EVENT_PROGRAM:
            if (channel != SX_MIDI_PERCUSSION_CHANNEL) {
                song->channel[channel].program = (uint8_t)(event->a & 127u);
            }
            break;
        case SX_MIDI_EVENT_CONTROL:
            if (event->a == 7u) {
                song->channel[channel].volume = (uint8_t)(event->b & 127u);
            } else if (event->a == 10u) {
                song->channel[channel].pan = (uint8_t)(event->b & 127u);
            } else if (event->a == 120u || event->a == 123u) {
                sx_midi_voice_release_channel(song, channel);
            }
            break;
        case SX_MIDI_EVENT_PITCH: {
            size_t index;

            song->channel[channel].bend = (uint16_t)((((uint32_t)event->b << 7) |
                                                       (uint32_t)event->a) & 0x3fffu);
            if (channel == SX_MIDI_PERCUSSION_CHANNEL) {
                break;
            }
            for (index = 0; index < SX_MIDI_MAX_VOICES; ++index) {
                struct sx_midi_voice* voice = &song->voices[index];

                if (voice->active && voice->channel == channel) {
                    sx_midi_voice_set_pitch(song, voice);
                }
            }
        } break;
        case SX_MIDI_EVENT_TEMPO:
            song->tempo_us = event->tempo_us;
            sx_midi_update_threshold(song);
            break;
        default:
            break;
    }
}

static void sx_midi_apply_events(struct sx_midi_song* song) {
    while (song->next_event < song->event_count &&
           song->events[song->next_event].tick <= song->tick) {
        sx_midi_apply_event(song, &song->events[song->next_event]);
        song->next_event += 1;
    }
}

/* Factor de pitch del sample, Q16: barrido de ataque y vibrato. Cuando el
 * timbre no usa ninguno de los dos devuelve 65536 y el render no paga nada. */
static uint32_t sx_midi_voice_pitch_factor(struct sx_midi_voice* voice) {
    uint32_t factor = 65536u;

    if (voice->sweep_scale > 65536u) {
        factor = voice->sweep_scale - voice->sweep_step;
        if (factor < 65536u) {
            factor = 65536u;
        }
        voice->sweep_scale = factor;
    }
    if (voice->vibrato_depth != 0u) {
        int32_t lfo;
        int32_t offset;

        voice->lfo_phase += voice->lfo_step;
        lfo = g_sine[(voice->lfo_phase >> 22) & (SX_MIDI_SINE_SIZE - 1u)];
        offset = (int32_t)(((int64_t)lfo * (int64_t)voice->vibrato_depth) >> 15);
        factor = (uint32_t)(((uint64_t)factor * (uint64_t)(65536 + offset)) >> 16);
    }
    return factor;
}

/* Un operador OPL2: la tabla del seno del motor y las tres variantes de forma
 * del chip sobre el mismo ciclo. */
static int32_t sx_midi_fm_operator(uint32_t phase, uint8_t waveform) {
    int32_t sine = g_sine[(phase >> 22) & (SX_MIDI_SINE_SIZE - 1u)];

    switch (waveform & 3u) {
        case 1: /* media onda: el semiciclo negativo no suena */
            return sine < 0 ? 0 : sine;
        case 2: /* seno rectificado */
            return sine < 0 ? -sine : sine;
        case 3: /* pulso: solo el primer y el tercer cuarto del ciclo */
            return ((phase >> 30) & 1u) != 0u ? 0 : (sine < 0 ? -sine : sine);
        default:
            return sine;
    }
}

/* Ganancia lineal del operador: su nivel fijo mas la envolvente, en pasos de
 * 0.75 dB. El tremolo del chip suma hasta seis pasos en el pico. */
static uint16_t sx_midi_fm_gain(const struct sx_midi_voice* voice, int index, int32_t am) {
    int32_t level = (int32_t)voice->fm_level[index] +
                    (voice->fm_att[index] >> SX_MIDI_FM_ATT_Q) + am;

    if (level < 0) {
        level = 0;
    } else if (level > SX_MIDI_FM_ATT_LEVELS - 1) {
        level = SX_MIDI_FM_ATT_LEVELS - 1;
    }
    return g_fm_att_gain[level];
}

/* Una etapa de las dos envolventes, en Q16. La voz cambia de etapa cuando llega
 * la portadora, que es la que se oye: el modulador puede seguir su curso sin
 * cerrar la nota. Un operador percusivo no se detiene en su nivel de sostenido,
 * sigue cayendo: ese es el sonido que se apaga solo, y el que deja que un
 * instrumento de dos voces no necesite nota de apagado. */
static void sx_midi_fm_envelope(struct sx_midi_voice* voice) {
    int index;

    switch (voice->stage) {
        case SX_MIDI_STAGE_ATTACK:
            for (index = 0; index < 2; ++index) {
                voice->fm_att[index] -= voice->fm_attack_step[index];
                if (voice->fm_att[index] < 0) {
                    voice->fm_att[index] = 0;
                }
            }
            if (voice->fm_att[1] == 0) {
                voice->stage = SX_MIDI_STAGE_DECAY;
            }
            break;
        case SX_MIDI_STAGE_RELEASE:
            for (index = 0; index < 2; ++index) {
                voice->fm_att[index] += voice->fm_release_step[index];
                if (voice->fm_att[index] > SX_MIDI_FM_ATT_FULL) {
                    voice->fm_att[index] = SX_MIDI_FM_ATT_FULL;
                }
            }
            if (voice->fm_att[1] >= SX_MIDI_FM_ATT_FULL) {
                voice->active = 0;
            }
            break;
        case SX_MIDI_STAGE_DECAY:
        case SX_MIDI_STAGE_SUSTAIN:
        default: {
            int32_t target[2];
            int finished = 1;

            for (index = 0; index < 2; ++index) {
                target[index] = voice->fm_sustain[index] != 0u
                    ? voice->fm_sl[index]
                    : SX_MIDI_FM_ATT_FULL;
                if (voice->fm_att[index] < target[index]) {
                    voice->fm_att[index] += voice->fm_decay_step[index];
                    if (voice->fm_att[index] > target[index]) {
                        voice->fm_att[index] = target[index];
                    }
                }
                if (voice->fm_att[index] < target[index]) {
                    finished = 0;
                }
            }
            if (finished) {
                if (voice->fm_sustain[1] == 0u) {
                    voice->active = 0;
                } else {
                    voice->stage = SX_MIDI_STAGE_SUSTAIN;
                }
            }
            break;
        }
    }
}

/* Devuelve el sample de una voz FM: el modulador mueve la fase de la portadora
 * --o se suma, si el instrumento pide la conexion aditiva-- y cada operador
 * lleva su propia atenuacion. La sub-voz que suena es `fm_sub`: los
 * instrumentos dobles traen dos juegos de operadores distintos. */
static int32_t sx_midi_fm_voice_render(struct sx_midi_voice* voice) {
    int sub = voice->fm_sub != 0u ? 1 : 0;
    uint8_t mod_wave = voice->patch->mod[sub].waveform;
    uint8_t car_wave = voice->patch->car[sub].waveform;
    uint32_t mod_step = voice->fm_mod_step;
    uint32_t car_step = voice->fm_car_step;
    int32_t am = 0;
    int32_t mod_out;
    int32_t car_out;

    sx_midi_fm_envelope(voice);

    /* Vibrato del chip: +-7 centesimos de semitono a 6.1 Hz, sobre los dos
     * operadores. */
    if (voice->fm_vib != 0u) {
        int32_t lfo = g_sine[(voice->fm_vib_phase >> 22) & (SX_MIDI_SINE_SIZE - 1u)];
        uint32_t factor = (uint32_t)(65536 + ((lfo * 265) >> 15));

        mod_step = (uint32_t)(((uint64_t)mod_step * factor) >> 16);
        car_step = (uint32_t)(((uint64_t)car_step * factor) >> 16);
        voice->fm_vib_phase += voice->fm_vib_step;
    }
    /* Tremolo del chip: la envolvente baja hasta 4.5 dB a 3.7 Hz. */
    if (voice->fm_am != 0u) {
        int32_t lfo = g_sine[(voice->fm_am_phase >> 22) & (SX_MIDI_SINE_SIZE - 1u)];

        am = (int32_t)(((int64_t)(lfo + 32768) * 6) >> 16);
        voice->fm_am_phase += voice->fm_am_step;
    }

    /* El modulador se realimenta con sus dos ultimas salidas YA con nivel: es
     * el filo de los metales y el ruido de los platos, y decae con la
     * envolvente en vez de quedarse fijo. Sin feedback no se suma nada. */
    {
        uint32_t phase = voice->fm_mod_phase;

        if (voice->fm_feedback != 0u) {
            int32_t feedback = (voice->fm_fb1 + voice->fm_fb2) >> 1;

            phase += (uint32_t)feedback << (10 + voice->fm_feedback);
        }
        mod_out = sx_midi_fm_operator(phase, mod_wave);
        mod_out = (int32_t)(((int64_t)mod_out *
                             (int32_t)sx_midi_fm_gain(voice, 0, am)) >> 15);
        voice->fm_fb2 = voice->fm_fb1;
        voice->fm_fb1 = mod_out;
        voice->fm_mod_phase += mod_step;
    }

    car_out = sx_midi_fm_operator(voice->fm_car_phase + ((uint32_t)mod_out << 18),
                                  car_wave);
    voice->fm_car_phase += car_step;
    car_out = (int32_t)(((int64_t)car_out * (int32_t)sx_midi_fm_gain(voice, 1, am)) >> 15);
    if (voice->fm_additive != 0u) {
        car_out += mod_out;
    }
    /* La ganancia de la portadora es lo que deja la voz: sirve para elegir a
     * quien robar cuando suenan mas notas que voces hay. */
    voice->env = (int32_t)sx_midi_fm_gain(voice, 1, am);
    return car_out;
}

/* Devuelve el sample de la voz en +-32767 y avanza fase y envolvente. */
static int32_t sx_midi_voice_render(struct sx_midi_voice* voice) {
    int32_t osc;
    int32_t sample;
    uint32_t factor;
    uint32_t step;
    uint32_t step2;

    if (voice->patch != 0) {
        return sx_midi_fm_voice_render(voice);
    }
    factor = sx_midi_voice_pitch_factor(voice);
    step = voice->base_step;
    step2 = voice->base_step2;

    if (factor != 65536u) {
        step = (uint32_t)(((uint64_t)voice->base_step * factor) >> 16);
        step2 = (uint32_t)(((uint64_t)voice->base_step2 * factor) >> 16);
        if (step == 0u) {
            step = 1u;
        }
    }

    osc = sx_midi_oscillator(voice, voice->wave, voice->phase, voice->wave_mix);
    if (voice->base_step2 != 0u) {
        int32_t second = sx_midi_oscillator(
            voice, voice->wave2, voice->phase2, voice->wave_mix);
        osc = (osc * 127 + second * (int32_t)voice->level2) /
              (127 + (int32_t)voice->level2);
    }
    if (voice->filter != 0) {
        voice->filter_state += (((osc - voice->filter_state) * (int32_t)voice->filter) >> 8);
        osc = voice->filter_state;
    }

    voice->phase += step;
    voice->phase2 += step2;

    switch (voice->stage) {
        case SX_MIDI_STAGE_ATTACK:
            voice->env += voice->attack_step;
            if (voice->env >= SX_MIDI_ENV_MAX) {
                voice->env = SX_MIDI_ENV_MAX;
                voice->stage = SX_MIDI_STAGE_DECAY;
            }
            break;
        case SX_MIDI_STAGE_DECAY:
            voice->env -= voice->decay_step;
            if (voice->env <= voice->sustain_level) {
                voice->env = voice->sustain_level;
                voice->stage = SX_MIDI_STAGE_SUSTAIN;
            }
            break;
        case SX_MIDI_STAGE_SUSTAIN:
            if (voice->sustain_level == 0) {
                voice->active = 0;
            }
            break;
        case SX_MIDI_STAGE_RELEASE:
        default:
            voice->env -= voice->release_step;
            if (voice->env <= 0) {
                voice->env = 0;
                voice->active = 0;
            }
            break;
    }

    sample = (int32_t)((((int64_t)osc * voice->env) >> 15) * voice->gain >> 15);
    return sample;
}

static unsigned char sx_midi_render_frame(struct sx_midi_song* song) {
    int32_t mixed = 0;
    size_t index;

    for (index = 0; index < SX_MIDI_MAX_VOICES; ++index) {
        struct sx_midi_voice* voice = &song->voices[index];

        if (!voice->active) {
            continue;
        }
        mixed += sx_midi_voice_render(voice);
    }

    mixed = (mixed * (int32_t)song->volume) / 127;
    if (mixed > 24576) {
        mixed = 24576 + (mixed - 24576) / 8;
    } else if (mixed < -24576) {
        mixed = -24576 + (mixed + 24576) / 8;
    }
    if (mixed > 32767) {
        mixed = 32767;
    }
    if (mixed < -32768) {
        mixed = -32768;
    }
    return (unsigned char)(128 + (mixed >> 8));
}

static void sx_midi_song_rewind(struct sx_midi_song* song, uint32_t rate) {
    int index;

    memset(song->voices, 0, sizeof(song->voices));
    for (index = 0; index < 16; ++index) {
        song->channel[index].program = 0;
        song->channel[index].volume = 100;
        song->channel[index].pan = 64;
        song->channel[index].bend = 8192;
    }
    song->rate = rate;
    song->tempo_us = SX_MIDI_DEFAULT_TEMPO_US;
    song->tick = 0;
    song->tick_accum = 0;
    song->next_event = 0;
    song->finished = 0;
    sx_midi_update_threshold(song);
}

/* --- API ------------------------------------------------------------------- */

struct sx_midi_song* sx_midi_song_create_with_bank(
    const void* data,
    size_t bytes,
    const void* genmidi,
    size_t genmidi_bytes)
{
    const unsigned char* source = (const unsigned char*)data;
    struct sx_midi_song* song;
    uint32_t header_length;
    uint32_t format;
    uint32_t tracks;
    uint32_t division;
    size_t position;
    uint32_t track;

    sx_midi_bank_init();

    if (data == 0 || bytes < 14u) {
        return 0;
    }
    if (memcmp(source, "MThd", 4) != 0) {
        return 0;
    }
    header_length = sx_midi_read_u32be(source + 4);
    if (header_length < 6u || 8u + header_length > bytes) {
        return 0;
    }
    format = sx_midi_read_u16be(source + 8);
    tracks = sx_midi_read_u16be(source + 10);
    division = sx_midi_read_u16be(source + 12);
    if (format > 1u || tracks == 0 || division == 0) {
        return 0;
    }
    if ((division & 0x8000u) != 0) {
        return 0; /* SMPTE: no soportado */
    }

    song = (struct sx_midi_song*)calloc(1, sizeof(*song));
    if (song == 0) {
        return 0;
    }
    song->division = (uint16_t)division;
    song->volume = 100;

    position = 8u + header_length;
    for (track = 0; track < tracks; ++track) {
        uint32_t length;

        if (position + 8u > bytes) {
            sx_midi_song_destroy(song);
            return 0;
        }
        if (memcmp(source + position, "MTrk", 4) != 0) {
            sx_midi_song_destroy(song);
            return 0;
        }
        length = sx_midi_read_u32be(source + position + 4);
        if (position + 8u + length > bytes) {
            sx_midi_song_destroy(song);
            return 0;
        }
        if (sx_midi_parse_track(song, source + position + 8u, length) < 0) {
            sx_midi_song_destroy(song);
            return 0;
        }
        position += 8u + length;
    }

    if (song->event_count == 0) {
        sx_midi_song_destroy(song);
        return 0;
    }

    sx_midi_sort_events(song);
    sx_midi_compute_timing(song);
    sx_midi_song_rewind(song, 0);
    /* Un banco que no se puede leer no rompe el tema: se queda con las familias,
     * que es lo que suena cuando el WAD no trae GENMIDI. */
    song->fm_bank = sx_midi_bank_parse(genmidi, genmidi_bytes);
    return song;
}

struct sx_midi_song* sx_midi_song_create(const void* data, size_t bytes) {
    return sx_midi_song_create_with_bank(data, bytes, 0, 0);
}

int sx_midi_song_uses_genmidi(const struct sx_midi_song* song) {
    return song != 0 && song->fm_bank != 0 ? 1 : 0;
}

void sx_midi_song_destroy(struct sx_midi_song* song) {
    if (song == 0) {
        return;
    }
    free(song->fm_bank);
    free(song->events);
    free(song);
}

uint32_t sx_midi_song_duration_ms(const struct sx_midi_song* song) {
    if (song == 0) {
        return 0;
    }
    return (uint32_t)((song->duration_us + 999u) / 1000u);
}

void sx_midi_song_set_volume(struct sx_midi_song* song, int volume) {
    if (song == 0) {
        return;
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 127) {
        volume = 127;
    }
    song->volume = (uint32_t)volume;
}

size_t sx_midi_song_render(
    struct sx_midi_song* song,
    unsigned char* out,
    size_t frame_count,
    uint32_t sample_rate_hz,
    int loop)
{
    size_t written;

    if (song == 0 || out == 0 || frame_count == 0 || sample_rate_hz == 0) {
        return 0;
    }
    if (song->rate != sample_rate_hz) {
        sx_midi_song_rewind(song, sample_rate_hz);
    }

    for (written = 0; written < frame_count; ++written) {
        if (song->finished) {
            if (!loop) {
                out[written] = 128;
                continue;
            }
            sx_midi_song_rewind(song, sample_rate_hz);
        }
        sx_midi_apply_events(song);
        out[written] = sx_midi_render_frame(song);

        song->tick_accum += (uint64_t)song->division * 1000000u;
        while (song->tick_accum >= song->tick_threshold) {
            song->tick_accum -= song->tick_threshold;
            song->tick += 1;
        }
        if (song->tick >= song->end_tick) {
            song->finished = 1;
        }
    }
    return written;
}

int sx_midi_song_render_all(
    struct sx_midi_song* song,
    uint32_t sample_rate_hz,
    unsigned char** out,
    uint32_t* frames)
{
    uint64_t total;
    unsigned char* buffer;

    if (song == 0 || out == 0 || frames == 0 || sample_rate_hz == 0) {
        return -1;
    }
    total = ((song->duration_us * sample_rate_hz) + 999999u) / 1000000u;
    if (total == 0) {
        total = 1;
    }
    if (total > 0xffffffffull) {
        return -1;
    }
    buffer = (unsigned char*)malloc((size_t)total);
    if (buffer == 0) {
        return -1;
    }
    sx_midi_song_rewind(song, sample_rate_hz);
    (void)sx_midi_song_render(song, buffer, (size_t)total, sample_rate_hz, 0);
    *out = buffer;
    *frames = (uint32_t)total;
    return 0;
}
