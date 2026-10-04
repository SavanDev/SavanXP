#include "libc.h"

#define GFX_MAX_WIDTH 1920
#define GFX_MAX_HEIGHT 1080

static uint32_t g_backbuffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];

static int append_char(char* buffer, int offset, int capacity, char value) {
    if (offset + 1 >= capacity) {
        return offset;
    }
    buffer[offset] = value;
    buffer[offset + 1] = '\0';
    return offset + 1;
}

static int append_text(char* buffer, int offset, int capacity, const char* text) {
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    buffer[offset] = '\0';
    return offset;
}

static int append_uint(char* buffer, int offset, int capacity, unsigned int value) {
    char digits[16];
    int digit_count = 0;

    if (value == 0) {
        return append_char(buffer, offset, capacity, '0');
    }

    while (value != 0 && digit_count < (int)sizeof(digits)) {
        digits[digit_count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (digit_count > 0) {
        offset = append_char(buffer, offset, capacity, digits[--digit_count]);
    }
    return offset;
}

static int append_int(char* buffer, int offset, int capacity, int value) {
    if (value < 0) {
        offset = append_char(buffer, offset, capacity, '-');
        value = -value;
    }
    return append_uint(buffer, offset, capacity, (unsigned int)value);
}

static void format_position(char* buffer, int x, int y) {
    int offset = 0;
    memset(buffer, 0, 64);
    offset = append_text(buffer, offset, 64, "Cursor: ");
    offset = append_int(buffer, offset, 64, x);
    offset = append_text(buffer, offset, 64, ", ");
    (void)append_int(buffer, offset, 64, y);
}

static void format_delta(char* buffer, int dx, int dy) {
    int offset = 0;
    memset(buffer, 0, 64);
    offset = append_text(buffer, offset, 64, "Last delta: ");
    offset = append_int(buffer, offset, 64, dx);
    offset = append_text(buffer, offset, 64, ", ");
    (void)append_int(buffer, offset, 64, dy);
}

/* El acumulado importa tanto como el ultimo tick: un tick suelto se ve igual
 * venga de donde venga, pero el total delata si el camino esta perdiendo
 * eventos (la coalescencia del WM o la cola del kernel) al no coincidir con lo
 * que la rueda giro de verdad. */
static void format_wheel(char* buffer, int wheel_last, int wheel_total) {
    int offset = 0;
    memset(buffer, 0, 64);
    offset = append_text(buffer, offset, 64, "Wheel: ");
    offset = append_int(buffer, offset, 64, wheel_last);
    offset = append_text(buffer, offset, 64, " (total ");
    offset = append_int(buffer, offset, 64, wheel_total);
    (void)append_text(buffer, offset, 64, ")");
}

static void format_buttons(char* buffer, uint32_t buttons) {
    int offset = 0;
    memset(buffer, 0, 96);
    offset = append_text(buffer, offset, 96, "Buttons: ");
    if (buttons == 0) {
        append_text(buffer, offset, 96, "none");
        return;
    }
    if ((buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
        offset = append_text(buffer, offset, 96, "left ");
    }
    if ((buttons & SAVANXP_MOUSE_BUTTON_RIGHT) != 0) {
        offset = append_text(buffer, offset, 96, "right ");
    }
    if ((buttons & SAVANXP_MOUSE_BUTTON_MIDDLE) != 0) {
        (void)append_text(buffer, offset, 96, "middle");
    }
}

static void draw_crosshair(struct savanxp_gfx_context* gfx, int x, int y) {
    gfx_vline(g_backbuffer, &gfx->info, x, y - 10, 21, gfx_rgb(255, 255, 255));
    gfx_hline(g_backbuffer, &gfx->info, x - 10, y, 21, gfx_rgb(255, 255, 255));
    gfx_rect(g_backbuffer, &gfx->info, x - 2, y - 2, 5, 5, gfx_rgb(14, 88, 161));
}

/* El panel de lecturas fija el ancho; abajo queda una zona libre para mover el
 * puntero, que es de lo que se trata la prueba. Alto del panel = las cinco
 * lineas completas; estaba en 98 y le cortaba la ultima por dos pixeles. */
#define MOUSETEST_PANEL_TOP 54
#define MOUSETEST_PANEL_HEIGHT 126
#define MOUSETEST_PLAYGROUND_HEIGHT 200

static void request_content_size(struct savanxp_gfx_context* gfx) {
    int text_width = gfx_text_width("Move the mouse, roll the wheel, click buttons, ESC exits");
    uint32_t width = (uint32_t)(40 + text_width + 40);
    uint32_t height = (uint32_t)(MOUSETEST_PANEL_TOP + MOUSETEST_PANEL_HEIGHT + MOUSETEST_PLAYGROUND_HEIGHT);

    if (width < 420u) {
        width = 420u;
    }
    if (gfx_request_content_size(gfx, width, height) == 0) {
        (void)gfx_wait_content_size(gfx, 250UL);
    }
}

static void draw_scene(struct savanxp_gfx_context* gfx, int cursor_x, int cursor_y, int delta_x, int delta_y,
    int wheel_last, int wheel_total, uint32_t buttons) {
    char line0[64];
    char line1[64];
    char line2[64];
    char line3[96];

    format_position(line0, cursor_x, cursor_y);
    format_delta(line1, delta_x, delta_y);
    format_wheel(line2, wheel_last, wheel_total);
    format_buttons(line3, buttons);

    gfx_clear(g_backbuffer, &gfx->info, gfx_rgb(18, 59, 102));
    gfx_rect(g_backbuffer, &gfx->info, 0, 0, (int)gfx->info.width, 34, gfx_rgb(6, 40, 78));
    gfx_hline(g_backbuffer, &gfx->info, 0, 34, (int)gfx->info.width, gfx_rgb(145, 201, 236));
    gfx_blit_text(g_backbuffer, &gfx->info, 36, 10, "SavanXP mouse test", gfx_rgb(255, 255, 255));

    gfx_rect(g_backbuffer, &gfx->info, 24, MOUSETEST_PANEL_TOP, (int)gfx->info.width - 48, MOUSETEST_PANEL_HEIGHT, gfx_rgb(225, 232, 238));
    gfx_frame(g_backbuffer, &gfx->info, 24, MOUSETEST_PANEL_TOP, (int)gfx->info.width - 48, MOUSETEST_PANEL_HEIGHT, gfx_rgb(60, 90, 120));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 70, "Move the mouse, roll the wheel, click buttons, ESC exits", gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 92, line0, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 114, line1, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 136, line2, gfx_rgb(0, 0, 0));
    gfx_blit_text(g_backbuffer, &gfx->info, 40, 158, line3, gfx_rgb(0, 0, 0));

    draw_crosshair(gfx, cursor_x, cursor_y);
}

/* Smoke del camino del puntero real: a diferencia de windowd --cursor-repro,
 * que inyecta savanxp_mouse_event a mano y por lo tanto no toca la cola de
 * virtio-input, este binario lee /dev/mouse0 de verdad mientras el harness host
 * (`./build.sh smoke pointer-smoke --virtio`) mueve el raton emulado por QMP.
 *
 * Es lo unico que pasa por la cola del device, y esa cola hay que reponer para
 * avisarle al device que hay hueco: un refill que calcula mal la direccion de
 * notify tumba la VM entera con un #PF la primera vez que el puntero se mueve,
 * mucho despues del arranque y sin que ningun escenario lo viera.
 *
 * Tambien afirma lo que el recorrido tiene que conservar: que la posicion
 * absoluta llega como posicion -- con el flag puesto y con el valor del host --
 * y no como un delta que el invitado acumularia. El host manda el centro y la
 * esquina del rango del tablet (0..32767), y esos son el centro y la esquina de
 * la pantalla.
 *
 * Sin checkpoint de rueda: el QMP de este QEMU solo acepta los ejes rel 'x' y
 * 'y', asi que un tick de rueda no se puede pedir por ahi (hmp mouse_move si lo
 * haria, y ese monitor es del runner, no del driver). La rueda pasa por el
 * mismo notify, asi que no es lo que esta regresion mide.
 */
#define POINTER_SELFTEST_TIMEOUT_MS 20000u

/* Dos pixeles de tolerancia: el mapeo del rango del tablet al tamano de pantalla
 * redondea (kernel/virtio_input.cpp, normalize_axis), asi que dos pixeles no
 * son margen sino el redondeo. Un consumidor que sumara las posiciones, o que
 * ignorara la bandera y solo usara deltas, se quedaria a cientos de pixeles. */
#define POINTER_POSITION_TOLERANCE 2

enum pointer_step {
    POINTER_STEP_CENTER = 0,
    POINTER_STEP_BUTTON,
    POINTER_STEP_CORNER,
    POINTER_STEP_COUNT
};

static const char* const g_pointer_step_label[POINTER_STEP_COUNT] = {
    "posicion absoluta al centro",
    "boton izquierdo",
    "posicion absoluta en la esquina"
};

static int near_pixel(int value, int expected) {
    int difference = value - expected;
    if (difference < 0) {
        difference = -difference;
    }
    return difference <= POINTER_POSITION_TOLERANCE;
}

static int pointer_selftest(void) {
    struct savanxp_gpu_info info = {0};
    struct savanxp_mouse_event event = {0};
    long gpu_fd;
    long mouse_fd;
    unsigned long deadline_ms;
    int step = 0;
    int center_x;
    int center_y;
    int corner_x;
    int corner_y;

    /* Sesion grafica por /dev/gpu0 y no por gfx_open: gfx_open habla con el
     * compositor, y este escenario corre sin windowd -- igual que kbdtest. Es
     * la sesion la que hace que el kernel encole el puntero: sin dueno de
     * pantalla, ui::graphics_active() es falso y el evento se descarta. */
    gpu_fd = gpu_open();
    if (gpu_fd < 0) {
        puts_fd(2, "POINTER SMOKE FAIL /dev/gpu0 no disponible\n");
        return 1;
    }
    if (gpu_get_info((int)gpu_fd, &info) < 0) {
        puts_fd(2, "POINTER SMOKE FAIL GPU_IOC_GET_INFO fallo\n");
        savanxp_close((int)gpu_fd);
        return 1;
    }
    if (gpu_acquire((int)gpu_fd) < 0) {
        puts_fd(2, "POINTER SMOKE FAIL GPU_IOC_ACQUIRE fallo\n");
        savanxp_close((int)gpu_fd);
        return 1;
    }

    mouse_fd = savanxp_open_mode("/dev/mouse0", SAVANXP_OPEN_READ);
    if (mouse_fd < 0) {
        puts_fd(2, "POINTER SMOKE FAIL /dev/mouse0 no disponible\n");
        gpu_release((int)gpu_fd);
        savanxp_close((int)gpu_fd);
        return 1;
    }

    center_x = (int)info.width / 2;
    center_y = (int)info.height / 2;
    corner_x = (int)info.width - 1;
    corner_y = (int)info.height - 1;

    /* El harness host espera esta linea antes de tocar el raton: sin ella las
     * posiciones absolutas podrian llegar antes de que este proceso sea el
     * dueno de la sesion y se perderian. */
    puts_out("POINTER SMOKE READY\n");

    deadline_ms = uptime_ms() + POINTER_SELFTEST_TIMEOUT_MS;
    while (step < POINTER_STEP_COUNT) {
        while (savanxp_read((int)mouse_fd, &event, sizeof(event)) == (long)sizeof(event)) {
            if (step == POINTER_STEP_BUTTON) {
                if ((event.buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0u) {
                    continue;
                }
            } else {
                /* Both position checkpoints want the ABSOLUTE flag set: it is the
                 * claim that the position travelled as a position. A relative
                 * device (PS/2) or a kernel that collapsed the tablet to deltas
                 * leaves the flag clear and never satisfies either one. */
                if ((event.flags & SAVANXP_MOUSE_FLAG_ABSOLUTE) == 0u) {
                    continue;
                }
                if (!near_pixel(event.absolute_x, step == POINTER_STEP_CENTER ? center_x : corner_x)) {
                    continue;
                }
                if (!near_pixel(event.absolute_y, step == POINTER_STEP_CENTER ? center_y : corner_y)) {
                    continue;
                }
            }

            printf("mousetest: checkpoint '%s' OK\n", g_pointer_step_label[step]);
            step += 1;
            if (step >= POINTER_STEP_COUNT) {
                break;
            }
        }

        if (step >= POINTER_STEP_COUNT) {
            break;
        }
        if (uptime_ms() >= deadline_ms) {
            printf("POINTER SMOKE FAIL timeout esperando '%s'\n", g_pointer_step_label[step]);
            savanxp_close((int)mouse_fd);
            gpu_release((int)gpu_fd);
            savanxp_close((int)gpu_fd);
            return 1;
        }
        sleep_ms(20);
    }

    savanxp_close((int)mouse_fd);
    gpu_release((int)gpu_fd);
    savanxp_close((int)gpu_fd);
    puts_out("POINTER SMOKE PASS\n");
    return 0;
}

int main(int argc, char** argv) {
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event key_event;
    struct savanxp_gui_pointer_event pointer_event;
    long mouse_fd;
    int cursor_x;
    int cursor_y;
    int delta_x = 0;
    int delta_y = 0;
    int wheel_last = 0;
    int wheel_total = 0;
    uint32_t buttons = 0;
    int needs_redraw = 1;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        return pointer_selftest();
    }

    if (gfx_open(&gfx) < 0) {
        puts_fd(2, "mousetest: open failed\n");
        return 1;
    }
    if (gfx.info.width > GFX_MAX_WIDTH || gfx.info.height > GFX_MAX_HEIGHT || (gfx.info.pitch / 4u) > GFX_MAX_WIDTH) {
        puts_fd(2, "mousetest: framebuffer too large\n");
        gfx_close(&gfx);
        return 1;
    }

    mouse_fd = gfx_pointer_open();
    if (mouse_fd < 0) {
        puts_fd(2, "mousetest: pointer channel not available\n");
        gfx_close(&gfx);
        return 1;
    }
    if (gfx_acquire(&gfx) < 0) {
        puts_fd(2, "mousetest: acquire failed\n");
        savanxp_close((int)mouse_fd);
        gfx_close(&gfx);
        return 1;
    }
    request_content_size(&gfx);

    cursor_x = 0;
    cursor_y = 0;

    for (;;) {
        while (gfx_poll_event(&gfx, &key_event) > 0) {
            if (key_event.type == SAVANXP_INPUT_EVENT_RESIZED) {
                (void)gfx_apply_resize_event(&gfx, &key_event);
                if (cursor_x >= (int)gfx.info.width) {
                    cursor_x = (int)gfx.info.width - 1;
                }
                if (cursor_y >= (int)gfx.info.height) {
                    cursor_y = (int)gfx.info.height - 1;
                }
                needs_redraw = 1;
                continue;
            }
            if (key_event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && key_event.key == SAVANXP_KEY_ESC) {
                gfx_release(&gfx);
                savanxp_close((int)mouse_fd);
                gfx_close(&gfx);
                return 0;
            }
        }

        while (gfx_poll_pointer((int)mouse_fd, &pointer_event) > 0) {
            delta_x = pointer_event.x - cursor_x;
            delta_y = pointer_event.y - cursor_y;
            cursor_x = pointer_event.x;
            cursor_y = pointer_event.y;
            buttons = pointer_event.buttons;
            if (pointer_event.wheel != 0) {
                wheel_last = pointer_event.wheel;
                wheel_total += pointer_event.wheel;
            }

            if (cursor_x < 0) {
                cursor_x = 0;
            }
            if (cursor_y < 0) {
                cursor_y = 0;
            }
            if (cursor_x >= (int)gfx.info.width) {
                cursor_x = (int)gfx.info.width - 1;
            }
            if (cursor_y >= (int)gfx.info.height) {
                cursor_y = (int)gfx.info.height - 1;
            }

            needs_redraw = 1;
        }

        if (!needs_redraw) {
            sleep_ms(16);
            continue;
        }

        draw_scene(&gfx, cursor_x, cursor_y, delta_x, delta_y, wheel_last, wheel_total, buttons);
        if (gfx_present(&gfx, g_backbuffer) < 0) {
            break;
        }
        needs_redraw = 0;
    }

    gfx_release(&gfx);
    savanxp_close((int)mouse_fd);
    gfx_close(&gfx);
    puts_fd(2, "mousetest: present failed\n");
    return 1;
}
