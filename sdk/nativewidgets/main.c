/* Native widgets demo: the first interactive native form.
 *
 * Built as an external app and stamped EI_OSABI=0x53:
 *
 *     ./tools/build-user.sh --source sdk/nativewidgets --name nativewidgets --native
 *
 * Connects to windowd like any client (sxnwm_connect) and is driven
 * through include/sxn/sxnwidgets.h: a label, a button, a text field and
 * two state squares (button activations, field has text). Pointer clicks
 * and keys arrive on the native input ring; Esc ends the demo with a final
 * composed frame and exit 10 (1 on failure or timeout). The windowd
 * selftest scripts the whole interaction (click, type, Esc) and samples
 * the squares from the composed framebuffer. Layout margins are shared
 * with that selftest; the surface is always bigger (generic overlays are
 * hundreds of pixels wide).
 */

#include <stdint.h>

#include "savanxp/syscall.h"
#include "savanxp/wm_protocol.h"
#include "sxn/sxn.h"
#include "sxn/sxnwm.h"
#include "sxn/sxnwidgets.h"

/* Surface-local layout, mirrored in windowd_native_widgets_selftest. */
#define WIDGET_BUTTON_X 20
#define WIDGET_BUTTON_Y 40
#define WIDGET_BUTTON_W 120
#define WIDGET_BUTTON_H 28
#define WIDGET_FIELD_X 20
#define WIDGET_FIELD_Y 80
#define WIDGET_FIELD_W 200
#define WIDGET_FIELD_H 24
#define WIDGET_SQ_Y 120
#define WIDGET_SQ_1_X 20
#define WIDGET_SQ_2_X 44
#define WIDGET_SQ_SIZE 16
#define WIDGET_SQ_OFF 0x00222222u
#define WIDGET_SQ_ON 0x0000FF00u

#define WIDGET_ROUNDS 120
#define WIDGET_WAIT_MS 500

static int g_failures = 0;

static void check(int ok) {
    if (!ok) {
        g_failures += 1;
    }
}

static void itoau(long value, char* buffer) {
    char reversed[24];
    int length = 0;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    while (value > 0 && length < 23) {
        reversed[length] = (char)('0' + (value % 10));
        value /= 10;
        length += 1;
    }
    for (int index = 0; index < length; index += 1) {
        buffer[index] = reversed[length - 1 - index];
    }
    buffer[length] = '\0';
}

static struct sxnw_label g_title = {{20, 12, 200, 12}, "Native widgets"};
static struct sxnw_button g_button = {{WIDGET_BUTTON_X, WIDGET_BUTTON_Y, WIDGET_BUTTON_W, WIDGET_BUTTON_H}, "OK", 0};
static struct sxnw_field g_field = {{WIDGET_FIELD_X, WIDGET_FIELD_Y, WIDGET_FIELD_W, WIDGET_FIELD_H}, "", 0, 0};
static int g_clicks = 0;
static int g_square_1 = 0;
static int g_square_2 = 0;
static char g_counter_text[32] = {"clicks: 0"};

static void paint_all(struct sxnwm_client* wm) {
    struct sxnw_rect background = {0, 0, (int)wm->width, (int)wm->height};
    struct sxnw_rect square_1 = {WIDGET_SQ_1_X, WIDGET_SQ_Y, WIDGET_SQ_SIZE, WIDGET_SQ_SIZE};
    struct sxnw_rect square_2 = {WIDGET_SQ_2_X, WIDGET_SQ_Y, WIDGET_SQ_SIZE, WIDGET_SQ_SIZE};

    sxnw_fill_rect(wm, &background, SXNW_COLOR_FACE);
    sxnw_draw_label(wm, &g_title);
    sxnw_draw_button(wm, &g_button);
    sxnw_draw_field(wm, &g_field);
    sxnw_fill_rect(wm, &square_1, g_square_1 ? WIDGET_SQ_ON : WIDGET_SQ_OFF);
    sxnw_fill_rect(wm, &square_2, g_square_2 ? WIDGET_SQ_ON : WIDGET_SQ_OFF);
    sxnw_draw_text(wm, 20, 144, g_counter_text, SXNW_COLOR_BLACK);
}

static void refresh_counter(void) {
    char digits[24];
    int index = 0;
    int base = 0;

    itoau(g_clicks, digits);
    g_counter_text[0] = 'c';
    g_counter_text[1] = 'l';
    g_counter_text[2] = 'i';
    g_counter_text[3] = 'c';
    g_counter_text[4] = 'k';
    g_counter_text[5] = 's';
    g_counter_text[6] = ':';
    g_counter_text[7] = ' ';
    base = 8;
    while (digits[index] != '\0' && base < 30) {
        g_counter_text[base] = digits[index];
        base += 1;
        index += 1;
    }
    g_counter_text[base] = '\0';
}

static long repaint(struct sxnwm_client* wm) {
    long sequence = 0;

    paint_all(wm);
    sequence = sxnwm_submit_full(wm);
    if (sequence == -SAVANXP_EAGAIN) {
        /* Slots busy: wait for the previous frame, then retry once. */
        if (sxnwm_wait_composed(wm, wm->header->submit_sequence, 40, 500) != 0) {
            return -SAVANXP_ETIMEDOUT;
        }
        sequence = sxnwm_submit_full(wm);
    }
    if (sequence <= 0) {
        return sequence;
    }
    return sxnwm_wait_composed(wm, (uint64_t)sequence, 40, 500);
}

int main(void) {
    struct sxnwm_client wm;
    struct savanxp_wm_event event;
    int done = 0;
    int dirty = 0;
    int round = 0;

    check(sxnwm_connect(&wm) == 0);
    if (g_failures != 0 || wm.width < 240u || wm.height < 170u) {
        sxn_log("nativewidgets: no surface");
        sxn_exit(1);
    }
    check(repaint(&wm) == 0);
    check(sxn_log("nativewidgets: form up") == 0);

    for (round = 0; round < WIDGET_ROUNDS && !done; round += 1) {
        while (sxnwm_poll_input(&wm, &event) == 0) {
            if (event.kind == SAVANXP_WM_EVENT_KEY && event.payload.key.type == SAVANXP_INPUT_EVENT_KEY_DOWN) {
                if (event.payload.key.key == 27) {
                    done = 1;
                    check(sxn_log("nativewidgets: esc") == 0);
                    break;
                }
                if (sxnw_field_key(&g_field, event.payload.key.ascii)) {
                    dirty = 1;
                    if (g_field.length == 1) {
                        check(sxn_log("nativewidgets: typed") == 0);
                    }
                }
            } else if (event.kind == SAVANXP_WM_EVENT_POINTER && event.window_id == 0) {
                int px = event.payload.pointer.x;
                int py = event.payload.pointer.y;

                if ((event.payload.pointer.buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
                    if (sxnw_hit(&g_button.rect, px, py)) {
                        if (!g_button.pressed) {
                            g_button.pressed = 1;
                            dirty = 1;
                        }
                    } else if (sxnw_hit(&g_field.rect, px, py)) {
                        if (!g_field.focused) {
                            g_field.focused = 1;
                            dirty = 1;
                        }
                    } else if (g_field.focused) {
                        g_field.focused = 0;
                        dirty = 1;
                    }
                } else if (g_button.pressed) {
                    g_button.pressed = 0;
                    dirty = 1;
                    if (sxnw_hit(&g_button.rect, px, py)) {
                        g_clicks += 1;
                        g_square_1 = 1;
                        refresh_counter();
                        check(sxn_log("nativewidgets: click") == 0);
                    }
                }
            }
        }
        if (g_field.length > 0) {
            g_square_2 = 1;
        }
        if (dirty) {
            check(repaint(&wm) == 0);
            dirty = 0;
        }
        if (done) {
            break;
        }
        check(sxn_event_reset(wm.wake_fd) == 0);
        (void)sxn_wait_one(wm.wake_fd, WIDGET_WAIT_MS);
    }
    check(done);

    sxn_exit(g_failures == 0 ? 10 : 1);
    __builtin_unreachable();
}
