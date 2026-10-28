/* Native windowed client (keyboard-reactive): the first native window.
 *
 * Built as an external app and stamped EI_OSABI=0x53:
 *
 *     ./tools/build-user.sh --source sdk/nativedemo --name nativedemo --native
 *
 * Connects to windowd like any other client (protocol v5): sxnwm_connect
 * asks for its input on a ring and maps the surface and the ring that come
 * with the welcome. Speaks through include/sxn/sxnwm.h, the toolkit seed. Paints a marker,
 * waits until composed, then drains input until a key arrives, paints
 * green, waits for that composed, and exits 10 (1 on any failure or
 * timeout). The windowd selftest injects the key and samples both colors
 * from the composed framebuffer.
 */

#include <stdint.h>

#include "savanxp/syscall.h"
#include "wm/wm_protocol.h"
#include "sxn/sxn.h"
#include "sxn/sxnwm.h"

#define DEMO_MARKER 0x00E01050u
#define DEMO_GREEN 0x0000FF00u
#define DEMO_INPUT_ROUNDS 60
#define DEMO_INPUT_WAIT_MS 500

static int g_failures = 0;

static void check(int ok) {
    if (!ok) {
        g_failures += 1;
    }
}

static void paint_full(struct sxnwm_client* wm, uint32_t color) {
    uint32_t x = 0;
    uint32_t y = 0;

    for (y = 0; y < wm->height; y += 1) {
        for (x = 0; x < wm->width; x += 1) {
            wm->pixels[y * wm->stride + x] = color;
        }
    }
}

int main(void) {
    struct sxnwm_client wm;
    struct savanxp_wm_event event;
    long sequence = 0;
    long result = 0;
    int got_key = 0;
    int round = 0;

    check(sxnwm_connect(&wm) == 0);
    if (g_failures != 0) {
        sxn_log("nativedemo: no surface");
        sxn_exit(1);
    }

    paint_full(&wm, DEMO_MARKER);
    sequence = sxnwm_submit_full(&wm);
    check(sequence > 0);
    if (sequence > 0) {
        check(sxnwm_wait_composed(&wm, (uint64_t)sequence, 40, 500) == 0);
    }
    check(sxn_log("nativedemo: submitted") == 0);

    /* Drain input until a key arrives (bounded: the selftest injects one).
     * Drain-first ordering: a record landing between drain and reset stays
     * queued for the next drain, so nothing is lost, only delayed. */
    for (round = 0; round < DEMO_INPUT_ROUNDS && !got_key; round += 1) {
        while (sxnwm_poll_input(&wm, &event) == 0) {
            if (event.kind == SAVANXP_WM_EVENT_KEY) {
                got_key = 1;
                break;
            }
        }
        if (got_key) {
            break;
        }
        check(sxn_event_reset(wm.wake_fd) == 0);
        (void)sxn_wait_one(wm.wake_fd, DEMO_INPUT_WAIT_MS);
    }
    check(got_key);
    if (!got_key) {
        sxn_log("nativedemo: no key");
        sxn_exit(1);
    }

    paint_full(&wm, DEMO_GREEN);
    sequence = sxnwm_submit_full(&wm);
    check(sequence > 0);
    if (sequence > 0) {
        check(sxnwm_wait_composed(&wm, (uint64_t)sequence, 40, 500) == 0);
    }
    check(sxn_log("nativedemo: green") == 0);

    result = g_failures == 0 ? 10 : 1;
    sxn_exit((int)result);
    __builtin_unreachable();
}
