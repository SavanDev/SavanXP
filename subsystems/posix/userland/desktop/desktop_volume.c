#include "desktop.h"
#include "desktop_icons.h"
#include "sxn/sxnaudio.h"

#include <stdio.h>

/*
 * The volume popup: a popup of the desktop, anchored in the tray corner above
 * the speaker button. No widgets: it paints with sxgui's palette and handles
 * its own pointer. Sliders apply live; the speaker glyph beside the output
 * device toggles mute; "Sound settings..." opens the SUN Sound sheet. The
 * speaker button or a press outside closes it.
 *
 *   Volume                                   [speaker]
 *   ---------------------------------------------------
 *   [speaker] Output device
 *             Default output (AC'97)
 *   [====================O-------]  75%
 *   ---------------------------------------------------
 *   Applications
 *   [icon] Doom
 *   [=========O------------------]  55%
 *   ...
 *   ---------------------------------------------------
 *                                    Sound settings...
 *
 * Master volume and mute are kernel ioctls on /dev/audio0, persisted in the
 * profile's audio.cfg as `volume` does. The application rows come from
 * audiod's native control port, the same snapshot the Sound sheet reads: it
 * needs no network stack, unlike the legacy UDP census, which only answers
 * where an IPv4 address is configured -- not on a host without a supported
 * NIC. Application levels are saved by audiod itself. Each row shows the icon
 * of the window that the stream's process owns.
 */

#define VP_WIDTH 232
#define VP_PAD 10
#define VP_TEXT_X 34
#define VP_PERCENT_WIDTH 40

#define VP_TITLE_Y 8
#define VP_RULE1_Y 32
#define VP_DEVICE_Y 40
#define VP_MASTER_Y 76
#define VP_RULE2_Y 102
#define VP_APPS_TITLE_Y 110
#define VP_APP_Y(row) (130 + (row) * 40)
#define VP_APP_ROWS 3
#define VP_RULE3_Y VP_APP_Y(VP_APP_ROWS)
#define VP_LINK_Y (VP_RULE3_Y + 8)
#define VP_HEIGHT (VP_LINK_Y + 26)

/* A slider is a sunken track with a raised thumb across [x, x+width). */
#define VP_SLIDER_X VP_PAD
#define VP_SLIDER_WIDTH (VP_WIDTH - VP_PAD * 2 - VP_PERCENT_WIDTH)
#define VP_SLIDER_HEIGHT 20
#define VP_TRACK_HEIGHT 4
#define VP_THUMB_WIDTH 10
#define VP_THUMB_HEIGHT 18
#define VP_TRAVEL (VP_SLIDER_WIDTH - VP_THUMB_WIDTH)

#define VP_WHEEL_STEP 5
#define VP_POLL_MS 500u
#define VP_QUERY_TIMEOUT_MS 1000u
#define VP_RETRY_MS 1000u

#define VP_LINK_TEXT "Sound settings..."
#define VP_LINK_COLOUR SXGUI_RGB(0, 0, 255)

/* The user's, in the profile (savanxp_settings_path), as `volume` keeps it. */
#define VP_CONFIG_NAME "audio.cfg"

enum
{
    VP_DRAG_NONE = -1,
    VP_DRAG_MASTER = -2,
};

static int vp_in_rect(struct sx_rect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

static int vp_clamp_volume(int volume)
{
    return volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

/* The whole slider row at `y`, which is also its hit area. */
static struct sx_rect vp_slider_rect(int y)
{
    return sx_rect_make(VP_SLIDER_X, y, VP_SLIDER_WIDTH, VP_SLIDER_HEIGHT);
}

static struct sx_rect vp_thumb_rect(int y, int volume)
{
    return sx_rect_make(
        VP_SLIDER_X + vp_clamp_volume(volume) * VP_TRAVEL / 100,
        y + (VP_SLIDER_HEIGHT - VP_THUMB_HEIGHT) / 2,
        VP_THUMB_WIDTH,
        VP_THUMB_HEIGHT);
}

/* The volume for a thumb whose left edge is at x. */
static int vp_volume_at(int x)
{
    return vp_clamp_volume(((x - VP_SLIDER_X) * 100 + VP_TRAVEL / 2) / VP_TRAVEL);
}

static struct sx_rect vp_mute_rect(void)
{
    return sx_rect_make(VP_PAD - 2, VP_DEVICE_Y - 2, 22, 22);
}

static struct sx_rect vp_link_rect(void)
{
    int width = gfx_text_width(VP_LINK_TEXT);
    return sx_rect_make(VP_WIDTH - VP_PAD - width - 2, VP_LINK_Y - 2, width + 4, gfx_text_height() + 4);
}

static int vp_app_slider_y(int row)
{
    return VP_APP_Y(row) + 16;
}

/* ---- state ------------------------------------------------------------------ */

static long g_audio_fd = -1;
static int g_volume = 100;
static int g_muted = 0;
static char g_device[96];

static struct sxnaudio_control g_control = {-1, -1};
static struct savanxp_audiod_snapshot g_snapshot;
static int g_census_valid = 0;
static uint32_t g_sequence = 0;
static uint32_t g_pending = 0;
static uint64_t g_query_ms = 0;
static uint64_t g_next_query_ms = 0;

static int g_drag = VP_DRAG_NONE;
static uint32_t g_drag_id = 0;
static int g_drag_offset = 0;
static int g_mute_armed = 0;
static int g_hot = 0; /* 1 mute glyph, 2 link */
static uint32_t g_last_buttons = 0;

/* Rows shown: the census up to VP_APP_ROWS, the last one becoming "+N more"
 * when it does not fit. */
static int vp_rows_shown(void)
{
    int count = g_census_valid ? (int)g_snapshot.count : 0;
    return count > VP_APP_ROWS ? VP_APP_ROWS - 1 : count;
}

static struct savanxp_audiod_stream_state *vp_stream(uint32_t id)
{
    uint32_t index;
    for (index = 0; index < g_snapshot.count; ++index)
    {
        if (g_snapshot.streams[index].id == id)
        {
            return &g_snapshot.streams[index];
        }
    }
    return 0;
}

/* ---- painting --------------------------------------------------------------- */

static void vp_rule(struct sx_painter *painter, int y)
{
    sx_painter_hline(painter, VP_PAD, y, VP_WIDTH - VP_PAD * 2, SXGUI_COLOR_SHADOW);
    sx_painter_hline(painter, VP_PAD, y + 1, VP_WIDTH - VP_PAD * 2, SXGUI_COLOR_LIGHT);
}

/* 16x16 speaker; crossed out in red when muted. */
static void vp_speaker(struct sx_painter *painter, int x, int y, int muted)
{
    const struct sx_point cone[] = {{x + 5, y + 5}, {x + 10, y + 1}, {x + 10, y + 14}, {x + 5, y + 10}};
    sx_painter_fill_rect(painter, sx_rect_make(x + 1, y + 5, 5, 6), SXGUI_COLOR_BEVEL);
    sx_painter_fill_polygon(painter, cone, 4, SXGUI_COLOR_BEVEL);
    sx_painter_draw_polyline(painter, cone, 4, 1, SXGUI_COLOR_DARK);
    sx_painter_draw_frame(painter, sx_rect_make(x + 1, y + 5, 5, 6), SXGUI_COLOR_DARK);
    if (muted)
    {
        const uint32_t red = SXGUI_RGB(192, 0, 0);
        sx_painter_draw_line(painter, x + 11, y + 5, x + 15, y + 10, red);
        sx_painter_draw_line(painter, x + 12, y + 5, x + 16, y + 10, red);
        sx_painter_draw_line(painter, x + 15, y + 5, x + 11, y + 10, red);
        sx_painter_draw_line(painter, x + 16, y + 5, x + 12, y + 10, red);
        return;
    }
    if (sx_painter_push_clip(painter, sx_rect_make(x + 11, y, 6, 16)))
    {
        sx_painter_draw_ellipse(painter, sx_rect_make(x + 6, y + 4, 8, 8), SXGUI_COLOR_SELECT);
        sx_painter_draw_ellipse(painter, sx_rect_make(x + 4, y + 1, 12, 14), SXGUI_COLOR_SELECT);
        sx_painter_pop_clip(painter);
    }
}

/* The application's own 16x16 icon: the one its window shows in the
 * taskbar, found by the pid audiod reports for the stream. A stream without
 * a window (a console program, anonymous UDP) gets the generic icon. */
static void vp_app_icon(struct sx_painter *painter, int x, int y, uint32_t pid)
{
    struct desktop_embedded_bitmap storage;
    const struct desktop_embedded_bitmap *icon = desktop_taskbar_process_icon(pid, &storage);
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;

    if (icon == 0)
    {
        icon = desktop_icon_small(DESKTOP_ICON_DESKTOP);
    }
    if (icon == 0 || icon->pixels == 0)
    {
        return;
    }
    memset(&info, 0, sizeof(info));
    info.width = icon->width;
    info.height = icon->height;
    info.pitch = icon->width * 4u;
    info.bpp = 32;
    info.buffer_size = info.pitch * icon->height;
    sx_bitmap_wrap(&bitmap, (uint32_t *)icon->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, x, y);
}

static void vp_slider(struct sx_painter *painter, int y, int volume, int pressed, int enabled)
{
    struct sx_rect track = sx_rect_make(
        VP_SLIDER_X, y + (VP_SLIDER_HEIGHT - VP_TRACK_HEIGHT) / 2, VP_SLIDER_WIDTH, VP_TRACK_HEIGHT);
    struct sx_rect thumb = vp_thumb_rect(y, volume);
    char percent[8];

    sx_painter_fill_rect(painter, track, SXGUI_COLOR_FIELD);
    desktop_bevel(painter, track, 1);
    sx_painter_fill_rect(painter, thumb, SXGUI_COLOR_FACE);
    desktop_bevel(painter, thumb, pressed);
    (void)snprintf(percent, sizeof(percent), "%d%%", vp_clamp_volume(volume));
    if (enabled)
    {
        sx_painter_draw_text(
            painter, VP_WIDTH - VP_PAD - gfx_text_width(percent),
            y + (VP_SLIDER_HEIGHT - gfx_text_height()) / 2, percent, SXGUI_COLOR_TEXT);
    }
    else
    {
        sx_painter_draw_text(
            painter, VP_WIDTH - VP_PAD - gfx_text_width(percent),
            y + (VP_SLIDER_HEIGHT - gfx_text_height()) / 2, percent, SXGUI_COLOR_DISABLED_TEXT);
    }
}

/* Cuts the text to max_width without an ellipsis: the painter does not clip,
 * and a long name would spill over the percentage. */
static void vp_fit(char *out, size_t capacity, const char *text, int max_width)
{
    size_t length = 0;

    if (capacity == 0)
    {
        return;
    }
    out[0] = '\0';
    while (text[length] != '\0' && length + 1u < capacity)
    {
        out[length] = text[length];
        out[length + 1u] = '\0';
        if (gfx_text_width(out) > max_width && length > 0)
        {
            out[length] = '\0';
            break;
        }
        ++length;
    }
}

static void volume_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    struct sx_rect link = vp_link_rect();
    char text[64];
    int font;
    int row;
    int shown = vp_rows_shown();

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    desktop_bevel(&painter, frame, 0);

    font = sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
    sx_painter_draw_text(&painter, VP_PAD, VP_TITLE_Y, "Volume", SXGUI_COLOR_TEXT);
    (void)sx_painter_set_font(&painter, font);
    vp_speaker(&painter, VP_WIDTH - VP_PAD - 16, VP_TITLE_Y + 2, g_muted);
    vp_rule(&painter, VP_RULE1_Y);

    /* Output device: the glyph is the mute toggle, raised while hovered. */
    if (g_hot == 1)
    {
        desktop_bevel(&painter, vp_mute_rect(), g_mute_armed);
    }
    vp_speaker(&painter, VP_PAD + 1, VP_DEVICE_Y + 1, g_muted);
    sx_painter_draw_text(&painter, VP_TEXT_X, VP_DEVICE_Y, "Output device", SXGUI_COLOR_TEXT);
    vp_fit(text, sizeof(text), g_device, VP_WIDTH - VP_TEXT_X - VP_PAD);
    sx_painter_draw_text(&painter, VP_TEXT_X, VP_DEVICE_Y + 16, text, SXGUI_COLOR_SHADOW);
    vp_slider(&painter, VP_MASTER_Y, g_volume, g_drag == VP_DRAG_MASTER, !g_muted);
    vp_rule(&painter, VP_RULE2_Y);

    sx_painter_draw_text(&painter, VP_PAD, VP_APPS_TITLE_Y, "Applications", SXGUI_COLOR_TEXT);
    if (!g_census_valid || g_snapshot.count == 0)
    {
        sx_painter_draw_text(
            &painter, VP_PAD, VP_APP_Y(0) + 4,
            g_census_valid ? "No application is playing sound." : "Audio service unavailable.",
            SXGUI_COLOR_DISABLED_TEXT);
    }
    for (row = 0; row < shown; ++row)
    {
        const struct savanxp_audiod_stream_state *stream = &g_snapshot.streams[row];
        const char *name = stream->name[0] != '\0' ? stream->name : "Unnamed application";

        vp_app_icon(&painter, VP_PAD, VP_APP_Y(row) - 2, stream->pid);
        vp_fit(text, sizeof(text), name, VP_SLIDER_WIDTH - (VP_TEXT_X - VP_PAD));
        sx_painter_draw_text(&painter, VP_TEXT_X, VP_APP_Y(row), text, SXGUI_COLOR_TEXT);
        if (stream->muted)
        {
            sx_painter_draw_text(
                &painter, VP_WIDTH - VP_PAD - gfx_text_width("Muted"), VP_APP_Y(row), "Muted",
                SXGUI_COLOR_DISABLED_TEXT);
        }
        vp_slider(&painter, vp_app_slider_y(row), stream->volume,
                  g_drag == row && g_drag_id == stream->id, !stream->muted);
    }
    if (g_census_valid && (int)g_snapshot.count > shown && shown == VP_APP_ROWS - 1)
    {
        (void)snprintf(text, sizeof(text), "+%u more in Sound settings",
                       (unsigned)(g_snapshot.count - (uint32_t)shown));
        sx_painter_draw_text(&painter, VP_PAD, VP_APP_Y(shown) + 4, text, SXGUI_COLOR_TEXT);
    }
    vp_rule(&painter, VP_RULE3_Y);

    sx_painter_draw_text(&painter, link.x + 2, link.y + 2, VP_LINK_TEXT, VP_LINK_COLOUR);
    sx_painter_hline(&painter, link.x + 2, link.y + 2 + gfx_text_height(), link.width - 4, VP_LINK_COLOUR);
}

/* ---- master volume ------------------------------------------------------------ */

/* Saves the pair the kernel already holds: the ioctl is the source of truth,
 * the file only remembers it for the next boot. Same as `volume`. */
static void vp_persist(void)
{
    char text[16];
    char path[256];
    long config_fd;

    (void)snprintf(text, sizeof(text), "%d %d\n", g_volume, g_muted);
    if (savanxp_settings_path(VP_CONFIG_NAME, 1, path, sizeof(path)) < 0)
    {
        return;
    }
    config_fd = savanxp_open_mode(path, SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (config_fd < 0)
    {
        return;
    }
    (void)savanxp_write((int)config_fd, text, strlen(text));
    savanxp_close((int)config_fd);
    (void)savanxp_sync();
}

static uint32_t vp_set_master(int volume)
{
    volume = vp_clamp_volume(volume);
    if (volume == g_volume)
    {
        return DESKTOP_ACTION_NONE;
    }
    g_volume = volume;
    (void)audio_set_volume((int)g_audio_fd, g_volume);
    return DESKTOP_ACTION_TASKBAR_CHANGED | DESKTOP_ACTION_REPAINT;
}

/* The output device's name: the selected playback connector, as Sound shows
 * it on its Hardware tab. */
static void vp_read_device(void)
{
    uint32_t index;

    (void)snprintf(g_device, sizeof(g_device), "Default device");
    for (index = 0; index < SAVANXP_AUDIO_MAX_ENDPOINTS; ++index)
    {
        struct savanxp_audio_endpoint_query query;
        const struct savanxp_audio_endpoint *endpoint = &query.endpoint;

        memset(&query, 0, sizeof(query));
        query.index = index;
        if (savanxp_ioctl((int)g_audio_fd, AUDIO_IOC_GET_ENDPOINT, (unsigned long)(uintptr_t)&query) < 0)
        {
            break;
        }
        query.endpoint.name[sizeof(query.endpoint.name) - 1] = '\0';
        query.endpoint.device_name[sizeof(query.endpoint.device_name) - 1] = '\0';
        if ((endpoint->flags & SAVANXP_AUDIO_PLAYBACK) && (endpoint->flags & SAVANXP_AUDIO_SELECTED))
        {
            (void)snprintf(g_device, sizeof(g_device), "%s (%s)", endpoint->name, endpoint->device_name);
            return;
        }
    }
}

/* ---- application census -------------------------------------------------------- */

static void vp_control_drop(uint64_t now_ms)
{
    sxnaudio_control_close(&g_control);
    g_pending = 0;
    g_census_valid = 0;
    g_next_query_ms = now_ms + VP_RETRY_MS;
    if (g_drag >= 0)
    {
        g_drag = VP_DRAG_NONE;
    }
}

static void vp_app_set(struct savanxp_audiod_stream_state *stream, int volume)
{
    volume = vp_clamp_volume(volume);
    if (g_control.request < 0 || volume == (int)stream->volume)
    {
        return;
    }
    if (sxnaudio_control_change(&g_control, stream, SAVANXP_AUDIOD_STREAM_VOLUME, (uint8_t)volume) == 0)
    {
        stream->volume = (uint8_t)volume;
        /* A reply to an earlier query may predate this change: discard it so
         * the slider does not jump back, and ask again after the command. */
        g_pending = 0;
        g_next_query_ms = 0;
    }
}

static int volume_open(void)
{
    long status;

    g_drag = VP_DRAG_NONE;
    g_drag_id = 0;
    g_mute_armed = 0;
    g_hot = 0;
    g_last_buttons = 0;
    g_census_valid = 0;
    g_pending = 0;
    g_next_query_ms = 0;
    memset(&g_snapshot, 0, sizeof(g_snapshot));

    /* Only for the ioctls: nothing is ever read or written through this fd. */
    g_audio_fd = (long)audio_open();
    if (g_audio_fd < 0)
    {
        puts_fd(2, "desktop: /dev/audio0 unavailable\n");
        return -1;
    }
    status = audio_get_volume((int)g_audio_fd);
    if (status < 0 || status > 100)
    {
        puts_fd(2, "desktop: AUDIO_IOC_GET_VOLUME failed\n");
        savanxp_close((int)g_audio_fd);
        g_audio_fd = -1;
        return -1;
    }
    g_volume = (int)status;
    status = audio_get_muted((int)g_audio_fd);
    if (status != 0 && status != 1)
    {
        puts_fd(2, "desktop: AUDIO_IOC_GET_MUTED failed\n");
        savanxp_close((int)g_audio_fd);
        g_audio_fd = -1;
        return -1;
    }
    g_muted = (int)status;
    vp_read_device();

    /* Without audiod the popup still opens: master and mute are kernel
     * ioctls. The tick retries the connection. */
    if (sxnaudio_control_open(&g_control) < 0)
    {
        g_control.request = g_control.reply = -1;
        g_next_query_ms = (uint64_t)uptime_ms() + VP_RETRY_MS;
    }
    return 0;
}

static void volume_close(void)
{
    sxnaudio_control_close(&g_control);
    if (g_audio_fd >= 0)
    {
        savanxp_close((int)g_audio_fd);
        g_audio_fd = -1;
    }
}

static uint32_t volume_tick(void)
{
    uint64_t now_ms = (uint64_t)uptime_ms();
    uint32_t actions = DESKTOP_ACTION_NONE;
    struct savanxp_audiod_snapshot snapshot;
    long result;

    if (g_control.request < 0)
    {
        if (now_ms >= g_next_query_ms && sxnaudio_control_open(&g_control) < 0)
        {
            g_control.request = g_control.reply = -1;
            g_next_query_ms = now_ms + VP_RETRY_MS;
        }
        return actions;
    }
    while ((result = sxnaudio_control_receive_full(&g_control, &snapshot, 0, 0, 0)) > 0)
    {
        if (result != 1 || !g_pending || snapshot.request_id != g_pending)
        {
            continue;
        }
        g_pending = 0;
        g_snapshot = snapshot;
        g_census_valid = 1;
        g_next_query_ms = now_ms + VP_POLL_MS;
        /* The dragged row follows its stream id; a stream that ended drops
         * the drag. */
        if (g_drag >= 0)
        {
            uint32_t index;
            g_drag = VP_DRAG_NONE;
            for (index = 0; index < g_snapshot.count && (int)index < vp_rows_shown(); ++index)
            {
                if (g_snapshot.streams[index].id == g_drag_id)
                {
                    g_drag = (int)index;
                }
            }
        }
        actions |= DESKTOP_ACTION_REPAINT;
    }
    if (result < 0 || (g_pending && now_ms - g_query_ms >= VP_QUERY_TIMEOUT_MS))
    {
        vp_control_drop(now_ms);
        return actions | DESKTOP_ACTION_REPAINT;
    }
    if (!g_pending && now_ms >= g_next_query_ms)
    {
        ++g_sequence;
        if (g_sequence == 0)
        {
            ++g_sequence;
        }
        result = sxnaudio_control_query(&g_control, g_sequence);
        if (result == 0)
        {
            g_pending = g_sequence;
            g_query_ms = now_ms;
        }
        else if (result != -SAVANXP_EAGAIN)
        {
            vp_control_drop(now_ms);
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    return actions;
}

/* ---- pointer ---------------------------------------------------------------------- */

static uint32_t volume_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    uint32_t actions = DESKTOP_ACTION_NONE;
    int x = pointer->x;
    int y = pointer->y;
    int hot = vp_in_rect(vp_mute_rect(), x, y) ? 1 : vp_in_rect(vp_link_rect(), x, y) ? 2 : 0;
    int over_row = -1;
    int row;

    (void)gfx;
    for (row = 0; row < vp_rows_shown(); ++row)
    {
        if (y >= VP_APP_Y(row) - 2 && y < VP_APP_Y(row) + 38 && x >= 0 && x < VP_WIDTH)
        {
            over_row = row;
        }
    }

    /* Reconcile with the real button state, only when the event carries no
     * edge: if the button is released outside the popup, another window gets
     * the up, and a drag or the armed mute would hang until the next click. */
    if (((down | up) & SAVANXP_MOUSE_BUTTON_LEFT) == 0 && (pointer->buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0)
    {
        if (g_drag == VP_DRAG_MASTER)
        {
            vp_persist();
        }
        if (g_drag != VP_DRAG_NONE || g_mute_armed)
        {
            actions |= DESKTOP_ACTION_REPAINT;
        }
        g_drag = VP_DRAG_NONE;
        g_mute_armed = 0;
    }
    if (hot != g_hot)
    {
        g_hot = hot;
        actions |= DESKTOP_ACTION_REPAINT;
    }

    if (pointer->wheel != 0)
    {
        if (over_row >= 0)
        {
            struct savanxp_audiod_stream_state *stream = &g_snapshot.streams[over_row];
            vp_app_set(stream, (int)stream->volume + pointer->wheel * VP_WHEEL_STEP);
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else
        {
            uint32_t changed = vp_set_master(g_volume + pointer->wheel * VP_WHEEL_STEP);
            if (changed)
            {
                vp_persist();
            }
            actions |= changed;
        }
    }

    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        struct sx_rect master = vp_slider_rect(VP_MASTER_Y);
        if (hot == 2)
        {
            if (desktop_launch("/system/sound.sxe", SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) >= 0)
            {
                return DESKTOP_ACTION_CLOSE;
            }
        }
        else if (hot == 1)
        {
            g_mute_armed = 1;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        else if (vp_in_rect(master, x, y))
        {
            struct sx_rect thumb = vp_thumb_rect(VP_MASTER_Y, g_volume);
            /* Grab the thumb where it was pressed instead of re-centring it. */
            g_drag = VP_DRAG_MASTER;
            g_drag_offset = vp_in_rect(thumb, x, y) ? x - thumb.x : VP_THUMB_WIDTH / 2;
            actions |= vp_set_master(vp_volume_at(x - g_drag_offset)) | DESKTOP_ACTION_REPAINT;
        }
        else if (over_row >= 0 && vp_in_rect(vp_slider_rect(vp_app_slider_y(over_row)), x, y))
        {
            struct savanxp_audiod_stream_state *stream = &g_snapshot.streams[over_row];
            struct sx_rect thumb = vp_thumb_rect(vp_app_slider_y(over_row), stream->volume);
            g_drag = over_row;
            g_drag_id = stream->id;
            g_drag_offset = vp_in_rect(thumb, x, y) ? x - thumb.x : VP_THUMB_WIDTH / 2;
            vp_app_set(stream, vp_volume_at(x - g_drag_offset));
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if ((up & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        if (g_mute_armed)
        {
            if (hot == 1)
            {
                g_muted = !g_muted;
                (void)audio_set_muted((int)g_audio_fd, g_muted);
                vp_persist();
                actions |= DESKTOP_ACTION_TASKBAR_CHANGED;
            }
            g_mute_armed = 0;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        if (g_drag == VP_DRAG_MASTER)
        {
            vp_persist();
        }
        if (g_drag != VP_DRAG_NONE)
        {
            g_drag = VP_DRAG_NONE;
            actions |= DESKTOP_ACTION_REPAINT;
        }
    }
    else if (g_drag == VP_DRAG_MASTER)
    {
        actions |= vp_set_master(vp_volume_at(x - g_drag_offset));
    }
    else if (g_drag >= 0)
    {
        struct savanxp_audiod_stream_state *stream = vp_stream(g_drag_id);
        if (stream == 0)
        {
            g_drag = VP_DRAG_NONE;
        }
        else
        {
            vp_app_set(stream, vp_volume_at(x - g_drag_offset));
        }
        actions |= DESKTOP_ACTION_REPAINT;
    }
    g_last_buttons = pointer->buttons;
    return actions;
}

const struct desktop_popup_ops desktop_volume_ops = {
    VP_WIDTH,
    VP_HEIGHT,
    1,
    volume_open,
    volume_close,
    volume_pointer,
    volume_tick,
    volume_paint,
    0,
    0,
    0,
};
