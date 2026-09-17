/* Media Player: reproductor de audio y video sobre FFmpeg para SavanXP.
 *
 *   mediaplayer [archivo]              la ventana; con archivo, lo reproduce
 *   mediaplayer --probe <archivo>      \
 *   mediaplayer --selftest ...          > modos sin ventana, ver selftest.c
 *   mediaplayer --gpu-hold <ms> <arch> /
 *
 * Todo corre en un solo hilo: el loop atiende la entrada, le da una vuelta a la
 * reproduccion (playback_pump) y duerme en poll() lo que esta le diga, que
 * nunca es mas que lo que aguanta el colchon de audio. La arquitectura y sus
 * limites estan en docs/MEDIA_PLAYER.md. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savanxp/libc.h"
#include "savanxp/sxgui.h"

#include <libavutil/log.h>

#include "media.h"
#include "playback.h"
#include "selftest.h"

#include "icons.inc"

#define MP_SEEK_HEIGHT 16
#define MP_CONTROLS_HEIGHT (SXGUI_MARGIN + MP_SEEK_HEIGHT + SXGUI_GAP + SXGUI_BUTTON_HEIGHT + SXGUI_MARGIN)
#define MP_ICON_BUTTON_WIDTH 32
#define MP_BUTTON_GROUP_GAP 10
#define MP_TIME_WIDTH 112
#define MP_VOLUME_WIDTH 80
#define MP_VOLUME_ICON_WIDTH 20
/* Tamano de la ventana: el video a su tamano natural, achicado para entrar en
 * esto; y nunca mas angosta que lo que piden los controles. */
#define MP_MAX_VIDEO_WIDTH 960
#define MP_MAX_VIDEO_HEIGHT 540
#define MP_MIN_WIDTH 480
#define MP_AUDIO_VIDEO_HEIGHT 160
#define MP_UI_REFRESH_NS 250000000ULL
#define MP_SEEK_STEP_US 5000000LL
#define MP_VOLUME_STEP 10

#define MP_OPEN_WIDTH 360
#define MP_OPEN_HEIGHT 112
#define MP_DLG_ROW (18 + 4)

enum {
    W_OPEN = 0,
    W_BACK,
    W_PLAY,
    W_STOP,
    W_FORWARD,
    W_TIME,
    W_VOLUME_ICON,
    W_VOLUME,
    W_COUNT
};

struct mp_state {
    struct sxgui_app app;
    struct playback playback;
    struct sxgui_widget widgets[W_COUNT];
    char time_text[48];
    char message[192];

    struct sx_rect video_rect;
    struct sx_rect frame_rect;
    struct sx_rect seek_rect;

    int dragging;          /* arrastrando la barra de progreso */
    int64_t drag_us;
    uint32_t last_buttons;

    int size_requested;    /* el WM atiende un solo pedido de tamano */
    int needs_full;
    int needs_controls;
    unsigned long long last_ui_ns;

    struct sxgui_dialog open_dialog;
    struct sxgui_widget open_widgets[4];
    char open_path[SAVANXP_DESKTOP_LAUNCH_PATH_CAPACITY];
};

static struct mp_state g;

/* ---- utilidades ---------------------------------------------------------- */

static void format_time(char* out, size_t capacity, int64_t us) {
    long long seconds = us > 0 ? us / 1000000 : 0;
    if (seconds >= 3600) {
        snprintf(out, capacity, "%lld:%02lld:%02lld", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    } else {
        snprintf(out, capacity, "%lld:%02lld", seconds / 60, seconds % 60);
    }
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static int point_in(struct sx_rect rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

static int has_file(void) {
    return g.playback.state != PLAYBACK_EMPTY;
}

/* ---- layout -------------------------------------------------------------- */

/* Rect del cuadro dentro del area de video: el tamano de presentacion achicado
 * (o agrandado) para entrar entero, centrado, con barras negras en lo que sobra. */
static void layout_frame(void) {
    int display_w = 0;
    int display_h = 0;
    int width = 0;
    int height = 0;

    memset(&g.frame_rect, 0, sizeof(g.frame_rect));
    if (!has_file() || !media_has_video(&g.playback.media)) {
        return;
    }
    media_display_size(&g.playback.media, &display_w, &display_h);
    if (display_w <= 0 || display_h <= 0 || g.video_rect.width <= 0 || g.video_rect.height <= 0) {
        return;
    }
    width = g.video_rect.width;
    height = (int)((long long)display_h * width / display_w);
    if (height > g.video_rect.height) {
        height = g.video_rect.height;
        width = (int)((long long)display_w * height / display_h);
    }
    if (width < 2 || height < 2) {
        return;
    }
    g.frame_rect = sx_rect_make(g.video_rect.x + (g.video_rect.width - width) / 2,
                                g.video_rect.y + (g.video_rect.height - height) / 2, width, height);
}

static void layout(void) {
    const int width = (int)g.app.gfx.info.width;
    const int height = (int)g.app.gfx.info.height;
    int video_h = height - MP_CONTROLS_HEIGHT;
    int row_y = 0;
    int x = SXGUI_MARGIN;
    int right = 0;

    if (video_h < 0) {
        video_h = 0;
    }
    g.video_rect = sx_rect_make(0, 0, width, video_h);
    g.seek_rect = sx_rect_make(SXGUI_MARGIN, video_h + SXGUI_MARGIN, width - SXGUI_MARGIN * 2, MP_SEEK_HEIGHT);
    row_y = g.seek_rect.y + MP_SEEK_HEIGHT + SXGUI_GAP;

    /* Abrir aparte; el transporte, pegado como una sola botonera. */
    g.widgets[W_OPEN].rect = sx_rect_make(x, row_y, MP_ICON_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT);
    x += MP_ICON_BUTTON_WIDTH + MP_BUTTON_GROUP_GAP;
    for (int index = W_BACK; index <= W_FORWARD; ++index) {
        g.widgets[index].rect = sx_rect_make(x, row_y, MP_ICON_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT);
        x += MP_ICON_BUTTON_WIDTH + 2;
    }
    x += MP_BUTTON_GROUP_GAP - 2;
    g.widgets[W_TIME].rect = sx_rect_make(x, row_y + (SXGUI_BUTTON_HEIGHT - SXGUI_FIELD_HEIGHT) / 2, MP_TIME_WIDTH,
                                          SXGUI_FIELD_HEIGHT);

    right = width - SXGUI_MARGIN;
    g.widgets[W_VOLUME].rect =
        sx_rect_make(right - MP_VOLUME_WIDTH, row_y + (SXGUI_BUTTON_HEIGHT - SXGUI_SCROLLBAR_THICKNESS) / 2,
                     MP_VOLUME_WIDTH, SXGUI_SCROLLBAR_THICKNESS);
    g.widgets[W_VOLUME_ICON].rect = sx_rect_make(right - MP_VOLUME_WIDTH - 4 - MP_VOLUME_ICON_WIDTH,
                                                 row_y + (SXGUI_BUTTON_HEIGHT - MP_ICON_SIZE) / 2,
                                                 MP_VOLUME_ICON_WIDTH, MP_ICON_SIZE);
    layout_frame();
}

static void on_resize(struct sxgui_app* app) {
    (void)app;
    layout();
    g.needs_full = 1;
}

/* Le pide a la ventana el tamano que le queda bien a lo que se abrio.
 *
 * Una sola vez: en una ventana redimensionable el WM atiende solo el PRIMER
 * pedido, para que una app no le pelee la geometria al usuario (ver
 * docs/WM_SUBSYSTEM.md). Por eso la ventana vacia no pide nada -- gastaria el
 * pedido en un tamano que no es el del video --, y los archivos que se abren
 * despues se acomodan con barras negras en la ventana que haya. */
static void request_window_size(void) {
    int width = MP_MIN_WIDTH;
    int video_h = MP_AUDIO_VIDEO_HEIGHT;

    if (g.size_requested || !has_file()) {
        return;
    }
    g.size_requested = 1;

    if (has_file() && media_has_video(&g.playback.media)) {
        int display_w = 0;
        int display_h = 0;
        media_display_size(&g.playback.media, &display_w, &display_h);
        if (display_w > 0 && display_h > 0) {
            width = display_w;
            video_h = display_h;
            if (width > MP_MAX_VIDEO_WIDTH) {
                video_h = (int)((long long)video_h * MP_MAX_VIDEO_WIDTH / width);
                width = MP_MAX_VIDEO_WIDTH;
            }
            if (video_h > MP_MAX_VIDEO_HEIGHT) {
                width = (int)((long long)width * MP_MAX_VIDEO_HEIGHT / video_h);
                video_h = MP_MAX_VIDEO_HEIGHT;
            }
            if (width < MP_MIN_WIDTH) {
                width = MP_MIN_WIDTH;
            }
        }
    }
    (void)sxgui_app_set_content_size(&g.app, width, video_h + MP_CONTROLS_HEIGHT);
    layout();
    g.needs_full = 1;
}

/* ---- pintado ------------------------------------------------------------- */

static void update_controls_text(void) {
    const unsigned long long now = playback_now_ns();
    char position[16];
    char duration[16];
    int64_t at = g.dragging ? g.drag_us : playback_position_us(&g.playback, now);

    if (!has_file()) {
        snprintf(g.time_text, sizeof(g.time_text), "--:-- / --:--");
    } else {
        format_time(position, sizeof(position), at);
        if (playback_duration_us(&g.playback) > 0) {
            format_time(duration, sizeof(duration), playback_duration_us(&g.playback));
        } else {
            snprintf(duration, sizeof(duration), "--:--");
        }
        snprintf(g.time_text, sizeof(g.time_text), "%s / %s", position, duration);
    }
    g.widgets[W_TIME].text = g.time_text;
    for (int index = W_BACK; index <= W_FORWARD; ++index) {
        const int seek_button = index == W_BACK || index == W_FORWARD;
        if (has_file() && (!seek_button || g.playback.media.seekable)) {
            g.widgets[index].flags &= ~SXGUI_FLAG_DISABLED;
        } else {
            g.widgets[index].flags |= SXGUI_FLAG_DISABLED;
        }
    }
}

static void paint_centered_text(struct sx_painter* painter, int y, const char* text, uint32_t colour) {
    const int width = sx_painter_text_width(painter, text);
    sx_painter_draw_text(painter, g.video_rect.x + (g.video_rect.width - width) / 2, y, text, colour);
}

/* Copia el ultimo cuadro escalado al backbuffer. Solo re-escala si cambio el
 * tamano: un repintado de los controles no tiene por que pagar swscale. */
static int blit_frame(int rescale) {
    const struct sx_rect rect = g.frame_rect;
    const uint32_t stride = gfx_stride_pixels(&g.app.gfx.info);
    const uint32_t* pixels = NULL;
    struct media* media = &g.playback.media;
    int row = 0;

    if (rect.width <= 0 || !has_file() || !media_has_shown_frame(media)) {
        return 0;
    }
    if (rescale || media->scaled == NULL || media->scaled_width != rect.width ||
        media->scaled_height != rect.height) {
        pixels = media_scale_video(media, rect.width, rect.height);
    } else {
        pixels = (const uint32_t*)(void*)media->scaled;
    }
    if (pixels == NULL) {
        return 0;
    }
    for (row = 0; row < rect.height; ++row) {
        memcpy(&g.app.gfx.pixels[(uint32_t)(rect.y + row) * stride + (uint32_t)rect.x], &pixels[row * rect.width],
               (size_t)rect.width * sizeof(uint32_t));
    }
    return 1;
}

static void paint_video_area(void) {
    struct sx_painter* painter = &g.app.ui.painter;
    const int line = sx_painter_text_height(painter) + 4;
    int y = g.video_rect.y + g.video_rect.height / 2 - line;

    if (g.video_rect.height <= 0) {
        return;
    }
    sx_painter_fill_rect(painter, g.video_rect, gfx_rgb(0, 0, 0));
    if (blit_frame(0)) {
        return;
    }

    if (g.message[0] != '\0') {
        paint_centered_text(painter, y, g.message, gfx_rgb(230, 120, 110));
        paint_centered_text(painter, y + line, "Press O to open another file.", gfx_rgb(160, 160, 160));
    } else if (!has_file()) {
        paint_centered_text(painter, y, "No file open.", gfx_rgb(220, 220, 220));
        paint_centered_text(painter, y + line, "Press O or click Open.", gfx_rgb(160, 160, 160));
    } else if (!media_has_video(&g.playback.media)) {
        const char* title = media_metadata(&g.playback.media, "title");
        const char* artist = media_metadata(&g.playback.media, "artist");
        char detail[96];
        snprintf(detail, sizeof(detail), "%s audio", media_audio_codec_name(&g.playback.media));
        paint_centered_text(painter, y - line, title != NULL ? title : base_name(g.playback.path),
                            gfx_rgb(240, 240, 240));
        if (artist != NULL) {
            paint_centered_text(painter, y, artist, gfx_rgb(190, 190, 190));
        }
        paint_centered_text(painter, y + line, media_has_audio(&g.playback.media) ? detail : "No audio device.",
                            gfx_rgb(140, 140, 140));
    }
}

static void paint_seek_bar(void) {
    struct sx_painter* painter = &g.app.ui.painter;
    const struct sx_rect rect = g.seek_rect;
    struct sx_rect inner = sx_rect_make(rect.x + SXGUI_BORDER_SUNKEN, rect.y + SXGUI_BORDER_SUNKEN,
                                        rect.width - SXGUI_BORDER_SUNKEN * 2, rect.height - SXGUI_BORDER_SUNKEN * 2);
    const int64_t duration = playback_duration_us(&g.playback);
    int64_t position = g.dragging ? g.drag_us : playback_position_us(&g.playback, playback_now_ns());
    int filled = 0;

    if (rect.width <= 0 || inner.width <= 0) {
        return;
    }
    sx_painter_fill_rect(painter, inner, SXGUI_COLOR_FIELD);
    sxgui_draw_sunken_edge(painter, rect);
    if (duration <= 0) {
        return;
    }
    if (position > duration) {
        position = duration;
    }
    filled = (int)(position * inner.width / duration);
    if (filled > 0) {
        sx_painter_fill_rect(painter, sx_rect_make(inner.x, inner.y, filled, inner.height), SXGUI_COLOR_SELECT);
    }
}

static void blit_icon(const uint32_t* pixels, int x, int y) {
    struct sx_bitmap bitmap;

    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.pixels = (uint32_t*)pixels;
    bitmap.info.width = MP_ICON_SIZE;
    bitmap.info.height = MP_ICON_SIZE;
    bitmap.info.pitch = MP_ICON_SIZE * (uint32_t)sizeof(uint32_t);
    bitmap.info.bpp = 32u;
    bitmap.info.buffer_size = MP_ICON_SIZE * MP_ICON_SIZE * (uint32_t)sizeof(uint32_t);
    bitmap.format = SX_PIXEL_FORMAT_BGRA8888;
    sx_painter_blit_bitmap(&g.app.ui.painter, &bitmap, x, y);
}

/* Glifo de un control, centrado en su rect y hundido un pixel mientras se
 * aprieta, como el texto de un boton. Deshabilitado va con el relieve grabado
 * del toolkit (sxchrome_draw_glyph_disabled): la silueta en blanco corrida un
 * pixel y encima en gris, asi se lee igual que un rotulo deshabilitado. */
static void paint_icon(struct sx_rect rect, const uint32_t* icon, int pressed, int disabled) {
    uint32_t light[MP_ICON_SIZE * MP_ICON_SIZE];
    uint32_t shadow[MP_ICON_SIZE * MP_ICON_SIZE];
    const int x = rect.x + (rect.width - MP_ICON_SIZE) / 2 + (pressed ? 1 : 0);
    const int y = rect.y + (rect.height - MP_ICON_SIZE) / 2 + (pressed ? 1 : 0);
    int index = 0;

    if (!disabled) {
        blit_icon(icon, x, y);
        return;
    }
    for (index = 0; index < MP_ICON_SIZE * MP_ICON_SIZE; ++index) {
        const uint32_t alpha = icon[index] & 0xff000000u;
        light[index] = alpha | (SXGUI_COLOR_LIGHT & 0x00ffffffu);
        shadow[index] = alpha | (SXGUI_COLOR_SHADOW & 0x00ffffffu);
    }
    blit_icon(light, x + 1, y + 1);
    blit_icon(shadow, x, y);
}

static void paint_control_icons(void) {
    static const struct {
        int widget;
        const uint32_t* icon;
    } fixed[] = {
        {W_OPEN, mp_icon_document_open},
        {W_BACK, mp_icon_media_seek_backward},
        {W_STOP, mp_icon_media_playback_stop},
        {W_FORWARD, mp_icon_media_seek_forward},
    };
    const uint32_t* volume_icon = mp_icon_audio_volume_high;
    size_t index = 0;

    for (index = 0; index < sizeof(fixed) / sizeof(fixed[0]); ++index) {
        const struct sxgui_widget* widget = &g.widgets[fixed[index].widget];
        paint_icon(widget->rect, fixed[index].icon, widget->pressed && widget->hover,
                   (widget->flags & SXGUI_FLAG_DISABLED) != 0);
    }
    {
        const struct sxgui_widget* play = &g.widgets[W_PLAY];
        paint_icon(play->rect,
                   g.playback.state == PLAYBACK_PLAYING ? mp_icon_media_playback_pause : mp_icon_media_playback_start,
                   play->pressed && play->hover, (play->flags & SXGUI_FLAG_DISABLED) != 0);
    }

    if (g.playback.volume == 0) {
        volume_icon = mp_icon_audio_volume_muted;
    } else if (g.playback.volume < 34) {
        volume_icon = mp_icon_audio_volume_low;
    } else if (g.playback.volume < 67) {
        volume_icon = mp_icon_audio_volume_medium;
    }
    paint_icon(g.widgets[W_VOLUME_ICON].rect, volume_icon, 0, 0);
}

static void compose(void) {
    update_controls_text();
    sxgui_paint_content(&g.app.ui);
    paint_control_icons();
    paint_video_area();
    paint_seek_bar();
    sxgui_paint_overlay(&g.app.ui);
}

static void quit(void) {
    sxgui_app_quit(&g.app, 0);
}

static void present_full(void) {
    compose();
    if (gfx_present(&g.app.gfx, g.app.gfx.pixels) < 0) {
        quit();
    }
    g.needs_full = 0;
    g.needs_controls = 0;
    g.last_ui_ns = playback_now_ns();
}

static void present_controls(void) {
    const int top = g.video_rect.height;
    const int height = (int)g.app.gfx.info.height - top;

    if (height <= 0 || sxgui_dialog_active(&g.app.ui)) {
        present_full();
        return;
    }
    compose();
    if (gfx_present_region(&g.app.gfx, g.app.gfx.pixels, 0, (uint32_t)top, g.app.gfx.info.width,
                           (uint32_t)height) < 0) {
        quit();
    }
    g.needs_controls = 0;
    g.last_ui_ns = playback_now_ns();
}

/* Callback de playback_pump: llego la hora de un cuadro. */
static void on_present(void* user, struct media* media) {
    const struct sx_rect rect = g.frame_rect;
    (void)user;
    (void)media;

    if (rect.width <= 0 || !blit_frame(1)) {
        return;
    }
    /* Con un dialogo abierto el cuadro nuevo quedaria encima de el: ahi se
     * recompone todo. Si no, solo viaja el rect del video. */
    if (sxgui_dialog_active(&g.app.ui) || g.needs_full) {
        g.needs_full = 1;
        return;
    }
    if (gfx_present_region(&g.app.gfx, g.app.gfx.pixels, (uint32_t)rect.x, (uint32_t)rect.y,
                           (uint32_t)rect.width, (uint32_t)rect.height) < 0) {
        quit();
    }
}

/* ---- acciones ------------------------------------------------------------ */

static void open_file(const char* path) {
    g.message[0] = '\0';
    if (!playback_open(&g.playback, path)) {
        snprintf(g.message, sizeof(g.message), "Cannot open %s: %s", base_name(path), g.playback.error);
        playback_close(&g.playback);
    }
    request_window_size();
    /* Con o sin pedido atendido, el cuadro del archivo nuevo tiene otras
     * proporciones. */
    layout();
    if (has_file()) {
        /* Muestra el primer cuadro antes de arrancar: si el archivo tarda en
         * empezar a sonar, que al menos se vea que abrio. */
        unsigned long wait = 0;
        (void)playback_pump(&g.playback, playback_now_ns(), on_present, NULL, &wait);
        playback_play(&g.playback);
    }
    g.needs_full = 1;
}

static void seek_relative(int64_t delta_us) {
    const unsigned long long now = playback_now_ns();
    int64_t target = playback_position_us(&g.playback, now) + delta_us;
    if (!has_file() || !g.playback.media.seekable) {
        return;
    }
    if (target < 0) {
        target = 0;
    }
    if (target > playback_duration_us(&g.playback)) {
        target = playback_duration_us(&g.playback);
    }
    (void)playback_seek(&g.playback, target);
    g.needs_full = 1;
}

static void set_volume(int volume) {
    playback_set_volume(&g.playback, volume);
    g.widgets[W_VOLUME].value = g.playback.volume;
    g.needs_controls = 1;
}

static void on_open_accept(struct sxgui_widget* widget, void* user) {
    char path[sizeof(g.open_path)];
    (void)widget;
    (void)user;
    snprintf(path, sizeof(path), "%s", g.open_path);
    sxgui_dialog_end(&g.app.ui, 1);
    if (path[0] != '\0') {
        open_file(path);
    }
    g.needs_full = 1;
}

static void on_open_cancel(struct sxgui_widget* widget, void* user) {
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g.app.ui, 0);
    g.needs_full = 1;
}

static void show_open_dialog(void) {
    if (sxgui_dialog_active(&g.app.ui)) {
        return;
    }
    snprintf(g.open_path, sizeof(g.open_path), "%s", has_file() ? g.playback.path : "/disk/media/");
    g.open_widgets[1].caret = (int)strlen(g.open_path);
    sxgui_dialog_begin(&g.app.ui, &g.open_dialog, MP_OPEN_WIDTH, MP_OPEN_HEIGHT);
    g.needs_full = 1;
}

static void on_button(struct sxgui_widget* widget, void* user) {
    (void)user;
    if (widget == &g.widgets[W_OPEN]) {
        show_open_dialog();
    } else if (widget == &g.widgets[W_BACK]) {
        seek_relative(-MP_SEEK_STEP_US);
    } else if (widget == &g.widgets[W_FORWARD]) {
        seek_relative(MP_SEEK_STEP_US);
    } else if (widget == &g.widgets[W_PLAY]) {
        playback_toggle(&g.playback);
    } else if (widget == &g.widgets[W_STOP]) {
        playback_stop(&g.playback);
    }
    g.needs_full = 1;
}

static void on_volume(struct sxgui_widget* widget, void* user) {
    (void)user;
    set_volume(widget->value);
}

/* ---- entrada ------------------------------------------------------------- */

static int handle_key(const struct savanxp_input_event* event) {
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || sxgui_dialog_active(&g.app.ui)) {
        return 0;
    }
    switch (event->key) {
        case SAVANXP_KEY_LEFT:
            seek_relative(-MP_SEEK_STEP_US);
            return 1;
        case SAVANXP_KEY_RIGHT:
            seek_relative(MP_SEEK_STEP_US);
            return 1;
        case SAVANXP_KEY_UP:
            set_volume(g.playback.volume + MP_VOLUME_STEP);
            return 1;
        case SAVANXP_KEY_DOWN:
            set_volume(g.playback.volume - MP_VOLUME_STEP);
            return 1;
        case SAVANXP_KEY_HOME:
            if (has_file()) {
                (void)playback_seek(&g.playback, 0);
                g.needs_full = 1;
            }
            return 1;
        default:
            break;
    }
    if (event->ascii == ' ') {
        playback_toggle(&g.playback);
        g.needs_full = 1;
        return 1;
    }
    if (event->ascii == 'o' || event->ascii == 'O') {
        show_open_dialog();
        return 1;
    }
    return 0;
}

static int64_t seek_position_at(int x) {
    const int inner_x = g.seek_rect.x + SXGUI_BORDER_SUNKEN;
    const int inner_w = g.seek_rect.width - SXGUI_BORDER_SUNKEN * 2;
    int offset = x - inner_x;
    if (inner_w <= 0) {
        return 0;
    }
    if (offset < 0) {
        offset = 0;
    }
    if (offset > inner_w) {
        offset = inner_w;
    }
    return playback_duration_us(&g.playback) * offset / inner_w;
}

/* La barra de progreso y el area de video no son widgets: se atienden antes
 * que el toolkit. Devuelve 1 si el evento era para ellos. */
static int handle_pointer(const struct savanxp_gui_pointer_event* event) {
    const int left = (event->buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0;
    const int pressed = left && (g.last_buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0;
    const int released = !left && (g.last_buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0;
    struct sx_rect seek_hit = g.seek_rect;
    int consumed = 0;

    g.last_buttons = event->buttons;
    if (sxgui_dialog_active(&g.app.ui)) {
        return 0;
    }
    if (event->wheel != 0) {
        set_volume(g.playback.volume + event->wheel * 5);
        consumed = 1;
    }

    seek_hit.y -= 4;
    seek_hit.height += 8;
    if (g.dragging) {
        g.drag_us = seek_position_at(event->x);
        g.needs_controls = 1;
        if (released) {
            g.dragging = 0;
            (void)playback_seek(&g.playback, g.drag_us);
            g.needs_full = 1;
        }
        return 1;
    }
    if (pressed && point_in(seek_hit, event->x, event->y) && has_file() && g.playback.media.seekable) {
        g.dragging = 1;
        g.drag_us = seek_position_at(event->x);
        g.needs_controls = 1;
        return 1;
    }
    if (pressed && point_in(g.video_rect, event->x, event->y) && has_file()) {
        playback_toggle(&g.playback);
        g.needs_full = 1;
        return 1;
    }
    return consumed;
}

static void pump_input(void) {
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;

    while (gfx_poll_event(&g.app.gfx, &event) > 0) {
        if (event.type == SAVANXP_INPUT_EVENT_RESIZED) {
            (void)gfx_apply_resize_event(&g.app.gfx, &event);
            sxgui_context_retarget(&g.app.ui, g.app.gfx.pixels, &g.app.gfx.info);
            on_resize(&g.app);
            continue;
        }
        if (handle_key(&event)) {
            continue;
        }
        if (sxgui_handle_key(&g.app.ui, &event)) {
            g.needs_full = 1;
            continue;
        }
        if (event.type == SAVANXP_INPUT_EVENT_KEY_DOWN && event.key == SAVANXP_KEY_ESC) {
            quit();
        }
    }

    while (g.app.pointer_fd >= 0 && gfx_poll_pointer((int)g.app.pointer_fd, &pointer) > 0) {
        int shape = 0;
        if (!handle_pointer(&pointer) && sxgui_handle_pointer(&g.app.ui, &pointer)) {
            g.needs_full = 1;
        }
        shape = sxgui_cursor_shape(&g.app.ui);
        if (shape != g.app.last_sent_cursor_shape &&
            gfx_desktop_set_cursor_shape(&g.app.gfx, (uint32_t)shape) == 0) {
            g.app.last_sent_cursor_shape = shape;
        }
    }
}

/* ---- armado y loop ------------------------------------------------------- */

static void build_widgets(void) {
    const struct sx_rect zero = sx_rect_make(0, 0, 0, 0);

    /* Botones de icono: sin rotulo, el glifo lo pinta paint_control_icons. */
    g.widgets[W_OPEN] = sxgui_button(zero, "", on_button, NULL);
    g.widgets[W_BACK] = sxgui_button(zero, "", on_button, NULL);
    g.widgets[W_PLAY] = sxgui_button(zero, "", on_button, NULL);
    g.widgets[W_STOP] = sxgui_button(zero, "", on_button, NULL);
    g.widgets[W_FORWARD] = sxgui_button(zero, "", on_button, NULL);
    g.widgets[W_TIME] = sxgui_label(zero, g.time_text);
    g.widgets[W_TIME].flags |= SXGUI_FLAG_SUNKEN;
    g.widgets[W_VOLUME_ICON] = sxgui_label(zero, "");
    g.widgets[W_VOLUME] = sxgui_scrollbar(zero, 0, 100, 10, 100);
    g.widgets[W_VOLUME].flags |= SXGUI_FLAG_HSCROLL;
    g.widgets[W_VOLUME].on_action = on_volume;

    g.open_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN, MP_OPEN_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        "Media file:");
    g.open_widgets[1] = sxgui_textfield(sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN + MP_DLG_ROW,
                                                     MP_OPEN_WIDTH - SXGUI_DIALOG_MARGIN * 2, SXGUI_FIELD_HEIGHT),
                                        g.open_path, sizeof(g.open_path));
    g.open_widgets[2] = sxgui_button(
        sx_rect_make(MP_OPEN_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH * 2 - SXGUI_GAP,
                     MP_OPEN_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                     SXGUI_BUTTON_HEIGHT),
        "Open", on_open_accept, NULL);
    g.open_widgets[3] =
        sxgui_button(sx_rect_make(MP_OPEN_WIDTH - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_WIDTH,
                                  MP_OPEN_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT, SXGUI_BUTTON_WIDTH,
                                  SXGUI_BUTTON_HEIGHT),
                     "Cancel", on_open_cancel, NULL);
    g.open_dialog.title = "Open";
    g.open_dialog.widgets = g.open_widgets;
    g.open_dialog.widget_count = 4;
    g.open_dialog.initial_focus = 1;
    g.open_dialog.default_button = 2;
}

static int run_window(const char* path) {
    memset(&g, 0, sizeof(g));
    playback_init(&g.playback);
    build_widgets();
    update_controls_text();

    if (sxgui_app_init(&g.app, "mediaplayer", g.widgets, W_COUNT) < 0) {
        return 1;
    }
    g.app.on_resize = on_resize;

    layout();
    if (path != NULL) {
        open_file(path);
    }
    present_full();

    while (g.app.running) {
        struct savanxp_pollfd ready;
        unsigned long wait_ms = 50;
        unsigned long long now = 0;
        unsigned flags = 0;

        pump_input();
        if (gfx_should_close(&g.app.gfx)) {
            break;
        }

        now = playback_now_ns();
        flags = playback_pump(&g.playback, now, on_present, NULL, &wait_ms);
        if ((flags & PLAYBACK_PUMP_STATE_CHANGED) != 0) {
            g.needs_full = 1;
        }
        if (g.playback.state == PLAYBACK_PLAYING && now - g.last_ui_ns >= MP_UI_REFRESH_NS) {
            g.needs_controls = 1;
        }

        if (g.needs_full) {
            present_full();
        } else if (g.needs_controls) {
            present_controls();
        }
        if (!g.app.running) {
            break;
        }

        ready.fd = g.app.gfx.input_fd;
        ready.events = SAVANXP_POLLIN;
        ready.revents = 0;
        (void)savanxp_poll(&ready, 1, (long)wait_ms);
    }

    playback_close(&g.playback);
    gfx_release(&g.app.gfx);
    if (g.app.pointer_fd >= 0) {
        savanxp_close((int)g.app.pointer_fd);
    }
    gfx_close(&g.app.gfx);
    return g.app.exit_code;
}

int main(int argc, char** argv) {
    /* Los avisos de FFmpeg van a stdout, que en el escritorio no mira nadie y en
     * el harness ensucia el log: solo los errores. */
    av_log_set_level(AV_LOG_ERROR);

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        return selftest_main(argc - 2, argv + 2);
    }
    if (argc >= 3 && strcmp(argv[1], "--probe") == 0) {
        return probe_main(argv[2]);
    }
    if (argc >= 4 && strcmp(argv[1], "--gpu-hold") == 0) {
        return gpu_hold_main((unsigned long)strtol(argv[2], NULL, 10), argv[3]);
    }
    return run_window(argc >= 2 ? argv[1] : NULL);
}
