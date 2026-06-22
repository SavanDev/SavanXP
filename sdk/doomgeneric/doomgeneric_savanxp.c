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
    .retire_event_fd = -1,
    .shutdown_event_fd = -1,
};
static uint32_t *g_present_buffer = 0;
static uint32_t *g_previous_frame = 0;
static uint32_t *g_scaled_row = 0;
static int g_present_buffer_owned = 0;
static uint32_t g_scale = 1;
static int g_offset_x = 0;
static int g_offset_y = 0;
static uint32_t g_scaled_width = 0;
static uint32_t g_scaled_height = 0;
static int g_previous_frame_valid = 0;

static void sx_fail(const char *message);

static void sx_configure_output_layout(void) {
    if (g_scaled_row != 0) {
        free(g_scaled_row);
        g_scaled_row = 0;
    }

    g_scale = g_gfx.info.width / DOOMGENERIC_RESX;
    if (g_gfx.info.height / DOOMGENERIC_RESY < g_scale) {
        g_scale = g_gfx.info.height / DOOMGENERIC_RESY;
    }
    if (g_scale == 0) {
        sx_fail("doomgeneric: framebuffer too small");
    }

    g_scaled_width = (uint32_t)(DOOMGENERIC_RESX * g_scale);
    g_scaled_height = (uint32_t)(DOOMGENERIC_RESY * g_scale);
    g_offset_x = (int)(g_gfx.info.width - g_scaled_width) / 2;
    g_offset_y = (int)(g_gfx.info.height - g_scaled_height) / 2;

    g_scaled_row = (uint32_t *)malloc((size_t)g_scaled_width * sizeof(uint32_t));
    if (g_scaled_row == 0) {
        sx_fail("doomgeneric: unable to allocate scale cache");
    }

    gfx_clear(g_present_buffer, &g_gfx.info, gfx_rgb(0, 0, 0));
    g_previous_frame_valid = 0;
}

static int sx_uses_client_surface(void) {
    return g_gfx.mapped_view != 0 && g_gfx.pixels != 0 && g_gfx.submit_event_fd >= 0;
}

static void sx_shutdown_video(void) {
    if (g_scaled_row != 0) {
        free(g_scaled_row);
        g_scaled_row = 0;
    }

    if (g_previous_frame != 0) {
        free(g_previous_frame);
        g_previous_frame = 0;
    }

    if (g_present_buffer_owned && g_present_buffer != 0) {
        free(g_present_buffer);
    }
    g_present_buffer = 0;
    g_present_buffer_owned = 0;

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

static void sx_blit_rows(int start_row, int end_row) {
    const uint32_t pitch = gfx_stride_pixels(&g_gfx.info);
    int source_y = 0;

    for (source_y = start_row; source_y <= end_row; ++source_y) {
        const uint32_t *source_row = DG_ScreenBuffer + ((size_t)source_y * DOOMGENERIC_RESX);
        uint32_t *expanded = g_scaled_row;
        uint32_t *destination_row = g_present_buffer + ((size_t)(g_offset_y + (source_y * (int)g_scale)) * pitch) + (size_t)g_offset_x;
        int source_x;
        int repeat_y;

        for (source_x = 0; source_x < DOOMGENERIC_RESX; ++source_x) {
            uint32_t colour = source_row[source_x];
            uint32_t repeat_x;
            for (repeat_x = 0; repeat_x < g_scale; ++repeat_x) {
                *expanded++ = colour;
            }
        }

        memcpy(destination_row, g_scaled_row, (size_t)g_scaled_width * sizeof(uint32_t));
        for (repeat_y = 1; repeat_y < (int)g_scale; ++repeat_y) {
            memcpy(destination_row + ((size_t)repeat_y * pitch), g_scaled_row, (size_t)g_scaled_width * sizeof(uint32_t));
        }
    }
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
 * DG_ScreenBuffer so it rides the existing dirty-row / scale / present pipeline
 * with no changes to the present logic. */

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

    sx_register_shutdown(sx_shutdown_video);

    if (sx_uses_client_surface()) {
        g_present_buffer = g_gfx.pixels;
        g_present_buffer_owned = 0;
    } else {
        g_present_buffer = (uint32_t *)calloc(1, gfx_buffer_bytes(&g_gfx.info));
        g_present_buffer_owned = 1;
        if (g_present_buffer == 0) {
            sx_fail("doomgeneric: unable to allocate present buffer");
        }
    }

    g_previous_frame = (uint32_t *)malloc((size_t)DOOMGENERIC_RESX * (size_t)DOOMGENERIC_RESY * sizeof(uint32_t));
    if (g_previous_frame == 0) {
        sx_fail("doomgeneric: unable to allocate previous frame buffer");
    }

    sx_configure_output_layout();
    sx_fps_init();
}

void DG_DrawFrame(void) {
    int dirty_start = DOOMGENERIC_RESY;
    int dirty_end = -1;
    int source_y = 0;

    sx_fps_stamp();

    if (!g_previous_frame_valid) {
        dirty_start = 0;
        dirty_end = DOOMGENERIC_RESY - 1;
        memcpy(g_previous_frame, DG_ScreenBuffer, (size_t)DOOMGENERIC_RESX * (size_t)DOOMGENERIC_RESY * sizeof(uint32_t));
        g_previous_frame_valid = 1;
    } else {
        for (source_y = 0; source_y < DOOMGENERIC_RESY; ++source_y) {
            const uint32_t *current_row = DG_ScreenBuffer + ((size_t)source_y * DOOMGENERIC_RESX);
            uint32_t *previous_row = g_previous_frame + ((size_t)source_y * DOOMGENERIC_RESX);

            if (memcmp(previous_row, current_row, (size_t)DOOMGENERIC_RESX * sizeof(uint32_t)) == 0) {
                continue;
            }

            memcpy(previous_row, current_row, (size_t)DOOMGENERIC_RESX * sizeof(uint32_t));
            if (source_y < dirty_start) {
                dirty_start = source_y;
            }
            dirty_end = source_y;
        }
    }

    if (dirty_end < dirty_start) {
        return;
    }

    sx_blit_rows(dirty_start, dirty_end);
    {
        unsigned long present_start = uptime_ms();
        long present_result = gfx_present_region(&g_gfx,
                                                 g_present_buffer,
                                                 (uint32_t)g_offset_x,
                                                 (uint32_t)(g_offset_y + (dirty_start * (int)g_scale)),
                                                 g_scaled_width,
                                                 (uint32_t)((dirty_end - dirty_start + 1) * (int)g_scale));
        if (present_result < 0) {
            eprintf("doomgeneric: gfx_present failed (%s)\n", result_error_string(present_result));
            sx_shutdown_exit(1);
        }
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
            (void)gfx_apply_resize_event(&g_gfx, &event);
            sx_configure_output_layout();
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
