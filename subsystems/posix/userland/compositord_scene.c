#include <stdint.h>
#include <string.h>

#include "compositord_scene.h"
#include "sxn/sxn.h"
/* After the SavanXP headers: it uses the cursor shape enum. */
#include "cursor_asset.h"

/*
 * The scene and its composition (docs/desktop/DESKTOP_ARCHITECTURE.md#the-
 * scene). Ported from windowd_render.c, where it lived until phase 4: layers
 * back to front, each painted once over damage ∩ its rectangle minus the
 * opaque layers in front of it, damage kept as an exact region.
 *
 * Everything a client can write -- its header, its batches -- is read as
 * hostile: geometry comes from the WM's ATTACH, and a batch that does not
 * check out marks the surface broken instead of being trusted.
 */

struct scene_surface
{
    int in_use;
    int broken;
    uint32_t flags;
    void *view;
    uint64_t section_size;
    struct savanxp_gpu_client_surface_header *header;
    struct savanxp_gpu_dirty_rect_batch *batches;
    uint32_t *pixels;
    uint32_t pitch;
    uint32_t capacity_width;
    uint32_t capacity_height;
    long wake_event;
    /* The last batch consumed; consumed but not yet composed (signalled after
     * the next compose); composed but not yet on the screen. */
    uint64_t consumed;
    uint64_t compose_pending;
    uint64_t retire_pending;
};

static struct scene_surface g_surfaces[SAVANXP_COMPOSITOR_MAX_SURFACES];

/* The WM's surface is composed from a private copy, refreshed at each commit
 * over the area the WM says it repainted. */
static long g_stage_section = -1;
static uint32_t *g_stage = 0;

static const struct savanxp_compositor_scene *g_scene_shared = 0;
static struct savanxp_compositor_scene g_scene;
static uint32_t g_layer_count = 0;

static uint32_t *g_display = 0;
static struct savanxp_fb_info g_display_info;

static struct sx_region g_damage;
static uint32_t g_broken = 0;

static int g_software_cursor = 0;
static struct scene_cursor g_cursor = {24, 24, SAVANXP_CURSOR_ARROW, 0};
/* Where the software cursor was last composed (visible = 0: nowhere). */
static struct scene_cursor g_cursor_drawn = {0, 0, SAVANXP_CURSOR_ARROW, 0};

static int map_failed(long value)
{
    return value < 0 && value > -4096;
}

static struct sx_rect rect_from(const struct savanxp_compositor_rect *rect)
{
    return sx_rect_make(rect->x, rect->y, rect->width, rect->height);
}

static struct sx_rect display_rect(void)
{
    return sx_rect_make(0, 0, (int)g_display_info.width, (int)g_display_info.height);
}

static void damage_rect(struct sx_rect rect)
{
    rect = sx_rect_intersect(rect, display_rect());
    if (!sx_rect_is_empty(rect))
    {
        sx_region_union_rect(&g_damage, rect);
    }
}

static struct scene_surface *surface_at(uint32_t surface_id)
{
    if (surface_id == 0 || surface_id >= SAVANXP_COMPOSITOR_MAX_SURFACES || !g_surfaces[surface_id].in_use)
    {
        return 0;
    }
    return &g_surfaces[surface_id];
}

static void cursor_bounds(const struct scene_cursor *cursor, struct sx_rect *out)
{
    const struct desktop_cursor_asset *asset;
    int shape = cursor->shape;

    if (shape < 0 || shape >= SAVANXP_CURSOR_SHAPE_COUNT)
    {
        shape = SAVANXP_CURSOR_ARROW;
    }
    asset = &k_desktop_cursor_assets[shape];
    *out = sx_rect_make(cursor->x - asset->hotspot_x, cursor->y - asset->hotspot_y, asset->width, asset->height);
}

static void damage_cursor(const struct scene_cursor *cursor)
{
    struct sx_rect bounds;

    if (cursor->visible)
    {
        cursor_bounds(cursor, &bounds);
        damage_rect(bounds);
    }
}

/* The source rectangle a layer may read: inside its surface's capacity. */
static int layer_source_valid(const struct savanxp_compositor_layer *layer, const struct scene_surface *surface)
{
    const struct savanxp_compositor_rect *src = &layer->src;

    return src->x >= 0 && src->y >= 0 && src->width > 0 && src->height > 0 &&
        layer->dst.width > 0 && layer->dst.height > 0 &&
        (uint32_t)src->x + (uint32_t)src->width <= surface->capacity_width &&
        (uint32_t)src->y + (uint32_t)src->height <= surface->capacity_height;
}

static int layer_scaled(const struct savanxp_compositor_layer *layer)
{
    return layer->src.width != layer->dst.width || layer->src.height != layer->dst.height;
}

/* Damages, on the screen, a rectangle of surface `surface_id` in its own
 * coordinates -- through every layer that shows it. A scaled layer is damaged
 * whole. */
static int damage_surface_rect(uint32_t surface_id, struct sx_rect local, int whole)
{
    uint32_t index;
    int shown = 0;

    for (index = 0; index < g_layer_count; ++index)
    {
        const struct savanxp_compositor_layer *layer = &g_scene.layers[index];
        struct sx_rect dst;

        if (layer->surface_id != surface_id)
        {
            continue;
        }
        shown = 1;
        dst = rect_from(&layer->dst);
        if (whole || layer_scaled(layer))
        {
            damage_rect(dst);
            continue;
        }
        damage_rect(sx_rect_intersect(
            sx_rect_make(dst.x - layer->src.x + local.x, dst.y - layer->src.y + local.y, local.width, local.height),
            dst));
    }
    return shown;
}

static void signal_wake(struct scene_surface *surface)
{
    if (surface->wake_event >= 0)
    {
        (void)sxn_event_set(surface->wake_event);
    }
}

static void mark_composed(struct scene_surface *surface, uint64_t sequence)
{
    if (surface->header != 0 && sequence != 0 && surface->header->composed_sequence < sequence)
    {
        surface->header->composed_sequence = sequence;
        signal_wake(surface);
    }
}

static void mark_retired(struct scene_surface *surface, uint64_t sequence)
{
    if (surface->header != 0 && sequence != 0 && surface->header->retired_sequence < sequence)
    {
        surface->header->retired_sequence = sequence;
        signal_wake(surface);
    }
}

/* Consumes the batches of one client surface, damaging through its layers. A
 * surface no layer shows is composed and retired at once: its client must
 * never stall on a window that is not on the screen. */
static void consume_surface(uint32_t surface_id, struct scene_surface *surface)
{
    uint64_t submitted;
    uint64_t next;
    int shown = 0;
    int consumed_any = 0;

    if (surface->header == 0 || surface->broken)
    {
        return;
    }
    submitted = *(volatile uint64_t *)&surface->header->submit_sequence;
    for (next = surface->consumed + 1u; next <= submitted; ++next)
    {
        const struct savanxp_gpu_dirty_rect_batch *batch =
            &surface->batches[(next - 1u) % SAVANXP_GPU_CLIENT_BATCH_CAPACITY];
        uint32_t rect_count = batch->rect_count;
        uint32_t rect_index;

        if (batch->submit_sequence != next || rect_count > SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS)
        {
            surface->broken = 1;
            g_broken |= 1u << surface_id;
            return;
        }
        if ((batch->flags & SAVANXP_GPU_SURFACE_PRESENT_BATCH_FLAG_FULL_SURFACE) != 0)
        {
            shown |= damage_surface_rect(surface_id, sx_rect_make(0, 0, 0, 0), 1);
        }
        else
        {
            for (rect_index = 0; rect_index < rect_count; ++rect_index)
            {
                const struct savanxp_gpu_dirty_rect *rect = &batch->rects[rect_index];

                if (rect->width == 0 || rect->height == 0 ||
                    rect->x >= surface->capacity_width || rect->y >= surface->capacity_height ||
                    rect->width > surface->capacity_width - rect->x ||
                    rect->height > surface->capacity_height - rect->y)
                {
                    surface->broken = 1;
                    g_broken |= 1u << surface_id;
                    return;
                }
                shown |= damage_surface_rect(
                    surface_id,
                    sx_rect_make((int)rect->x, (int)rect->y, (int)rect->width, (int)rect->height),
                    0);
            }
        }
        surface->consumed = next;
        consumed_any = 1;
    }
    if (!consumed_any)
    {
        return;
    }
    if (shown)
    {
        surface->compose_pending = surface->consumed;
    }
    else
    {
        mark_composed(surface, surface->consumed);
        mark_retired(surface, surface->consumed);
    }
}

void scene_consume_frames(void)
{
    uint32_t id;

    for (id = SAVANXP_COMPOSITOR_SURFACE_WM + 1u; id < SAVANXP_COMPOSITOR_MAX_SURFACES; ++id)
    {
        if (g_surfaces[id].in_use && (g_surfaces[id].flags & SAVANXP_COMPOSITOR_SURFACE_CLIENT) != 0)
        {
            consume_surface(id, &g_surfaces[id]);
        }
    }
}

void scene_set_display(uint32_t *pixels, const struct savanxp_fb_info *info)
{
    g_display = pixels;
    g_display_info = *info;
    sx_region_set_rect(&g_damage, display_rect());
    g_cursor_drawn.visible = 0;
}

void scene_set_software_cursor(int enabled)
{
    g_software_cursor = enabled;
    g_cursor_drawn.visible = 0;
    damage_cursor(&g_cursor);
}

int scene_map_scene_section(long section)
{
    long view = sxn_map_view(section, SXN_SECTION_READ);

    (void)sxn_close(section);
    if (map_failed(view))
    {
        return (int)view;
    }
    g_scene_shared = (const struct savanxp_compositor_scene *)(uintptr_t)view;
    return 0;
}

static void release_surface(struct scene_surface *surface)
{
    if (surface->view != 0)
    {
        (void)sxn_unmap_view((uint64_t)(uintptr_t)surface->view);
    }
    if (surface->wake_event >= 0)
    {
        (void)sxn_close(surface->wake_event);
    }
    memset(surface, 0, sizeof(*surface));
    surface->wake_event = -1;
}

int scene_attach(const struct savanxp_compositor_surface *desc, long section, long wake_event)
{
    struct scene_surface *surface;
    uint64_t pixel_bytes;
    long view;
    int client;

    if (desc == 0 || desc->surface_id == 0 || desc->surface_id >= SAVANXP_COMPOSITOR_MAX_SURFACES ||
        g_surfaces[desc->surface_id].in_use || desc->pitch == 0 ||
        desc->capacity_width == 0 || desc->capacity_height == 0 ||
        desc->capacity_width > desc->pitch / 4u)
    {
        goto refuse;
    }
    client = (desc->flags & SAVANXP_COMPOSITOR_SURFACE_CLIENT) != 0;
    if (client != (desc->surface_id != SAVANXP_COMPOSITOR_SURFACE_WM))
    {
        goto refuse;
    }
    pixel_bytes = (uint64_t)desc->pitch * desc->capacity_height;
    if ((uint64_t)desc->pixels_offset + pixel_bytes > desc->section_size ||
        (client && desc->pixels_offset <
            sizeof(struct savanxp_gpu_client_surface_header) +
            SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch)))
    {
        goto refuse;
    }

    view = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    (void)sxn_close(section);
    if (map_failed(view))
    {
        if (wake_event >= 0)
        {
            (void)sxn_close(wake_event);
        }
        return (int)view;
    }

    surface = &g_surfaces[desc->surface_id];
    memset(surface, 0, sizeof(*surface));
    surface->in_use = 1;
    surface->flags = desc->flags;
    surface->view = (void *)(uintptr_t)view;
    surface->section_size = desc->section_size;
    surface->pixels = (uint32_t *)((unsigned char *)surface->view + desc->pixels_offset);
    surface->pitch = desc->pitch;
    surface->capacity_width = desc->capacity_width;
    surface->capacity_height = desc->capacity_height;
    surface->wake_event = client ? wake_event : -1;
    if (client)
    {
        surface->header = (struct savanxp_gpu_client_surface_header *)surface->view;
        surface->batches = (struct savanxp_gpu_dirty_rect_batch *)
            ((unsigned char *)surface->view + sizeof(struct savanxp_gpu_client_surface_header));
        /* Whatever the client published before compositord had the surface
         * is consumed as of now (the WM puts it in the scene once it has). */
        surface->consumed = 0;
    }
    else
    {
        long stage;

        if (wake_event >= 0)
        {
            (void)sxn_close(wake_event);
        }
        stage = sxn_section_create(pixel_bytes, SXN_SECTION_READ | SXN_SECTION_WRITE);
        if (stage < 0)
        {
            release_surface(surface);
            return (int)stage;
        }
        view = sxn_map_view(stage, SXN_SECTION_READ | SXN_SECTION_WRITE);
        if (map_failed(view))
        {
            (void)sxn_close(stage);
            release_surface(surface);
            return (int)view;
        }
        g_stage_section = stage;
        g_stage = (uint32_t *)(uintptr_t)view;
    }
    return 0;

refuse:
    if (section >= 0)
    {
        (void)sxn_close(section);
    }
    if (wake_event >= 0)
    {
        (void)sxn_close(wake_event);
    }
    return -SAVANXP_EINVAL;
}

void scene_detach(uint32_t surface_id)
{
    struct scene_surface *surface = surface_at(surface_id);

    if (surface == 0)
    {
        return;
    }
    /* Whatever still shows it shows nothing now. */
    (void)damage_surface_rect(surface_id, sx_rect_make(0, 0, 0, 0), 1);
    if (surface_id == SAVANXP_COMPOSITOR_SURFACE_WM && g_stage != 0)
    {
        (void)sxn_unmap_view((uint64_t)(uintptr_t)g_stage);
        (void)sxn_close(g_stage_section);
        g_stage = 0;
        g_stage_section = -1;
    }
    release_surface(surface);
    g_broken &= ~(1u << surface_id);
}

static int layers_equal(const struct savanxp_compositor_layer *left, const struct savanxp_compositor_layer *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

/* Copies the WM's repaint of `rect` (screen coordinates, the WM surface's own)
 * into the stage. */
static void stage_rect(struct sx_rect rect)
{
    const struct scene_surface *wm = surface_at(SAVANXP_COMPOSITOR_SURFACE_WM);
    uint32_t stride;
    int row;

    if (wm == 0 || g_stage == 0)
    {
        return;
    }
    rect = sx_rect_intersect(rect, sx_rect_make(0, 0, (int)wm->capacity_width, (int)wm->capacity_height));
    if (sx_rect_is_empty(rect))
    {
        return;
    }
    stride = wm->pitch / 4u;
    for (row = rect.y; row < rect.y + rect.height; ++row)
    {
        memcpy(
            &g_stage[(size_t)row * stride + (size_t)rect.x],
            &wm->pixels[(size_t)row * stride + (size_t)rect.x],
            (size_t)rect.width * sizeof(uint32_t));
    }
}

int scene_commit(void)
{
    static struct savanxp_compositor_layer previous[SAVANXP_COMPOSITOR_MAX_LAYERS];
    uint32_t previous_count = g_layer_count;
    uint32_t damage_count;
    uint32_t index;
    struct scene_cursor cursor;

    if (g_scene_shared == 0)
    {
        return -SAVANXP_ENODEV;
    }
    memcpy(previous, g_scene.layers, sizeof(previous));
    memcpy(&g_scene, g_scene_shared, sizeof(g_scene));
    if (g_scene.magic != SAVANXP_COMPOSITOR_SCENE_MAGIC)
    {
        g_layer_count = 0;
        return -SAVANXP_EINVAL;
    }
    g_layer_count = g_scene.layer_count <= SAVANXP_COMPOSITOR_MAX_LAYERS ? g_scene.layer_count : 0u;

    /* Layers that changed, appeared or went away: their old and new places,
     * whatever damage the WM sent with them. */
    for (index = 0; index < SAVANXP_COMPOSITOR_MAX_LAYERS; ++index)
    {
        int had = index < previous_count;
        int has = index < g_layer_count;

        if (had && has && layers_equal(&previous[index], &g_scene.layers[index]))
        {
            continue;
        }
        if (had)
        {
            damage_rect(rect_from(&previous[index].dst));
        }
        if (has)
        {
            damage_rect(rect_from(&g_scene.layers[index].dst));
        }
    }

    /* The WM's own repaint: staged, and damaged. */
    damage_count = g_scene.damage_count;
    if ((g_scene.flags & SAVANXP_COMPOSITOR_SCENE_FULL_DAMAGE) != 0 || damage_count > SAVANXP_COMPOSITOR_MAX_DAMAGE)
    {
        stage_rect(display_rect());
        damage_rect(display_rect());
    }
    else
    {
        for (index = 0; index < damage_count; ++index)
        {
            struct sx_rect rect = rect_from(&g_scene.damage[index]);

            stage_rect(rect);
            damage_rect(rect);
        }
    }

    cursor.x = g_scene.cursor_x;
    cursor.y = g_scene.cursor_y;
    cursor.shape = (int)g_scene.cursor_shape;
    cursor.visible = g_scene.cursor_visible != 0u;
    if (g_software_cursor &&
        (cursor.x != g_cursor.x || cursor.y != g_cursor.y ||
         cursor.shape != g_cursor.shape || cursor.visible != g_cursor.visible))
    {
        damage_cursor(&g_cursor_drawn);
        damage_cursor(&cursor);
    }
    g_cursor = cursor;

    /* After the new layers: a window the WM just put in the scene shows the
     * frame that made it do so, at its new place. */
    scene_consume_frames();
    return 0;
}

int scene_has_damage(void)
{
    return !sx_region_is_empty(&g_damage);
}

static void paint_layer(struct sx_painter *painter, const struct savanxp_compositor_layer *layer)
{
    const struct scene_surface *surface = surface_at(layer->surface_id);
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;
    uint32_t *pixels;

    if (surface == 0)
    {
        return;
    }
    pixels = layer->surface_id == SAVANXP_COMPOSITOR_SURFACE_WM ? g_stage : surface->pixels;
    if (pixels == 0)
    {
        return;
    }
    memset(&info, 0, sizeof(info));
    info.width = (uint32_t)(layer->src.x + layer->src.width);
    info.height = (uint32_t)(layer->src.y + layer->src.height);
    info.pitch = surface->pitch;
    info.bpp = 32u;
    info.buffer_size = info.pitch * info.height;
    sx_bitmap_wrap(&bitmap, pixels, &info, SX_PIXEL_FORMAT_BGRX8888);
    if (layer_scaled(layer))
    {
        sx_painter_draw_scaled_bitmap_nearest(painter, &bitmap, rect_from(&layer->dst), rect_from(&layer->src));
    }
    else
    {
        sx_painter_blit_bitmap(painter, &bitmap, layer->dst.x - layer->src.x, layer->dst.y - layer->src.y);
    }
}

static void paint_cursor(struct sx_painter *painter, const struct scene_cursor *cursor)
{
    const struct desktop_cursor_asset *asset;
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;
    int shape = cursor->shape;

    if (shape < 0 || shape >= SAVANXP_CURSOR_SHAPE_COUNT)
    {
        shape = SAVANXP_CURSOR_ARROW;
    }
    asset = &k_desktop_cursor_assets[shape];
    memset(&info, 0, sizeof(info));
    info.width = (uint32_t)asset->width;
    info.height = (uint32_t)asset->height;
    info.pitch = (uint32_t)asset->width * (uint32_t)sizeof(uint32_t);
    info.bpp = 32u;
    info.buffer_size = info.pitch * info.height;
    sx_bitmap_wrap(&bitmap, (uint32_t *)asset->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_blit_bitmap(painter, &bitmap, cursor->x - asset->hotspot_x, cursor->y - asset->hotspot_y);
}

/* Whether a layer takes part: a surface that exists, a source inside it. */
static int layer_live(const struct savanxp_compositor_layer *layer)
{
    const struct scene_surface *surface = surface_at(layer->surface_id);

    return surface != 0 && layer_source_valid(layer, surface);
}

void scene_compose(struct sx_region *presented)
{
    /* One compositor thread: keep the working sets off the stack. */
    static struct sx_region visible;
    struct sx_bitmap display;
    struct sx_painter painter;
    struct sx_rect cursor_rect = sx_rect_make(0, 0, 0, 0);
    int draw_cursor = g_software_cursor && g_cursor.visible;
    uint32_t index;
    uint32_t id;

    sx_region_intersect_rect(&g_damage, display_rect());
    if (presented != 0)
    {
        sx_region_clear(presented);
    }
    if (g_display == 0 || sx_region_is_empty(&g_damage))
    {
        return;
    }
    if (draw_cursor)
    {
        cursor_bounds(&g_cursor, &cursor_rect);
    }

    sx_bitmap_wrap(&display, g_display, &g_display_info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &display);

    for (index = 0; index < g_layer_count; ++index)
    {
        const struct savanxp_compositor_layer *layer = &g_scene.layers[index];
        uint32_t front;

        if (!layer_live(layer))
        {
            continue;
        }
        sx_region_copy(&visible, &g_damage);
        sx_region_intersect_rect(&visible, rect_from(&layer->dst));
        for (front = index + 1u; front < g_layer_count && !sx_region_is_empty(&visible); ++front)
        {
            const struct savanxp_compositor_layer *above = &g_scene.layers[front];

            if ((above->flags & SAVANXP_COMPOSITOR_LAYER_OPAQUE) != 0 && layer_live(above))
            {
                sx_region_subtract_rect(&visible, rect_from(&above->dst));
            }
        }
        if (sx_region_is_empty(&visible))
        {
            continue;
        }
        sx_painter_clear_clip(&painter);
        if (sx_painter_push_clip_region(&painter, &visible))
        {
            paint_layer(&painter, layer);
            sx_painter_pop_clip(&painter);
        }
    }

    /* The software cursor, above everything; it blends, so it hides nothing. */
    if (draw_cursor)
    {
        sx_region_copy(&visible, &g_damage);
        sx_region_intersect_rect(&visible, cursor_rect);
        if (!sx_region_is_empty(&visible))
        {
            sx_painter_clear_clip(&painter);
            if (sx_painter_push_clip_region(&painter, &visible))
            {
                paint_cursor(&painter, &g_cursor);
                sx_painter_pop_clip(&painter);
            }
        }
        g_cursor_drawn = g_cursor;
    }
    else
    {
        g_cursor_drawn.visible = 0;
    }

    if (presented != 0)
    {
        sx_region_copy(presented, &g_damage);
    }
    sx_region_clear(&g_damage);

    for (id = SAVANXP_COMPOSITOR_SURFACE_WM + 1u; id < SAVANXP_COMPOSITOR_MAX_SURFACES; ++id)
    {
        struct scene_surface *surface = &g_surfaces[id];

        if (surface->in_use && surface->compose_pending != 0)
        {
            mark_composed(surface, surface->compose_pending);
            if (surface->retire_pending < surface->compose_pending)
            {
                surface->retire_pending = surface->compose_pending;
            }
            surface->compose_pending = 0;
        }
    }
}

void scene_retire(void)
{
    uint32_t id;

    for (id = SAVANXP_COMPOSITOR_SURFACE_WM + 1u; id < SAVANXP_COMPOSITOR_MAX_SURFACES; ++id)
    {
        struct scene_surface *surface = &g_surfaces[id];

        if (surface->in_use && surface->retire_pending != 0)
        {
            mark_retired(surface, surface->retire_pending);
            surface->retire_pending = 0;
        }
    }
}

struct scene_cursor scene_current_cursor(void)
{
    return g_cursor;
}

uint32_t scene_take_broken(void)
{
    uint32_t broken = g_broken;

    g_broken = 0;
    return broken;
}

void scene_reset(void)
{
    uint32_t id;

    for (id = 0; id < SAVANXP_COMPOSITOR_MAX_SURFACES; ++id)
    {
        if (g_surfaces[id].in_use)
        {
            scene_detach(id);
        }
    }
    if (g_scene_shared != 0)
    {
        (void)sxn_unmap_view((uint64_t)(uintptr_t)g_scene_shared);
        g_scene_shared = 0;
    }
    g_layer_count = 0;
    sx_region_clear(&g_damage);
}
