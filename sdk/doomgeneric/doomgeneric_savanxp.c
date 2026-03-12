#include "savanxp/libc.h"

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "savanxp_compat.h"

static struct savanxp_gfx_context g_gfx = {-1, -1, {0, 0, 0, 0, 0}};
static uint32_t *g_present_buffer = 0;
static uint32_t g_scale = 1;
static int g_offset_x = 0;
static int g_offset_y = 0;

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

static unsigned char sx_map_keycode(uint32_t key, int ascii) {
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
            if (ascii == ' ') {
                return KEY_USE;
            }
            if (ascii > 0) {
                return sx_unshift_ascii((unsigned char)ascii);
            }
            if (key >= 32 && key <= 126) {
                if (key == ' ') {
                    return KEY_USE;
                }
                return sx_unshift_ascii((unsigned char)key);
            }
            return 0;
    }
}

static void sx_shutdown_graphics(void) {
    if (g_gfx.fb_fd >= 0 || g_gfx.input_fd >= 0) {
        gfx_release(&g_gfx);
        gfx_close(&g_gfx);
    }
}

static void sx_blit_frame(void) {
    const int scaled_width = DOOMGENERIC_RESX * (int)g_scale;
    const int scaled_height = DOOMGENERIC_RESY * (int)g_scale;
    const uint32_t pitch = gfx_stride_pixels(&g_gfx.info);

    gfx_clear(g_present_buffer, &g_gfx.info, gfx_rgb(0, 0, 0));

    for (int y = 0; y < scaled_height; ++y) {
        const int source_y = y / (int)g_scale;
        uint32_t *row = g_present_buffer + (size_t)(g_offset_y + y) * pitch + (size_t)g_offset_x;
        for (int x = 0; x < scaled_width; ++x) {
            const int source_x = x / (int)g_scale;
            row[x] = DG_ScreenBuffer[source_y * DOOMGENERIC_RESX + source_x];
        }
    }
}

void DG_Init(void) {
    sx_make_dirs(FILES_DIR);
    sx_make_dirs(FILES_DIR "/savegames");

    if (gfx_open(&g_gfx) < 0) {
        eprintf("doomgeneric: gfx_open failed\n");
        sx_shutdown_exit(1);
    }
    if (gfx_acquire(&g_gfx) < 0) {
        eprintf("doomgeneric: gfx_acquire failed\n");
        sx_shutdown_exit(1);
    }

    sx_register_shutdown(sx_shutdown_graphics);

    g_present_buffer = (uint32_t *)calloc(1, gfx_buffer_bytes(&g_gfx.info));
    if (g_present_buffer == 0) {
        eprintf("doomgeneric: unable to allocate present buffer\n");
        sx_shutdown_exit(1);
    }

    g_scale = g_gfx.info.width / DOOMGENERIC_RESX;
    if (g_gfx.info.height / DOOMGENERIC_RESY < g_scale) {
        g_scale = g_gfx.info.height / DOOMGENERIC_RESY;
    }
    if (g_scale == 0) {
        eprintf("doomgeneric: framebuffer too small (%ux%u)\n", g_gfx.info.width, g_gfx.info.height);
        sx_shutdown_exit(1);
    }

    g_offset_x = (int)(g_gfx.info.width - (DOOMGENERIC_RESX * g_scale)) / 2;
    g_offset_y = (int)(g_gfx.info.height - (DOOMGENERIC_RESY * g_scale)) / 2;
}

void DG_DrawFrame(void) {
    sx_blit_frame();
    if (gfx_present(&g_gfx, g_present_buffer) < 0) {
        eprintf("doomgeneric: gfx_present failed\n");
        sx_shutdown_exit(1);
    }
}

void DG_SleepMs(uint32_t ms) {
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

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }
}
