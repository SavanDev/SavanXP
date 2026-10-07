/* Ejercita el camino de musica del port doomgeneric tal como lo usa el
 * programa: un lump MUS del WAD pasa por el mus2mid de upstream y el tema sale
 * de libsxmidi. Es la unica verificacion que no depende de QEMU, y cubre dos
 * trampas ya vistas: la convencion de mus2mid (devuelve distinto de cero al
 * FALLAR) y el orden por tick del parser. */

#include <stdint.h>

/* Las cabeceras del SDK se declaran con enlace C; sin el extern "C" el
 * compilador de C++ las manglea y el link no encuentra printf ni fopen. */
extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

#include <vector>

extern "C" {
#include "savanxp/midi.h"
#include "memio.h"
#include "mus2mid.h"
}

extern "C" void* Z_Malloc(int size, int tag, void* ptr) {
    (void)tag;
    (void)ptr;
    return malloc((size_t)size);
}

extern "C" void Z_Free(void* ptr) {
    free(ptr);
}

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

uint32_t read_u32le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Convierte un blob MUS a MIDI y lo renderiza. Devuelve cuantos frames
 * salieron y que fraccion no es silencio; frames=0 si algo fallo. */
bool render_mus(const unsigned char* mus, size_t mus_len, uint32_t* out_frames,
                double* out_loud) {
    MEMFILE* in = mem_fopen_read((void*)mus, mus_len);
    MEMFILE* out = mem_fopen_write();
    void* midi = 0;
    size_t midi_len = 0;
    struct sx_midi_song* song = 0;
    unsigned char* pcm = 0;
    uint32_t frames = 0;
    uint32_t loud = 0;
    bool ok = false;

    if (in == 0 || out == 0) {
        goto done;
    }
    if (mus2mid(in, out)) {
        goto done;
    }
    mem_get_buf(out, &midi, &midi_len);
    song = sx_midi_song_create(midi, midi_len);
    if (song == 0) {
        goto done;
    }
    if (sx_midi_song_render_all(song, SX_MIDI_DEFAULT_RATE, &pcm, &frames) < 0) {
        goto done;
    }
    if (sx_midi_song_duration_ms(song) == 0) {
        goto done;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        if (pcm[i] > 128 + 2 || pcm[i] + 2 < 128) {
            ++loud;
        }
    }
    *out_frames = frames;
    *out_loud = frames == 0 ? 0.0 : (double)loud / (double)frames;
    ok = true;

done:
    free(pcm);
    if (song != 0) {
        sx_midi_song_destroy(song);
    }
    if (in != 0) {
        mem_fclose(in);
    }
    if (out != 0) {
        mem_fclose(out);
    }
    return ok;
}

/* Igual pero con el banco OPL2 del WAD: el tema tiene que usar GENMIDI y
 * sonar. Es el camino que usa el port (MUS -> MIDI -> FM). */
bool render_mus_with_bank(const unsigned char* mus, size_t mus_len,
                          const unsigned char* bank, size_t bank_len,
                          uint32_t* out_frames, double* out_loud, int* out_uses) {
    MEMFILE* in = mem_fopen_read((void*)mus, mus_len);
    MEMFILE* out = mem_fopen_write();
    void* midi = 0;
    size_t midi_len = 0;
    struct sx_midi_song* song = 0;
    unsigned char* pcm = 0;
    uint32_t frames = 0;
    uint32_t loud = 0;
    bool ok = false;

    if (out_uses != 0) {
        *out_uses = 0;
    }
    if (in == 0 || out == 0) {
        goto done;
    }
    if (mus2mid(in, out)) {
        goto done;
    }
    mem_get_buf(out, &midi, &midi_len);
    song = sx_midi_song_create_with_bank(midi, midi_len, bank, bank_len);
    if (song == 0) {
        goto done;
    }
    if (out_uses != 0) {
        *out_uses = sx_midi_song_uses_genmidi(song);
    }
    if (sx_midi_song_render_all(song, SX_MIDI_DEFAULT_RATE, &pcm, &frames) < 0) {
        goto done;
    }
    if (sx_midi_song_duration_ms(song) == 0) {
        goto done;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        if (pcm[i] > 128 + 2 || pcm[i] + 2 < 128) {
            ++loud;
        }
    }
    *out_frames = frames;
    *out_loud = frames == 0 ? 0.0 : (double)loud / (double)frames;
    ok = true;

done:
    free(pcm);
    if (song != 0) {
        sx_midi_song_destroy(song);
    }
    if (in != 0) {
        mem_fclose(in);
    }
    if (out != 0) {
        mem_fclose(out);
    }
    return ok;
}

/* MUS minimo y valido: un presskey y el fin de partitura. Si no hay WAD a
 * mano, esto sigue probando el puente mus2mid -> libsxmidi. */
std::vector<unsigned char> build_tiny_mus() {
    std::vector<unsigned char> out;
    const unsigned char score[] = {
        0x90,             /* presskey ch0, ultimo evento del tick */
        0xbc,             /* C4 con el bit 7 puesto: hay velocidad */
        0xe4,             /* velocidad 100, ultimo byte del evento */
        0x30,             /* espera 48 ticks */
        0xe0,             /* scoreend */
    };
    const uint16_t scorestart = 14;
    out.push_back('M');
    out.push_back('U');
    out.push_back('S');
    out.push_back(0x1a);
    auto put16 = [&out](uint16_t v) {
        out.push_back((unsigned char)(v & 0xff));
        out.push_back((unsigned char)(v >> 8));
    };
    put16((uint16_t)sizeof(score));
    put16(scorestart);
    put16(1); /* canales primarios */
    put16(0);
    put16(0); /* sin instrumentos */
    out.insert(out.end(), score, score + sizeof(score));
    return out;
}

/* El WAD no vive en el repo (`.gitignore` excluye `*.wad`): cada usuario pone
 * el suyo y puede usar la version que quiera. La prueba no se ata a un nombre
 * de archivo: respeta `SAVANXP_DOOM_WAD` y, si no, prueba los IWAD habituales
 * en el directorio del port. Sin ninguno, se salta el lump real. */
bool file_readable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == 0) {
        return false;
    }
    fclose(f);
    return true;
}

bool pick_wad_path(char* out, size_t out_size) {
    static const char* const candidates[] = {
        "freedoom1.wad", "freedoom2.wad", "doom1.wad",
        "doom.wad", "doom2.wad", "freedoom.wad",
    };
    const char* env = getenv("SAVANXP_DOOM_WAD");

    if (env != 0 && env[0] != '\0' && file_readable(env)) {
        snprintf(out, out_size, "%s", env);
        return true;
    }
    for (const char* name : candidates) {
        snprintf(out, out_size, "%s/%s", SAVANXP_DOOM_WAD_DIR, name);
        if (file_readable(out)) {
            return true;
        }
    }
    return false;
}

/* Busca el primer lump MUS del WAD. Prefiere D_E1M1, el tema del primer
 * nivel, para que la prueba mire la musica que alguien oye al jugar. */
bool find_wad_mus(const char* path, std::vector<unsigned char>* lump_out) {
    FILE* f = fopen(path, "rb");
    if (f == 0) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 12) {
        fclose(f);
        return false;
    }
    std::vector<unsigned char> wad((size_t)size);
    if (fread(wad.data(), 1, wad.size(), f) != wad.size()) {
        fclose(f);
        return false;
    }
    fclose(f);

    if (memcmp(wad.data(), "IWAD", 4) != 0 && memcmp(wad.data(), "PWAD", 4) != 0) {
        return false;
    }
    uint32_t numlumps = read_u32le(wad.data() + 4);
    uint32_t infotable = read_u32le(wad.data() + 8);
    bool found_wanted = false;
    for (uint32_t i = 0; i < numlumps; ++i) {
        size_t entry = (size_t)infotable + (size_t)i * 16;
        if (entry + 16 > wad.size()) {
            break;
        }
        uint32_t pos = read_u32le(wad.data() + entry);
        uint32_t len = read_u32le(wad.data() + entry + 4);
        const char* name = (const char*)(wad.data() + entry + 8);
        if (len < 4 || (size_t)pos + len > wad.size()) {
            continue;
        }
        if (memcmp(wad.data() + pos, "MUS\x1a", 4) != 0) {
            continue;
        }
        bool wanted = strncmp(name, "D_E1M1", 6) == 0;
        if (wanted || lump_out->empty()) {
            lump_out->assign(wad.begin() + pos, wad.begin() + pos + len);
            found_wanted = wanted;
            if (wanted) {
                break;
            }
        }
    }
    return found_wanted || !lump_out->empty();
}

/* Busca el lump GENMIDI del WAD: el banco OPL2 que el port le pasa al tema. */
bool find_wad_genmidi(const char* path, std::vector<unsigned char>* lump_out) {
    FILE* f = fopen(path, "rb");
    if (f == 0) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 12) {
        fclose(f);
        return false;
    }
    std::vector<unsigned char> wad((size_t)size);
    if (fread(wad.data(), 1, wad.size(), f) != wad.size()) {
        fclose(f);
        return false;
    }
    fclose(f);

    if (memcmp(wad.data(), "IWAD", 4) != 0 && memcmp(wad.data(), "PWAD", 4) != 0) {
        return false;
    }
    uint32_t numlumps = read_u32le(wad.data() + 4);
    uint32_t infotable = read_u32le(wad.data() + 8);
    for (uint32_t i = 0; i < numlumps; ++i) {
        size_t entry = (size_t)infotable + (size_t)i * 16;
        if (entry + 16 > wad.size()) {
            break;
        }
        uint32_t pos = read_u32le(wad.data() + entry);
        uint32_t len = read_u32le(wad.data() + entry + 4);
        const char* name = (const char*)(wad.data() + entry + 8);
        if (len < 8 || (size_t)pos + len > wad.size()) {
            continue;
        }
        if (strncmp(name, "GENMIDI", 7) != 0) {
            continue;
        }
        if (memcmp(wad.data() + pos, "#OPL_II#", 8) != 0) {
            continue;
        }
        lump_out->assign(wad.begin() + pos, wad.begin() + pos + len);
        return true;
    }
    return false;
}

} // namespace

int main() {
    printf("DOOM MUS TEST START\n");

    {
        std::vector<unsigned char> mus = build_tiny_mus();
        uint32_t frames = 0;
        double loud = 0.0;
        bool ok = render_mus(mus.data(), mus.size(), &frames, &loud);
        check(ok, "un MUS sintetico pasa por mus2mid hasta libsxmidi");
        check(ok && loud > 0.5, "el tema sintetico no sale en silencio");
    }

    {
        char wad_path[1024];
        std::vector<unsigned char> lump;
        if (pick_wad_path(wad_path, sizeof(wad_path)) &&
            find_wad_mus(wad_path, &lump)) {
            uint32_t frames = 0;
            double loud = 0.0;
            bool ok = render_mus(lump.data(), lump.size(), &frames, &loud);
            check(ok, "un lump MUS real del WAD se renderiza");
            check(ok && loud > 0.5,
                  "la musica real del WAD suena, no queda en silencio");
            std::vector<unsigned char> bank;
            if (find_wad_genmidi(wad_path, &bank)) {
                uint32_t fm_frames = 0;
                double fm_loud = 0.0;
                int uses = 0;
                bool fm_ok = render_mus_with_bank(lump.data(), lump.size(),
                                                 bank.data(), bank.size(),
                                                 &fm_frames, &fm_loud, &uses);
                check(fm_ok && uses == 1,
                      "el MUS real con GENMIDI usa el banco OPL2");
                check(fm_ok && fm_loud > 0.2,
                      "el MUS real con GENMIDI suena");
                if (fm_ok) {
                    printf("  --   FM: %u frames, %.1f%% no silencio\n",
                           fm_frames, fm_loud * 100.0);
                }
            } else {
                printf("  --   sin GENMIDI en %s; se salta el camino FM real\n",
                       wad_path);
            }
        } else {
            printf("  --   sin WAD en %s (usa SAVANXP_DOOM_WAD); se salta "
                   "el lump real\n",
                   SAVANXP_DOOM_WAD_DIR);
        }
    }

    /* El MUS sintetico tambien pasa por el banco: sin GENMIDI cae en familias
     * y suena igual que sin banco. */
    {
        std::vector<unsigned char> mus = build_tiny_mus();
        uint32_t frames = 0;
        double loud = 0.0;
        int uses = -1;
        const unsigned char junk[16] = {'B', 'A', 'D'};
        bool ok = render_mus_with_bank(mus.data(), mus.size(), junk, sizeof(junk),
                                       &frames, &loud, &uses);
        check(ok && uses == 0, "un banco roto cae en familias");
        check(ok && loud > 0.5, "y el fallback del MUS sintetico suena");
    }

    printf(g_failures == 0 ? "DOOM MUS TEST PASS\n" : "DOOM MUS TEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
