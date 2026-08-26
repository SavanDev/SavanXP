#include "libc.h"
#include "shared/version.h"
#include "cursor_asset.h"
#include "desktop_icons.h"
#include "windowd_appinfo.h"
#include "desktop_wallpaper.h"
#include "windowd_layout.h"
#include "windowd_render.h"

#define WINDOWD_RGB_LITERAL(red, green, blue) (((uint32_t)(red) << 16) | ((uint32_t)(green) << 8) | (uint32_t)(blue))

static uint32_t *g_backbuffer = 0;

static const char *window_title_for_client(const struct windowd_client *client);

static void fill_embedded_bitmap_info(const struct desktop_embedded_bitmap *source, struct savanxp_fb_info *info)
{
    if (source == 0 || info == 0)
    {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->width = source->width;
    info->height = source->height;
    info->pitch = source->width * (uint32_t)sizeof(uint32_t);
    info->bpp = 32u;
    info->buffer_size = info->pitch * info->height;
}

static void draw_embedded_bitmap(struct sx_painter *painter, const struct desktop_embedded_bitmap *source, int x, int y)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info info;

    if (painter == 0 || source == 0 || source->pixels == 0 || source->width == 0 || source->height == 0)
    {
        return;
    }

    fill_embedded_bitmap_info(source, &info);
    sx_bitmap_wrap(&bitmap, (uint32_t *)source->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, x, y);
}

static int clip_rect_to_framebuffer(const struct savanxp_fb_info *info, struct sx_rect *rect)
{
    if (info == 0 || rect == 0 || rect->width <= 0 || rect->height <= 0)
    {
        return 0;
    }
    if (rect->x < 0)
    {
        rect->width += rect->x;
        rect->x = 0;
    }
    if (rect->y < 0)
    {
        rect->height += rect->y;
        rect->y = 0;
    }
    if (rect->width <= 0 || rect->height <= 0 ||
        rect->x >= (int)info->width || rect->y >= (int)info->height)
    {
        return 0;
    }
    if (sx_rect_right(*rect) > (int)info->width)
    {
        rect->width = (int)info->width - rect->x;
    }
    if (sx_rect_bottom(*rect) > (int)info->height)
    {
        rect->height = (int)info->height - rect->y;
    }
    return rect->width > 0 && rect->height > 0;
}

void windowd_set_backbuffer(uint32_t *pixels)
{
    g_backbuffer = pixels;
}

void windowd_dirty_rect_reset(struct windowd_dirty_rect *dirty)
{
    if (dirty != 0)
    {
        sx_rect_set_clear(&dirty->rects);
    }
}

void windowd_dirty_rect_add(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info, int x, int y, int width, int height)
{
    struct sx_rect rect = sx_rect_make(x, y, width, height);

    if (dirty == 0 || !clip_rect_to_framebuffer(info, &rect))
    {
        return;
    }
    (void)sx_rect_set_add(&dirty->rects, rect);
}

void windowd_dirty_rect_add_fullscreen(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info)
{
    if (info != 0)
    {
        windowd_dirty_rect_add(dirty, info, 0, 0, (int)info->width, (int)info->height);
    }
}

void windowd_dirty_rect_add_cursor(struct windowd_dirty_rect *dirty, const struct savanxp_fb_info *info, int cursor_x, int cursor_y, int shape)
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    windowd_cursor_bounds(cursor_x, cursor_y, shape, &x, &y, &width, &height);
    windowd_dirty_rect_add(dirty, info, x, y, width, height);
}

void windowd_dirty_rect_add_client(struct windowd_dirty_rect *dirty, const struct windowd_client *client)
{
    struct sx_rect rect;

    if (dirty == 0 || client == 0 || client->pid <= 0)
    {
        return;
    }
    rect = client->frame_visible ? windowd_client_frame_rect(client) : windowd_client_surface_rect(client);
    (void)sx_rect_set_add(&dirty->rects, rect);
}

int windowd_dirty_rect_valid(const struct windowd_dirty_rect *dirty)
{
    return dirty != 0 && sx_rect_set_valid(&dirty->rects);
}

size_t windowd_dirty_rect_count(const struct windowd_dirty_rect *dirty)
{
    return dirty != 0 ? dirty->rects.count : 0;
}

const struct sx_rect *windowd_dirty_rect_at(const struct windowd_dirty_rect *dirty, size_t index)
{
    if (dirty == 0 || index >= dirty->rects.count)
    {
        return 0;
    }
    return &dirty->rects.rects[index];
}

static void format_clock_text(char *buffer, unsigned int hours, unsigned int minutes)
{
    buffer[0] = (char)('0' + (hours / 10u));
    buffer[1] = (char)('0' + (hours % 10u));
    buffer[2] = ':';
    buffer[3] = (char)('0' + (minutes / 10u));
    buffer[4] = (char)('0' + (minutes % 10u));
    buffer[5] = '\0';
}

unsigned long windowd_current_clock_stamp(char *buffer)
{
    struct savanxp_realtime now = {0};

    if (realtime(&now) == 0 && now.valid != 0)
    {
        format_clock_text(buffer, (unsigned int)now.hour, (unsigned int)now.minute);
        return ((unsigned long)now.hour * 60UL) + (unsigned long)now.minute;
    }

    {
        unsigned long total_minutes = uptime_ms() / 60000UL;
        unsigned int hours = (unsigned int)((total_minutes / 60UL) % 24UL);
        unsigned int minutes = (unsigned int)(total_minutes % 60UL);
        format_clock_text(buffer, hours, minutes);
        return total_minutes % (24UL * 60UL);
    }
}

static const char *window_title_for_client(const struct windowd_client *client)
{
    if (client == 0)
    {
        return "App";
    }
    /* Ya resuelto al crear la ventana: .sxmeta > tabla por path > el path. */
    return windowd_presentation_label(&client->presentation, client->path);
}

static void draw_button(struct sx_painter *painter, struct sx_rect rect, uint32_t face, int pressed)
{
    const uint32_t shadow = gfx_rgb(88, 88, 88);
    const uint32_t dark = gfx_rgb(48, 48, 48);
    const uint32_t light = gfx_rgb(255, 255, 255);
    const uint32_t highlight = gfx_rgb(223, 223, 223);

    sx_painter_fill_rect(painter, rect, face);
    if (!pressed)
    {
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, 1), highlight);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, 1, rect.height - 2), highlight);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + rect.height - 2, rect.width - 2, 1), shadow);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 2, rect.y + 1, 1, rect.height - 2), shadow);
    }
    else
    {
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), dark);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, 1), shadow);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, 1, rect.height - 2), shadow);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), light);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + rect.height - 2, rect.width - 2, 1), highlight);
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 2, rect.y + 1, 1, rect.height - 2), highlight);
    }
}

static void draw_inset_box(struct sx_painter *painter, struct sx_rect rect, uint32_t face)
{
    const uint32_t shadow = gfx_rgb(88, 88, 88);
    const uint32_t dark = gfx_rgb(48, 48, 48);
    const uint32_t light = gfx_rgb(255, 255, 255);
    const uint32_t highlight = gfx_rgb(223, 223, 223);

    sx_painter_fill_rect(painter, rect, face);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), shadow);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), shadow);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, 1), dark);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + 1, 1, rect.height - 2), dark);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), light);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), light);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + 1, rect.y + rect.height - 2, rect.width - 2, 1), highlight);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 2, rect.y + 1, 1, rect.height - 2), highlight);
}
static void draw_close_button(struct sx_painter *painter, const struct windowd_client *client)
{
    struct sx_rect rect;
    int inset = 0;
    int glyph_size = 0;
    int index = 0;

    if (painter == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    rect = windowd_client_close_button_rect(client);
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    draw_button(painter, rect, gfx_rgb(196, 199, 203), 0);

    inset = rect.width >= 16 ? 4 : 3;
    glyph_size = rect.width - (inset * 2);
    if (glyph_size < 4)
    {
        glyph_size = 4;
    }

    for (index = 0; index < glyph_size; ++index)
    {
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + inset + index, rect.y + inset + index, 1, 1), gfx_rgb(32, 32, 32));
        sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - inset - 1 - index, rect.y + inset + index, 1, 1), gfx_rgb(32, 32, 32));
    }
}

static void draw_minimize_button(struct sx_painter *painter, const struct windowd_client *client)
{
    struct sx_rect rect;

    if (painter == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    rect = windowd_client_minimize_button_rect(client);
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    draw_button(painter, rect, gfx_rgb(196, 199, 203), 0);
    sx_painter_fill_rect(
        painter,
        sx_rect_make(rect.x + 4, rect.y + rect.height - 6, rect.width - 8, 2),
        gfx_rgb(32, 32, 32));
}

static void draw_maximize_button(struct sx_painter *painter, const struct windowd_client *client)
{
    struct sx_rect rect;

    if (painter == 0 || client == 0 || client->pid <= 0 || !client->frame_visible)
    {
        return;
    }

    rect = windowd_client_maximize_button_rect(client);
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    draw_button(painter, rect, gfx_rgb(196, 199, 203), 0);
    sx_painter_draw_frame(
        painter,
        sx_rect_make(rect.x + 4, rect.y + 4, rect.width - 8, rect.height - 8),
        gfx_rgb(32, 32, 32));
    if (client->maximized)
    {
        sx_painter_draw_frame(
            painter,
            sx_rect_make(rect.x + 6, rect.y + 6, rect.width - 8, rect.height - 8),
            gfx_rgb(32, 32, 32));
    }
}


static void draw_cursor(struct sx_painter *painter, int shape, int x, int y)
{
    const struct desktop_cursor_asset *asset;
    struct sx_bitmap cursor_bitmap;
    struct savanxp_fb_info info;

    if (shape < 0 || shape >= SAVANXP_CURSOR_SHAPE_COUNT)
    {
        shape = SAVANXP_CURSOR_ARROW;
    }
    asset = &k_desktop_cursor_assets[shape];

    info.width = (uint32_t)asset->width;
    info.height = (uint32_t)asset->height;
    info.pitch = (uint32_t)asset->width * sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = (uint32_t)(asset->width * asset->height) * sizeof(uint32_t);

    sx_bitmap_wrap(&cursor_bitmap, (uint32_t *)asset->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &cursor_bitmap, x - asset->hotspot_x, y - asset->hotspot_y);
}

static void draw_client(struct sx_painter *painter, const struct windowd_client *client)
{
    struct sx_bitmap bitmap;
    struct sx_rect surface_rect;
    struct sx_rect frame_rect;
    struct desktop_embedded_bitmap icon_storage;
    const struct desktop_embedded_bitmap *icon = windowd_presentation_icon(&client->presentation, &icon_storage);
    uint32_t title_colour = client != 0 && client->active
        ? windowd_presentation_accent(&client->presentation)
        : gfx_rgb(126, 132, 142);
    uint32_t frame_face = client != 0 && client->active
        ? gfx_rgb(208, 212, 219)
        : gfx_rgb(188, 192, 198);

    if (client == 0 || client->pid <= 0 || client->pixels == 0 || client->minimized)
    {
        return;
    }

    surface_rect = windowd_client_surface_rect(client);
    frame_rect = windowd_client_frame_rect(client);

    if (client->frame_visible)
    {
        draw_button(painter, frame_rect, frame_face, 0);
        sx_painter_fill_rect(painter, sx_rect_make(frame_rect.x + 2, frame_rect.y + 2, frame_rect.width - 4, WINDOWD_WINDOW_TITLEBAR_HEIGHT - 4), title_colour);
        draw_embedded_bitmap(painter, icon, frame_rect.x + 6, frame_rect.y + (WINDOWD_WINDOW_TITLEBAR_HEIGHT - 16) / 2);
        sx_painter_draw_text(painter, frame_rect.x + 26, frame_rect.y + (WINDOWD_WINDOW_TITLEBAR_HEIGHT - gfx_text_height()) / 2, window_title_for_client(client), gfx_rgb(255, 255, 255));
        draw_minimize_button(painter, client);
        draw_maximize_button(painter, client);
        draw_close_button(painter, client);
    }

    sx_bitmap_wrap(&bitmap, client->pixels, &client->surface_info, SX_PIXEL_FORMAT_BGRX8888);
    if (surface_rect.width != (int)client->surface_info.width ||
        surface_rect.height != (int)client->surface_info.height)
    {
        sx_painter_draw_scaled_bitmap_nearest(
            painter,
            &bitmap,
            surface_rect,
            sx_rect_make(0, 0, (int)client->surface_info.width, (int)client->surface_info.height));
    }
    else
    {
        sx_painter_blit_bitmap(painter, &bitmap, surface_rect.x, surface_rect.y);
    }
}

static long rect_set_total_area(const struct sx_rect_set *set)
{
    long area = 0;
    size_t index;

    for (index = 0; index < set->count; ++index)
    {
        if (!sx_rect_is_empty(set->rects[index]))
        {
            area += (long)set->rects[index].width * (long)set->rects[index].height;
        }
    }
    return area;
}

static int rect_set_intersects(const struct sx_rect_set *set, struct sx_rect hole)
{
    size_t index;

    for (index = 0; index < set->count; ++index)
    {
        if (!sx_rect_is_empty(sx_rect_intersect(set->rects[index], hole)))
        {
            return 1;
        }
    }
    return 0;
}

int windowd_region_selftest(void)
{
    struct sx_rect_set set;
    struct sx_rect base = sx_rect_make(0, 0, 100, 100);
    struct sx_rect hole;
    const long base_area = 100 * 100;

    /* Disjoint hole leaves the region untouched. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, base);
    (void)sx_rect_set_subtract_rect(&set, sx_rect_make(200, 200, 50, 50));
    if (rect_set_total_area(&set) != base_area)
    {
        return 1;
    }

    /* A hole that fully contains the rect leaves the region empty. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, sx_rect_make(10, 10, 20, 20));
    (void)sx_rect_set_subtract_rect(&set, sx_rect_make(0, 0, 100, 100));
    if (sx_rect_set_valid(&set) || rect_set_total_area(&set) != 0)
    {
        return 1;
    }

    /* Corner overlap: area shrinks by exactly the overlap, nothing left over
     * intersects the hole. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, base);
    hole = sx_rect_make(50, 50, 100, 100); /* 50x50 = 2500 inside base */
    (void)sx_rect_set_subtract_rect(&set, hole);
    if (rect_set_total_area(&set) != base_area - 2500 || rect_set_intersects(&set, hole))
    {
        return 1;
    }

    /* Centre hole splits the rect into a four-strip ring. */
    sx_rect_set_clear(&set);
    (void)sx_rect_set_add(&set, base);
    hole = sx_rect_make(40, 40, 20, 20); /* 400 in the middle */
    (void)sx_rect_set_subtract_rect(&set, hole);
    if (rect_set_total_area(&set) != base_area - 400 || rect_set_intersects(&set, hole))
    {
        return 1;
    }

    return 0;
}

/* Compositor layers, back-to-front. Each layer carries its screen-space bounds
 * and whether it is fully opaque (a valid occluder for layers behind it). The
 * compose pass paints every layer exactly once over its visible region only:
 * visible = damage ∩ bounds − (union of opaque bounds in front). */
enum windowd_layer_kind
{
    /* Solo el fallback: normalmente el fondo lo provee el cliente shellui. */
    WINDOWD_LAYER_BACKGROUND = 0,
    WINDOWD_LAYER_CLIENT,
    /* UI del WM (Ctrl+Esc): conmutador de ventanas, sobre todo salvo el cursor. */
    WINDOWD_LAYER_TASKLIST,
    WINDOWD_LAYER_CURSOR,
};

struct windowd_layer
{
    int kind;
    int opaque;
    struct sx_rect bounds;
    const struct windowd_client *client;
};

/* Tope de capas no-overlay simultaneas: background client, background(iconos),
 * shell_client, taskbar, welcome, menu, confirm, context menu, tasklist y
 * cursor = 10; +11 deja margen. */
#define WINDOWD_MAX_COMPOSE_LAYERS (WINDOWD_MAX_OVERLAY_CLIENTS + 11)

/* Un cliente se compone recien cuando publico su PRIMER frame. Antes de eso su
 * superficie esta en blanco y su geometria todavia puede cambiar -- una app
 * pide el tamano de su contenido durante el arranque (size hint, fd 11) --, asi
 * que dibujarla antes muestra por unos milisegundos una ventana vacia del
 * tamano generico que enseguida encoge. Mientras tanto el usuario igual tiene
 * feedback: any_overlay_client_starting() pone el cursor en WAIT. */
static int client_is_drawable(const struct windowd_client *client)
{
    return client != 0 && client->pid > 0 && client->pixels != 0 &&
        !client->minimized && client->consumed_submit_sequence > 0;
}

static struct sx_rect client_occluder_rect(const struct windowd_client *client)
{
    return client->frame_visible ? windowd_client_frame_rect(client) : windowd_client_surface_rect(client);
}

/* Task List (Ctrl+Esc): lista las ventanas abiertas -- incluidas las
 * minimizadas, que sin taskbar no tendrian otra via de vuelta -- con Switch To
 * / End Task, como el Task List de NT 3.5. */
static void draw_tasklist(struct sx_painter *painter, struct windowd_session *session)
{
    static const char *k_button_labels[WINDOWD_TASKLIST_BUTTON_COUNT] = {"Switch To", "End Task", "Cancel"};
    const struct savanxp_fb_info *info = &session->gfx.info;
    int task_count = windowd_task_count(session);
    struct sx_rect dialog = windowd_tasklist_rect(info, task_count);
    struct sx_rect list = windowd_tasklist_list_rect(info, task_count);
    int first = windowd_tasklist_first_visible(task_count, session->tasklist_selected);
    int visible = windowd_tasklist_visible_count(task_count);
    int index;

    sx_painter_fill_rect(painter, dialog, gfx_rgb(198, 202, 208));
    draw_button(painter, dialog, gfx_rgb(198, 202, 208), 0);

    sx_painter_fill_rect(
        painter,
        sx_rect_make(dialog.x + 3, dialog.y + 3, dialog.width - 6, WINDOWD_TASKLIST_TITLE_HEIGHT - 4),
        gfx_rgb(40, 76, 140));
    sx_painter_draw_text(
        painter,
        dialog.x + 9,
        dialog.y + 3 + ((WINDOWD_TASKLIST_TITLE_HEIGHT - 4 - gfx_text_height()) / 2),
        "Task List",
        gfx_rgb(255, 255, 255));

    draw_inset_box(painter, list, gfx_rgb(255, 255, 255));
    if (task_count == 0)
    {
        sx_painter_draw_text(painter, list.x + 6, list.y + 3, "(sin ventanas abiertas)", gfx_rgb(112, 116, 122));
    }

    for (index = 0; index < visible; ++index)
    {
        int task_index = first + index;
        int is_shell = 0;
        int slot = -1;
        const struct windowd_client *client = windowd_task_client(session, task_index, &is_shell, &slot);
        struct sx_rect row = windowd_tasklist_item_rect(info, task_count, index);
        const char *label = "Ventana";
        int selected = (task_index == session->tasklist_selected);

        if (client == 0 || sx_rect_is_empty(row))
        {
            continue;
        }
        label = window_title_for_client(client);

        if (selected)
        {
            sx_painter_fill_rect(painter, row, gfx_rgb(0, 0, 128));
        }
        if (sx_painter_push_clip(painter, row))
        {
            sx_painter_draw_text(
                painter,
                row.x + 5,
                row.y + ((row.height - gfx_text_height()) / 2),
                label,
                selected ? gfx_rgb(255, 255, 255) : gfx_rgb(16, 20, 24));
            /* Marca las minimizadas: son las que sin taskbar quedarian perdidas. */
            if (client->minimized)
            {
                sx_painter_draw_text(
                    painter,
                    row.x + row.width - gfx_text_width("(minimizada)") - 6,
                    row.y + ((row.height - gfx_text_height()) / 2),
                    "(minimizada)",
                    selected ? gfx_rgb(198, 208, 226) : gfx_rgb(112, 116, 122));
            }
            sx_painter_pop_clip(painter);
        }
    }

    for (index = 0; index < WINDOWD_TASKLIST_BUTTON_COUNT; ++index)
    {
        struct sx_rect rect = windowd_tasklist_button_rect(info, task_count, index);
        int label_width = gfx_text_width(k_button_labels[index]);

        draw_button(painter, rect, gfx_rgb(198, 202, 208), 0);
        sx_painter_draw_text(
            painter,
            rect.x + ((rect.width - label_width) / 2),
            rect.y + ((rect.height - gfx_text_height()) / 2),
            k_button_labels[index],
            gfx_rgb(16, 20, 24));
    }
}

/* Capas del WM: superficies de clientes y el cursor. Las compone el compositor
 * del WM y se quedan aca cuando el shell pase a ser un cliente aparte (A2). */
static void wm_paint_layer(
    struct sx_painter *painter,
    struct windowd_session *session,
    const struct windowd_layer *layer,
    int cursor_x,
    int cursor_y)
{
    switch (layer->kind)
    {
    case WINDOWD_LAYER_BACKGROUND:
        /* Fallback: el fondo normalmente lo provee shellui. Releemos el modo
         * persistido para respetar el fondo que haya elegido progman, que es
         * otro proceso y no puede tocar nuestro estado en memoria. */
        (void)desktop_wallpaper_reload();
        desktop_wallpaper_draw(painter, &session->gfx.info);
        break;
    case WINDOWD_LAYER_CLIENT:
        draw_client(painter, layer->client);
        break;
    case WINDOWD_LAYER_TASKLIST:
        draw_tasklist(painter, session);
        break;
    case WINDOWD_LAYER_CURSOR:
        draw_cursor(painter, session->current_cursor_shape, cursor_x, cursor_y);
        break;
    default:
        break;
    }
}

/* Ya no hay capas de shell: retirado el chrome Win95 (A2.4c), todo lo que
 * compone windowd es suyo -- fondo de fallback, superficies de clientes, Task
 * List y cursor. El chrome vive en procesos cliente: el fondo en shellui y el
 * launcher en progman. */
/* Back-to-front layer list for the current frame. */
static int build_layers(
    struct windowd_session *session,
    int cursor_x,
    int cursor_y,
    struct windowd_layer *layers)
{
    const struct savanxp_fb_info *info = &session->gfx.info;
    int count = 0;
    int order_index;
    int cur_x = 0;
    int cur_y = 0;
    int cur_w = 0;
    int cur_h = 0;

    /* Fondo del z-order. Con el chrome retirado (A2.4c) el escritorio es solo
     * wallpaper, y lo dibuja el cliente shellui: su superficie es la capa opaca
     * de mas atras. Si no esta listo (boot antes del primer frame, o murio),
     * windowd lo dibuja el mismo como fallback para no dejar el fondo sin
     * pintar. Ya no hay capa de iconos ni taskbar encima. */
    if (client_is_drawable(&session->background_client))
    {
        layers[count].kind = WINDOWD_LAYER_CLIENT;
        layers[count].opaque = 1;
        layers[count].bounds = client_occluder_rect(&session->background_client);
        layers[count].client = &session->background_client;
        ++count;
    }
    else
    {
        layers[count].kind = WINDOWD_LAYER_BACKGROUND;
        layers[count].opaque = 1;
        layers[count].bounds = sx_rect_make(0, 0, (int)info->width, (int)info->height);
        layers[count].client = 0;
        ++count;
    }

    if (session->fullscreen_slot >= 0 && session->fullscreen_slot < WINDOWD_MAX_OVERLAY_CLIENTS)
    {
        const struct windowd_client *fullscreen_client = &session->overlay_clients[session->fullscreen_slot];
        if (client_is_drawable(fullscreen_client))
        {
            layers[count].kind = WINDOWD_LAYER_CLIENT;
            layers[count].opaque = 1;
            layers[count].bounds = sx_rect_make(0, 0, (int)info->width, (int)info->height);
            layers[count].client = fullscreen_client;
            ++count;
        }
        /* Tambien en fullscreen: si no, Ctrl+Esc abriria un Task List invisible
         * y no habria forma de salir de una app a pantalla completa. */
        if (session->tasklist_open)
        {
            layers[count].kind = WINDOWD_LAYER_TASKLIST;
            layers[count].opaque = 1;
            layers[count].bounds = windowd_tasklist_rect(info, windowd_task_count(session));
            layers[count].client = 0;
            ++count;
        }
        if (!session->hw_cursor_enabled)
        {
            windowd_cursor_bounds(cursor_x, cursor_y, session->current_cursor_shape, &cur_x, &cur_y, &cur_w, &cur_h);
            layers[count].kind = WINDOWD_LAYER_CURSOR;
            layers[count].opaque = 0;
            layers[count].bounds = sx_rect_make(cur_x, cur_y, cur_w, cur_h);
            layers[count].client = 0;
            ++count;
        }
        return count;
    }

    if (client_is_drawable(&session->shell_client))
    {
        layers[count].kind = WINDOWD_LAYER_CLIENT;
        layers[count].opaque = 1;
        layers[count].bounds = client_occluder_rect(&session->shell_client);
        layers[count].client = &session->shell_client;
        ++count;
    }

    for (order_index = 0; order_index < session->overlay_count; ++order_index)
    {
        int slot = session->overlay_order[order_index];
        if (slot < 0 || slot >= WINDOWD_MAX_OVERLAY_CLIENTS)
        {
            continue;
        }
        if (!client_is_drawable(&session->overlay_clients[slot]))
        {
            continue;
        }
        layers[count].kind = WINDOWD_LAYER_CLIENT;
        layers[count].opaque = 1;
        layers[count].bounds = client_occluder_rect(&session->overlay_clients[slot]);
        layers[count].client = &session->overlay_clients[slot];
        ++count;
    }

    /* El Task List va sobre todo: es el conmutador de ventanas. */
    if (session->tasklist_open)
    {
        layers[count].kind = WINDOWD_LAYER_TASKLIST;
        layers[count].opaque = 1;
        layers[count].bounds = windowd_tasklist_rect(info, windowd_task_count(session));
        layers[count].client = 0;
        ++count;
    }

    if (!session->hw_cursor_enabled)
    {
        windowd_cursor_bounds(cursor_x, cursor_y, session->current_cursor_shape, &cur_x, &cur_y, &cur_w, &cur_h);
        layers[count].kind = WINDOWD_LAYER_CURSOR;
        layers[count].opaque = 0; /* BGRA cursor blends; never an occluder. */
        layers[count].bounds = sx_rect_make(cur_x, cur_y, cur_w, cur_h);
        layers[count].client = 0;
        ++count;
    }

    return count;
}

void windowd_draw_desktop(
    struct windowd_session *session,
    int cursor_x,
    int cursor_y,
    const struct windowd_dirty_rect *dirty)
{
    /* Single-threaded compositor: keep the working sets off the stack. */
    static struct windowd_layer layers[WINDOWD_MAX_COMPOSE_LAYERS];
    static struct sx_rect_set damage;
    static struct sx_rect_set visible;
    struct sx_bitmap backbuffer_bitmap;
    struct sx_painter painter;
    int layer_count = 0;
    int layer_index;

    if (session == 0 || g_backbuffer == 0)
    {
        return;
    }

    sx_bitmap_wrap(&backbuffer_bitmap, g_backbuffer, &session->gfx.info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &backbuffer_bitmap);

    /* Damage region for this frame; an empty/invalid dirty set forces a full
     * repaint (still occlusion-aware: each layer painted once). */
    sx_rect_set_clear(&damage);
    if (dirty != 0 && windowd_dirty_rect_valid(dirty))
    {
        size_t i;
        for (i = 0; i < dirty->rects.count; ++i)
        {
            (void)sx_rect_set_add(&damage, dirty->rects.rects[i]);
        }
    }
    if (!sx_rect_set_valid(&damage))
    {
        (void)sx_rect_set_add(&damage, sx_rect_make(0, 0, (int)session->gfx.info.width, (int)session->gfx.info.height));
    }

    layer_count = build_layers(session, cursor_x, cursor_y, layers);

    /* Paint back-to-front; each layer only over the area not covered by an
     * opaque layer in front of it. */
    for (layer_index = 0; layer_index < layer_count; ++layer_index)
    {
        const struct windowd_layer *layer = &layers[layer_index];
        size_t damage_index;
        size_t visible_index;
        int front;

        sx_rect_set_clear(&visible);
        for (damage_index = 0; damage_index < damage.count; ++damage_index)
        {
            struct sx_rect clipped = sx_rect_intersect(damage.rects[damage_index], layer->bounds);
            if (!sx_rect_is_empty(clipped))
            {
                (void)sx_rect_set_add(&visible, clipped);
            }
        }

        for (front = layer_index + 1; front < layer_count && sx_rect_set_valid(&visible); ++front)
        {
            if (layers[front].opaque)
            {
                (void)sx_rect_set_subtract_rect(&visible, layers[front].bounds);
            }
        }

        for (visible_index = 0; visible_index < visible.count; ++visible_index)
        {
            struct sx_rect sub = visible.rects[visible_index];
            if (sx_rect_is_empty(sub))
            {
                continue;
            }
            sx_painter_clear_clip(&painter);
            sx_painter_add_clip_rect(&painter, sub);
            wm_paint_layer(&painter, session, layer, cursor_x, cursor_y);
        }
    }
}
