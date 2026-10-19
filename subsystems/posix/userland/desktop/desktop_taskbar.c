#include "desktop.h"
#include "savanxp/wm_shell_protocol.h"

#include "desktop_icons.h"
#include "start_logo.h"

/*
 * The taskbar: the desktop's panel window, not windowd chrome.
 *
 * The explorer.exe model: windowd manages windows, the shell shows them. It
 * lists the open windows and carries the Start button, which opens the Start
 * menu (desktop_startmenu.c) with progman's catalog as a flat list. No
 * notification area: the clock in the corner is a button that opens the
 * calendar (desktop_clock.c), and the speaker and layout buttons open their own
 * popups (desktop_volume.c, desktop_keyboard.c).
 *
 * What it cannot know on its own -- which windows exist, which is active, which
 * is minimized -- windowd tells it through the shared section on
 * SAVANXP_WM_FD_WINDOW_LIST (savanxp/wm_shell_protocol.h), and clicks go back as
 * requests on SAVANXP_WM_FD_SHELL_REQUEST. The taskbar NEVER activates a window
 * by itself: it asks, and windowd decides.
 */

#define TASKBAR_BUTTON_MAX_WIDTH 160
#define TASKBAR_BUTTON_MIN_WIDTH 40
#define TASKBAR_BUTTON_GAP 2
#define TASKBAR_MARGIN 2
#define TASKBAR_ICON_SIZE 16
#define TASKBAR_LAYOUT_WIDTH 32
#define TASKBAR_VOLUME_WIDTH 32
#define TASKBAR_CLOCK_WIDTH 64
#define TASKBAR_START_WIDTH 64
#define TASKBAR_HIT_START (-3)
#define TASKBAR_HIT_LAYOUT (-2)
#define TASKBAR_HIT_VOLUME (-4)
#define TASKBAR_HIT_CLOCK (-5)

/* The indicators (layout, speaker) are read through ioctls on fds opened once
 * and never read or written. The desktop's own popups ask for a refresh as
 * soon as they change something (DESKTOP_ACTION_TASKBAR_CHANGED); the polling
 * every TASKBAR_LAYOUT_POLL_FRAMES passes catches changes made elsewhere, such
 * as the `volume` command. */
#define TASKBAR_LAYOUT_POLL_FRAMES 30
static const char *k_layout_labels[2] = {"ES", "EN"};

/* /dev/audio0, for the read ioctls only. */
static long g_audio_fd = -1;
static int g_audio_muted = 0;
static int g_audio_volume = 100;

/* El boton del altavoz usa el mismo arte que el reproductor (nombres de la
 * Icon Naming Specification): el generador lo empaqueta en start_logo.h y
 * asi no hay dos dibujos que diverjan. Los tercios son los del reproductor
 * (mediaplayer.c): 0..33 bajo, 34..66 medio, 67..100 alto. */
static const struct savanxp_embedded_bitmap_asset *taskbar_volume_glyph(void)
{
    if (g_audio_muted || g_audio_volume <= 0)
    {
        return &k_volume_muted;
    }
    if (g_audio_volume < 34)
    {
        return &k_volume_low;
    }
    if (g_audio_volume < 67)
    {
        return &k_volume_medium;
    }
    return &k_volume_high;
}

static struct savanxp_wm_window_list *g_list;
/* Copia estable para pintar: la seccion la escribe el WM en cualquier momento,
 * asi que se saca una foto consistente y se dibuja de ahi. */
static struct savanxp_wm_window_list g_snapshot;
static int g_pressed_index = -1;
static int g_active_layout = SAVANXP_KEYBOARD_LAYOUT_ES;
static long g_input_fd = -1;

/* W2K-style clock: HH:MM from the RTC (UTC, which is what the RTC gives, see
 * time.h), falling back to uptime when there is no valid RTC. g_clock_stamp
 * is the minute of the day the text shows; -1 forces the first paint. It is
 * refreshed in the same polling pass as the layout and the volume: the RTC
 * has a resolution of one second and the text changes once a minute. */
static char g_clock_text[6] = "00:00";
static long g_clock_stamp = -1;

static void taskbar_clock_format(char *buffer, unsigned int hours, unsigned int minutes)
{
    buffer[0] = (char)('0' + (hours / 10u));
    buffer[1] = (char)('0' + (hours % 10u));
    buffer[2] = ':';
    buffer[3] = (char)('0' + (minutes / 10u));
    buffer[4] = (char)('0' + (minutes % 10u));
    buffer[5] = '\0';
}

/* Relee el reloj y devuelve distinto de cero si el texto cambio. */
static int taskbar_clock_refresh(void)
{
    struct savanxp_realtime now;
    unsigned long stamp;
    char text[6];

    memset(&now, 0, sizeof(now));
    if (realtime(&now) == 0 && now.valid != 0)
    {
        stamp = ((unsigned long)now.hour * 60UL) + (unsigned long)now.minute;
        taskbar_clock_format(text, (unsigned int)now.hour, (unsigned int)now.minute);
    }
    else
    {
        unsigned long total_minutes = uptime_ms() / 60000UL;
        unsigned int hours = (unsigned int)((total_minutes / 60UL) % 24UL);
        unsigned int minutes = (unsigned int)(total_minutes % 60UL);

        stamp = total_minutes % (24UL * 60UL);
        taskbar_clock_format(text, hours, minutes);
    }
    if ((long)stamp != g_clock_stamp ||
        text[0] != g_clock_text[0] || text[1] != g_clock_text[1] ||
        text[2] != g_clock_text[2] || text[3] != g_clock_text[3] ||
        text[4] != g_clock_text[4])
    {
        g_clock_stamp = (long)stamp;
        g_clock_text[0] = text[0];
        g_clock_text[1] = text[1];
        g_clock_text[2] = text[2];
        g_clock_text[3] = text[3];
        g_clock_text[4] = text[4];
        g_clock_text[5] = '\0';
        return 1;
    }
    return 0;
}

/*
 * Lectura con seqlock: si la secuencia es impar el WM esta escribiendo, y si
 * cambio entre el antes y el despues la copia salio mezclada. En los dos casos
 * se reintenta. Sin esto, un repintado podria mostrar media lista vieja y media
 * nueva -- por ejemplo un titulo de una ventana con el flag de otra.
 */
static int taskbar_snapshot(void)
{
    int attempt;

    if (g_list == 0)
    {
        return 0;
    }
    for (attempt = 0; attempt < 8; ++attempt)
    {
        uint64_t before = g_list->sequence;
        uint64_t after;

        if ((before & 1u) != 0)
        {
            continue;
        }
        memcpy(&g_snapshot, g_list, sizeof(g_snapshot));
        after = g_list->sequence;
        if (before == after)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * Icono del boton: el propio de la ventana si el WM lo mando, y el generico si
 * no. `storage` lo aporta el llamador para describir los pixeles que viven en
 * la entrada del snapshot sin copiarlos de nuevo -- es el mismo reparto que
 * windowd_presentation_icon() del lado del WM.
 *
 * El generico deja de ser lo unico que se ve: hasta que la entrada transporto
 * los pixeles, TODA ventana caia aca porque icon_id ya no distingue apps (el
 * set horneado se redujo al generico cuando cada binario consiguio su .sxicon,
 * ver docs/userland/SXE_FORMAT.md).
 */
static const struct desktop_embedded_bitmap *taskbar_entry_icon(
    const struct savanxp_wm_window_entry *entry,
    struct desktop_embedded_bitmap *storage)
{
    /* El lado se valida contra el tope y no solo contra 0: los pixeles son un
     * arreglo fijo, y un extent mayor haria que el blit leyera fuera de la
     * entrada. */
    if (entry != 0 && storage != 0 &&
        entry->icon_extent != 0u && entry->icon_extent <= SAVANXP_WM_WINDOW_ICON_EXTENT)
    {
        storage->width = entry->icon_extent;
        storage->height = entry->icon_extent;
        storage->pixels = entry->icon_pixels;
        return storage;
    }
    return desktop_icon_small((enum desktop_icon_id)(entry != 0 ? entry->icon_id : 0u));
}

static int taskbar_button_width(const struct savanxp_fb_info *info, int count)
{
    int usable;
    int width;

    if (count <= 0)
    {
        return 0;
    }
    usable = (int)info->width - (TASKBAR_MARGIN * 2) - (TASKBAR_BUTTON_GAP * (count - 1))
        - TASKBAR_CLOCK_WIDTH - TASKBAR_BUTTON_GAP
        - TASKBAR_LAYOUT_WIDTH - TASKBAR_BUTTON_GAP
        - TASKBAR_VOLUME_WIDTH - TASKBAR_BUTTON_GAP
        - TASKBAR_START_WIDTH - TASKBAR_BUTTON_GAP;
    width = usable / count;
    if (width > TASKBAR_BUTTON_MAX_WIDTH)
    {
        width = TASKBAR_BUTTON_MAX_WIDTH;
    }
    return width;
}

static struct sx_rect taskbar_button_rect(const struct savanxp_fb_info *info, int count, int index)
{
    int width = taskbar_button_width(info, count);
    int height = (int)info->height - (TASKBAR_MARGIN * 2);

    return sx_rect_make(
        TASKBAR_MARGIN + TASKBAR_START_WIDTH + TASKBAR_BUTTON_GAP + index * (width + TASKBAR_BUTTON_GAP),
        TASKBAR_MARGIN,
        width,
        height);
}

static struct sx_rect taskbar_start_rect(const struct savanxp_fb_info *info)
{
    int height = (int)info->height - (TASKBAR_MARGIN * 2);

    return sx_rect_make(
        TASKBAR_MARGIN,
        TASKBAR_MARGIN,
        TASKBAR_START_WIDTH,
        height);
}

static struct sx_rect taskbar_clock_rect(const struct savanxp_fb_info *info)
{
    int height = (int)info->height - (TASKBAR_MARGIN * 2);

    return sx_rect_make(
        (int)info->width - TASKBAR_MARGIN - TASKBAR_CLOCK_WIDTH,
        TASKBAR_MARGIN,
        TASKBAR_CLOCK_WIDTH,
        height);
}

static struct sx_rect taskbar_layout_rect(const struct savanxp_fb_info *info)
{
    int height = (int)info->height - (TASKBAR_MARGIN * 2);

    return sx_rect_make(
        (int)info->width - TASKBAR_MARGIN - TASKBAR_CLOCK_WIDTH - TASKBAR_BUTTON_GAP - TASKBAR_LAYOUT_WIDTH,
        TASKBAR_MARGIN,
        TASKBAR_LAYOUT_WIDTH,
        height);
}

static struct sx_rect taskbar_volume_rect(const struct savanxp_fb_info *info)
{
    int height = (int)info->height - (TASKBAR_MARGIN * 2);

    return sx_rect_make(
        (int)info->width - TASKBAR_MARGIN - TASKBAR_CLOCK_WIDTH - TASKBAR_BUTTON_GAP - TASKBAR_LAYOUT_WIDTH - TASKBAR_BUTTON_GAP - TASKBAR_VOLUME_WIDTH,
        TASKBAR_MARGIN,
        TASKBAR_VOLUME_WIDTH,
        height);
}

static void taskbar_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    int count = (int)g_snapshot.count;
    int index;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    sx_painter_fill_rect(&painter, sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height), SXGUI_COLOR_FACE);
    /* Filo claro arriba: la barra se lee como una superficie levantada sobre el
     * escritorio, igual que en Win95. */
    sx_painter_fill_rect(&painter, sx_rect_make(0, 0, (int)gfx->info.width, 1), SXGUI_COLOR_LIGHT);

    {
        /* Start button, Windows style: logo on the left and text, sunken
         * while it is pressed and while the Start menu is open. */
        struct sx_rect start_rect = taskbar_start_rect(&gfx->info);
        struct savanxp_fb_info logo_info;
        struct sx_bitmap logo_bitmap;
        const char *start_label = "Start";
        int sunken = (g_pressed_index == TASKBAR_HIT_START) || desktop_popup_is_open(DESKTOP_POPUP_START_MENU);
        int logo_x = start_rect.x + 4 + (sunken ? 1 : 0);
        int logo_y = start_rect.y + (start_rect.height - (int)k_start_logo.height) / 2 + (sunken ? 1 : 0);
        int start_text_x = logo_x + (int)k_start_logo.width + 4;
        int start_text_y = start_rect.y + (start_rect.height - gfx_text_height()) / 2 + (sunken ? 1 : 0);

        sx_painter_fill_rect(&painter, start_rect, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, start_rect, sunken);

        logo_info.width = k_start_logo.width;
        logo_info.height = k_start_logo.height;
        logo_info.pitch = k_start_logo.width * 4u;
        logo_info.bpp = 32;
        logo_info.buffer_size = logo_info.pitch * k_start_logo.height;
        sx_bitmap_wrap(&logo_bitmap, (uint32_t *)k_start_logo.pixels, &logo_info, SX_PIXEL_FORMAT_BGRA8888);
        sx_painter_blit_bitmap(&painter, &logo_bitmap, logo_x, logo_y);
        sx_painter_draw_text(&painter, start_text_x, start_text_y, start_label, SXGUI_COLOR_TEXT);
    }

    for (index = 0; index < count && index < (int)SAVANXP_WM_MAX_WINDOWS; ++index)
    {
        const struct savanxp_wm_window_entry *entry = &g_snapshot.windows[index];
        struct sx_rect rect = taskbar_button_rect(&gfx->info, count, index);
        int active = (entry->flags & SAVANXP_WM_WINDOW_FLAG_ACTIVE) != 0;
        int sunken = active || index == g_pressed_index;
        int text_x = rect.x + 6;
        const struct desktop_embedded_bitmap *icon;
        struct desktop_embedded_bitmap own_icon;

        if (rect.width < TASKBAR_BUTTON_MIN_WIDTH)
        {
            break;
        }

        sx_painter_fill_rect(&painter, rect, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, rect, sunken);

        icon = taskbar_entry_icon(entry, &own_icon);
        if (icon != 0 && rect.width > TASKBAR_ICON_SIZE + 24)
        {
            struct savanxp_fb_info icon_info;
            struct sx_bitmap icon_bitmap;

            icon_info.width = icon->width;
            icon_info.height = icon->height;
            icon_info.pitch = icon->width * 4u;
            icon_info.bpp = 32;
            icon_info.buffer_size = icon_info.pitch * icon->height;
            sx_bitmap_wrap(&icon_bitmap, (uint32_t *)icon->pixels, &icon_info, SX_PIXEL_FORMAT_BGRA8888);
            sx_painter_blit_bitmap(
                &painter,
                &icon_bitmap,
                text_x,
                rect.y + (rect.height - (int)icon->height) / 2);
            text_x += TASKBAR_ICON_SIZE + 4;
        }

        {
            /* El texto se recorta al ancho del boton: con muchas ventanas los
             * botones se angostan y un titulo largo se derramaria sobre el de
             * al lado. */
            struct sx_rect clip = sx_rect_make(
                text_x,
                rect.y,
                rect.x + rect.width - 4 - text_x,
                rect.height);
            if (sx_painter_push_clip(&painter, clip))
            {
                int text_y = rect.y + (rect.height - gfx_text_height()) / 2;
                /* Un boton hundido corre su contenido un pixel, como los de
                 * sxgui: es lo que da la sensacion de que se aprieta. */
                sx_painter_draw_text(
                    &painter,
                    text_x + (sunken ? 1 : 0),
                    text_y + (sunken ? 1 : 0),
                    entry->title,
                    SXGUI_COLOR_TEXT);
                sx_painter_pop_clip(&painter);
            }
        }
    }

    {
        struct sx_rect layout_rect = taskbar_layout_rect(&gfx->info);
        const char *label = k_layout_labels[g_active_layout];
        int text_x = layout_rect.x + (layout_rect.width - gfx_text_width(label)) / 2;
        int text_y = layout_rect.y + (layout_rect.height - gfx_text_height()) / 2;

        sx_painter_fill_rect(&painter, layout_rect, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, layout_rect, g_pressed_index == TASKBAR_HIT_LAYOUT);
        sx_painter_draw_text(&painter, text_x, text_y, label, SXGUI_COLOR_TEXT);
    }

    {
        /* Boton del altavoz: a la izquierda del layout, mismo bisel. El
         * glifo cambia con el nivel y el mute (el popup aplica directo en
         * el kernel y la barra se entera por polling, igual que el
         * layout). */
        struct sx_rect volume_rect = taskbar_volume_rect(&gfx->info);
        const struct savanxp_embedded_bitmap_asset *glyph = taskbar_volume_glyph();
        int sunken = (g_pressed_index == TASKBAR_HIT_VOLUME);

        sx_painter_fill_rect(&painter, volume_rect, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, volume_rect, sunken);
        {
            int icon_x = volume_rect.x + (volume_rect.width - (int)glyph->width) / 2 + (sunken ? 1 : 0);
            int icon_y = volume_rect.y + (volume_rect.height - (int)glyph->height) / 2 + (sunken ? 1 : 0);
            struct savanxp_fb_info icon_info;
            struct sx_bitmap icon_bitmap;

            icon_info.width = glyph->width;
            icon_info.height = glyph->height;
            icon_info.pitch = glyph->width * 4u;
            icon_info.bpp = 32;
            icon_info.buffer_size = icon_info.pitch * glyph->height;
            sx_bitmap_wrap(
                &icon_bitmap,
                (uint32_t *)glyph->pixels,
                &icon_info,
                SX_PIXEL_FORMAT_BGRA8888);
            sx_painter_blit_bitmap(&painter, &icon_bitmap, icon_x, icon_y);
        }
    }

    {
        /* Reloj estilo W2K: pozo hundido en el borde derecho, texto centrado.
         * Es boton desde que tiene popup: click = toggle del calendario. */
        struct sx_rect clock_rect = taskbar_clock_rect(&gfx->info);
        int text_shift = (g_pressed_index == TASKBAR_HIT_CLOCK) ? 1 : 0;
        int text_x = clock_rect.x + (clock_rect.width - gfx_text_width(g_clock_text)) / 2 + text_shift;
        int text_y = clock_rect.y + (clock_rect.height - gfx_text_height()) / 2 + text_shift;

        sx_painter_fill_rect(&painter, clock_rect, SXGUI_COLOR_FACE);
        desktop_bevel(&painter, clock_rect, 1);
        sx_painter_draw_text(&painter, text_x, text_y, g_clock_text, SXGUI_COLOR_TEXT);
    }
}

static int taskbar_hit(const struct savanxp_fb_info *info, int x, int y)
{
    int count = (int)g_snapshot.count;
    int index;
    struct sx_rect clock_rect = taskbar_clock_rect(info);
    struct sx_rect layout_rect = taskbar_layout_rect(info);
    struct sx_rect volume_rect = taskbar_volume_rect(info);
    struct sx_rect start_rect = taskbar_start_rect(info);

    if (x >= start_rect.x && x < start_rect.x + start_rect.width &&
        y >= start_rect.y && y < start_rect.y + start_rect.height)
    {
        return TASKBAR_HIT_START;
    }

    if (x >= volume_rect.x && x < volume_rect.x + volume_rect.width &&
        y >= volume_rect.y && y < volume_rect.y + volume_rect.height)
    {
        return TASKBAR_HIT_VOLUME;
    }

    if (x >= clock_rect.x && x < clock_rect.x + clock_rect.width &&
        y >= clock_rect.y && y < clock_rect.y + clock_rect.height)
    {
        return TASKBAR_HIT_CLOCK;
    }

    if (x >= layout_rect.x && x < layout_rect.x + layout_rect.width &&
        y >= layout_rect.y && y < layout_rect.y + layout_rect.height)
    {
        return TASKBAR_HIT_LAYOUT;
    }

    for (index = 0; index < count && index < (int)SAVANXP_WM_MAX_WINDOWS; ++index)
    {
        struct sx_rect rect = taskbar_button_rect(info, count, index);
        if (rect.width < TASKBAR_BUTTON_MIN_WIDTH)
        {
            break;
        }
        if (x >= rect.x && x < rect.x + rect.width &&
            y >= rect.y && y < rect.y + rect.height)
        {
            return index;
        }
    }
    return -1;
}

static void taskbar_request(uint32_t action, uint32_t window_id)
{
    struct savanxp_wm_shell_request request;

    memset(&request, 0, sizeof(request));
    request.action = action;
    request.window_id = window_id;
    (void)savanxp_write(SAVANXP_WM_FD_SHELL_REQUEST, &request, sizeof(request));
}

/* Polling countdown for the indicators and the clock, in loop passes. */
static unsigned int g_indicator_countdown = 0;
static uint32_t g_last_buttons = 0;
static uint64_t g_last_sequence = 0;

int desktop_taskbar_init(void)
{
    g_list = (struct savanxp_wm_window_list *)map_view(
        SAVANXP_WM_FD_WINDOW_LIST, SAVANXP_SECTION_READ);
    if (g_list == 0 || result_is_error((long)g_list))
    {
        g_list = 0;
        puts_fd(2, "desktop: could not map the window list\n");
        return -1;
    }

    /* Only for the layout ioctl -- this fd is never read. */
    g_input_fd = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ);
    /* Only for the volume read ioctls -- nothing is ever written through it.
     * Without audio there is no speaker to show: the button stays as is. */
    g_audio_fd = (long)audio_open();
    (void)desktop_taskbar_refresh_indicators();
    (void)taskbar_clock_refresh();
    return 0;
}

void desktop_taskbar_shutdown(void)
{
    if (g_input_fd >= 0)
    {
        savanxp_close((int)g_input_fd);
        g_input_fd = -1;
    }
    if (g_audio_fd >= 0)
    {
        savanxp_close((int)g_audio_fd);
        g_audio_fd = -1;
    }
    if (g_list != 0)
    {
        (void)unmap_view(g_list);
        g_list = 0;
    }
}

int desktop_taskbar_refresh_indicators(void)
{
    int changed = 0;

    if (g_input_fd >= 0)
    {
        long layout = input_get_layout((int)g_input_fd);
        if ((layout == SAVANXP_KEYBOARD_LAYOUT_ES || layout == SAVANXP_KEYBOARD_LAYOUT_EN) &&
            (int)layout != g_active_layout)
        {
            g_active_layout = (int)layout;
            changed = 1;
        }
    }
    if (g_audio_fd >= 0)
    {
        long muted = audio_get_muted((int)g_audio_fd);
        long volume = audio_get_volume((int)g_audio_fd);
        if ((muted == 0 || muted == 1) && (int)muted != g_audio_muted)
        {
            g_audio_muted = (int)muted;
            changed = 1;
        }
        if (volume >= 0 && volume <= 100 && (int)volume != g_audio_volume)
        {
            g_audio_volume = (int)volume;
            changed = 1;
        }
    }
    return changed;
}

uint32_t desktop_taskbar_pointer(const struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    uint32_t down = pointer->buttons & ~g_last_buttons;
    uint32_t up = g_last_buttons & ~pointer->buttons;
    int index = taskbar_hit(&gfx->info, pointer->x, pointer->y);
    uint32_t actions = DESKTOP_ACTION_NONE;

    g_last_buttons = pointer->buttons;
    if ((down & SAVANXP_MOUSE_BUTTON_LEFT) != 0)
    {
        g_pressed_index = index;
        return DESKTOP_ACTION_REPAINT;
    }
    if ((up & SAVANXP_MOUSE_BUTTON_LEFT) == 0)
    {
        return DESKTOP_ACTION_NONE;
    }

    if (index == TASKBAR_HIT_START && index == g_pressed_index)
    {
        desktop_toggle_popup(DESKTOP_POPUP_START_MENU);
    }
    else if (index == TASKBAR_HIT_LAYOUT && index == g_pressed_index)
    {
        desktop_toggle_popup(DESKTOP_POPUP_KEYBOARD);
    }
    else if (index == TASKBAR_HIT_VOLUME && index == g_pressed_index)
    {
        desktop_toggle_popup(DESKTOP_POPUP_VOLUME);
    }
    else if (index == TASKBAR_HIT_CLOCK && index == g_pressed_index)
    {
        desktop_toggle_popup(DESKTOP_POPUP_CLOCK);
    }
    else if (index >= 0 && index == g_pressed_index && index < (int)g_snapshot.count)
    {
        const struct savanxp_wm_window_entry *entry = &g_snapshot.windows[index];
        /* A click on the window that is ALREADY active minimizes it, as in
         * Win95: the button is a toggle, not just "bring this to me". */
        int active = (entry->flags & SAVANXP_WM_WINDOW_FLAG_ACTIVE) != 0;
        int minimized = (entry->flags & SAVANXP_WM_WINDOW_FLAG_MINIMIZED) != 0;

        desktop_close_popups();
        taskbar_request(
            (active && !minimized) ? SAVANXP_WM_SHELL_MINIMIZE : SAVANXP_WM_SHELL_ACTIVATE,
            entry->window_id);
    }
    g_pressed_index = -1;
    actions |= DESKTOP_ACTION_REPAINT;
    return actions;
}

uint32_t desktop_taskbar_tick(void)
{
    uint32_t actions = DESKTOP_ACTION_NONE;

    if (g_list != 0 && g_list->sequence != g_last_sequence && taskbar_snapshot())
    {
        g_last_sequence = g_snapshot.sequence;
        actions |= DESKTOP_ACTION_REPAINT;
    }

    /* Changes made outside the desktop are caught here. The clock goes in the
     * same pass: the RTC has a resolution of one second and the text changes
     * once a minute. */
    if (g_indicator_countdown == 0)
    {
        int clock_changed = taskbar_clock_refresh();
        int indicators_changed = desktop_taskbar_refresh_indicators();

        if (clock_changed || indicators_changed)
        {
            actions |= DESKTOP_ACTION_REPAINT;
        }
        g_indicator_countdown = TASKBAR_LAYOUT_POLL_FRAMES;
    }
    else
    {
        g_indicator_countdown -= 1;
    }
    return actions;
}

void desktop_taskbar_paint(struct savanxp_gfx_context *gfx)
{
    taskbar_paint(gfx);
}
