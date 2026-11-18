/* A SUN program with an SXGUI window, built outside the tree against the SDK.
 *
 *     ./tools/build-user.sh --source sdk/sungui --name sungui --native --gui
 *
 * --native --gui links SUN's application frame (include/sxn/sxngui.h) and the
 * toolkit, and generates sungui.sunres.h from sungui.sxres: the title and
 * icon the window publishes. This is the same frame Task Manager uses.
 *
 * The window has a label and a Close button, and paints a square in its
 * centre. The windowd selftest (windowd-smoke) checks it end to end:
 *
 *   - the square starts as SUNGUI_MARKER;
 *   - the selftest sends the key 'A', and the square turns SUNGUI_GREEN;
 *   - once green has been on screen for a moment the program exits with 10.
 *
 * It exits with 1 when no key arrives within SUNGUI_TIMEOUT_MS, and with 0
 * from the Close button.
 */

#include <stdint.h>

#include "sxgfx/sxgui.h"
#include "sxn/sxn.h"
#include "sxn/sxngui.h"
#include "sungui.sunres.h"

#define SUNGUI_MARKER 0x0030A0E0u
#define SUNGUI_GREEN 0x0000FF00u
#define SUNGUI_SQUARE 60
#define SUNGUI_WIDTH 320
#define SUNGUI_HEIGHT 200
#define SUNGUI_TICK_MS 100
/* Green stays up this many ticks before the exit, so it gets composed. */
#define SUNGUI_GREEN_TICKS 10
#define SUNGUI_TIMEOUT_MS 60000

enum {
    SUNGUI_LABEL,
    SUNGUI_CLOSE,
    SUNGUI_WIDGET_COUNT
};

static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[SUNGUI_WIDGET_COUNT];
static int g_green = 0;
static int g_green_ticks = 0;
static unsigned long g_started_ms = 0;

static void on_close_button(struct sxgui_widget* widget, void* user) {
    (void)widget;
    (void)user;
    sxngui_app_quit(&g_app, 0);
}

static void layout(struct sxngui_app* app) {
    int width = (int)app->info.width;

    g_widgets[SUNGUI_LABEL].rect = sx_rect_make(12, 12, width - 24, 20);
    g_widgets[SUNGUI_CLOSE].rect = sx_rect_make(width - 12 - SXGUI_BUTTON_WIDTH, 12 + 24,
                                                SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT);
}

static void on_paint(struct sxngui_app* app) {
    int x = ((int)app->info.width - SUNGUI_SQUARE) / 2;
    int y = ((int)app->info.height - SUNGUI_SQUARE) / 2;

    sx_painter_fill_rect(&app->ui.painter, sx_rect_make(x, y, SUNGUI_SQUARE, SUNGUI_SQUARE),
                         g_green ? SUNGUI_GREEN : SUNGUI_MARKER);
}

static int on_key(struct sxngui_app* app, const struct savanxp_input_event* event) {
    if (event->type == SAVANXP_INPUT_EVENT_KEY_DOWN && (event->ascii == 'A' || event->ascii == 'a') && !g_green) {
        g_green = 1;
        (void)sxn_log("sungui: key");
        sxngui_app_request_repaint(app);
        return 1;
    }
    return 0;
}

static void on_tick(struct sxngui_app* app) {
    if (g_green) {
        g_green_ticks += 1;
        if (g_green_ticks >= SUNGUI_GREEN_TICKS) {
            (void)sxn_log("sungui: done");
            sxngui_app_quit(app, 10);
        }
    } else if (sxngui_uptime_ms() - g_started_ms >= SUNGUI_TIMEOUT_MS) {
        (void)sxn_log("sungui: no key");
        sxngui_app_quit(app, 1);
    }
}

int main(void) {
    g_widgets[SUNGUI_LABEL] = sxgui_label(sx_rect_make(0, 0, 0, 0), "A SUN window from the SDK.");
    g_widgets[SUNGUI_CLOSE] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Close", on_close_button, 0);

    if (sxngui_app_init(&g_app, &sun_resources, g_widgets, SUNGUI_WIDGET_COUNT) < 0) {
        (void)sxn_log("sungui: no window from windowd");
        return 1;
    }
    g_app.on_paint = on_paint;
    g_app.on_key = on_key;
    g_app.on_resize = layout;
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = SUNGUI_TICK_MS;
    g_started_ms = sxngui_uptime_ms();
    (void)sxngui_app_set_content_size(&g_app, SUNGUI_WIDTH, SUNGUI_HEIGHT);
    layout(&g_app);
    (void)sxn_log("sungui: window up");
    return sxngui_app_run(&g_app);
}
