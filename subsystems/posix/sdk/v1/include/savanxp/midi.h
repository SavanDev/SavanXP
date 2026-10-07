#pragma once

#include <stddef.h>
#include <stdint.h>

/* Sintetizador MIDI del sistema, en libsxmidi.so.0.4.
 *
 * El runtime del userland v1 no tiene sintesis: `audio.h` reproduce voces
 * unsigned-8-bit mono ya decodificadas y el demonio solo mezcla PCM. Esta
 * libreria cubre el hueco que hay antes: parsea un Standard MIDI File (tipo 0
 * o 1) en memoria, lo ejecuta contra un banco de instrumentos General MIDI
 * horneado en el propio codigo (sin soundfont que instalar) y escribe PCM.
 *
 * El destino es exactamente el formato que el mixer espera de una voz: mono
 * unsigned-8-bit al rate que pida el llamador. Un tema se renderiza entero, se
 * arranca como una voz con loop y el mixer se ocupa del resto; `DoomGeneric`
 * lo usa asi. `sx_midi_song_render` tambien sirve por bloques para un reproductor
 * que quiera streaming.
 *
 * La libreria no depende de libmath ni de libc mas alla de malloc/free/memcpy/
 * memset, que resuelve contra el ejecutable como el resto de las librerias del
 * sistema. Es enteramente punto fijo: sin `double` y sin `sin()`.
 *
 * Sin pitch bend de precision, sin pan estereo y sin efectos (chorus/reverb).
 * Sin banco, el sintetizador usa una aproximacion por familias de programa, no
 * un GM completo; con el `GENMIDI` de un WAD de Doom suena el banco OPL2 de dos
 * operadores que el juego le programaba al chip. Ver docs/MIDI.md.
 */

/* Rate por defecto para renderizar un tema entero. 22050 Hz mono u8 mantiene
 * un tema de dos minutos en ~2.6 MB y deja que el mixer convierta al rate del
 * dispositivo, igual que ya hace con los efectos. */
#define SX_MIDI_DEFAULT_RATE 22050u

/* Tope de voces simultaneas. Doom MUS tiene 16 canales y en la practica usa
 * una nota por canal a la vez; 32 deja aire para MIDI polifonico. */
#define SX_MIDI_MAX_VOICES 32u

struct sx_midi_song;

#ifdef __cplusplus
extern "C" {
#endif

/* Parsea un SMF (tipo 0 o 1) en memoria. Devuelve 0 si los datos no son un
 * MIDI valido o si no hay memoria. `data` solo se lee durante la llamada: el
 * tema guarda sus propios eventos. */
struct sx_midi_song* sx_midi_song_create(const void* data, size_t bytes);

/* Igual, pero el tema suena con el banco OPL2 del WAD: `genmidi` es el lump
 * `GENMIDI` tal cual --el formato `#OPL_II#` con sus 175 instrumentos de dos
 * operadores-- y `genmidi_bytes` su tamano. Sin banco, con uno ilegible o con
 * menos bytes de los que el formato necesita, el tema cae en las familias y
 * suena exactamente como `sx_midi_song_create`.
 *
 * El banco es del TEMA y no del proceso: no hay estado global, y dos temas del
 * mismo programa pueden sonar con bancos distintos. `genmidi` solo se lee
 * durante la llamada, como `data`. */
struct sx_midi_song* sx_midi_song_create_with_bank(
    const void* data,
    size_t bytes,
    const void* genmidi,
    size_t genmidi_bytes);

/* 1 si el tema suena con el banco OPL2 que se le paso y 0 si cayo en las
 * familias. Es para que el llamador pueda contarlo, no para elegir: la caida ya
 * esta resuelta dentro de `sx_midi_song_create_with_bank`. */
int sx_midi_song_uses_genmidi(const struct sx_midi_song* song);

void sx_midi_song_destroy(struct sx_midi_song* song);

/* Duracion total del tema en milisegundos, con el mapa de tempo aplicado y
 * redondeada hacia arriba al siguiente sample. 0 si el tema esta vacio. */
uint32_t sx_midi_song_duration_ms(const struct sx_midi_song* song);

/* Volumen de reproduccion 0..127, aplicado al mezclar. Fuera de rango se
 * recorta. No afecta a lo ya escrito. */
void sx_midi_song_set_volume(struct sx_midi_song* song, int volume);

/* Escribe hasta `frame_count` frames mono u8 a `sample_rate_hz` en `out` y
 * avanza el tema. Devuelve los frames escritos:
 *  - sin loop, se detiene al terminar el tema y puede devolver menos que
 *    `frame_count` (0 cuando ya no queda nada);
 *  - con loop, al llegar al final vuelve al principio y siempre llena.
 * El sample de silencio es 128, como el resto del mixer. */
size_t sx_midi_song_render(
    struct sx_midi_song* song,
    unsigned char* out,
    size_t frame_count,
    uint32_t sample_rate_hz,
    int loop);

/* Atajo para el caso de un tema entero: reserva el buffer con malloc y
 * devuelve la duracion renderizada a `sample_rate_hz`. `out` y `frames` son de
 * salida; la propiedad del buffer es del llamador, que lo libera con free.
 * Devuelve 0 o -1 sin memoria. */
int sx_midi_song_render_all(
    struct sx_midi_song* song,
    uint32_t sample_rate_hz,
    unsigned char** out,
    uint32_t* frames);

#ifdef __cplusplus
}
#endif
