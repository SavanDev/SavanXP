#include "savanxp/libc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "savanxp_compat.h"

static struct savanxp_gfx_context g_gfx = {-1, -1, {0, 0, 0, 0, 0}, -1, 0, 0, 0, 0};
static uint32_t *g_present_buffer = 0;
static uint32_t *g_previous_frame = 0;
static uint32_t *g_scaled_row = 0;
static int g_mouse_fd = -1;
static int g_present_buffer_owned = 0;
static uint32_t g_scale = 1;
static int g_offset_x = 0;
static int g_offset_y = 0;
static uint32_t g_scaled_width = 0;
static uint32_t g_scaled_height = 0;
static int g_previous_frame_valid = 0;

static int sx_uses_client_surface(void) {
    return g_gfx.mapped_view != 0 && g_gfx.pixels != 0 && g_gfx.present_fd >= 0;
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

    if (g_mouse_fd >= 0) {
        close(g_mouse_fd);
        g_mouse_fd = -1;
    }

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

void DG_Init(void) {
    sx_prepare_data_dirs();

    if (gfx_open(&g_gfx) < 0) {
        sx_fail("doomgeneric: gfx_open failed");
    }
    if (gfx_acquire(&g_gfx) < 0) {
        sx_fail("doomgeneric: gfx_acquire failed");
    }

    g_mouse_fd = (int)mouse_open();
    if (g_mouse_fd < 0) {
        g_mouse_fd = -1;
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

    g_previous_frame = (uint32_t *)malloc((size_t)DOOMGENERIC_RESX * (size_t)DOOMGENERIC_RESY * sizeof(uint32_t));
    if (g_previous_frame == 0) {
        sx_fail("doomgeneric: unable to allocate previous frame buffer");
    }

    gfx_clear(g_present_buffer, &g_gfx.info, gfx_rgb(0, 0, 0));
}

void DG_DrawFrame(void) {
    int dirty_start = DOOMGENERIC_RESY;
    int dirty_end = -1;
    int source_y = 0;

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
    if (gfx_present_region(&g_gfx,
                           g_present_buffer,
                           (uint32_t)g_offset_x,
                           (uint32_t)(g_offset_y + (dirty_start * (int)g_scale)),
                           g_scaled_width,
                           (uint32_t)((dirty_end - dirty_start + 1) * (int)g_scale)) < 0) {
        sx_fail("doomgeneric: gfx_present failed");
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
    struct savanxp_mouse_event event;
    int accumulated_x = 0;
    int accumulated_y = 0;
    int mapped_buttons = 0;
    int have_event = 0;
    int poll_result;

    if (buttons == 0 || delta_x == 0 || delta_y == 0 || g_mouse_fd < 0) {
        return 0;
    }

    while ((poll_result = mouse_poll_event(g_mouse_fd, &event)) > 0) {
        accumulated_x += event.delta_x;
        accumulated_y += event.delta_y;
        mapped_buttons = 0;

        if ((event.buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
            mapped_buttons |= 1 << 0;
        }
        if ((event.buttons & SAVANXP_MOUSE_BUTTON_RIGHT) != 0) {
            mapped_buttons |= 1 << 1;
        }
        if ((event.buttons & SAVANXP_MOUSE_BUTTON_MIDDLE) != 0) {
            mapped_buttons |= 1 << 2;
        }

        have_event = 1;
    }

    if (!have_event) {
        return 0;
    }

    *buttons = mapped_buttons;
    *delta_x = accumulated_x;
    *delta_y = accumulated_y;
    return 1;
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
