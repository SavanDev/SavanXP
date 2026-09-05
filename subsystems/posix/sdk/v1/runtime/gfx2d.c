#include "savanxp/libc.h"

static uint32_t sx_blend_bgra8888_over_rgb(uint32_t destination, uint32_t source)
{
    uint32_t alpha = (source >> 24) & 0xffu;
    uint32_t inverse_alpha = 255u - alpha;
    uint32_t source_blue = source & 0xffu;
    uint32_t source_green = (source >> 8) & 0xffu;
    uint32_t source_red = (source >> 16) & 0xffu;
    uint32_t destination_blue = destination & 0xffu;
    uint32_t destination_green = (destination >> 8) & 0xffu;
    uint32_t destination_red = (destination >> 16) & 0xffu;
    uint32_t blended_blue = 0;
    uint32_t blended_green = 0;
    uint32_t blended_red = 0;

    if (alpha == 0u)
    {
        return destination;
    }
    if (alpha == 255u)
    {
        return source & 0x00ffffffu;
    }

    blended_blue = ((source_blue * alpha) + (destination_blue * inverse_alpha) + 127u) / 255u;
    blended_green = ((source_green * alpha) + (destination_green * inverse_alpha) + 127u) / 255u;
    blended_red = ((source_red * alpha) + (destination_red * inverse_alpha) + 127u) / 255u;
    return (blended_red << 16) | (blended_green << 8) | blended_blue;
}

static int sx_clip_rect_to_bitmap(const struct sx_bitmap* bitmap, struct sx_rect* rect)
{
    int right = 0;
    int bottom = 0;

    if (bitmap == 0 || rect == 0 || bitmap->pixels == 0 || rect->width <= 0 || rect->height <= 0)
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
        rect->x >= (int)bitmap->info.width || rect->y >= (int)bitmap->info.height)
    {
        return 0;
    }

    right = rect->x + rect->width;
    bottom = rect->y + rect->height;
    if (right > (int)bitmap->info.width)
    {
        right = (int)bitmap->info.width;
    }
    if (bottom > (int)bitmap->info.height)
    {
        bottom = (int)bitmap->info.height;
    }
    rect->width = right - rect->x;
    rect->height = bottom - rect->y;
    return rect->width > 0 && rect->height > 0;
}

static struct sx_rect sx_apply_painter_clip(const struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0 || !painter->has_clip)
    {
        return rect;
    }
    return sx_rect_intersect(rect, painter->clip_rect);
}

/* Recorre los pedazos de dispositivo que sobreviven al clip: el rect pedido,
 * intersecado con el clip_rect y con la region si hay, y recortado al bitmap.
 *
 * Sin region rinde un solo pedazo, que es el caso de siempre. Con region rinde
 * uno por tramo, y los tramos son DISJUNTOS -- eso es lo que hace seguro que
 * cada primitiva se dibuje por pedazos: ningun pixel se toca dos veces, asi que
 * el texto antialiased no se re-mezcla ni los bordes se duplican. */
struct sx_clip_iter {
    const struct sx_painter* painter;
    struct sx_rect rect;
    int band;
    int span;
    int emitted_plain;
};

static void sx_clip_iter_init(struct sx_clip_iter* it, const struct sx_painter* painter, struct sx_rect device_rect)
{
    it->painter = painter;
    it->rect = sx_apply_painter_clip(painter, device_rect);
    it->band = 0;
    it->span = 0;
    it->emitted_plain = 0;
}

static int sx_clip_iter_next(struct sx_clip_iter* it, struct sx_rect* out)
{
    const struct sx_painter* painter = it->painter;
    const struct sx_region* region;

    if (sx_rect_is_empty(it->rect))
    {
        return 0;
    }

    region = painter->clip_region;
    if (region == 0 || region->band_count <= 0)
    {
        if (it->emitted_plain)
        {
            return 0;
        }
        it->emitted_plain = 1;
        *out = it->rect;
        return sx_clip_rect_to_bitmap(painter->target, out);
    }

    while (it->band < region->band_count)
    {
        const struct sx_region_band* band = &region->bands[it->band];
        const int y0 = band->y0 + painter->clip_region_origin.y;
        const int y1 = band->y1 + painter->clip_region_origin.y;

        if (it->span >= band->span_count)
        {
            it->band += 1;
            it->span = 0;
            continue;
        }
        {
            const struct sx_region_span* span = &band->spans[it->span];
            struct sx_rect piece = sx_rect_make(
                span->x0 + painter->clip_region_origin.x,
                y0,
                span->x1 - span->x0,
                y1 - y0);

            it->span += 1;
            piece = sx_rect_intersect(piece, it->rect);
            if (sx_rect_is_empty(piece))
            {
                continue;
            }
            *out = piece;
            if (!sx_clip_rect_to_bitmap(painter->target, out))
            {
                continue;
            }
            return 1;
        }
    }
    return 0;
}

/* Coordenadas del llamador -> coordenadas de dispositivo. Toda funcion publica
 * que recibe posiciones pasa por aca una sola vez; de ahi para adentro el
 * codigo trabaja en coordenadas de dispositivo, que es donde viven el clip, el
 * bitmap y el anclaje de las tramas. */
static struct sx_rect sx_to_device_rect(const struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0)
    {
        return rect;
    }
    return sx_rect_translate(rect, painter->origin.x, painter->origin.y);
}

const uint8_t sx_pattern_checker_50[8] = {
    0xaau, 0x55u, 0xaau, 0x55u, 0xaau, 0x55u, 0xaau, 0x55u,
};

struct sx_brush sx_brush_solid(uint32_t colour)
{
    struct sx_brush brush;
    memset(&brush, 0, sizeof(brush));
    brush.colour = colour;
    brush.back_colour = colour;
    return brush;
}

struct sx_brush sx_brush_pattern(const uint8_t pattern[8], uint32_t colour, uint32_t back_colour)
{
    struct sx_brush brush;
    int index;

    memset(&brush, 0, sizeof(brush));
    brush.colour = colour;
    brush.back_colour = back_colour;
    if (pattern == 0)
    {
        brush.back_colour = colour;
        return brush;
    }
    for (index = 0; index < 8; ++index)
    {
        brush.pattern[index] = pattern[index];
    }
    brush.has_pattern = 1;
    return brush;
}

struct sx_brush sx_brush_pattern_transparent(const uint8_t pattern[8], uint32_t colour)
{
    struct sx_brush brush = sx_brush_pattern(pattern, colour, colour);
    brush.transparent = brush.has_pattern;
    return brush;
}

/* 1 si la trama pinta en esa celda de la grilla de dispositivo. El AND con 7
 * mantiene el anclaje absoluto incluso para coordenadas negativas, porque las
 * posiciones ya vienen clipeadas al bitmap y son no negativas. */
static int sx_brush_covers(const struct sx_brush* brush, int device_x, int device_y)
{
    return (brush->pattern[device_y & 7] >> (7 - (device_x & 7))) & 1u;
}

struct sx_rect sx_rect_make(int x, int y, int width, int height)
{
    struct sx_rect rect = {x, y, width, height};
    return rect;
}

int sx_rect_is_empty(struct sx_rect rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

int sx_rect_right(struct sx_rect rect)
{
    return rect.x + rect.width;
}

int sx_rect_bottom(struct sx_rect rect)
{
    return rect.y + rect.height;
}

struct sx_rect sx_rect_translate(struct sx_rect rect, int dx, int dy)
{
    rect.x += dx;
    rect.y += dy;
    return rect;
}

struct sx_rect sx_rect_intersect(struct sx_rect left, struct sx_rect right)
{
    struct sx_rect result;
    int x = left.x > right.x ? left.x : right.x;
    int y = left.y > right.y ? left.y : right.y;
    int r = sx_rect_right(left) < sx_rect_right(right) ? sx_rect_right(left) : sx_rect_right(right);
    int b = sx_rect_bottom(left) < sx_rect_bottom(right) ? sx_rect_bottom(left) : sx_rect_bottom(right);

    result.x = x;
    result.y = y;
    result.width = r - x;
    result.height = b - y;
    if (result.width < 0)
    {
        result.width = 0;
    }
    if (result.height < 0)
    {
        result.height = 0;
    }
    return result;
}

struct sx_rect sx_rect_union(struct sx_rect left, struct sx_rect right)
{
    struct sx_rect result;
    int x = left.x < right.x ? left.x : right.x;
    int y = left.y < right.y ? left.y : right.y;
    int r = sx_rect_right(left) > sx_rect_right(right) ? sx_rect_right(left) : sx_rect_right(right);
    int b = sx_rect_bottom(left) > sx_rect_bottom(right) ? sx_rect_bottom(left) : sx_rect_bottom(right);

    if (sx_rect_is_empty(left))
    {
        return right;
    }
    if (sx_rect_is_empty(right))
    {
        return left;
    }

    result.x = x;
    result.y = y;
    result.width = r - x;
    result.height = b - y;
    return result;
}

int sx_rect_contains_point(struct sx_rect rect, int x, int y)
{
    return !sx_rect_is_empty(rect) &&
        x >= rect.x && y >= rect.y &&
        x < sx_rect_right(rect) &&
        y < sx_rect_bottom(rect);
}

void sx_bitmap_wrap(struct sx_bitmap* bitmap, uint32_t* pixels, const struct savanxp_fb_info* info, uint32_t format)
{
    if (bitmap == 0)
    {
        return;
    }

    memset(bitmap, 0, sizeof(*bitmap));
    if (info != 0)
    {
        bitmap->info = *info;
    }
    bitmap->pixels = pixels;
    bitmap->format = format;
}

void sx_painter_init(struct sx_painter* painter, struct sx_bitmap* bitmap)
{
    if (painter == 0)
    {
        return;
    }
    painter->target = bitmap;
    painter->clip_rect = sx_rect_make(0, 0, 0, 0);
    painter->has_clip = 0;
    painter->clip_depth = 0;
    painter->origin.x = 0;
    painter->origin.y = 0;
    painter->origin_depth = 0;
    painter->clip_region = 0;
    painter->clip_region_origin.x = 0;
    painter->clip_region_origin.y = 0;
}

int sx_painter_push_origin(struct sx_painter* painter, int dx, int dy)
{
    if (painter == 0 || painter->origin_depth >= SX_PAINTER_ORIGIN_STACK_DEPTH)
    {
        return 0;
    }

    painter->saved_origin[painter->origin_depth] = painter->origin;
    painter->origin_depth += 1;
    painter->origin.x += dx;
    painter->origin.y += dy;
    return 1;
}

void sx_painter_pop_origin(struct sx_painter* painter)
{
    if (painter == 0 || painter->origin_depth <= 0)
    {
        return;
    }
    painter->origin_depth -= 1;
    painter->origin = painter->saved_origin[painter->origin_depth];
}

struct sx_point sx_painter_origin(const struct sx_painter* painter)
{
    struct sx_point origin = {0, 0};
    if (painter != 0)
    {
        origin = painter->origin;
    }
    return origin;
}

struct sx_rect sx_painter_clip_bounds(const struct sx_painter* painter)
{
    struct sx_rect bounds;

    if (painter == 0)
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    if (painter->has_clip)
    {
        bounds = painter->clip_rect;
    }
    else if (painter->target != 0)
    {
        bounds = sx_rect_make(0, 0, (int)painter->target->info.width, (int)painter->target->info.height);
    }
    else
    {
        return sx_rect_make(0, 0, 0, 0);
    }
    return sx_rect_translate(bounds, -painter->origin.x, -painter->origin.y);
}

void sx_painter_clear_clip(struct sx_painter* painter)
{
    if (painter != 0)
    {
        painter->has_clip = 0;
        painter->clip_region = 0;
    }
}

void sx_painter_add_clip_rect(struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0)
    {
        return;
    }
    rect = sx_to_device_rect(painter, rect);
    if (!painter->has_clip)
    {
        painter->clip_rect = rect;
        painter->has_clip = 1;
        return;
    }
    painter->clip_rect = sx_rect_intersect(painter->clip_rect, rect);
}

/* Save the current clip and narrow it to the intersection with rect. Returns 0
 * if the stack is full (clip unchanged). Pair every successful push with a pop;
 * widget trees use this to confine each node's drawing to its parent. */
int sx_painter_push_clip(struct sx_painter* painter, struct sx_rect rect)
{
    if (painter == 0 || painter->clip_depth >= SX_PAINTER_CLIP_STACK_DEPTH)
    {
        return 0;
    }

    painter->saved_clip[painter->clip_depth] = painter->clip_rect;
    painter->saved_has_clip[painter->clip_depth] = painter->has_clip;
    painter->saved_clip_region[painter->clip_depth] = painter->clip_region;
    painter->saved_clip_region_origin[painter->clip_depth] = painter->clip_region_origin;
    painter->clip_depth += 1;

    rect = sx_to_device_rect(painter, rect);
    if (painter->has_clip)
    {
        painter->clip_rect = sx_rect_intersect(painter->clip_rect, rect);
    }
    else
    {
        painter->clip_rect = rect;
    }
    painter->has_clip = 1;
    return 1;
}

int sx_painter_push_clip_region(struct sx_painter* painter, const struct sx_region* region)
{
    struct sx_rect bounds;

    if (painter == 0 || painter->clip_depth >= SX_PAINTER_CLIP_STACK_DEPTH)
    {
        return 0;
    }
    if (region == 0 || sx_region_is_empty(region))
    {
        /* Region vacia = no se dibuja nada. Un clip degenerado lo expresa sin
         * necesidad de un flag aparte. */
        return sx_painter_push_clip(painter, sx_rect_make(0, 0, 0, 0));
    }

    /* El bounding box va al clip_rect: asi el descarte rapido de cada primitiva
     * sigue siendo una interseccion de rects y solo se recorren bandas cuando
     * el rect pedido de verdad cae adentro. */
    bounds = sx_region_bounds(region);
    if (!sx_painter_push_clip(painter, bounds))
    {
        return 0;
    }
    painter->clip_region = region;
    painter->clip_region_origin = painter->origin;
    return 1;
}

void sx_painter_pop_clip(struct sx_painter* painter)
{
    if (painter == 0 || painter->clip_depth <= 0)
    {
        return;
    }
    painter->clip_depth -= 1;
    painter->clip_rect = painter->saved_clip[painter->clip_depth];
    painter->has_clip = painter->saved_has_clip[painter->clip_depth];
    painter->clip_region = painter->saved_clip_region[painter->clip_depth];
    painter->clip_region_origin = painter->saved_clip_region_origin[painter->clip_depth];
}

/* Rellena un rect que YA esta en coordenadas de dispositivo. Es el punto por el
 * que pasa todo relleno solido: las funciones publicas traducen por el origen y
 * delegan aca, asi el origen se aplica exactamente una vez. */
static void sx_fill_rect_device(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    struct sx_clip_iter iter;
    struct sx_rect piece;

    for (sx_clip_iter_init(&iter, painter, rect); sx_clip_iter_next(&iter, &piece); )
    {
        gfx_rect(painter->target->pixels, &painter->target->info, piece.x, piece.y, piece.width, piece.height, colour);
    }
}

/* Igual que la anterior pero con brush. El recorrido por pixel solo se paga
 * cuando el brush tiene trama; el caso solido cae al relleno de rects de una. */
static void sx_fill_rect_device_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    struct sx_clip_iter iter;
    struct sx_rect piece;
    uint32_t stride;

    if (!brush->has_pattern)
    {
        sx_fill_rect_device(painter, rect, brush->colour);
        return;
    }

    stride = gfx_stride_pixels(&painter->target->info);
    for (sx_clip_iter_init(&iter, painter, rect); sx_clip_iter_next(&iter, &piece); )
    {
        int x;
        int y;

        for (y = piece.y; y < sx_rect_bottom(piece); ++y)
        {
            uint32_t* row = painter->target->pixels + ((size_t)y * stride);
            for (x = piece.x; x < sx_rect_right(piece); ++x)
            {
                if (sx_brush_covers(brush, x, y))
                {
                    row[x] = brush->colour;
                }
                else if (!brush->transparent)
                {
                    row[x] = brush->back_colour;
                }
            }
        }
    }
}

/* Las cuatro tiras del borde del rect ORIGINAL, cada una clipeada por separado.
 * Clipear el rect primero y enmarcar el resultado trazaria un borde espurio
 * alrededor de cada fragmento de repintado parcial (por ejemplo la huella de
 * daño del cursor sobre un dialogo), que es justo el bug de residuos. Pintar
 * los bordes verdaderos solo toca pixeles de borde que existen de verdad. */
static void sx_draw_frame_device(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    sx_fill_rect_device_brush(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), brush);
    if (rect.height > 1)
    {
        sx_fill_rect_device_brush(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), brush);
    }
    if (rect.height > 2)
    {
        sx_fill_rect_device_brush(painter, sx_rect_make(rect.x, rect.y + 1, 1, rect.height - 2), brush);
        if (rect.width > 1)
        {
            sx_fill_rect_device_brush(painter, sx_rect_make(rect.x + rect.width - 1, rect.y + 1, 1, rect.height - 2), brush);
        }
    }
}

void sx_painter_fill(struct sx_painter* painter, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    /* El target entero, no el area del origen actual: es un clear. */
    sx_fill_rect_device(
        painter,
        sx_rect_make(0, 0, (int)painter->target->info.width, (int)painter->target->info.height),
        colour);
}

void sx_painter_fill_rect(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_to_device_rect(painter, rect), colour);
}

void sx_painter_fill_rect_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    if (painter == 0 || painter->target == 0 || brush == 0)
    {
        return;
    }
    sx_fill_rect_device_brush(painter, sx_to_device_rect(painter, rect), brush);
}

void sx_painter_set_pixel(struct sx_painter* painter, int x, int y, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_rect_make(x + painter->origin.x, y + painter->origin.y, 1, 1), colour);
}

void sx_painter_hline(struct sx_painter* painter, int x, int y, int width, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_rect_make(x + painter->origin.x, y + painter->origin.y, width, 1), colour);
}

void sx_painter_vline(struct sx_painter* painter, int x, int y, int height, uint32_t colour)
{
    if (painter == 0 || painter->target == 0)
    {
        return;
    }
    sx_fill_rect_device(painter, sx_rect_make(x + painter->origin.x, y + painter->origin.y, 1, height), colour);
}

void sx_painter_draw_frame(struct sx_painter* painter, struct sx_rect rect, uint32_t colour)
{
    struct sx_brush brush = sx_brush_solid(colour);
    sx_painter_draw_frame_brush(painter, rect, &brush);
}

void sx_painter_draw_frame_brush(struct sx_painter* painter, struct sx_rect rect, const struct sx_brush* brush)
{
    if (painter == 0 || painter->target == 0 || brush == 0 || rect.width <= 0 || rect.height <= 0)
    {
        return;
    }
    sx_draw_frame_device(painter, sx_to_device_rect(painter, rect), brush);
}

/* Copia el pedazo `dst_rect` (dispositivo, ya clipeado) tomando del origen la
 * zona correspondiente. La separacion existe para que el clip por region pueda
 * llamarla una vez por tramo sin duplicar la logica de copia. */
static void sx_blit_bitmap_piece(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    int dst_x,
    int dst_y,
    struct sx_rect dst_rect)
{
    struct sx_rect src_rect;
    size_t row_bytes;
    uint32_t src_stride;
    uint32_t dst_stride;
    int row;

    src_rect = sx_rect_make(dst_rect.x - dst_x, dst_rect.y - dst_y, dst_rect.width, dst_rect.height);
    src_stride = gfx_stride_pixels(&source->info);
    dst_stride = gfx_stride_pixels(&painter->target->info);

    if (source->format == SX_PIXEL_FORMAT_BGRA8888)
    {
        for (row = 0; row < dst_rect.height; ++row)
        {
            uint32_t* destination = painter->target->pixels + ((size_t)(dst_rect.y + row) * dst_stride) + (size_t)dst_rect.x;
            const uint32_t* source_pixels = source->pixels + ((size_t)(src_rect.y + row) * src_stride) + (size_t)src_rect.x;
            int column;

            for (column = 0; column < dst_rect.width; ++column)
            {
                destination[column] = sx_blend_bgra8888_over_rgb(destination[column], source_pixels[column]);
            }
        }
        return;
    }

    row_bytes = (size_t)dst_rect.width * sizeof(uint32_t);

    /* Filas pegadas de los dos lados (el blit cubre el ancho completo de origen
     * y destino): el rectangulo es un solo tramo lineal y va en una sola copia
     * en vez de una llamada por fila. Pega en el blit de una superficie entera
     * -- fondo, cliente a pantalla completa -- no en los rects sucios chicos. */
    if (dst_rect.x == 0 && src_rect.x == 0 &&
        (uint32_t)dst_rect.width == dst_stride && (uint32_t)dst_rect.width == src_stride)
    {
        memcpy(
            painter->target->pixels + ((size_t)dst_rect.y * dst_stride),
            source->pixels + ((size_t)src_rect.y * src_stride),
            row_bytes * (size_t)dst_rect.height);
        return;
    }

    for (row = 0; row < dst_rect.height; ++row)
    {
        memcpy(
            painter->target->pixels + ((size_t)(dst_rect.y + row) * dst_stride) + (size_t)dst_rect.x,
            source->pixels + ((size_t)(src_rect.y + row) * src_stride) + (size_t)src_rect.x,
            row_bytes);
    }
}

void sx_painter_blit_bitmap(struct sx_painter* painter, const struct sx_bitmap* source, int dst_x, int dst_y)
{
    struct sx_clip_iter iter;
    struct sx_rect piece;

    if (painter == 0 || painter->target == 0 || source == 0 || source->pixels == 0)
    {
        return;
    }

    dst_x += painter->origin.x;
    dst_y += painter->origin.y;
    sx_clip_iter_init(
        &iter, painter,
        sx_rect_make(dst_x, dst_y, (int)source->info.width, (int)source->info.height));
    while (sx_clip_iter_next(&iter, &piece))
    {
        sx_blit_bitmap_piece(painter, source, dst_x, dst_y, piece);
    }
}

void sx_painter_draw_scaled_bitmap_nearest(
    struct sx_painter* painter,
    const struct sx_bitmap* source,
    struct sx_rect destination,
    struct sx_rect source_rect)
{
    struct sx_clip_iter iter;
    struct sx_rect target_rect;
    uint32_t src_stride;
    uint32_t dst_stride;
    int x;
    int y;

    if (painter == 0 || painter->target == 0 || source == 0 || source->pixels == 0 ||
        sx_rect_is_empty(destination) || sx_rect_is_empty(source_rect))
    {
        return;
    }

    /* El origen del muestreo lo provee el llamador: acotarlo al bitmap fuente es
     * lo unico que impide que un source_rect fuera de rango lea fuera del
     * buffer, porque source_x/source_y se derivan de el y no se vuelven a
     * validar en el bucle. */
    source_rect = sx_rect_intersect(
        source_rect,
        sx_rect_make(0, 0, (int)source->info.width, (int)source->info.height));
    if (sx_rect_is_empty(source_rect))
    {
        return;
    }

    destination = sx_to_device_rect(painter, destination);
    src_stride = gfx_stride_pixels(&source->info);
    dst_stride = gfx_stride_pixels(&painter->target->info);

    /* El muestreo se calcula siempre contra `destination` entera, no contra el
     * pedazo: asi cada pixel toma exactamente el mismo texel lo pinte de una o
     * partido en tramos por la region. */
    for (sx_clip_iter_init(&iter, painter, destination); sx_clip_iter_next(&iter, &target_rect); )
    {
        for (y = target_rect.y; y < sx_rect_bottom(target_rect); ++y)
        {
            int source_y = source_rect.y +
                (((y - destination.y) * source_rect.height) / destination.height);
            for (x = target_rect.x; x < sx_rect_right(target_rect); ++x)
            {
                int source_x = source_rect.x +
                    (((x - destination.x) * source_rect.width) / destination.width);
                uint32_t source_pixel = source->pixels[((size_t)source_y * src_stride) + (size_t)source_x];
                if (source->format == SX_PIXEL_FORMAT_BGRA8888)
                {
                    uint32_t* target = &painter->target->pixels[((size_t)y * dst_stride) + (size_t)x];
                    *target = sx_blend_bgra8888_over_rgb(*target, source_pixel);
                }
                else
                {
                    painter->target->pixels[((size_t)y * dst_stride) + (size_t)x] = source_pixel;
                }
            }
        }
    }
}

void sx_painter_draw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour)
{
    if (painter == 0 || painter->target == 0 || text == 0)
    {
        return;
    }
    x += painter->origin.x;
    y += painter->origin.y;

    if (painter->clip_region != 0)
    {
        /* Un blit por tramo. Los tramos son disjuntos, asi que ningun pixel de
         * glifo se mezcla dos veces -- que es exactamente lo que advierte el
         * comentario de gfx_blit_text_impl sobre repintar por fragmentos. */
        struct sx_clip_iter iter;
        struct sx_rect piece;
        struct sx_rect text_rect = sx_rect_make(x, y, gfx_text_width(text), gfx_text_height());

        for (sx_clip_iter_init(&iter, painter, text_rect); sx_clip_iter_next(&iter, &piece); )
        {
            gfx_blit_text_clip(painter->target->pixels, &painter->target->info, x, y, text, colour,
                                piece.x, piece.y, sx_rect_right(piece), sx_rect_bottom(piece));
        }
        return;
    }

    if (painter->has_clip)
    {
        struct sx_rect text_rect = sx_rect_make(x, y, gfx_text_width(text), gfx_text_height());
        text_rect = sx_rect_intersect(text_rect, painter->clip_rect);
        if (sx_rect_is_empty(text_rect))
        {
            return;
        }
        gfx_blit_text_clip(painter->target->pixels, &painter->target->info, x, y, text, colour,
                            painter->clip_rect.x, painter->clip_rect.y,
                            sx_rect_right(painter->clip_rect), sx_rect_bottom(painter->clip_rect));
        return;
    }
    gfx_blit_text(painter->target->pixels, &painter->target->info, x, y, text, colour);
}

/* ---- Regiones por bandas ------------------------------------------------
 *
 * Invariante canonico, mantenido por construccion en cada operacion:
 *   - bandas ordenadas por y, disjuntas y no vacias;
 *   - dentro de cada banda, tramos ordenados, disjuntos y NO adyacentes
 *     (dos tramos que se tocan se fusionan);
 *   - bandas consecutivas que se tocan y tienen tramos identicos se fusionan.
 * Sin eso la misma forma tendria varias representaciones y comparar o iterar
 * dejaria de ser predecible. */

enum sx_region_op {
    SX_REGION_OP_UNION = 0,
    SX_REGION_OP_SUBTRACT = 1,
    SX_REGION_OP_INTERSECT = 2,
};

void sx_region_clear(struct sx_region* region)
{
    if (region == 0)
    {
        return;
    }
    region->band_count = 0;
    region->overflowed = 0;
}

int sx_region_is_empty(const struct sx_region* region)
{
    return region == 0 || region->band_count <= 0;
}

int sx_region_overflowed(const struct sx_region* region)
{
    return region != 0 && region->overflowed;
}

void sx_region_copy(struct sx_region* destination, const struct sx_region* source)
{
    if (destination == 0)
    {
        return;
    }
    if (source == 0)
    {
        sx_region_clear(destination);
        return;
    }
    *destination = *source;
}

struct sx_rect sx_region_bounds(const struct sx_region* region)
{
    int x0 = 0;
    int x1 = 0;
    int index;

    if (sx_region_is_empty(region))
    {
        return sx_rect_make(0, 0, 0, 0);
    }

    x0 = region->bands[0].spans[0].x0;
    x1 = region->bands[0].spans[region->bands[0].span_count - 1].x1;
    for (index = 1; index < region->band_count; ++index)
    {
        const struct sx_region_band* band = &region->bands[index];
        if (band->spans[0].x0 < x0)
        {
            x0 = band->spans[0].x0;
        }
        if (band->spans[band->span_count - 1].x1 > x1)
        {
            x1 = band->spans[band->span_count - 1].x1;
        }
    }
    return sx_rect_make(
        x0,
        region->bands[0].y0,
        x1 - x0,
        region->bands[region->band_count - 1].y1 - region->bands[0].y0);
}

size_t sx_region_rect_count(const struct sx_region* region)
{
    size_t total = 0;
    int index;

    if (sx_region_is_empty(region))
    {
        return 0;
    }
    for (index = 0; index < region->band_count; ++index)
    {
        total += (size_t)region->bands[index].span_count;
    }
    return total;
}

int sx_region_contains_point(const struct sx_region* region, int x, int y)
{
    int index;

    if (sx_region_is_empty(region))
    {
        return 0;
    }
    for (index = 0; index < region->band_count; ++index)
    {
        const struct sx_region_band* band = &region->bands[index];
        int span;

        if (y < band->y0)
        {
            return 0; /* bandas ordenadas: ya lo pasamos */
        }
        if (y >= band->y1)
        {
            continue;
        }
        for (span = 0; span < band->span_count; ++span)
        {
            if (x >= band->spans[span].x0 && x < band->spans[span].x1)
            {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Colapsa a bounding box y marca. Lo llaman los caminos que se quedaron sin
 * bandas o sin tramos: preferimos un superset marcado antes que perder area en
 * silencio. */
static void sx_region_collapse(struct sx_region* region)
{
    struct sx_rect bounds = sx_region_bounds(region);

    region->band_count = 0;
    region->overflowed = 1;
    if (sx_rect_is_empty(bounds))
    {
        return;
    }
    region->band_count = 1;
    region->bands[0].y0 = bounds.y;
    region->bands[0].y1 = sx_rect_bottom(bounds);
    region->bands[0].span_count = 1;
    region->bands[0].spans[0].x0 = bounds.x;
    region->bands[0].spans[0].x1 = sx_rect_right(bounds);
}

/* Agrega un tramo a una lista ORDENADA fusionando lo que toque. Devuelve 0 si
 * no entraba. */
static int sx_span_list_add(struct sx_region_span* spans, int* count, int x0, int x1)
{
    int index = 0;
    int insert;

    if (x0 >= x1)
    {
        return 1;
    }

    /* Absorbe todo lo que solape o toque, quedandose con la envolvente. */
    while (index < *count)
    {
        if (spans[index].x1 < x0)
        {
            ++index;
            continue;
        }
        if (spans[index].x0 > x1)
        {
            break;
        }
        if (spans[index].x0 < x0)
        {
            x0 = spans[index].x0;
        }
        if (spans[index].x1 > x1)
        {
            x1 = spans[index].x1;
        }
        {
            int shift;
            for (shift = index; shift + 1 < *count; ++shift)
            {
                spans[shift] = spans[shift + 1];
            }
        }
        *count -= 1;
    }

    if (*count >= SX_REGION_MAX_SPANS_PER_BAND)
    {
        return 0;
    }
    for (insert = *count; insert > index; --insert)
    {
        spans[insert] = spans[insert - 1];
    }
    spans[index].x0 = x0;
    spans[index].x1 = x1;
    *count += 1;
    return 1;
}

/* Aplica la operacion de una fila: los tramos de la banda contra el tramo
 * [rx0, rx1) del rect. Escribe en `out` y devuelve 0 si no entro. */
static int sx_span_list_apply(
    const struct sx_region_span* spans,
    int count,
    int rx0,
    int rx1,
    int op,
    int rect_covers_row,
    struct sx_region_span* out,
    int* out_count)
{
    int index;

    *out_count = 0;

    if (op == SX_REGION_OP_UNION)
    {
        for (index = 0; index < count; ++index)
        {
            if (!sx_span_list_add(out, out_count, spans[index].x0, spans[index].x1))
            {
                return 0;
            }
        }
        if (rect_covers_row && !sx_span_list_add(out, out_count, rx0, rx1))
        {
            return 0;
        }
        return 1;
    }

    if (op == SX_REGION_OP_INTERSECT)
    {
        if (!rect_covers_row)
        {
            return 1; /* fuera del rect no queda nada */
        }
        for (index = 0; index < count; ++index)
        {
            int x0 = spans[index].x0 > rx0 ? spans[index].x0 : rx0;
            int x1 = spans[index].x1 < rx1 ? spans[index].x1 : rx1;
            if (x0 < x1 && !sx_span_list_add(out, out_count, x0, x1))
            {
                return 0;
            }
        }
        return 1;
    }

    /* SUBTRACT: cada tramo puede sobrevivir entero, partirse en dos o morir. */
    for (index = 0; index < count; ++index)
    {
        const int x0 = spans[index].x0;
        const int x1 = spans[index].x1;

        if (!rect_covers_row || rx1 <= x0 || rx0 >= x1)
        {
            if (!sx_span_list_add(out, out_count, x0, x1))
            {
                return 0;
            }
            continue;
        }
        if (x0 < rx0 && !sx_span_list_add(out, out_count, x0, rx0))
        {
            return 0;
        }
        if (rx1 < x1 && !sx_span_list_add(out, out_count, rx1, x1))
        {
            return 0;
        }
    }
    return 1;
}

static int sx_bands_equal(const struct sx_region_band* left, const struct sx_region_band* right)
{
    int index;

    if (left->span_count != right->span_count)
    {
        return 0;
    }
    for (index = 0; index < left->span_count; ++index)
    {
        if (left->spans[index].x0 != right->spans[index].x0 ||
            left->spans[index].x1 != right->spans[index].x1)
        {
            return 0;
        }
    }
    return 1;
}

/* Anexa una banda al resultado, fusionandola con la anterior si son contiguas y
 * tienen los mismos tramos (eso es lo que mantiene canonica la forma). */
static int sx_region_append_band(
    struct sx_region* out,
    int y0,
    int y1,
    const struct sx_region_span* spans,
    int span_count)
{
    struct sx_region_band* band;

    if (y0 >= y1 || span_count <= 0)
    {
        return 1;
    }

    if (out->band_count > 0)
    {
        struct sx_region_band* previous = &out->bands[out->band_count - 1];
        struct sx_region_band candidate;
        int index;

        candidate.span_count = span_count;
        for (index = 0; index < span_count; ++index)
        {
            candidate.spans[index] = spans[index];
        }
        if (previous->y1 == y0 && sx_bands_equal(previous, &candidate))
        {
            previous->y1 = y1;
            return 1;
        }
    }

    if (out->band_count >= SX_REGION_MAX_BANDS)
    {
        return 0;
    }
    band = &out->bands[out->band_count];
    band->y0 = y0;
    band->y1 = y1;
    band->span_count = span_count;
    {
        int index;
        for (index = 0; index < span_count; ++index)
        {
            band->spans[index] = spans[index];
        }
    }
    out->band_count += 1;
    return 1;
}

/* Motor unico de las tres operaciones contra un rect.
 *
 * Reconstruye la region cortando en Y por todos los bordes que importan (los de
 * cada banda y los del rect), y para cada franja resultante aplica la operacion
 * de tramos. Hacerlo asi -- en vez de una rutina por operacion -- es lo que
 * mantiene el invariante canonico en un solo lugar. */
static void sx_region_apply_rect(struct sx_region* region, struct sx_rect rect, int op)
{
    /* El resultado se arma aparte y recien al final se copia: las franjas leen
     * la region original mientras se construye la nueva. */
    static struct sx_region scratch;
    const int ry0 = rect.y;
    const int ry1 = sx_rect_bottom(rect);
    const int rx0 = rect.x;
    const int rx1 = sx_rect_right(rect);
    const int rect_empty = sx_rect_is_empty(rect);
    int cut = 0;
    int band_index;
    int ok = 1;

    if (region == 0)
    {
        return;
    }
    if (rect_empty)
    {
        /* Union y resta con un rect vacio no hacen nada; intersecar, todo. */
        if (op == SX_REGION_OP_INTERSECT)
        {
            sx_region_clear(region);
        }
        return;
    }
    if (op == SX_REGION_OP_UNION && sx_region_is_empty(region))
    {
        sx_region_set_rect(region, rect);
        return;
    }
    if (op != SX_REGION_OP_UNION && sx_region_is_empty(region))
    {
        return;
    }

    scratch.band_count = 0;
    scratch.overflowed = region->overflowed;

    /* Barrido por Y: en cada vuelta, `cut` es el tope de lo ya emitido y se
     * calcula el proximo borde relevante. */
    cut = region->bands[0].y0;
    if (op == SX_REGION_OP_UNION && ry0 < cut)
    {
        cut = ry0;
    }

    while (ok)
    {
        int next = 0;
        int have_next = 0;
        const struct sx_region_band* covering = 0;
        int rect_covers_row;
        struct sx_region_span out_spans[SX_REGION_MAX_SPANS_PER_BAND];
        int out_count = 0;

        /* Banda que cubre `cut`, y proximo borde por delante. */
        for (band_index = 0; band_index < region->band_count; ++band_index)
        {
            const struct sx_region_band* band = &region->bands[band_index];
            if (band->y0 <= cut && cut < band->y1)
            {
                covering = band;
                if (!have_next || band->y1 < next)
                {
                    next = band->y1;
                    have_next = 1;
                }
            }
            else if (band->y0 > cut && (!have_next || band->y0 < next))
            {
                next = band->y0;
                have_next = 1;
            }
        }
        if (op == SX_REGION_OP_UNION)
        {
            if (ry0 > cut && (!have_next || ry0 < next))
            {
                next = ry0;
                have_next = 1;
            }
            if (ry1 > cut && (!have_next || ry1 < next))
            {
                next = ry1;
                have_next = 1;
            }
        }
        else if (covering != 0)
        {
            if (ry0 > cut && ry0 < next)
            {
                next = ry0;
            }
            if (ry1 > cut && ry1 < next)
            {
                next = ry1;
            }
        }
        if (!have_next)
        {
            break;
        }

        rect_covers_row = (cut >= ry0 && cut < ry1);
        if (covering != 0 || (op == SX_REGION_OP_UNION && rect_covers_row))
        {
            ok = sx_span_list_apply(
                covering != 0 ? covering->spans : 0,
                covering != 0 ? covering->span_count : 0,
                rx0, rx1, op, rect_covers_row,
                out_spans, &out_count);
            if (ok)
            {
                ok = sx_region_append_band(&scratch, cut, next, out_spans, out_count);
            }
        }
        cut = next;
    }

    if (!ok)
    {
        /* No entro: quedarse con el superset. Para union y resta el bounding box
         * de lo que habia ya cubre; para union hay que incluir tambien el rect. */
        if (op == SX_REGION_OP_UNION)
        {
            struct sx_rect merged = sx_rect_union(sx_region_bounds(region), rect);
            sx_region_set_rect(region, merged);
        }
        else
        {
            sx_region_collapse(region);
        }
        region->overflowed = 1;
        return;
    }

    scratch.overflowed = region->overflowed;
    *region = scratch;
}

void sx_region_set_rect(struct sx_region* region, struct sx_rect rect)
{
    if (region == 0)
    {
        return;
    }
    sx_region_clear(region);
    if (sx_rect_is_empty(rect))
    {
        return;
    }
    region->band_count = 1;
    region->bands[0].y0 = rect.y;
    region->bands[0].y1 = sx_rect_bottom(rect);
    region->bands[0].span_count = 1;
    region->bands[0].spans[0].x0 = rect.x;
    region->bands[0].spans[0].x1 = sx_rect_right(rect);
}

void sx_region_union_rect(struct sx_region* region, struct sx_rect rect)
{
    sx_region_apply_rect(region, rect, SX_REGION_OP_UNION);
}

void sx_region_subtract_rect(struct sx_region* region, struct sx_rect rect)
{
    sx_region_apply_rect(region, rect, SX_REGION_OP_SUBTRACT);
}

void sx_region_intersect_rect(struct sx_region* region, struct sx_rect rect)
{
    sx_region_apply_rect(region, rect, SX_REGION_OP_INTERSECT);
}

void sx_rect_set_clear(struct sx_rect_set* set)
{
    if (set != 0)
    {
        memset(set, 0, sizeof(*set));
    }
}

static void sx_rect_set_compact(struct sx_rect_set* set)
{
    size_t index = 0;

    if (set == 0)
    {
        return;
    }

    while (index < set->count)
    {
        if (!sx_rect_is_empty(set->rects[index]))
        {
            ++index;
            continue;
        }

        while (index + 1 < set->count)
        {
            set->rects[index] = set->rects[index + 1];
            ++index;
        }
        set->count -= 1;
        if (index < SX_RECT_SET_CAPACITY)
        {
            memset(&set->rects[set->count], 0, sizeof(set->rects[set->count]));
        }
        index = 0;
    }
}

static int sx_ranges_overlap(int a_start, int a_end, int b_start, int b_end)
{
    return a_start < b_end && b_start < a_end;
}

static int sx_rects_should_merge(struct sx_rect left, struct sx_rect right)
{
    if (!sx_rect_is_empty(sx_rect_intersect(left, right)))
    {
        return 1;
    }
    if ((sx_rect_right(left) == right.x || sx_rect_right(right) == left.x) &&
        sx_ranges_overlap(left.y, sx_rect_bottom(left), right.y, sx_rect_bottom(right)))
    {
        return 1;
    }
    if ((sx_rect_bottom(left) == right.y || sx_rect_bottom(right) == left.y) &&
        sx_ranges_overlap(left.x, sx_rect_right(left), right.x, sx_rect_right(right)))
    {
        return 1;
    }
    return 0;
}

int sx_rect_set_add(struct sx_rect_set* set, struct sx_rect rect)
{
    size_t index = 0;

    if (set == 0 || sx_rect_is_empty(rect))
    {
        return 0;
    }

    for (index = 0; index < set->count; ++index)
    {
        struct sx_rect existing = set->rects[index];
        if (sx_rect_is_empty(existing))
        {
            continue;
        }

        if (sx_rects_should_merge(existing, rect))
        {
            set->rects[index] = sx_rect_union(existing, rect);
            sx_rect_set_compact(set);
            return 1;
        }
    }

    if (set->count < SX_RECT_SET_CAPACITY)
    {
        set->rects[set->count++] = rect;
        return 1;
    }

    set->rects[0] = sx_rect_union(set->rects[0], rect);
    for (index = 1; index < set->count; ++index)
    {
        set->rects[0] = sx_rect_union(set->rects[0], set->rects[index]);
        memset(&set->rects[index], 0, sizeof(set->rects[index]));
    }
    set->count = 1;
    return 1;
}

/* Append without merging adjacent rects. Subtraction relies on the resulting
 * sub-rects staying distinct (merging them back would re-cover the hole). On
 * overflow the rect is unioned into slot 0: that only ever grows coverage
 * (superset), which is safe for the visible region because the compositor
 * paints back-to-front and any over-painted area is re-covered by the layer in
 * front. It never under-paints. */
static void sx_rect_set_push_raw(struct sx_rect_set* set, struct sx_rect rect)
{
    if (set == 0 || sx_rect_is_empty(rect))
    {
        return;
    }
    if (set->count < SX_RECT_SET_CAPACITY)
    {
        set->rects[set->count++] = rect;
        return;
    }
    set->rects[0] = sx_rect_union(set->rects[0], rect);
}

int sx_rect_set_subtract_rect(struct sx_rect_set* set, struct sx_rect hole)
{
    struct sx_rect_set result;
    size_t index;

    if (set == 0)
    {
        return 0;
    }
    if (sx_rect_is_empty(hole))
    {
        return 1;
    }

    sx_rect_set_clear(&result);
    for (index = 0; index < set->count; ++index)
    {
        struct sx_rect r = set->rects[index];
        struct sx_rect overlap;

        if (sx_rect_is_empty(r))
        {
            continue;
        }

        overlap = sx_rect_intersect(r, hole);
        if (sx_rect_is_empty(overlap))
        {
            sx_rect_set_push_raw(&result, r);
            continue;
        }

        /* Up to four strips of r that fall outside the hole: above, below, and
         * the left/right slivers within the hole's vertical band. */
        if (overlap.y > r.y)
        {
            sx_rect_set_push_raw(&result, sx_rect_make(r.x, r.y, r.width, overlap.y - r.y));
        }
        if (sx_rect_bottom(overlap) < sx_rect_bottom(r))
        {
            sx_rect_set_push_raw(&result, sx_rect_make(r.x, sx_rect_bottom(overlap), r.width, sx_rect_bottom(r) - sx_rect_bottom(overlap)));
        }
        if (overlap.x > r.x)
        {
            sx_rect_set_push_raw(&result, sx_rect_make(r.x, overlap.y, overlap.x - r.x, overlap.height));
        }
        if (sx_rect_right(overlap) < sx_rect_right(r))
        {
            sx_rect_set_push_raw(&result, sx_rect_make(sx_rect_right(overlap), overlap.y, sx_rect_right(r) - sx_rect_right(overlap), overlap.height));
        }
    }

    *set = result;
    return 1;
}

int sx_rect_set_add_translated(struct sx_rect_set* set, struct sx_rect rect, int dx, int dy)
{
    return sx_rect_set_add(set, sx_rect_translate(rect, dx, dy));
}

struct sx_rect sx_rect_set_bounds(const struct sx_rect_set* set)
{
    struct sx_rect bounds = sx_rect_make(0, 0, 0, 0);
    size_t index;

    if (set == 0 || set->count == 0)
    {
        return bounds;
    }

    bounds = set->rects[0];
    for (index = 1; index < set->count; ++index)
    {
        bounds = sx_rect_union(bounds, set->rects[index]);
    }
    return bounds;
}

int sx_rect_set_valid(const struct sx_rect_set* set)
{
    return set != 0 && set->count != 0;
}
