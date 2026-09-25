#include "savanxp/libc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "savanxp_compat.h"

static struct savanxp_gfx_context g_gfx = {
    .fb_fd = -1,
    .input_fd = -1,
    .submit_event_fd = -1,
    .wake_event_fd = -1,
};
static struct sx_scaled_presenter g_presenter;

static void sx_fail(const char *message);

static void sx_shutdown_video(void) {
    sx_scaled_presenter_destroy(&g_presenter);

    if (g_gfx.fb_fd >= 0 || g_gfx.input_fd >= 0) {
        gfx_release(&g_gfx);
        gfx_close(&g_gfx);
        g_gfx.fb_fd = -1;
        g_gfx.input_fd = -1;
    }
}

static void sx_fail(const char *message) {
    eprintf("%s\n", message);
    sx_shutdown_exit(1);
}

static void sx_prepare_data_dirs(void) {
    if (sx_make_dirs(FILES_DIR) < 0 || sx_make_dirs(FILES_DIR "/savegames") < 0) {
        sx_fail("doomgeneric: failed to prepare data directories");
    }
}

static unsigned char sx_unshift_ascii(unsigned char key) {
    switch (key) {
        case '!': return '1';
        case '@': return '2';
        case '#': return '3';
        case '$': return '4';
        case '%': return '5';
        case '^': return '6';
        case '&': return '7';
        case '*': return '8';
        case '(': return '9';
        case ')': return '0';
        case '_': return '-';
        case '+': return '=';
        case '{': return '[';
        case '}': return ']';
        case '|': return '\\';
        case ':': return ';';
        case '"': return '\'';
        case '<': return ',';
        case '>': return '.';
        case '?': return '/';
        case '~': return '`';
        default:
            if (key >= 'A' && key <= 'Z') {
                return (unsigned char)(key - 'A' + 'a');
            }
            return key;
    }
}

static unsigned char sx_map_special_key(uint32_t key) {
    switch (key) {
        case SAVANXP_KEY_BACKSPACE: return KEY_BACKSPACE;
        case SAVANXP_KEY_TAB: return KEY_TAB;
        case SAVANXP_KEY_ENTER: return KEY_ENTER;
        case SAVANXP_KEY_ESC: return KEY_ESCAPE;
        case SAVANXP_KEY_UP: return KEY_UPARROW;
        case SAVANXP_KEY_DOWN: return KEY_DOWNARROW;
        case SAVANXP_KEY_LEFT: return KEY_LEFTARROW;
        case SAVANXP_KEY_RIGHT: return KEY_RIGHTARROW;
        case SAVANXP_KEY_SHIFT: return KEY_RSHIFT;
        case SAVANXP_KEY_CTRL: return KEY_FIRE;
        case SAVANXP_KEY_ALT: return KEY_RALT;
        case SAVANXP_KEY_ALT_GR: return KEY_RALT;
        case SAVANXP_KEY_CAPSLOCK: return KEY_CAPSLOCK;
        case SAVANXP_KEY_HOME: return KEY_HOME;
        case SAVANXP_KEY_END: return KEY_END;
        case SAVANXP_KEY_PAGE_UP: return KEY_PGUP;
        case SAVANXP_KEY_PAGE_DOWN: return KEY_PGDN;
        case SAVANXP_KEY_INSERT: return KEY_INS;
        case SAVANXP_KEY_DELETE: return KEY_DEL;
        case SAVANXP_KEY_F1: return KEY_F1;
        case SAVANXP_KEY_F2: return KEY_F2;
        case SAVANXP_KEY_F3: return KEY_F3;
        case SAVANXP_KEY_F4: return KEY_F4;
        case SAVANXP_KEY_F5: return KEY_F5;
        case SAVANXP_KEY_F6: return KEY_F6;
        case SAVANXP_KEY_F7: return KEY_F7;
        case SAVANXP_KEY_F8: return KEY_F8;
        case SAVANXP_KEY_F9: return KEY_F9;
        case SAVANXP_KEY_F10: return KEY_F10;
        case SAVANXP_KEY_F11: return KEY_F11;
        case SAVANXP_KEY_F12: return KEY_F12;
        default:
            return 0;
    }
}

static unsigned char sx_map_printable_key(uint32_t key, int ascii) {
    if (ascii == ' ' || key == ' ') {
        return KEY_USE;
    }

    if (ascii > 0) {
        return sx_unshift_ascii((unsigned char)ascii);
    }

    if (key >= 32 && key <= 126) {
        return sx_unshift_ascii((unsigned char)key);
    }

    return 0;
}

static unsigned char sx_map_keycode(uint32_t key, int ascii) {
    const unsigned char special = sx_map_special_key(key);
    return special != 0 ? special : sx_map_printable_key(key, ascii);
}

/* --- FPS / present-latency debug overlay --------------------------------- */
/* Doom runs as a desktop-compositor client: every gfx_present_region() blocks
 * in gfx_wait_for_client_idle() until the previous frame has been composed.
 * We cannot read the VirtIO GPU driver stats from here (the compositor owns
 * /dev/gpu0), so the overlay reports the client-visible signal instead: the
 * presented frame rate plus how long Doom stays blocked inside the present call
 * (average and peak ms over the sampling window).  That stall is precisely what
 * starves the audio mixer and caps the frame rate, so it is the number to
 * watch while tuning the GPU path. The text is stamped straight into
 * DG_ScreenBuffer so it rides the SDK presenter's dirty-row / scale pipeline
 * with no extra Doom-side compositing. */

#define SX_FPS_WINDOW_MS 500u

static struct savanxp_fb_info g_fps_info;
static int g_fps_enabled = 1;
static unsigned long g_fps_window_start = 0;
static unsigned int g_fps_frames = 0;
static unsigned long g_fps_block_accum_ms = 0;
static unsigned long g_fps_block_peak_ms = 0;
static char g_fps_text[64] = "FPS --";

static char *sx_append_str(char *out, char *end, const char *text) {
    while (*text != '\0' && out < end - 1) {
        *out++ = *text++;
    }
    *out = '\0';
    return out;
}

static char *sx_append_uint(char *out, char *end, unsigned long value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    } while (value != 0 && count < (int)sizeof(digits));
    while (count > 0 && out < end - 1) {
        *out++ = digits[--count];
    }
    *out = '\0';
    return out;
}

static void sx_fps_init(void) {
    memset(&g_fps_info, 0, sizeof(g_fps_info));
    g_fps_info.width = DOOMGENERIC_RESX;
    g_fps_info.height = DOOMGENERIC_RESY;
    g_fps_info.pitch = DOOMGENERIC_RESX * (uint32_t)sizeof(uint32_t);
    g_fps_info.bpp = 32;
    g_fps_info.buffer_size = (size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(uint32_t);
    g_fps_window_start = uptime_ms();
    g_fps_frames = 0;
    g_fps_block_accum_ms = 0;
    g_fps_block_peak_ms = 0;
}

/* Rebuild the overlay string and emit a log line once per sampling window.
 * block_ms is the wall time Doom spent blocked inside the present call. */
static void sx_fps_sample(unsigned long block_ms) {
    unsigned long now;
    unsigned long elapsed;

    if (!g_fps_enabled) {
        return;
    }

    g_fps_frames += 1;
    g_fps_block_accum_ms += block_ms;
    if (block_ms > g_fps_block_peak_ms) {
        g_fps_block_peak_ms = block_ms;
    }

    now = uptime_ms();
    elapsed = now - g_fps_window_start;
    if (elapsed < SX_FPS_WINDOW_MS || g_fps_frames == 0) {
        return;
    }

    {
        unsigned int fps = (unsigned int)(((unsigned long)g_fps_frames * 1000u) / elapsed);
        unsigned long avg_tenths = (g_fps_block_accum_ms * 10u) / g_fps_frames;
        char *p = g_fps_text;
        char *end = g_fps_text + sizeof(g_fps_text);

        p = sx_append_str(p, end, "FPS ");
        p = sx_append_uint(p, end, fps);
        p = sx_append_str(p, end, "  blk ");
        p = sx_append_uint(p, end, avg_tenths / 10u);
        p = sx_append_str(p, end, ".");
        p = sx_append_uint(p, end, avg_tenths % 10u);
        p = sx_append_str(p, end, "/");
        p = sx_append_uint(p, end, g_fps_block_peak_ms);
        p = sx_append_str(p, end, "ms");

        eprintf("doomgeneric: %s (%u presents / %u ms)\n",
                g_fps_text, g_fps_frames, (unsigned int)elapsed);
    }

    g_fps_frames = 0;
    g_fps_block_accum_ms = 0;
    g_fps_block_peak_ms = 0;
    g_fps_window_start = now;
}

/* Stamp the cached overlay text into DG_ScreenBuffer (doom 320x200 space) so it
 * is scaled and presented by the normal frame path. */
static void sx_fps_stamp(void) {
    int cell_h;
    int box_w;

    if (!g_fps_enabled || DG_ScreenBuffer == 0 || g_fps_info.width == 0) {
        return;
    }

    cell_h = gfx_cell_height();
    box_w = gfx_text_width_mono(g_fps_text) + 4;
    gfx_rect(DG_ScreenBuffer, &g_fps_info, 1, 1, box_w, cell_h + 2, gfx_rgb(0, 0, 0));
    gfx_blit_text_mono(DG_ScreenBuffer, &g_fps_info, 3, 2, g_fps_text, gfx_rgb(255, 240, 64));
}

void DG_Init(void) {
    sx_prepare_data_dirs();

    {
        long open_result = gfx_open(&g_gfx);
        if (open_result < 0) {
            eprintf("doomgeneric: gfx_open failed (%s)\n", result_error_string(open_result));
            sx_shutdown_exit(1);
        }
    }
    if (gfx_acquire(&g_gfx) < 0) {
        sx_fail("doomgeneric: gfx_acquire failed");
    }

    if (sx_scaled_presenter_init(&g_presenter,
                                 &g_gfx,
                                 DOOMGENERIC_RESX,
                                 DOOMGENERIC_RESY,
                                 DG_ScreenBuffer,
                                 gfx_rgb(0, 0, 0)) < 0) {
        sx_fail("doomgeneric: scaled presenter initialization failed");
    }
    sx_register_shutdown(sx_shutdown_video);
    sx_fps_init();
}

void DG_DrawFrame(void) {
    unsigned long present_start;
    long present_result;

    sx_fps_stamp();
    present_start = uptime_ms();
    present_result = sx_scaled_presenter_present(&g_presenter, DG_ScreenBuffer);
    if (present_result < 0) {
        eprintf("doomgeneric: gfx_present failed (%s)\n", result_error_string(present_result));
        sx_shutdown_exit(1);
    }
    if (present_result > 0) {
        sx_fps_sample(uptime_ms() - present_start);
    }
}

void DG_SleepMs(uint32_t ms) {
    if (ms <= 1) {
        yield();
        return;
    }

    sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)uptime_ms();
}

int DG_GetKey(int *pressed, unsigned char *doom_key) {
    struct savanxp_input_event event;

    if (pressed == 0 || doom_key == 0) {
        return 0;
    }

    while (gfx_poll_event(&g_gfx, &event) > 0) {
        if (event.type == SAVANXP_INPUT_EVENT_RESIZED) {
            if (gfx_apply_resize_event(&g_gfx, &event) < 0 ||
                sx_scaled_presenter_retarget(&g_presenter, gfx_rgb(0, 0, 0)) < 0) {
                sx_fail("doomgeneric: scaled presenter resize failed");
            }
            continue;
        }
        unsigned char mapped = sx_map_keycode(event.key, event.ascii);
        if (mapped == 0) {
            continue;
        }

        *pressed = event.type == SAVANXP_INPUT_EVENT_KEY_DOWN;
        *doom_key = mapped;
        return 1;
    }

    return 0;
}

int DG_GetMouse(int *buttons, int *delta_x, int *delta_y) {
    (void)buttons;
    (void)delta_x;
    (void)delta_y;
    return 0;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }
}
