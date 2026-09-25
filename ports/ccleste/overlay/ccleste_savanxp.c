/*
 * SavanXP frontend for ccleste (Celeste Classic).
 *
 * Upstream splits the port in two: the engine (celeste.c, celeste.h,
 * tilemap.h) depends on nothing but the C standard library, and the host
 * frontend (sdl12main.c) implements the PICO-8 callback surface on top of SDL.
 * SavanXP has no SDL, so this file replaces the frontend wholesale and the
 * engine is compiled untouched: no patch, and not one line of sdl12main.c is
 * part of the port. The contract is the `Celeste_P8_set_call_func` callback in
 * celeste.h, and the drawing rules below follow sdl12main.c so the game looks
 * the same: index 0 is transparent, `pal()` rewrites the live 16-entry
 * palette, and `map()` blits 8x8 tiles straight out of the sheet.
 *
 * Music is the one deliberate omission. The five tracks ship as OGG Vorbis and
 * SavanXP has no Vorbis decoder; decoding them to PCM on the host would cost
 * roughly 24 MB, which does not fit the persistent image. `CELESTE_P8_MUSIC`
 * is therefore a logged no-op and the game plays silent. See ports/ccleste/README.md.
 */
#include "savanxp/libc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/audio.h"

#include "celeste.h"
#include "ccleste_savanxp_assets.h"
#include "tilemap.h"

#define CCLESTE_PICO8_W 128
#define CCLESTE_PICO8_H 128
#define CCLESTE_TILE 8
#define CCLESTE_TILES_PER_ROW 16
#define CCLESTE_PALETTE_ENTRIES 16

/* PICO-8 button bits, in the order the engine's btn() expects. The names come
 * from the upstream README table, the bit order from mainLoop in sdl12main.c. */
enum {
    CCLESTE_BTN_LEFT = 1u << 0,
    CCLESTE_BTN_RIGHT = 1u << 1,
    CCLESTE_BTN_UP = 1u << 2,
    CCLESTE_BTN_DOWN = 1u << 3,
    CCLESTE_BTN_JUMP = 1u << 4,
    CCLESTE_BTN_DASH = 1u << 5,
};

#define CCLESTE_SFX_VOICES 8u
#define CCLESTE_SFX_VOLUME 127
#define CCLESTE_SFX_CENTRE 127
/* Hold R this many frames to restart the run, matching upstream's held F9. */
#define CCLESTE_RESET_HOLD_FRAMES 30u
/* Celeste Classic is a 30 Hz PICO-8 game; running the loop flat out would make
 * it unplayable and starve the mixer. */
#define CCLESTE_FRAME_MS 33u
/* Frames the headless self-test drives the engine for. */
#define CCLESTE_SELFTEST_FRAMES 120u

/* The sound effect ids the game asks for, from LoadData in sdl12main.c. */
static const int ccleste_sfx_ids[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 14, 15, 16, 23, 35, 37, 38, 40, 50, 51, 54, 55,
};
#define CCLESTE_SFX_SLOTS ((int)(sizeof(ccleste_sfx_ids) / sizeof(ccleste_sfx_ids[0])))

/* The PICO-8 default palette. pal() copies entries out of here into the live
 * palette, so this table is the immutable half of the colour state. */
static const unsigned char ccleste_base_palette[CCLESTE_PALETTE_ENTRIES][3] = {
    {0x00, 0x00, 0x00}, {0x1d, 0x2b, 0x53}, {0x7e, 0x25, 0x53}, {0x00, 0x87, 0x51},
    {0xab, 0x52, 0x36}, {0x5f, 0x57, 0x4f}, {0xc2, 0xc3, 0xc7}, {0xff, 0xf1, 0xe8},
    {0xff, 0x00, 0x4d}, {0xff, 0xa3, 0x00}, {0xff, 0xec, 0x27}, {0x00, 0xe4, 0x36},
    {0x29, 0xad, 0xff}, {0x83, 0x76, 0x9c}, {0xff, 0x77, 0xa8}, {0xff, 0xcc, 0xaa},
};

static uint32_t ccleste_palette[CCLESTE_PALETTE_ENTRIES];
static uint32_t ccleste_frame[CCLESTE_PICO8_W * CCLESTE_PICO8_H];

static struct sx_bmp ccleste_tiles;
static struct sx_bmp ccleste_font;
static struct sx_wav ccleste_sfx[CCLESTE_SFX_SLOTS];
static unsigned ccleste_sfx_index[CCLESTE_SFX_SLOTS];

static struct savanxp_gfx_context ccleste_gfx = {
    .fb_fd = -1,
    .input_fd = -1,
    .submit_event_fd = -1,
    .wake_event_fd = -1,
};
static struct sx_scaled_presenter ccleste_presenter;
static struct sx_audio_mixer ccleste_mixer = { .fd = -1 };

static unsigned char* ccleste_initial_state;
static unsigned ccleste_buttons;
static unsigned ccleste_voice_cursor;
static unsigned ccleste_reset_frames;
static int ccleste_reset_held;
static int ccleste_paused;
static int ccleste_screenshake = 1;
static int ccleste_video_open;
static int ccleste_music_reported;
static int ccleste_display_ready;

static void ccleste_reset_palette(void) {
    int index;

    for (index = 0; index < CCLESTE_PALETTE_ENTRIES; ++index) {
        ccleste_palette[index] = gfx_rgb(ccleste_base_palette[index][0],
                                         ccleste_base_palette[index][1],
                                         ccleste_base_palette[index][2]);
    }
}

static void ccleste_shutdown_video(void) {
    if (ccleste_video_open) {
        sx_scaled_presenter_destroy(&ccleste_presenter);
        gfx_release(&ccleste_gfx);
        gfx_close(&ccleste_gfx);
        ccleste_gfx.fb_fd = -1;
        ccleste_gfx.input_fd = -1;
        ccleste_video_open = 0;
    }
}

static void ccleste_release_assets(void) {
    int index;

    sx_bmp_release(&ccleste_tiles);
    sx_bmp_release(&ccleste_font);
    for (index = 0; index < CCLESTE_SFX_SLOTS; ++index) {
        sx_wav_release(&ccleste_sfx[index]);
    }
}

static void ccleste_fail(const char* message) {
    eprintf("ccleste: %s\n", message);
    ccleste_shutdown_video();
    exit(1);
}

/* --- Assets --------------------------------------------------------------- */

static int ccleste_load_assets(void) {
    char path[256];
    int index;
    int loaded_sfx = 0;

    snprintf(path, sizeof(path), "%s/gfx.bmp", CCLESTE_DATA_DIR);
    if (sx_bmp_load(path, &ccleste_tiles) < 0) {
        eprintf("ccleste: falta la hoja de tiles %s/gfx.bmp\n", CCLESTE_DATA_DIR);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/font.bmp", CCLESTE_DATA_DIR);
    if (sx_bmp_load(path, &ccleste_font) < 0) {
        eprintf("ccleste: falta la fuente %s/font.bmp\n", CCLESTE_DATA_DIR);
        return -1;
    }

    for (index = 0; index < CCLESTE_SFX_SLOTS; ++index) {
        char name[16];

        ccleste_sfx_index[index] = 0u;
        snprintf(name, sizeof(name), "snd%d.wav", ccleste_sfx_ids[index]);
        snprintf(path, sizeof(path), "%s/%s", CCLESTE_DATA_DIR, name);
        if (sx_wav_load(path, &ccleste_sfx[index]) < 0) {
            /* A missing effect is not fatal: the game plays that cue silent,
             * exactly as it does upstream when Mix_LoadWAV fails. */
            ccleste_sfx[index].samples = 0;
            continue;
        }
        ccleste_sfx_index[index] = (unsigned)ccleste_sfx_ids[index];
        loaded_sfx += 1;
    }

    eprintf("ccleste: assets %dx%d tiles, %dx%d font, %d/%d efectos\n",
            ccleste_tiles.width, ccleste_tiles.height,
            ccleste_font.width, ccleste_font.height,
            loaded_sfx, CCLESTE_SFX_SLOTS);
    return 0;
}

/* --- Drawing -------------------------------------------------------------- */

static void ccleste_plot(int x, int y, uint32_t colour) {
    if (x < 0 || y < 0 || x >= CCLESTE_PICO8_W || y >= CCLESTE_PICO8_H) {
        return;
    }
    ccleste_frame[y * CCLESTE_PICO8_W + x] = colour;
}

static void ccleste_fill(int x0, int y0, int x1, int y1, uint32_t colour) {
    int x;
    int y;

    for (y = y0; y <= y1; ++y) {
        for (x = x0; x <= x1; ++x) {
            ccleste_plot(x, y, colour);
        }
    }
}

/* Blits one 8x8 cell of `sheet` at PICO-8 coordinates. With `colour` negative
 * the cell's own indices select the palette entry; otherwise every non-zero
 * pixel is painted in `colour`, which is how p8_print uses the font sheet. */
static void ccleste_blit_cell(const struct sx_bmp* sheet, int cell_x, int cell_y,
                              int x, int y, int colour, int flip_x, int flip_y) {
    int row;

    for (row = 0; row < CCLESTE_TILE; ++row) {
        int source_y = cell_y * CCLESTE_TILE + row;
        int dest_y = y + (flip_y != 0 ? (CCLESTE_TILE - 1 - row) : row);

        if (source_y < 0 || source_y >= sheet->height) {
            continue;
        }
        {
            int column;

            for (column = 0; column < CCLESTE_TILE; ++column) {
                int source_x = cell_x * CCLESTE_TILE + column;
                int dest_x = x + (flip_x != 0 ? (CCLESTE_TILE - 1 - column) : column);
                unsigned char index;

                if (source_x < 0 || source_x >= sheet->width) {
                    continue;
                }
                index = sheet->pixels[source_y * sheet->width + source_x];
                if (index == 0) {
                    continue; /* index 0 is transparent everywhere in this game */
                }
                ccleste_plot(dest_x, dest_y,
                             colour >= 0 ? ccleste_palette[colour & 15]
                                         : ccleste_palette[index & 15]);
            }
        }
    }
}

static int ccleste_clamp(int value) {
    if (value < 0) {
        return 0;
    }
    if (value >= CCLESTE_PICO8_W) {
        return CCLESTE_PICO8_W - 1;
    }
    return value;
}

/* Bresenham, with the endpoints clamped first the way p8_line does upstream. */
static void ccleste_line(int x0, int y0, int x1, int y1, int colour) {
    uint32_t pixel = ccleste_palette[colour & 15];
    int dx;
    int dy;
    int x_step;
    int y_step;
    int error;

    x0 = ccleste_clamp(x0);
    y0 = ccleste_clamp(y0);
    x1 = ccleste_clamp(x1);
    y1 = ccleste_clamp(y1);

    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    dy = y1 > y0 ? y0 - y1 : y1 - y0;
    x_step = x0 < x1 ? 1 : -1;
    y_step = y0 < y1 ? 1 : -1;
    error = dx + dy;

    for (;;) {
        int doubled;

        ccleste_plot(x0, y0, pixel);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        doubled = 2 * error;
        if (doubled >= dy) {
            error += dy;
            x0 += x_step;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += y_step;
        }
    }
}

/* circfill(x, y, r, col) in PICO-8 space. The small radii are cross-shaped
 * rather than round, which is how the game draws its particles. */
static void ccleste_circfill(int cx, int cy, int r, int colour) {
    uint32_t pixel = ccleste_palette[colour & 15];
    int x;
    int y;

    if (r <= 1) {
        ccleste_fill(cx - 1, cy, cx + 1, cy, pixel);
        ccleste_fill(cx, cy - 1, cx, cy + 1, pixel);
        return;
    }
    if (r <= 2) {
        ccleste_fill(cx - 2, cy - 1, cx + 2, cy + 1, pixel);
        ccleste_fill(cx - 1, cy - 2, cx + 1, cy + 2, pixel);
        return;
    }
    if (r <= 3) {
        ccleste_fill(cx - 3, cy - 1, cx + 3, cy + 1, pixel);
        ccleste_fill(cx - 1, cy - 3, cx + 1, cy + 3, pixel);
        ccleste_fill(cx - 2, cy - 2, cx + 2, cy + 2, pixel);
        return;
    }

    /* PICO-8 fills the disc. Upstream only strokes the outline here and notes
     * it believes the game never asks for a radius this large. */
    for (y = -r; y <= r; ++y) {
        for (x = -r; x <= r; ++x) {
            if (x * x + y * y <= r * r) {
                ccleste_plot(cx + x, cy + y, pixel);
            }
        }
    }
}

static void ccleste_print(const char* text, int x, int y, int colour) {
    while (*text != '\0') {
        unsigned char glyph = (unsigned char)*text++;

        glyph &= 0x7fu;
        ccleste_blit_cell(&ccleste_font,
                          glyph % CCLESTE_TILES_PER_ROW,
                          glyph / CCLESTE_TILES_PER_ROW,
                          x, y, colour, 0, 0);
        x += 4;
    }
}

/* --- Audio ---------------------------------------------------------------- */

static void ccleste_play_sfx(int id) {
    int index;
    int chosen = -1;
    unsigned voice;

    for (index = 0; index < CCLESTE_SFX_SLOTS; ++index) {
        if (ccleste_sfx[index].samples != 0 && ccleste_sfx_index[index] == (unsigned)id) {
            chosen = index;
            break;
        }
    }
    if (chosen < 0 || !sx_audio_mixer_active(&ccleste_mixer)) {
        return;
    }

    /* Round-robin over the voices: a new effect steals a slot rather than being
     * dropped, which is the same policy as SDL_mixer's channel -1. */
    voice = ccleste_voice_cursor % CCLESTE_SFX_VOICES;
    ccleste_voice_cursor = (ccleste_voice_cursor + 1u) % CCLESTE_SFX_VOICES;

    if (sx_audio_mixer_start_voice(&ccleste_mixer, voice,
                                   ccleste_sfx[chosen].samples,
                                   ccleste_sfx[chosen].sample_count,
                                   ccleste_sfx[chosen].sample_rate_hz,
                                   SX_AUDIO_PITCH_NORMAL_Q16,
                                   CCLESTE_SFX_VOLUME,
                                   CCLESTE_SFX_CENTRE) < 0) {
        eprintf("ccleste: no se pudo reproducir el efecto %d\n", id);
    }
}

/* --- Tilemap -------------------------------------------------------------- */

static int ccleste_tile_flags(int tile) {
    if (tile < 0 || tile >= (int)(sizeof(tile_flags) / sizeof(tile_flags[0]))) {
        return 0;
    }
    return tile_flags[tile];
}

static int ccleste_tile_flag(int tile, int flag) {
    return (ccleste_tile_flags(tile) & (1u << flag)) != 0;
}

static int ccleste_map_tile(int x, int y) {
    if (x < 0 || y < 0 || x >= 128 || y >= 128) {
        return 0;
    }
    return tilemap_data[x + y * 128];
}

/* --- The PICO-8 callback surface ------------------------------------------ */

static int ccleste_pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...) {
    /* The shake offset lives here, exactly as it does in sdl12main.c: drawing
     * subtracts it, so turning the shake off is a matter of not writing the
     * camera rather than of touching the engine. */
    static int camera_x = 0;
    static int camera_y = 0;
    va_list args;
    int result = 0;

    if (!ccleste_screenshake) {
        camera_x = 0;
        camera_y = 0;
    }

    va_start(args, call);

#define CCLESTE_ARG() va_arg(args, int)
#define CCLESTE_BOOL_ARG() ((Celeste_P8_bool_t)va_arg(args, int))
#define CCLESTE_RETURN(value) do { result = (value); goto done; } while (0)

    switch (call) {
        case CELESTE_P8_MUSIC: {
            /* music(idx, fade, mask) */
            int index = CCLESTE_ARG();

            (void)CCLESTE_ARG(); /* fade */
            (void)CCLESTE_ARG(); /* mask */
            if (!ccleste_music_reported) {
                eprintf("ccleste: sin reproductor de musica; la pista %d se ignora\n", index);
                ccleste_music_reported = 1;
            }
        } break;

        case CELESTE_P8_SPR: {
            /* spr(sprite, x, y, cols, rows, flipx, flipy) */
            int sprite = CCLESTE_ARG();
            int x = CCLESTE_ARG();
            int y = CCLESTE_ARG();
            int cols = CCLESTE_ARG();
            int rows = CCLESTE_ARG();
            int flip_x = CCLESTE_BOOL_ARG();
            int flip_y = CCLESTE_BOOL_ARG();

            (void)cols;
            (void)rows;
            if (sprite >= 0) {
                ccleste_blit_cell(&ccleste_tiles,
                                  sprite % CCLESTE_TILES_PER_ROW,
                                  sprite / CCLESTE_TILES_PER_ROW,
                                  x - camera_x, y - camera_y, -1, flip_x, flip_y);
            }
        } break;

        case CELESTE_P8_BTN: {
            /* btn(b) */
            int b = CCLESTE_ARG();

            CCLESTE_RETURN(b >= 0 && b < 6 ? (int)((ccleste_buttons >> b) & 1u) : 0);
        } break;

        case CELESTE_P8_SFX:
            ccleste_play_sfx(CCLESTE_ARG());
            break;

        case CELESTE_P8_PAL: {
            /* pal(a, b): live palette entry a takes base colour b. */
            int a = CCLESTE_ARG();
            int b = CCLESTE_ARG();

            if (a >= 0 && a < CCLESTE_PALETTE_ENTRIES &&
                b >= 0 && b < CCLESTE_PALETTE_ENTRIES) {
                ccleste_palette[a] = gfx_rgb(ccleste_base_palette[b][0],
                                             ccleste_base_palette[b][1],
                                             ccleste_base_palette[b][2]);
            }
        } break;

        case CELESTE_P8_PAL_RESET:
            ccleste_reset_palette();
            break;

        case CELESTE_P8_CIRCFILL: {
            /* circfill(x, y, r, col) */
            int cx = CCLESTE_ARG() - camera_x;
            int cy = CCLESTE_ARG() - camera_y;
            int r = CCLESTE_ARG();
            int col = CCLESTE_ARG();

            ccleste_circfill(cx, cy, r, col);
        } break;

        case CELESTE_P8_PRINT: {
            /* print(str, x, y, col) */
            const char* text = va_arg(args, const char*);
            int x = CCLESTE_ARG() - camera_x;
            int y = CCLESTE_ARG() - camera_y;
            int col = CCLESTE_ARG() % CCLESTE_PALETTE_ENTRIES;

            ccleste_print(text, x, y, col);
        } break;

        case CELESTE_P8_RECTFILL: {
            /* rectfill(x0, y0, x1, y1, col). Upstream does not reorder the
             * corners, so an inverted rectangle simply draws nothing. */
            int x0 = CCLESTE_ARG() - camera_x;
            int y0 = CCLESTE_ARG() - camera_y;
            int x1 = CCLESTE_ARG() - camera_x;
            int y1 = CCLESTE_ARG() - camera_y;
            int col = CCLESTE_ARG();

            if (x1 >= x0 && y1 >= y0) {
                int left = x0 < 0 ? 0 : x0;
                int top = y0 < 0 ? 0 : y0;
                int right = x1 >= CCLESTE_PICO8_W ? CCLESTE_PICO8_W - 1 : x1;
                int bottom = y1 >= CCLESTE_PICO8_H ? CCLESTE_PICO8_H - 1 : y1;

                if (left <= right && top <= bottom) {
                    ccleste_fill(left, top, right, bottom,
                                 ccleste_palette[col & 15]);
                }
            }
        } break;

        case CELESTE_P8_LINE: {
            /* line(x0, y0, x1, y1, col) */
            int x0 = CCLESTE_ARG() - camera_x;
            int y0 = CCLESTE_ARG() - camera_y;
            int x1 = CCLESTE_ARG() - camera_x;
            int y1 = CCLESTE_ARG() - camera_y;
            int col = CCLESTE_ARG();

            ccleste_line(x0, y0, x1, y1, col);
        } break;

        case CELESTE_P8_MGET:
            /* mget(tx, ty) */
            CCLESTE_RETURN(ccleste_map_tile(CCLESTE_ARG(), CCLESTE_ARG()));
            break;

        case CELESTE_P8_CAMERA:
            /* camera(x, y) */
            if (ccleste_screenshake) {
                camera_x = CCLESTE_ARG();
                camera_y = CCLESTE_ARG();
            }
            break;

        case CELESTE_P8_FGET:
            /* fget(tile, flag) */
            CCLESTE_RETURN(ccleste_tile_flag(CCLESTE_ARG(), CCLESTE_ARG()));
            break;

        case CELESTE_P8_MAP: {
            /* map(mx, my, tx, ty, mw, mh, mask) */
            int mx = CCLESTE_ARG();
            int my = CCLESTE_ARG();
            int tx = CCLESTE_ARG();
            int ty = CCLESTE_ARG();
            int width = CCLESTE_ARG();
            int height = CCLESTE_ARG();
            int mask = CCLESTE_ARG();
            int column;

            for (column = 0; column < width; ++column) {
                int row;

                for (row = 0; row < height; ++row) {
                    int tile = ccleste_map_tile(mx + column, my + row);
                    int draw = mask == 0
                        || (mask == 4 && ccleste_tile_flags(tile) == 4)
                        || ccleste_tile_flag(tile, mask != 4 ? mask - 1 : mask);

                    if (!draw) {
                        continue;
                    }
                    ccleste_blit_cell(&ccleste_tiles,
                                      tile % CCLESTE_TILES_PER_ROW,
                                      tile / CCLESTE_TILES_PER_ROW,
                                      tx + column * CCLESTE_TILE - camera_x,
                                      ty + row * CCLESTE_TILE - camera_y, -1, 0, 0);
                }
            }
        } break;
    }

done:
    va_end(args);
    return result;

#undef CCLESTE_ARG
#undef CCLESTE_BOOL_ARG
#undef CCLESTE_RETURN
}

/* --- Input ---------------------------------------------------------------- */

/* Returns 1 when the key belongs to the port. The engine polls btn() for held
 * state, so movement keys latch here and are cleared on their key-up. */
static int ccleste_handle_key(uint32_t key, int down) {
    unsigned bit = 0;
    int action = 0;
    int owned = 1;

    switch (key) {
        case SAVANXP_KEY_LEFT: bit = CCLESTE_BTN_LEFT; break;
        case SAVANXP_KEY_RIGHT: bit = CCLESTE_BTN_RIGHT; break;
        case SAVANXP_KEY_UP: bit = CCLESTE_BTN_UP; break;
        case SAVANXP_KEY_DOWN: bit = CCLESTE_BTN_DOWN; break;
        case 'Z':
        case 'C':
        case 'N': bit = CCLESTE_BTN_JUMP; break;
        case 'X':
        case 'V':
        case 'M': bit = CCLESTE_BTN_DASH; break;
        case SAVANXP_KEY_ESC: action = down ? 1 : 0; break;
        case 'R':
            /* The reset is a hold, so it is driven from the frame loop; the
             * press only latches and the release clears the counter. */
            ccleste_reset_held = down;
            if (!down) {
                ccleste_reset_frames = 0;
            }
            break;
        case 'E': action = down ? 2 : 0; break;
        case 'S': action = down ? 3 : 0; break;
        case 'D': action = down ? 4 : 0; break;
        default: owned = 0; break;
    }

    if (owned == 0) {
        return 0;
    }
    if (bit != 0) {
        if (down) {
            ccleste_buttons |= bit;
        } else {
            ccleste_buttons &= ~bit;
        }
        return 1;
    }
    if (action == 0) {
        return 1;
    }

    switch (action) {
        case 1:
            ccleste_paused = !ccleste_paused;
            break;
        case 2:
            ccleste_screenshake = !ccleste_screenshake;
            eprintf("ccleste: screenshake %s\n", ccleste_screenshake ? "on" : "off");
            break;
        case 3:
        case 4:
            /* Shift+S / Shift+D are the upstream save and load keys. This port
             * keeps the state in memory only, so both are announced and
             * dropped rather than writing an undocumented file. */
            eprintf("ccleste: %s state (sin soporte de disco en este port)\n",
                    action == 3 ? "save" : "load");
            break;
        default:
            break;
    }
    return 1;
}

static void ccleste_pump_input(void) {
    struct savanxp_input_event event;

    while (gfx_poll_event(&ccleste_gfx, &event) > 0) {
        if (event.type == SAVANXP_INPUT_EVENT_RESIZED) {
            if (gfx_apply_resize_event(&ccleste_gfx, &event) < 0 ||
                sx_scaled_presenter_retarget(&ccleste_presenter, ccleste_palette[0]) < 0) {
                ccleste_fail("no se pudo reajustar la ventana");
            }
            continue;
        }
        (void)ccleste_handle_key(event.key, event.type == SAVANXP_INPUT_EVENT_KEY_DOWN);
    }
}

/* --- Frame ---------------------------------------------------------------- */

static void ccleste_maybe_reset(void) {
    if (!ccleste_reset_held) {
        ccleste_reset_frames = 0;
        return;
    }
    ccleste_reset_frames += 1;
    if (ccleste_reset_frames < CCLESTE_RESET_HOLD_FRAMES) {
        return;
    }

    ccleste_reset_frames = 0;
    ccleste_reset_held = 0;
    ccleste_paused = 0;
    if (ccleste_initial_state != 0) {
        Celeste_P8_load_state(ccleste_initial_state);
        Celeste_P8_set_rndseed((unsigned)uptime_ms());
        Celeste_P8_init();
    }
    eprintf("ccleste: reset\n");
}

static void ccleste_tick(void) {
    ccleste_maybe_reset();

    if (ccleste_paused) {
        int x0 = CCLESTE_PICO8_W / 2 - 3 * 4;
        int y0 = 8;

        ccleste_fill(x0 - 1, y0 - 1, 6 * 4 + x0 + 1, 6 + y0 + 1, ccleste_palette[6]);
        ccleste_fill(x0, y0, 6 * 4 + x0, 6 + y0, ccleste_palette[0]);
        ccleste_print("paused", x0 + 1, y0 + 1, 7);
    } else {
        Celeste_P8_update();
        Celeste_P8_draw();
    }

    {
        long presented = sx_scaled_presenter_present(&ccleste_presenter, ccleste_frame);

        if (presented < 0) {
            eprintf("ccleste: gfx_present fallo (%s)\n", result_error_string(presented));
            ccleste_shutdown_video();
            exit(1);
        }
        /* The smoke flow waits for this token before capturing a frame, so it
         * has to mean "a frame with real content reached the compositor" and
         * not merely "the first frame was handed over". */
        if (presented > 0 && !ccleste_display_ready) {
            ccleste_display_ready = 1;
            printf("CCLESTE DISPLAY READY\n");
        }
    }
    (void)sx_audio_mixer_update(&ccleste_mixer);
}

/* --- Entry point ---------------------------------------------------------- */

static int ccleste_selftest(void) {
    unsigned long frames = 0;
    unsigned long total_samples = 0;
    int loaded_sfx = 0;
    int index;

    for (index = 0; index < CCLESTE_SFX_SLOTS; ++index) {
        if (ccleste_sfx[index].samples != 0) {
            loaded_sfx += 1;
            total_samples += ccleste_sfx[index].sample_count;
        }
    }
    if (ccleste_tiles.pixels == 0 || ccleste_font.pixels == 0) {
        eprintf("ccleste: selftest FAIL (hojas de pixeles vacias)\n");
        return 1;
    }
    if (loaded_sfx == 0) {
        eprintf("ccleste: selftest FAIL (ningun efecto de sonido decodifico)\n");
        return 1;
    }

    /* Drive the engine headless: no window and no audio device, just the
     * update/draw/callback contract over real frames. */
    Celeste_P8_set_call_func(ccleste_pico8emu);
    Celeste_P8_set_rndseed(1u);
    Celeste_P8_init();
    while (frames < CCLESTE_SELFTEST_FRAMES) {
        Celeste_P8_update();
        Celeste_P8_draw();
        frames += 1;
    }

    eprintf("ccleste: selftest %d/%d efectos, %lu muestras, %lu frames\n",
            loaded_sfx, CCLESTE_SFX_SLOTS, total_samples, frames);
    printf("CCLESTE SELFTEST PASS\n");
    return 0;
}

int main(int argc, char** argv) {
    int selftest = 0;
    int index;
    long opened;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--selftest") == 0) {
            selftest = 1;
        } else {
            eprintf("ccleste: opcion desconocida '%s'\n", argv[index]);
            return 2;
        }
    }

    ccleste_reset_palette();

    if (ccleste_load_assets() < 0) {
        eprintf("ccleste: el port necesita los assets del juego en %s\n", CCLESTE_DATA_DIR);
        eprintf("ccleste: se instalan con ports/ccleste/build.sh\n");
        return 1;
    }
    if (selftest) {
        int status = ccleste_selftest();

        ccleste_release_assets();
        return status;
    }

    opened = gfx_open(&ccleste_gfx);
    if (opened < 0) {
        eprintf("ccleste: gfx_open fallo (%s)\n", result_error_string(opened));
        ccleste_release_assets();
        return 1;
    }
    /* From here on the context is open, so every failure below goes through
     * ccleste_fail() and still closes it. */
    ccleste_video_open = 1;
    if (gfx_acquire(&ccleste_gfx) < 0) {
        ccleste_fail("gfx_acquire fallo");
    }
    if (sx_scaled_presenter_init(&ccleste_presenter, &ccleste_gfx,
                                 CCLESTE_PICO8_W, CCLESTE_PICO8_H,
                                 ccleste_frame, ccleste_palette[0]) < 0) {
        ccleste_fail("no se pudo inicializar el presentador escalado");
    }
    if (sx_audio_mixer_init(&ccleste_mixer, CCLESTE_SFX_VOICES,
                            SX_AUDIO_DEFAULT_MAX_DELTA_MS) < 0) {
        eprintf("ccleste: sin dispositivo de audio; el juego corre sin sonido\n");
    }

    Celeste_P8_set_call_func(ccleste_pico8emu);

    /* The reset snapshot has to be taken before the first init, the same order
     * main() uses upstream. */
    ccleste_initial_state = (unsigned char*)malloc(Celeste_P8_get_state_size());
    if (ccleste_initial_state == 0) {
        ccleste_fail("sin memoria para el snapshot de reset");
    }
    Celeste_P8_save_state(ccleste_initial_state);
    Celeste_P8_set_rndseed((unsigned)uptime_ms());
    Celeste_P8_init();

    eprintf("ccleste: listo\n");

    while (!gfx_should_close(&ccleste_gfx)) {
        unsigned long started = uptime_ms();
        unsigned long spent;

        ccleste_pump_input();
        ccleste_tick();

        /* Pace to 30 Hz. Presenting already blocks until the compositor is
         * done, so this only ever waits; it never shortens a slow frame. */
        spent = uptime_ms() - started;
        if (spent < CCLESTE_FRAME_MS) {
            (void)sleep_ms(CCLESTE_FRAME_MS - spent);
        }
    }

    free(ccleste_initial_state);
    ccleste_initial_state = 0;
    sx_audio_mixer_destroy(&ccleste_mixer);
    ccleste_shutdown_video();
    ccleste_release_assets();
    return 0;
}
